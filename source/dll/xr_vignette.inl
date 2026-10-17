// xr_vignette.inl: vinheta de conforto (escurece a visão periférica enquanto o corpo anda ou gira pelo
// analógico; movimento da cabeça não conta). Incluído por xr_session.inl. Thread de render.
//
// É uma camada quad presa à cabeça (espaço VIEW do OpenXR) com uma textura preta de alfa radial. A
// intensidade é quantizada em 8 níveis: a textura só é refeita quando o nível muda.

static XrSwapchain g_vigSwap = XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_vigImgs;
static ID3D11Texture2D* g_vigTex = nullptr;
static int g_vigLevel = -1, g_vigStrength = -1;
static float g_vigAmt = 0.0f;
static XrCompositionLayerQuad g_vigLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
static const int VIG_TEX = 256;
static const float VIG_DIST = 0.5f, VIG_SIZE = 2.0f;           // quad de 2 m a 0,5 m: cobre ~126 graus

static void VignetteBuild(int strength, int level) {
    // raio normalizado (0 = centro, 1 = borda do quad) a partir do ângulo: r = tan(ang) / tan(63,4°)
    static const float inDeg[4] = { 0, 36, 30, 24 }, outDeg[4] = { 0, 58, 50, 42 };
    float r0 = std::tan(inDeg[strength] * 0.0174533f) / 2.0f, r1 = std::tan(outDeg[strength] * 0.0174533f) / 2.0f;
    float amt = level / 7.0f;
    std::vector<uint32_t> px(VIG_TEX * VIG_TEX);
    for (int y = 0; y < VIG_TEX; ++y) for (int x = 0; x < VIG_TEX; ++x) {
        float u = (x + 0.5f) / VIG_TEX * 2.0f - 1.0f, v = (y + 0.5f) / VIG_TEX * 2.0f - 1.0f;
        float r = std::sqrt(u * u + v * v);
        float t = (r - r0) / (std::max)(1e-3f, r1 - r0); t = t < 0 ? 0 : t > 1 ? 1 : t;
        t = t * t * (3 - 2 * t);
        uint32_t a = (uint32_t)(255.0f * t * amt + 0.5f);
        px[y * VIG_TEX + x] = a << 24;                          // preto pré-multiplicado
    }
    ID3D11DeviceContext* ctx = nullptr; g_gameDevice->GetImmediateContext(&ctx);
    if (ctx) { ctx->UpdateSubresource(g_vigTex, 0, nullptr, px.data(), VIG_TEX * 4, 0); ctx->Release(); }
}

static int VignetteLayer(const XrCompositionLayerBaseHeader** out) {
    // intensidade: sobe rápido (0,1 s), desce devagar (0,35 s)
    float target = (std::max)((float)g_motionWalk, (float)g_motionTurn);
    float dt = 1.0f / 72.0f;
    g_vigAmt += (target - g_vigAmt) * (target > g_vigAmt ? (std::min)(1.0f, dt / 0.10f) : (std::min)(1.0f, dt / 0.35f));
    g_motionTurn = g_motionTurn * 0.9f;                         // giro em passos: apaga sozinho
    if (g_vignette <= 0 || g_vignette > 3 || g_viewSpace == XR_NULL_HANDLE) return 0;
    int level = (int)(g_vigAmt * 7.0f + 0.5f);
    if (level <= 0) return 0;
    if (!g_vigTex) {
        DXGI_FORMAT fmt = PickSwapFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
        D3D11_TEXTURE2D_DESC td = {}; td.Width = VIG_TEX; td.Height = VIG_TEX; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_vigTex))) g_vigTex = nullptr;
        if (g_vigTex && !MakeSwap(g_vigSwap, g_vigImgs, VIG_TEX, VIG_TEX, fmt)) { g_vigTex->Release(); g_vigTex = nullptr; }
        Log("vr: vinheta %s", g_vigTex ? "pronta" : "FALHOU");
        if (!g_vigTex) { g_vignette = 0; return 0; }
    }
    if (level != g_vigLevel || g_vignette != g_vigStrength) { VignetteBuild(g_vignette, level); g_vigLevel = level; g_vigStrength = g_vignette; }
    if (!CopyIntoSwap(g_vigSwap, g_vigImgs, g_vigTex, false, VIG_TEX, VIG_TEX)) return 0;
    g_vigLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    g_vigLayer.space = g_viewSpace; g_vigLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_vigLayer.subImage.swapchain = g_vigSwap;
    g_vigLayer.subImage.imageRect = { { 0, 0 }, { VIG_TEX, VIG_TEX } };
    g_vigLayer.pose.orientation = { 0, 0, 0, 1 }; g_vigLayer.pose.position = { 0, 0, -VIG_DIST };
    g_vigLayer.size = { VIG_SIZE, VIG_SIZE };
    out[0] = (const XrCompositionLayerBaseHeader*)&g_vigLayer;
    return 1;
}
