// xr_cursor.inl: cursor do mouse no headset. Incluído por xr_session.inl.
//
// O cursor do Windows é desenhado pelo sistema por cima da janela (cursor de hardware): ele nunca
// entra no backbuffer, por isso aparecia só no monitor. Aqui ele é copiado (mesmo desenho, mesmo
// ponto quente) para uma textura e desenhado em cada imagem que vai para o headset, na mesma
// posição em pixels da janela, logo depois da cópia do backbuffer.
// O estado do contexto do jogo fica intacto: o desenho roda num ID3DDeviceContextState próprio
// (SwapDeviceContextState troca e destroca todo o estado do pipeline).
#include <d3dcompiler.h>
#include <map>

static bool  g_cursorOn    = true;        // vr_cursor
static volatile bool g_cursorForce = false; // laser na interface: desenhar a seta mesmo com o cursor escondido
static float g_cursorScale = 1.0f;        // vr_cursor_escala

static ID3D11DeviceContext1*  g_curCtx1 = nullptr;
static ID3DDeviceContextState* g_curState = nullptr;
static ID3D11VertexShader*    g_curVS = nullptr;
static ID3D11PixelShader*     g_curPS = nullptr;
static ID3D11Buffer*          g_curCB = nullptr;
static ID3D11BlendState*      g_curBlend = nullptr;
static ID3D11SamplerState*    g_curSamp = nullptr;
static ID3D11RasterizerState* g_curRast = nullptr;
static ID3D11ShaderResourceView* g_curSRV = nullptr;
static HCURSOR g_curHandle = nullptr;
static int     g_curW = 0, g_curH = 0, g_curHotX = 0, g_curHotY = 0;
static bool    g_curFailed = false;
static std::map<ID3D11Texture2D*, ID3D11RenderTargetView*> g_curRTV;

static const char* kCursorHlsl = R"(
cbuffer C : register(b0) { float4 rect; };
Texture2D tex : register(t0);
SamplerState smp : register(s0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    float2 uv = float2(id & 1, id >> 1);
    V o; o.p = float4(lerp(rect.x, rect.z, uv.x), lerp(rect.y, rect.w, uv.y), 0, 1); o.uv = uv; return o;
}
float4 ps(V i) : SV_Target { return tex.Sample(smp, i.uv); }
)";

static bool CursorInit() {
    if (g_curFailed) return false;
    if (g_curState) return true;
    g_curFailed = true;                                       // só tenta uma vez
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { Log("vr: cursor: d3dcompiler_47.dll indisponivel"); return false; }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kCursorHlsl, strlen(kCursorHlsl), "cursor", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kCursorHlsl, strlen(kCursorHlsl), "cursor", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        Log("vr: cursor: shader nao compilou: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    ID3D11Device* dev = g_gameDevice;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_curVS);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_curPS);
    vsb->Release(); psb->Release();
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
    dev->CreateBuffer(&bd, nullptr, &g_curCB);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;            // cor pré-multiplicada
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bl, &g_curBlend);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_curSamp);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_curRast);

    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (!ctx || FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_curCtx1))) {
        if (ctx) ctx->Release();
        Log("vr: cursor: sem ID3D11DeviceContext1"); return false;
    }
    ctx->Release();
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1))) { Log("vr: cursor: sem ID3D11Device1"); return false; }
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel(), got;
    UINT flags = (dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    HRESULT hr = dev1->CreateDeviceContextState(flags, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), &got, &g_curState);
    dev1->Release();
    if (FAILED(hr)) { Log("vr: cursor: CreateDeviceContextState falhou (0x%08X)", (unsigned)hr); return false; }
    if (!g_curVS || !g_curPS || !g_curCB || !g_curBlend || !g_curSamp || !g_curRast) { Log("vr: cursor: recursos nao criados"); return false; }
    g_curFailed = false;
    Log("vr: cursor pronto (desenhado nas imagens do headset)");
    return true;
}

// Converte o cursor do Windows numa textura com alfa (desenha sobre preto e sobre branco:
// alfa = 255 - (branco - preto); funciona também para cursores monocromáticos e invertidos).
static bool CursorTexture(HCURSOR hc) {
    if (hc == g_curHandle && g_curSRV) return true;
    if (g_curSRV) { g_curSRV->Release(); g_curSRV = nullptr; }
    g_curHandle = hc;
    ICONINFO ii = {};
    if (!GetIconInfo(hc, &ii)) return false;
    BITMAP bm = {};
    GetObjectA(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
    int w = bm.bmWidth, h = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    g_curHotX = (int)ii.xHotspot; g_curHotY = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return false;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    uint32_t *pb = nullptr, *pw = nullptr;
    HBITMAP bb = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&pb, nullptr, 0);
    HBITMAP bw = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void**)&pw, nullptr, 0);
    bool ok = bb && bw && pb && pw;
    if (ok) {
        for (int i = 0; i < w * h; ++i) { pb[i] = 0xff000000; pw[i] = 0xffffffff; }
        HGDIOBJ old = SelectObject(mdc, bb); DrawIconEx(mdc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(mdc, bw); DrawIconEx(mdc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(mdc, old);
        GdiFlush();
        std::vector<uint32_t> px(w * h);
        for (int i = 0; i < w * h; ++i) {
            int b0 = pb[i] & 0xff, g0 = (pb[i] >> 8) & 0xff, r0 = (pb[i] >> 16) & 0xff;
            int g1 = (pw[i] >> 8) & 0xff;
            int a = 255 - (g1 - g0); a = a < 0 ? 0 : (a > 255 ? 255 : a);
            if (g1 < g0) { a = 255; r0 = g0 = b0 = 255; }    // pixel invertido (XOR): fica branco
            px[i] = (uint32_t)b0 | ((uint32_t)g0 << 8) | ((uint32_t)r0 << 16) | ((uint32_t)a << 24);   // BGRA pré-multiplicado
        }
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { px.data(), (UINT)(w * 4), 0 };
        ID3D11Texture2D* t = nullptr;
        ok = SUCCEEDED(g_gameDevice->CreateTexture2D(&td, &sd, &t)) && t &&
             SUCCEEDED(g_gameDevice->CreateShaderResourceView(t, nullptr, &g_curSRV));
        if (t) t->Release();
    }
    if (bb) DeleteObject(bb);
    if (bw) DeleteObject(bw);
    DeleteDC(mdc); ReleaseDC(nullptr, sdc);
    g_curW = w; g_curH = h;
    static int logged = 0;
    if (logged++ < 3) Log("vr: cursor %dx%d (ponto quente %d,%d) %s", w, h, g_curHotX, g_curHotY, ok ? "ok" : "FALHOU");
    return ok;
}

// Desenha o cursor na imagem (w x h = mesmo tamanho do backbuffer copiado). hwnd = janela do jogo.
static void DrawCursorOn(ID3D11Texture2D* img, uint32_t w, uint32_t h, HWND hwnd) {
    if (!g_cursorOn || !img || !hwnd) return;
    CURSORINFO ci = { sizeof(ci) };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) {
        if (!g_cursorForce) return;
        ci.hCursor = LoadCursor(nullptr, IDC_ARROW);
        GetCursorPos(&ci.ptScreenPos);
    }
    POINT pt = ci.ptScreenPos;
    if (!ScreenToClient(hwnd, &pt)) return;
    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom) return;
    if (!CursorInit() || !CursorTexture(ci.hCursor)) return;

    auto it = g_curRTV.find(img);
    if (it == g_curRTV.end()) {
        D3D11_TEXTURE2D_DESC td; img->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rv = {};
        rv.Format = td.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                  : td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : td.Format;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* v = nullptr;
        if (FAILED(g_gameDevice->CreateRenderTargetView(img, &rv, &v))) v = nullptr;
        it = g_curRTV.emplace(img, v).first;
        if (!v) Log("vr: cursor: RTV da imagem do headset falhou (formato %d)", (int)td.Format);
    }
    if (!it->second) return;

    float sx = (float)w / (float)rc.right, sy = (float)h / (float)rc.bottom;
    float x0 = (pt.x - g_curHotX * g_cursorScale) * sx, y0 = (pt.y - g_curHotY * g_cursorScale) * sy;
    float x1 = x0 + g_curW * g_cursorScale * sx, y1 = y0 + g_curH * g_cursorScale * sy;
    float rect[4] = { x0 / w * 2.0f - 1.0f, 1.0f - y0 / h * 2.0f, x1 / w * 2.0f - 1.0f, 1.0f - y1 / h * 2.0f };

    ID3DDeviceContextState* prev = nullptr;
    g_curCtx1->SwapDeviceContextState(g_curState, &prev);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_curCtx1->Map(g_curCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, rect, 16); g_curCtx1->Unmap(g_curCB, 0); }
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    g_curCtx1->RSSetViewports(1, &vp);
    g_curCtx1->RSSetState(g_curRast);
    g_curCtx1->OMSetRenderTargets(1, &it->second, nullptr);
    float bf[4] = {};
    g_curCtx1->OMSetBlendState(g_curBlend, bf, 0xffffffff);
    g_curCtx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_curCtx1->IASetInputLayout(nullptr);
    g_curCtx1->VSSetShader(g_curVS, nullptr, 0);
    g_curCtx1->VSSetConstantBuffers(0, 1, &g_curCB);
    g_curCtx1->PSSetShader(g_curPS, nullptr, 0);
    g_curCtx1->PSSetShaderResources(0, 1, &g_curSRV);
    g_curCtx1->PSSetSamplers(0, 1, &g_curSamp);
    g_curCtx1->Draw(4, 0);
    ID3D11RenderTargetView* none = nullptr;
    g_curCtx1->OMSetRenderTargets(1, &none, nullptr);
    g_curCtx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
}
