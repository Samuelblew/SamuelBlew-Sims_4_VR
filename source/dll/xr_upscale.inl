// xr_upscale.inl: da imagem do jogo para a imagem de cada olho do headset, com upscaling.
// Incluído por xr_session.inl (thread de render, dentro do Present).
//
// O jogo desenha cada olho no próprio backbuffer (tamanho da janela, ex.: 1920x1009), mas o headset quer
// outra resolução e outro formato de pixel (ex.: 2080x2096 por olho, quase quadrado). Antes a cópia era
// direta (o olho ficava com a resolução da janela). Agora a imagem do olho tem a resolução do headset
// (vezes o supersampling de saída) e é reconstruída por:
//   Bilinear   = filtro simples;
//   FSR 1.0    = AMD FidelityFX Super Resolution (EASU: upscaling com bordas + RCAS: nitidez);
//   CAS        = AMD Contrast Adaptive Sharpening (redimensiona e dá nitidez numa passada).
// "Desligado" mantém a cópia direta antiga. Os shaders vêm de fsr\*.hlsl (bytecode do fxc).
// O estado do pipeline do jogo fica intacto (ID3DDeviceContextState próprio, como o do cursor).

#include "fsr/out/sh_VSFull.h"
#include "fsr/out/sh_PSEasu.h"
#include "fsr/out/sh_PSRcas.h"
#include "fsr/out/sh_PSBilinear.h"
#include "fsr/out/sh_PSCas.h"

enum { UP_OFF = 0, UP_BILINEAR = 1, UP_FSR = 2, UP_CAS = 3 };
static int   g_upMethod   = UP_FSR;    // vr_upscaler: 0 desligado, 1 bilinear, 2 FSR, 3 CAS
static float g_upSharp    = 0.6f;      // vr_nitidez: 0 = suave .. 1 = máxima
static float g_outScale   = 1.0f;      // vr_supersampling: resolução da imagem do olho x a recomendada pelo headset

static ID3DDeviceContextState* g_upState = nullptr;
static ID3D11DeviceContext1*   g_upCtx1 = nullptr;
static ID3D11VertexShader*     g_upVS = nullptr;
static ID3D11PixelShader*      g_upPS[4] = {};      // EASU, RCAS, bilinear, CAS
static ID3D11Buffer*           g_upCB = nullptr;
static ID3D11SamplerState*     g_upSamp = nullptr;
static ID3D11RasterizerState*  g_upRast = nullptr;
static bool                    g_upFailed = false;
// cópia do backbuffer (o backbuffer não pode ser lido por shader) e imagem intermediária do FSR
static ID3D11Texture2D* g_upSrc = nullptr; static ID3D11ShaderResourceView* g_upSrcSRV = nullptr;
static uint32_t g_upSrcW = 0, g_upSrcH = 0; static DXGI_FORMAT g_upSrcFmt = DXGI_FORMAT_UNKNOWN;
static ID3D11Texture2D* g_upMid = nullptr; static ID3D11ShaderResourceView* g_upMidSRV = nullptr; static ID3D11RenderTargetView* g_upMidRTV = nullptr;
static uint32_t g_upMidW = 0, g_upMidH = 0;
struct UpRtv { ID3D11RenderTargetView* v; bool linearize; };
static std::map<ID3D11Texture2D*, UpRtv> g_upRTV;

// Tamanho da imagem de cada olho para um backbuffer w x h.
static void UpscaleOutSize(uint32_t bw, uint32_t bh, uint32_t& ow, uint32_t& oh) {
    if (g_upMethod == UP_OFF || !g_eyeWidth || !g_eyeHeight) { ow = bw; oh = bh; return; }
    float s = (std::max)(0.5f, (std::min)(2.0f, g_outScale));
    ow = (uint32_t)(g_eyeWidth * s + 0.5f); oh = (uint32_t)(g_eyeHeight * s + 0.5f);
    if (g_eyeMaxW && ow > g_eyeMaxW) ow = g_eyeMaxW;
    if (g_eyeMaxH && oh > g_eyeMaxH) oh = g_eyeMaxH;
    ow &= ~1u; oh &= ~1u;
}

static bool UpscaleInit() {
    if (g_upFailed) return false;
    if (g_upState) return true;
    g_upFailed = true;
    ID3D11Device* dev = g_gameDevice;
    bool ok = SUCCEEDED(dev->CreateVertexShader(g_shVSFull, sizeof(g_shVSFull), nullptr, &g_upVS)) &&
              SUCCEEDED(dev->CreatePixelShader(g_shPSEasu, sizeof(g_shPSEasu), nullptr, &g_upPS[0])) &&
              SUCCEEDED(dev->CreatePixelShader(g_shPSRcas, sizeof(g_shPSRcas), nullptr, &g_upPS[1])) &&
              SUCCEEDED(dev->CreatePixelShader(g_shPSBilinear, sizeof(g_shPSBilinear), nullptr, &g_upPS[2])) &&
              SUCCEEDED(dev->CreatePixelShader(g_shPSCas, sizeof(g_shPSCas), nullptr, &g_upPS[3]));
    if (!ok) { Log("vr: upscaling: shaders FALHARAM"); return false; }
    D3D11_BUFFER_DESC bd = { 64, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    dev->CreateBuffer(&bd, nullptr, &g_upCB);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_upSamp);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_upRast);
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    if (!ctx || FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_upCtx1))) { if (ctx) ctx->Release(); Log("vr: upscaling: sem ID3D11DeviceContext1"); return false; }
    ctx->Release();
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) { Log("vr: upscaling: sem ID3D11Device1"); return false; }
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), got;
    HRESULT hr = dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &got, &g_upState);
    dev1->Release();
    if (FAILED(hr)) { Log("vr: upscaling: CreateDeviceContextState falhou (0x%08X)", (unsigned)hr); return false; }
    g_upFailed = false;
    Log("vr: upscaling pronto (FSR 1.0, CAS, bilinear)");
    return true;
}

static DXGI_FORMAT TypelessOf(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    default: return f;
    }
}
static DXGI_FORMAT UnormOf(DXGI_FORMAT f) {
    switch (TypelessOf(f)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

// Vista de desenho na imagem do headset: sem conversão (bytes iguais aos do jogo) se a imagem aceitar uma
// vista UNORM; senão a vista é _SRGB e o shader lineariza a cor antes (linearize = true).
static UpRtv UpRtvFor(ID3D11Texture2D* img) {
    auto it = g_upRTV.find(img);
    if (it != g_upRTV.end()) return it->second;
    D3D11_TEXTURE2D_DESC td; img->GetDesc(&td);
    UpRtv r = { nullptr, false };
    D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    bool typeless = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS || td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || td.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    rv.Format = typeless ? UnormOf(td.Format) : td.Format;
    r.linearize = !typeless && (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    if (FAILED(g_gameDevice->CreateRenderTargetView(img, &rv, &r.v))) { r.v = nullptr; Log("vr: upscaling: vista da imagem do olho falhou (formato %d)", (int)td.Format); }
    else Log("vr: upscaling: imagem do olho formato %d -> vista %d (linearizar=%d)", (int)td.Format, (int)rv.Format, (int)r.linearize);
    g_upRTV.emplace(img, r);
    return r;
}

static bool UpEnsureSrc(ID3D11Texture2D* bb) {
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    if (g_upSrc && d.Width == g_upSrcW && d.Height == g_upSrcH && d.Format == g_upSrcFmt) return true;
    if (g_upSrcSRV) { g_upSrcSRV->Release(); g_upSrcSRV = nullptr; }
    if (g_upSrc) { g_upSrc->Release(); g_upSrc = nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = d.Width; td.Height = d.Height; td.MipLevels = 1; td.ArraySize = 1; td.Format = TypelessOf(d.Format);
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = UnormOf(d.Format); sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_upSrc)) || FAILED(g_gameDevice->CreateShaderResourceView(g_upSrc, &sv, &g_upSrcSRV))) {
        Log("vr: upscaling: copia do backbuffer %ux%u formato %d FALHOU", d.Width, d.Height, (int)d.Format); g_upFailed = true; return false;
    }
    g_upSrcW = d.Width; g_upSrcH = d.Height; g_upSrcFmt = d.Format;
    return true;
}
static bool UpEnsureMid(uint32_t w, uint32_t h) {
    if (g_upMid && w == g_upMidW && h == g_upMidH) return true;
    if (g_upMidSRV) { g_upMidSRV->Release(); g_upMidSRV = nullptr; }
    if (g_upMidRTV) { g_upMidRTV->Release(); g_upMidRTV = nullptr; }
    if (g_upMid) { g_upMid->Release(); g_upMid = nullptr; }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_upMid)) || FAILED(g_gameDevice->CreateShaderResourceView(g_upMid, nullptr, &g_upMidSRV)) ||
        FAILED(g_gameDevice->CreateRenderTargetView(g_upMid, nullptr, &g_upMidRTV))) { Log("vr: upscaling: imagem intermediaria FALHOU"); g_upFailed = true; return false; }
    g_upMidW = w; g_upMidH = h;
    return true;
}

static uint32_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// Uma passada de tela cheia: src -> rtv (w x h) com o shader ps e as constantes c[16].
static void UpPass(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h, ID3D11PixelShader* ps, const uint32_t c[16]) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_upCtx1->Map(g_upCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, c, 64); g_upCtx1->Unmap(g_upCB, 0); }
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    g_upCtx1->RSSetViewports(1, &vp);
    g_upCtx1->RSSetState(g_upRast);
    g_upCtx1->OMSetRenderTargets(1, &rtv, nullptr);
    g_upCtx1->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    g_upCtx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_upCtx1->IASetInputLayout(nullptr);
    g_upCtx1->VSSetShader(g_upVS, nullptr, 0);
    g_upCtx1->PSSetShader(ps, nullptr, 0);
    g_upCtx1->PSSetConstantBuffers(0, 1, &g_upCB);
    g_upCtx1->PSSetShaderResources(0, 1, &src);
    g_upCtx1->PSSetSamplers(0, 1, &g_upSamp);
    g_upCtx1->Draw(3, 0);
    ID3D11ShaderResourceView* noSrv = nullptr; ID3D11RenderTargetView* noRtv = nullptr;
    g_upCtx1->PSSetShaderResources(0, 1, &noSrv);
    g_upCtx1->OMSetRenderTargets(1, &noRtv, nullptr);
}

// Backbuffer -> imagem do olho (ow x oh). Devolve false se não deu (quem chama faz a cópia direta).
static bool UpscaleInto(ID3D11Texture2D* dst, ID3D11Texture2D* bb, uint32_t ow, uint32_t oh) {
    if (g_upMethod == UP_OFF || !UpscaleInit() || !UpEnsureSrc(bb)) return false;
    UpRtv out = UpRtvFor(dst);
    if (!out.v) return false;
    int method = g_upMethod;
    float iw = (float)g_upSrcW, ih = (float)g_upSrcH, fw = (float)ow, fh = (float)oh;
    bool casOk = (fw * fh) / (iw * ih) <= 4.0f;               // CAS redimensiona até 4x a área
    if (method == UP_FSR && (fw < iw || fh < ih)) method = casOk ? UP_CAS : UP_BILINEAR;   // EASU só aumenta
    if (method == UP_CAS && !casOk) method = UP_FSR;
    if (method == UP_FSR && !UpEnsureMid(ow, oh)) method = UP_BILINEAR;
    float sharp = (std::max)(0.0f, (std::min)(1.0f, g_upSharp));
    ID3DDeviceContextState* prev = nullptr;
    g_upCtx1->SwapDeviceContextState(g_upState, &prev);
    g_upCtx1->CopyResource(g_upSrc, bb);
    uint32_t c[16] = {};
    if (method == UP_FSR) {
        // EASU (FsrEasuCon)
        c[0] = FBits(iw / fw); c[1] = FBits(ih / fh); c[2] = FBits(0.5f * iw / fw - 0.5f); c[3] = FBits(0.5f * ih / fh - 0.5f);
        c[4] = FBits(1.0f / iw); c[5] = FBits(1.0f / ih); c[6] = FBits(1.0f / iw); c[7] = FBits(-1.0f / ih);
        c[8] = FBits(-1.0f / iw); c[9] = FBits(2.0f / ih); c[10] = FBits(1.0f / iw); c[11] = FBits(2.0f / ih);
        c[12] = FBits(0.0f); c[13] = FBits(4.0f / ih); c[14] = 0; c[15] = 0;
        UpPass(g_upSrcSRV, g_upMidRTV, ow, oh, g_upPS[0], c);
        // RCAS (FsrRcasCon): nitidez 0..1 -> 2..0 "stops" (0 = máxima)
        uint32_t r[16] = {};
        r[0] = FBits(exp2f(-(2.0f - 2.0f * sharp))); r[15] = out.linearize ? 1u : 0u;
        UpPass(g_upMidSRV, out.v, ow, oh, g_upPS[1], r);
    } else if (method == UP_CAS) {
        // CasSetup
        float s = -1.0f / (8.0f + (5.0f - 8.0f) * sharp);
        c[0] = FBits(iw / fw); c[1] = FBits(ih / fh); c[2] = FBits(0.5f * iw / fw - 0.5f); c[3] = FBits(0.5f * ih / fh - 0.5f);
        c[4] = FBits(s); c[5] = 0; c[6] = FBits(8.0f * iw / fw); c[7] = 0;
        c[15] = out.linearize ? 1u : 0u;
        UpPass(g_upSrcSRV, out.v, ow, oh, g_upPS[3], c);
    } else {
        c[0] = FBits(1.0f / fw); c[1] = FBits(1.0f / fh); c[15] = out.linearize ? 1u : 0u;
        UpPass(g_upSrcSRV, out.v, ow, oh, g_upPS[2], c);
    }
    g_upCtx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    return true;
}

// Desenha uma textura (srv) num retângulo do backbuffer do jogo (o menu de configurações no monitor).
// (a vista do backbuffer é criada e solta a cada chamada: guardar referência impediria o jogo de
// redimensionar o swapchain)
static void BlitToBackbuffer(IDXGISwapChain* sc, ID3D11ShaderResourceView* srv, int x, int y, int w, int h) {
    if (!srv || w <= 0 || h <= 0 || !UpscaleInit()) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC td; bb->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rv = {}; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rv.Format = UnormOf(td.Format);
    ID3D11RenderTargetView* rtv = nullptr;
    if (td.SampleDesc.Count != 1 || FAILED(g_gameDevice->CreateRenderTargetView(bb, &rv, &rtv))) rtv = nullptr;
    if (rtv) {
        ID3DDeviceContextState* prev = nullptr;
        g_upCtx1->SwapDeviceContextState(g_upState, &prev);
        uint32_t c[16] = {};
        c[0] = FBits(1.0f / w); c[1] = FBits(1.0f / h); c[2] = FBits((float)x); c[3] = FBits((float)y);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_upCtx1->Map(g_upCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, c, 64); g_upCtx1->Unmap(g_upCB, 0); }
        D3D11_VIEWPORT vp = { (float)x, (float)y, (float)w, (float)h, 0, 1 };
        g_upCtx1->RSSetViewports(1, &vp);
        g_upCtx1->RSSetState(g_upRast);
        g_upCtx1->OMSetRenderTargets(1, &rtv, nullptr);
        g_upCtx1->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        g_upCtx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_upCtx1->IASetInputLayout(nullptr);
        g_upCtx1->VSSetShader(g_upVS, nullptr, 0);
        g_upCtx1->PSSetShader(g_upPS[2], nullptr, 0);
        g_upCtx1->PSSetConstantBuffers(0, 1, &g_upCB);
        g_upCtx1->PSSetShaderResources(0, 1, &srv);
        g_upCtx1->PSSetSamplers(0, 1, &g_upSamp);
        g_upCtx1->Draw(3, 0);
        ID3D11ShaderResourceView* noSrv = nullptr; ID3D11RenderTargetView* noRtv = nullptr;
        g_upCtx1->PSSetShaderResources(0, 1, &noSrv);
        g_upCtx1->OMSetRenderTargets(1, &noRtv, nullptr);
        g_upCtx1->SwapDeviceContextState(prev, nullptr);
        if (prev) prev->Release();
        rtv->Release();
    }
    bb->Release();
}

// A imagem do olho mudou de tamanho (configuração): os swapchains antigos são trocados no próximo frame.
static void UpscaleForgetImages() { for (auto& kv : g_upRTV) if (kv.second.v) kv.second.v->Release(); g_upRTV.clear(); }
