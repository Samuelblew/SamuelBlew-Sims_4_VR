// xr_ui.inl: interface do jogo em VR. Incluído por xr_session.inl (depois do xr_cursor.inl).
//
// 1) Interface separada da cena. O Scaleform da EA (Scaleform::Render::Sims4::HAL) desenha a interface
//    por um contexto do motor cujo campo +0x1d0 é um ID3D11DeviceContext de verdade, chamado na mesma
//    thread. Entre HAL::BeginScene (vtable+9) e HAL::EndScene (vtable+10), o alvo de desenho da cena
//    (o mesmo RTV que estava ligado no BeginScene) é trocado pela nossa textura transparente do tamanho
//    da tela. Alvos temporários do Scaleform (filtros, PushRenderTarget) não são tocados.
//    Resultado: as imagens dos olhos ficam sem interface (nada duplicado) e a interface fica numa textura.
// 2) Telinha na mão esquerda: um painel (camada quad do OpenXR) com essa textura, preso à mão esquerda.
//    Aparece ao segurar o grip esquerdo olhando para a mão; some ao soltar. O cursor é desenhado nele.
// 3) Laser na mão direita: um raio (camada quad fina). Se acerta o painel, informa o ponto (u, v) da
//    interface para o vr_laser.inl mover o cursor e clicar ali.

static bool  g_uiSeparate   = true;     // ui_separada no ini
static float g_panelWidth   = 0.34f;    // ui_painel_largura (m)
static float g_panelLift    = 0.12f;    // ui_painel_altura (m acima da mão)
static float g_panelLookDeg = 40.0f;    // ui_painel_olhar (graus): precisa olhar para a mão para abrir
static bool  g_laserVisible = true;     // laser_visivel

// ---- textura da interface
static ID3D11Texture2D*        g_uiTex = nullptr;
static ID3D11RenderTargetView* g_uiRTV = nullptr;
static ID3D11Texture2D*        g_uiDepth = nullptr;
static ID3D11DepthStencilView* g_uiDSV = nullptr;
static uint32_t                g_uiW = 0, g_uiH = 0;
static volatile bool           g_uiRedirect = false;     // estéreo ativo: desviar a interface
static volatile LONG           g_uiClearPending = 1;     // limpar no próximo BeginScene
static volatile LONG           g_uiDraws = 0;            // diagnóstico: cenas de interface desviadas

// ---- hooks do Scaleform e do D3D11
static const char* SIG_HAL_BEGINSCENE = "40 53 56 57 41 56 48 83 EC 38 48 C7 44 24 20 FE FF FF FF 48 8B F1 48 8D 44 24 60 48 89 44 24 68 48 8D 15 ?? ?? ?? ?? 48";
static const char* SIG_HAL_ENDSCENE   = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 84 C0 74 ?? 48 8D 8B 58 03 00 00 E8 ?? ?? ?? ?? 48 8B 8B 80 C1 00 00";
using HalBegin_t = uint64_t (__fastcall*)(void* hal, void* a2, void* a3, void* a4);
using HalEnd_t   = bool (__fastcall*)(void* hal);
using OMSet_t    = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static HalBegin_t o_HalBegin = nullptr;
static HalEnd_t   o_HalEnd = nullptr;
static OMSet_t    o_OMSet = nullptr, o_OMSet2 = nullptr;
static thread_local bool t_inUI = false;
static thread_local ID3D11DeviceContext*    t_uiCtx = nullptr;
static thread_local ID3D11RenderTargetView* t_sceneRTV = nullptr;
static thread_local ID3D11DepthStencilView* t_sceneDSV = nullptr;

static volatile LONG g_uiSwaps = 0;
// Alvo de tela cheia (mesmo tamanho da interface)? Alvos menores são temporários do Scaleform (filtros).
static bool IsFullScreenRTV(ID3D11RenderTargetView* v, UINT* samples = nullptr) {
    if (!v) return false;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    if (!r) return false;
    ID3D11Texture2D* t = nullptr; bool full = false;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
        D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
        full = d.Width == g_uiW && d.Height == g_uiH;
        if (samples) *samples = d.SampleDesc.Count;
        t->Release();
    }
    r->Release();
    return full;
}
static void OMSetFiltered(OMSet_t orig, ID3D11DeviceContext* self, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (t_inUI && self == t_uiCtx && g_uiRTV && n >= 1 && rtv && rtv[0] && rtv[0] != g_uiRTV &&
        (rtv[0] == t_sceneRTV || IsFullScreenRTV(rtv[0]))) {
        InterlockedIncrement(&g_uiSwaps);
        ID3D11RenderTargetView* mine = g_uiRTV;
        orig(self, 1, &mine, g_uiDSV);                     // o alvo da cena vira a nossa textura
        return;
    }
    orig(self, n, rtv, dsv);
}
static void STDMETHODCALLTYPE hk_OMSet(ID3D11DeviceContext* s, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) { OMSetFiltered(o_OMSet, s, n, r, d); }
static void STDMETHODCALLTYPE hk_OMSet2(ID3D11DeviceContext* s, UINT n, ID3D11RenderTargetView* const* r, ID3D11DepthStencilView* d) { OMSetFiltered(o_OMSet2, s, n, r, d); }

static ID3D11DeviceContext* HalD3DContext(void* hal) {
    uint8_t* ectx = *(uint8_t**)((uint8_t*)hal + 0x1830 * 8);
    return ectx ? *(ID3D11DeviceContext**)(ectx + 0x1d0) : nullptr;
}

static uint64_t __fastcall hk_HalBegin(void* hal, void* a2, void* a3, void* a4) {
    uint64_t r = o_HalBegin(hal, a2, a3, a4);
    if (!(r & 0xff) || !g_uiSeparate || !g_uiRedirect || !g_uiRTV || !o_OMSet) return r;
    ID3D11DeviceContext* dc = HalD3DContext(hal);
    if (!dc) return r;
    ID3D11RenderTargetView* cur = nullptr; ID3D11DepthStencilView* curD = nullptr;
    dc->OMGetRenderTargets(1, &cur, &curD);
    {
        static volatile LONG logged = 0;
        if (InterlockedIncrement(&logged) <= 3) {
            UINT smp = 0; bool full = IsFullScreenRTV(cur, &smp);
            Log("ui: BeginScene do Scaleform: alvo atual %p (tela cheia=%d, amostras=%u) contexto %p", cur, (int)full, smp, dc);
        }
    }
    if (!cur) { if (curD) curD->Release(); return r; }
    t_uiCtx = dc; t_sceneRTV = cur; t_sceneDSV = curD; t_inUI = true;
    if (InterlockedExchange(&g_uiClearPending, 0)) {
        const float z[4] = { 0, 0, 0, 0 };
        dc->ClearRenderTargetView(g_uiRTV, z);
        dc->ClearDepthStencilView(g_uiDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    ID3D11RenderTargetView* mine = g_uiRTV;
    o_OMSet(dc, 1, &mine, g_uiDSV);
    InterlockedIncrement(&g_uiDraws);
    return r;
}
static bool __fastcall hk_HalEnd(void* hal) {
    bool r = o_HalEnd(hal);
    if (t_inUI) {
        t_inUI = false;
        if (t_uiCtx && o_OMSet) o_OMSet(t_uiCtx, 1, &t_sceneRTV, t_sceneDSV);   // devolve o alvo da cena
        if (t_sceneRTV) t_sceneRTV->Release();
        if (t_sceneDSV) t_sceneDSV->Release();
        t_sceneRTV = nullptr; t_sceneDSV = nullptr; t_uiCtx = nullptr;
    }
    return r;
}

// No Init (antes do MH_EnableHook geral).
static void XrUiInstallHooks() {
    if (!g_uiSeparate) { Log("ui: separacao desligada (ui_separada=0)"); return; }
    uint8_t* b = FindPattern(SIG_HAL_BEGINSCENE);
    uint8_t* e = FindPattern(SIG_HAL_ENDSCENE);
    if (b) MH_CreateHook(b, &hk_HalBegin, (void**)&o_HalBegin);
    if (e) MH_CreateHook(e, &hk_HalEnd, (void**)&o_HalEnd);
    Log("ui: Scaleform HAL BeginScene=%p EndScene=%p", b, e);
    if (!b || !e) g_uiSeparate = false;
}

// Na thread de render (primeiro frame do headset): hook do OMSetRenderTargets dos contextos.
static void XrUiInstallD3DHook() {
    static bool done = false;
    if (done || !g_uiSeparate || !g_gameDevice) return;
    done = true;
    ID3D11DeviceContext* def = nullptr;
    if (FAILED(g_gameDevice->CreateDeferredContext(0, &def)) || !def) { Log("ui: sem contexto deferred"); g_uiSeparate = false; return; }
    void* fDef = (*(void***)def)[33];
    def->Release();
    ID3D11DeviceContext* imm = nullptr; g_gameDevice->GetImmediateContext(&imm);
    void* fImm = imm ? (*(void***)imm)[33] : nullptr;
    if (imm) imm->Release();
    MH_STATUS s1 = MH_CreateHook(fDef, &hk_OMSet, (void**)&o_OMSet);
    if (s1 == MH_OK) s1 = MH_EnableHook(fDef);
    MH_STATUS s2 = MH_UNKNOWN;
    if (fImm && fImm != fDef) { s2 = MH_CreateHook(fImm, &hk_OMSet2, (void**)&o_OMSet2); if (s2 == MH_OK) s2 = MH_EnableHook(fImm); }
    Log("ui: OMSetRenderTargets deferred=%p (%d) imediato=%p (%d)", fDef, (int)s1, fImm, (int)s2);
    if (s1 != MH_OK) g_uiSeparate = false;
}

static void XrUiEnsureTarget(uint32_t w, uint32_t h) {
    if (!g_uiSeparate || (w == g_uiW && h == g_uiH && g_uiRTV)) return;
    // (na troca de tamanho, a textura antiga não é liberada: pode estar em uso numa lista gravada)
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = nullptr; ID3D11RenderTargetView* v = nullptr;
    ID3D11Texture2D* dt = nullptr; ID3D11DepthStencilView* dv = nullptr;
    bool ok = SUCCEEDED(g_gameDevice->CreateTexture2D(&td, nullptr, &t)) && SUCCEEDED(g_gameDevice->CreateRenderTargetView(t, nullptr, &v));
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ok = ok && SUCCEEDED(g_gameDevice->CreateTexture2D(&td, nullptr, &dt)) && SUCCEEDED(g_gameDevice->CreateDepthStencilView(dt, nullptr, &dv));
    if (!ok) { Log("ui: textura da interface %ux%u FALHOU", w, h); g_uiSeparate = false; return; }
    g_uiTex = t; g_uiDepth = dt; g_uiDSV = dv; g_uiW = w; g_uiH = h;
    MemoryBarrier();
    g_uiRTV = v;
    Log("ui: textura da interface %ux%u criada", w, h);
}

// ---- painel e laser (camadas do OpenXR)
static XrSwapchain g_panelSwap = XR_NULL_HANDLE, g_beamSwap = XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_panelImgs, g_beamImgs;
static uint32_t g_panelSW = 0, g_panelSH = 0;
static ID3D11Texture2D* g_beamTex = nullptr;
static bool  g_panelOpen = false;
static XrPosef g_panelRel = { { 0, 0, 0, 1 }, { 0, 0, 0 } };   // pose do painel relativa ao grip (fixada ao abrir)
// ---- câmera do jogo e interface presa a ela ("camada do mundo")
// Na primeira pessoa o jogo destaca e seleciona o que está no CENTRO da câmera dele, e desenha balões,
// roda de opções e etiquetas na tela projetando o mundo com essa câmera. A imagem dos olhos não depende
// dela (cada olho tem a sua pose), então ela pode apontar para outro lugar:
//   CABEÇA  (padrão)       = centro da cabeça;
//   LASER   (grip direito) = no raio do laser: o brilho segue o laser e o clique escolhe o objeto apontado;
//   TRAVADA (roda aberta)  = parada onde estava quando a roda abriu, para escolher as opções com o laser.
// A camada do mundo é a textura da interface num plano que reproduz essa câmera (pose + frustum) a
// g_worldUiDist metros: cada pixel fica na direção do ponto do mundo que representa. Só a região central
// (as bordas são o HUD, que fica na telinha da mão).
enum { GCAM_HEAD = 0, GCAM_LASER = 1, GCAM_LOCKED = 2 };
static volatile int g_gameCamMode = GCAM_HEAD;
static XrPosef g_gameCamXR = { { 0, 0, 0, 1 }, { 0, 0, 0 } };   // pose XR da câmera do jogo (laser/travada)
static SRWLOCK g_gameCamLock = SRWLOCK_INIT;
static bool  g_worldUi = true;                                  // ui_mundo
static float g_worldUiDist = 2.0f;                              // ui_mundo_distancia (m)
static float g_wuU0 = 0.10f, g_wuU1 = 0.90f, g_wuV0 = 0.04f, g_wuV1 = 0.80f;   // ui_mundo_regiao
static XrPosef g_wuPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
static float g_wuW = 1.0f, g_wuH = 1.0f;
static volatile bool g_pieOpen = false, g_pieLockPending = false;   // avisos do Python (-1005 / -1006)
static volatile ULONGLONG g_pieCloseAt = 0, g_pieOpenedAt = 0;
static volatile bool g_hitIsPie = false;

// Frustum da câmera do jogo em tangentes (esq, dir, cima, baixo): câmera central com a projeção do jogo.
static bool GameFrustum(float& left, float& right, float& top, float& bottom) {
    uint8_t* cam = (uint8_t*)g_fpsCam;
    if (!cam) return false;
    float tx = *(float*)(cam + 0x14), ty = *(float*)(cam + 0x18), ox = *(float*)(cam + 0x1f0), oy = *(float*)(cam + 0x1f4);
    const float* m = (const float*)(cam + 0x270);
    if (tx < 1e-3f || ty < 1e-3f) return false;
    left = ox + (m[0] - 0.5f) * tx; right = ox + (m[2] - 0.5f) * tx;
    top = oy + (m[1] - 0.5f) * ty; bottom = oy + (m[3] - 0.5f) * ty;
    return std::fabs(right - left) > 1e-4f && std::fabs(top - bottom) > 1e-4f;
}
static bool UiHitIsPie() { return g_hitIsPie; }

static XrPosef PoseMul(const XrPosef& a, const XrPosef& b) {     // a ∘ b
    XrPosef o;
    float bp[3] = { b.position.x, b.position.y, b.position.z }, rp[3];
    QuatRotateXR(&a.orientation.x, bp, rp);
    o.position = { a.position.x + rp[0], a.position.y + rp[1], a.position.z + rp[2] };
    const XrQuaternionf& p = a.orientation; const XrQuaternionf& q = b.orientation;
    o.orientation = { p.w * q.x + p.x * q.w + p.y * q.z - p.z * q.y, p.w * q.y - p.x * q.z + p.y * q.w + p.z * q.x,
                      p.w * q.z + p.x * q.y - p.y * q.x + p.z * q.w, p.w * q.w - p.x * q.x - p.y * q.y - p.z * q.z };
    return o;
}
static XrPosef PoseInv(const XrPosef& a) {
    XrPosef o; o.orientation = { -a.orientation.x, -a.orientation.y, -a.orientation.z, a.orientation.w };
    float p[3] = { -a.position.x, -a.position.y, -a.position.z }, rp[3];
    QuatRotateXR(&o.orientation.x, p, rp);
    o.position = { rp[0], rp[1], rp[2] };
    return o;
}
// Raio x quad (pose, w, h): devolve t e (u, v) se acertar.
static bool HitQuad(const XrPosef& qp, float w, float h, const float p0[3], const float dir[3], float& t, float& u, float& v) {
    float pn[3], px[3], py[3];
    { const float z0[3] = { 0, 0, 1 }, x0[3] = { 1, 0, 0 }, y0[3] = { 0, 1, 0 };
      QuatRotateXR(&qp.orientation.x, z0, pn); QuatRotateXR(&qp.orientation.x, x0, px); QuatRotateXR(&qp.orientation.x, y0, py); }
    float den = pn[0] * dir[0] + pn[1] * dir[1] + pn[2] * dir[2];
    if (std::fabs(den) < 1e-4f) return false;
    float cp[3] = { qp.position.x - p0[0], qp.position.y - p0[1], qp.position.z - p0[2] };
    t = (cp[0] * pn[0] + cp[1] * pn[1] + cp[2] * pn[2]) / den;
    if (t <= 0.0f || t > 10.0f) return false;
    float hp[3] = { p0[0] + dir[0] * t - qp.position.x, p0[1] + dir[1] * t - qp.position.y, p0[2] + dir[2] * t - qp.position.z };
    u = (hp[0] * px[0] + hp[1] * px[1] + hp[2] * px[2]) / w + 0.5f;
    v = 0.5f - (hp[0] * py[0] + hp[1] * py[1] + hp[2] * py[2]) / h;
    return u >= 0 && u <= 1 && v >= 0 && v <= 1;
}
static XrPosef g_panelPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
static float g_panelH = 0.2f;
struct UiHit { bool valid; float u, v; ULONGLONG t; };
static UiHit g_uiHit = {};
static SRWLOCK g_uiHitLock = SRWLOCK_INIT;
static volatile bool g_uiPanelVisible = false;
static volatile bool g_handGrabbing[2] = { false, false };   // mão ocupada agarrando outro Sim (vr_grab.inl)
static volatile bool g_menuPending = false;                   // Fase 3: o Python gravou as opções (-1007)
static volatile float g_laserHitLenXR = 0.0f;                 // Fase 3b: o raio termina no alvo do raycast (m, espaço do headset)
static volatile bool g_rayReady = false;                      // Fase 3b: o Python respondeu o raycast (-1011)
static int VrMenuLayers(const XrCompositionLayerBaseHeader** out);   // vr_menu.inl
static int VrStatusLayers(const XrCompositionLayerBaseHeader** out); // vr_status.inl (Fase 4)
static int VrConfigLayers(const XrCompositionLayerBaseHeader** out); // vr_config.inl (configurações)
static bool IsHeldComp(void* comp);                                   // vr_objgrab.inl (Fase 5)
static volatile bool g_objGrabReady = false, g_objGrabNone = false;   // respostas do Python (-1009/-1010)
static volatile int g_objReqHand = -1; static float g_objReqPalm[3] = {};   // pedido de agarrar objeto (vr_grab -> vr_objgrab)
static bool VrMenuHitNow(float& u, float& v); static void VrMenuHover(float u, float v); static void VrMenuClick(); static void VrMenuBack();

static bool MakeSwap(XrSwapchain& sw, std::vector<ID3D11Texture2D*>& imgs, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    if (sw != XR_NULL_HANDLE) { xrDestroySwapchain_(sw); sw = XR_NULL_HANDLE; imgs.clear(); }
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain_(g_xrSession, &ci, &sw))) { sw = XR_NULL_HANDLE; return false; }
    uint32_t ni = 0; xrEnumerateSwapchainImages_(sw, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> v(ni, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages_(sw, ni, &ni, (XrSwapchainImageBaseHeader*)v.data());
    for (auto& im : v) imgs.push_back(im.texture);
    return true;
}

// Copia src para a imagem atual do swapchain e, se pedido, desenha o cursor nela.
static bool CopyIntoSwap(XrSwapchain sw, std::vector<ID3D11Texture2D*>& imgs, ID3D11Texture2D* src, bool cursor, uint32_t w, uint32_t h) {
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(sw, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = 100000000;
    bool ok = false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage_(sw, &wi)) && idx < imgs.size()) {
        ID3D11DeviceContext* ctx = nullptr; g_gameDevice->GetImmediateContext(&ctx);
        if (ctx) { ctx->CopyResource(imgs[idx], src); ctx->Release(); ok = true; }
        if (ok && cursor) DrawCursorOn(imgs[idx], w, h, g_gameHwnd);
    }
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(sw, &ri);
    return ok;
}

static XrQuaternionf QuatFromAxes(const float x[3], const float y[3], const float z[3]) {
    float m00 = x[0], m11 = y[1], m22 = z[2], tr = m00 + m11 + m22, qx, qy, qz, qw;
    if (tr > 0) { float s = std::sqrt(tr + 1.0f) * 2; qw = 0.25f * s; qx = (y[2] - z[1]) / s; qy = (z[0] - x[2]) / s; qz = (x[1] - y[0]) / s; }
    else if (m00 > m11 && m00 > m22) { float s = std::sqrt(1 + m00 - m11 - m22) * 2; qw = (y[2] - z[1]) / s; qx = 0.25f * s; qy = (y[0] + x[1]) / s; qz = (z[0] + x[2]) / s; }
    else if (m11 > m22) { float s = std::sqrt(1 + m11 - m00 - m22) * 2; qw = (z[0] - x[2]) / s; qx = (y[0] + x[1]) / s; qy = 0.25f * s; qz = (z[1] + y[2]) / s; }
    else { float s = std::sqrt(1 + m22 - m00 - m11) * 2; qw = (x[1] - y[0]) / s; qx = (z[0] + x[2]) / s; qy = (z[1] + y[2]) / s; qz = 0.25f * s; }
    float l = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    return { qx / l, qy / l, qz / l, qw / l };
}
static void V3Norm(float v[3]) { float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void V3Cross(const float a[3], const float b[3], float o[3]) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static float V3Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Por frame do headset (estéreo). Preenche até 2 camadas (painel, laser) em out[]; devolve quantas.
static XrCompositionLayerQuad g_layerPanel = { XR_TYPE_COMPOSITION_LAYER_QUAD }, g_layerBeam = { XR_TYPE_COMPOSITION_LAYER_QUAD };
static int XrUiLayers(const XrCompositionLayerBaseHeader** out, DXGI_FORMAT bbFmt) {
    int n = 0;
    CtrlState c[2]; HmdPose hp;
    AcquireSRWLockShared(&g_hmdLock); c[0] = g_ctrl[0]; c[1] = g_ctrl[1]; hp = g_hmdPose; ReleaseSRWLockShared(&g_hmdLock);
    if (!hp.valid) return 0;
    const float head[3] = { hp.pos[0], hp.pos[1], hp.pos[2] };
    float hf[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(hp.quat, f0, hf); }

    // ---- painel na mão esquerda
    bool panelReady = g_uiSeparate && g_uiRTV && g_uiTex && InterlockedCompareExchange(&g_uiDraws, 0, 0) > 0;
    if (panelReady && c[0].gripValid) {
        const float* gp = &c[0].grip.position.x;
        float toHand[3] = { gp[0] - head[0], gp[1] - head[1], gp[2] - head[2] }; V3Norm(toHand);
        float look = std::acos((std::max)(-1.0f, (std::min)(1.0f, V3Dot(toHand, hf)))) * 57.2958f;
        if (!g_panelOpen && c[0].squeeze > 0.6f && look < g_panelLookDeg && !g_handGrabbing[0]) {
            g_panelOpen = true;
            float gup[3]; { const float y0[3] = { 0, 1, 0 }; QuatRotateXR(&c[0].grip.orientation.x, y0, gup); }
            float pos[3] = { c[0].grip.position.x + gup[0] * g_panelLift, c[0].grip.position.y + gup[1] * g_panelLift, c[0].grip.position.z + gup[2] * g_panelLift };
            float z[3] = { head[0] - pos[0], head[1] - pos[1], head[2] - pos[2] }; V3Norm(z);
            const float wy[3] = { 0, 1, 0 };
            float x[3]; V3Cross(wy, z, x); V3Norm(x);
            float y[3]; V3Cross(z, x, y);
            XrPosef want; want.position = { pos[0], pos[1], pos[2] }; want.orientation = QuatFromAxes(x, y, z);
            g_panelRel = PoseMul(PoseInv(c[0].grip), want);
            Log("ui: painel aberto na mao esquerda");
        }
        if (g_panelOpen && (c[0].squeeze < 0.3f || g_handGrabbing[0])) { g_panelOpen = false; Log("ui: painel fechado"); }
    } else g_panelOpen = false;
    g_uiPanelVisible = g_panelOpen;
    // roda de opções: fecha 0,3 s depois de escolher, ou sozinha após 30 s
    ULONGLONG nowp = GetTickCount64();
    if (g_pieOpen && ((g_pieCloseAt && nowp > g_pieCloseAt) || nowp - g_pieOpenedAt > 30000)) {
        g_pieOpen = false; g_pieCloseAt = 0; Log("ui: roda de opcoes encerrada (camera do jogo volta a seguir)");
    }
    float fl, fr, ft, fb;
    bool worldVis = panelReady && g_worldUi && GameFrustum(fl, fr, ft, fb);
    bool uiCopied = false;
    if (g_panelOpen || worldVis) {
        if (g_panelSW != g_uiW || g_panelSH != g_uiH || g_panelSwap == XR_NULL_HANDLE) {
            DXGI_FORMAT fmt = PickSwapFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
            if (MakeSwap(g_panelSwap, g_panelImgs, g_uiW, g_uiH, fmt)) { g_panelSW = g_uiW; g_panelSH = g_uiH; Log("ui: swapchain da interface %ux%u", g_uiW, g_uiH); }
        }
        uiCopied = g_panelSwap != XR_NULL_HANDLE && CopyIntoSwap(g_panelSwap, g_panelImgs, g_uiTex, g_cursorForce, g_uiW, g_uiH);   // cursor só com o laser na interface
    }
    if (worldVis && uiCopied) {
        // pose da câmera do jogo no espaço do headset
        XrPosef cp;
        if (g_gameCamMode == GCAM_HEAD) {
            cp.position = { (g_eyeLast[0].pose.position.x + g_eyeLast[1].pose.position.x) * 0.5f,
                            (g_eyeLast[0].pose.position.y + g_eyeLast[1].pose.position.y) * 0.5f,
                            (g_eyeLast[0].pose.position.z + g_eyeLast[1].pose.position.z) * 0.5f };
            cp.orientation = g_eyeLast[0].pose.orientation;
        } else { AcquireSRWLockShared(&g_gameCamLock); cp = g_gameCamXR; ReleaseSRWLockShared(&g_gameCamLock); }
        float D = g_worldUiDist;
        float xc = fl + (g_wuU0 + g_wuU1) * 0.5f * (fr - fl), yc = ft - (g_wuV0 + g_wuV1) * 0.5f * (ft - fb);
        XrPosef local; local.orientation = { 0, 0, 0, 1 }; local.position = { xc * D, yc * D, -D };
        g_wuPose = PoseMul(cp, local);
        g_wuW = (g_wuU1 - g_wuU0) * (fr - fl) * D; g_wuH = (g_wuV1 - g_wuV0) * (ft - fb) * D;
        static XrCompositionLayerQuad wl = { XR_TYPE_COMPOSITION_LAYER_QUAD };
        wl.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        wl.space = g_localSpace; wl.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        wl.subImage.swapchain = g_panelSwap;
        wl.subImage.imageRect = { { (int32_t)(g_wuU0 * g_uiW), (int32_t)(g_wuV0 * g_uiH) },
                                  { (int32_t)((g_wuU1 - g_wuU0) * g_uiW), (int32_t)((g_wuV1 - g_wuV0) * g_uiH) } };
        wl.pose = g_wuPose; wl.size = { g_wuW, g_wuH };
        out[n++] = (const XrCompositionLayerBaseHeader*)&wl;
    }
    if (g_panelOpen) {
        // preso à mão: pose relativa fixada ao abrir (não segue a cabeça)
        g_panelPose = PoseMul(c[0].grip, g_panelRel);
        g_panelH = g_panelWidth * (float)g_uiH / (float)(g_uiW ? g_uiW : 1);
        if (uiCopied) {
            g_layerPanel.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            g_layerPanel.space = g_localSpace; g_layerPanel.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            g_layerPanel.subImage.swapchain = g_panelSwap;
            g_layerPanel.subImage.imageRect = { { 0, 0 }, { (int32_t)g_uiW, (int32_t)g_uiH } };
            g_layerPanel.pose = g_panelPose; g_layerPanel.size = { g_panelWidth, g_panelH };
            out[n++] = (const XrCompositionLayerBaseHeader*)&g_layerPanel;
        }
    }

    // ---- laser na mão direita (+ acerto no painel)
    UiHit hit = {};
    if (g_laserOn && c[1].aimValid) {
        const float* p0 = &c[1].aim.position.x;
        float dir[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(&c[1].aim.orientation.x, f0, dir); }
        float len = g_laserHitLenXR > 0.05f ? g_laserHitLenXR : 3.0f, t, u, v;
        bool isPie = false;
        if (g_panelOpen && HitQuad(g_panelPose, g_panelWidth, g_panelH, p0, dir, t, u, v)) { hit = { true, u, v, GetTickCount64() }; len = t; }
        // camada do mundo: só recebe o laser com a roda aberta (senão a câmera do jogo segue o laser)
        if (worldVis && g_pieOpen && g_gameCamMode == GCAM_LOCKED && HitQuad(g_wuPose, g_wuW, g_wuH, p0, dir, t, u, v) && (!hit.valid || t < len)) {
            float fu = g_wuU0 + u * (g_wuU1 - g_wuU0), fv = g_wuV0 + v * (g_wuV1 - g_wuV0);   // região -> tela inteira
            hit = { true, fu, fv, GetTickCount64() }; len = t; isPie = true;
        }
        g_hitIsPie = isPie;
        if (g_laserVisible) {
            if (g_beamSwap == XR_NULL_HANDLE) {
                // raio: 8x128, centro claro e bordas transparentes (alfa pré-multiplicado)
                std::vector<uint32_t> px(8 * 128);
                for (int yy = 0; yy < 128; ++yy) for (int xx = 0; xx < 8; ++xx) {
                    float e = 1.0f - std::fabs((xx + 0.5f) / 8.0f * 2.0f - 1.0f);
                    float fade = yy < 100 ? 1.0f : (128 - yy) / 28.0f;   // ponta suave
                    int a = (int)(255 * e * e * fade);
                    int r = a * 120 / 255, g = a * 220 / 255, b = a;
                    px[yy * 8 + xx] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
                }
                DXGI_FORMAT fmt = PickSwapFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
                D3D11_TEXTURE2D_DESC td = {}; td.Width = 8; td.Height = 128; td.MipLevels = 1; td.ArraySize = 1;
                td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA sd = { px.data(), 8 * 4, 0 };
                if (SUCCEEDED(g_gameDevice->CreateTexture2D(&td, &sd, &g_beamTex))) MakeSwap(g_beamSwap, g_beamImgs, 8, 128, fmt);
                Log("ui: laser %s", g_beamSwap != XR_NULL_HANDLE ? "pronto" : "FALHOU");
            }
            if (g_beamSwap != XR_NULL_HANDLE && CopyIntoSwap(g_beamSwap, g_beamImgs, g_beamTex, false, 8, 128)) {
                float ctr[3] = { p0[0] + dir[0] * len * 0.5f, p0[1] + dir[1] * len * 0.5f, p0[2] + dir[2] * len * 0.5f };
                float y[3] = { -dir[0], -dir[1], -dir[2] };              // topo da textura = mão
                float tv[3] = { head[0] - ctr[0], head[1] - ctr[1], head[2] - ctr[2] };
                float d = V3Dot(tv, y);
                float z[3] = { tv[0] - y[0] * d, tv[1] - y[1] * d, tv[2] - y[2] * d }; V3Norm(z);
                float x[3]; V3Cross(y, z, x); V3Norm(x);
                g_layerBeam.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                g_layerBeam.space = g_localSpace; g_layerBeam.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                g_layerBeam.subImage.swapchain = g_beamSwap;
                g_layerBeam.subImage.imageRect = { { 0, 0 }, { 8, 128 } };
                g_layerBeam.pose.position = { ctr[0], ctr[1], ctr[2] };
                g_layerBeam.pose.orientation = QuatFromAxes(x, y, z);
                g_layerBeam.size = { 0.008f, len };
                out[n++] = (const XrCompositionLayerBaseHeader*)&g_layerBeam;
            }
        }
    }
    AcquireSRWLockExclusive(&g_uiHitLock); g_uiHit = hit; ReleaseSRWLockExclusive(&g_uiHitLock);
    n += VrMenuLayers(out + n);                               // Fase 3: menu radial próprio
    n += VrStatusLayers(out + n);                             // Fase 4: relógio de pulso
    n += VrConfigLayers(out + n);                             // configurações (por cima de tudo)
    static ULONGLONG lastUi = 0; ULONGLONG nu = GetTickCount64();
    if (nu - lastUi >= 5000) {
        lastUi = nu;
        Log("ui: cenas da interface desviadas=%ld trocas de alvo=%ld desvio=%d painel=%d laser=%d acerto=%d",
            g_uiDraws, g_uiSwaps, (int)g_uiRedirect, (int)g_panelOpen, (int)g_laserOn, (int)hit.valid);
    }
    return n;
}

static bool UiHitNow(float& u, float& v) {
    UiHit h; AcquireSRWLockShared(&g_uiHitLock); h = g_uiHit; ReleaseSRWLockShared(&g_uiHitLock);
    if (!h.valid || GetTickCount64() - h.t > 200) return false;
    u = h.u; v = h.v; return true;
}
