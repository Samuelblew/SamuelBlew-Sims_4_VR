// vr_status.inl: Fase 4, relógio de pulso (painel próprio com os dados da simulação).
// Incluído por fpc_rig.cpp (depois de vr_menu.inl). Vire o pulso esquerdo para o rosto, como um relógio.
//
// O Python (fpc.status, a cada 1 s) grava Mods/fpc_status.txt: nome, humor (hash, traduzido aqui pelo
// serviço do jogo), hora, dia, dinheiro, as 6 necessidades (0..1). O painel é desenhado com GDI+ e
// mostrado como camada OpenXR presa ao pulso (costas da mão).

static bool  g_statusOn = true;        // relogio no ini
static const int ST_W = 640, ST_H = 400;
static std::vector<uint32_t> g_stPixels;
static volatile bool g_stDirty = false;
static SRWLOCK g_stLock = SRWLOCK_INIT;
static std::string g_stLast;
static volatile bool g_stVisible = false;

static std::string ReadModsFile(const char* name) {
    char path[MAX_PATH];
    if (!SHGetSpecialFolderPathA(nullptr, path, CSIDL_PERSONAL, FALSE)) return "";
    strcat_s(path, "\\Electronic Arts\\The Sims 4\\Mods\\"); strcat_s(path, name);
    FILE* f = nullptr; fopen_s(&f, path, "rb");
    if (!f) return "";
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static void StatusRender(const std::string& txt) {
    std::wstring nome = L"", humor = L"", hora = L"--:--", dinheiro = L"";
    int dia = -1;
    std::vector<std::pair<std::wstring, float>> needs;
    size_t a = 0;
    while (a < txt.size()) {
        size_t b = txt.find('\n', a); std::string L = txt.substr(a, b == std::string::npos ? std::string::npos : b - a);
        a = b == std::string::npos ? txt.size() : b + 1;
        while (!L.empty() && L.back() == '\r') L.pop_back();
        size_t eq = L.find('='); if (eq == std::string::npos) continue;
        std::string k = L.substr(0, eq), v = L.substr(eq + 1);
        if (k == "nome") nome = Utf8ToW(v.c_str());
        else if (k == "humor_hash") humor = LocText((uint32_t)strtoul(v.c_str(), nullptr, 16));
        else if (k == "hora") hora = Utf8ToW(v.c_str());
        else if (k == "dia") dia = atoi(v.c_str());
        else if (k == "dinheiro") { wchar_t b2[64]; swprintf_s(b2, L"§ %d", atoi(v.c_str())); dinheiro = b2; }
        else if (k.rfind("nec_", 0) == 0) {
            std::wstring nm = Utf8ToW(k.substr(4).c_str());
            static const wchar_t* const en[][2] = { { L"Fome", L"Hunger" }, { L"Energia", L"Energy" }, { L"Bexiga", L"Bladder" },
                                                     { L"Higiene", L"Hygiene" }, { L"Social", L"Social" }, { L"Diversao", L"Fun" } };
            for (auto& e : en) if (nm == e[0]) nm = e[1];
            needs.push_back({ nm, (float)atof(v.c_str()) });
        }
    }
    static const wchar_t* dias[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    if (!g_gdipToken) { Gdiplus::GdiplusStartupInput si; Gdiplus::GdiplusStartup(&g_gdipToken, &si, nullptr); }
    Gdiplus::Bitmap bmp(ST_W, ST_H, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        Gdiplus::GraphicsPath bg; float rr = 36; Gdiplus::RectF R(4, 4, ST_W - 8, ST_H - 8);
        bg.AddArc(R.X, R.Y, rr * 2, rr * 2, 180, 90); bg.AddArc(R.X + R.Width - rr * 2, R.Y, rr * 2, rr * 2, 270, 90);
        bg.AddArc(R.X + R.Width - rr * 2, R.Y + R.Height - rr * 2, rr * 2, rr * 2, 0, 90); bg.AddArc(R.X, R.Y + R.Height - rr * 2, rr * 2, rr * 2, 90, 90);
        bg.CloseFigure();
        Gdiplus::SolidBrush bgb(Gdiplus::Color(230, 16, 26, 36)); g.FillPath(&bgb, &bg);
        Gdiplus::Pen bgp(Gdiplus::Color(220, 90, 190, 140), 4); g.DrawPath(&bgp, &bg);
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font fBig(&ff, 34, Gdiplus::FontStyleBold, Gdiplus::UnitPixel), fMid(&ff, 24, Gdiplus::FontStyleBold, Gdiplus::UnitPixel), fSm(&ff, 20, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush white(Gdiplus::Color(255, 240, 248, 240)), dim(Gdiplus::Color(255, 160, 200, 180)), money(Gdiplus::Color(255, 140, 230, 140));
        g.DrawString(nome.c_str(), -1, &fBig, Gdiplus::PointF(28, 18), &white);
        std::wstring tm = hora + (dia >= 0 && dia < 7 ? std::wstring(L"  ") + dias[dia] : L"");
        Gdiplus::StringFormat right; right.SetAlignment(Gdiplus::StringAlignmentFar);
        g.DrawString(tm.c_str(), -1, &fMid, Gdiplus::RectF(300, 22, ST_W - 328, 40), &right, &white);
        if (!humor.empty()) g.DrawString(humor.c_str(), -1, &fMid, Gdiplus::PointF(30, 66), &dim);
        g.DrawString(dinheiro.c_str(), -1, &fMid, Gdiplus::RectF(300, 66, ST_W - 328, 40), &right, &money);
        // necessidades: 2 colunas x 3 linhas
        for (int i = 0; i < (int)needs.size() && i < 6; ++i) {
            float x = 28.0f + (i % 2) * 300.0f, y = 128.0f + (i / 2) * 84.0f, w = 280.0f, v = needs[i].second;
            g.DrawString(needs[i].first.c_str(), -1, &fSm, Gdiplus::PointF(x, y), &white);
            Gdiplus::SolidBrush track(Gdiplus::Color(200, 50, 60, 70));
            g.FillRectangle(&track, Gdiplus::RectF(x, y + 32.0f, w, 22.0f));
            int r = (int)(255 * (std::min)(1.0f, 2.0f * (1.0f - v))), gg = (int)(255 * (std::min)(1.0f, 2.0f * v));
            Gdiplus::SolidBrush bar(Gdiplus::Color(255, (BYTE)(r * 0.85f), (BYTE)(gg * 0.85f), 60));
            g.FillRectangle(&bar, Gdiplus::RectF(x, y + 32.0f, w * v, 22.0f));
        }
    }
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, ST_W, ST_H);
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
        std::vector<uint32_t> px(ST_W * ST_H);
        for (int y = 0; y < ST_H; ++y) {
            const uint32_t* row = (const uint32_t*)((uint8_t*)bd.Scan0 + y * bd.Stride);
            for (int x = 0; x < ST_W; ++x) { uint32_t p = row[x]; px[y * ST_W + x] = (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff); }
        }
        bmp.UnlockBits(&bd);
        AcquireSRWLockExclusive(&g_stLock); g_stPixels.swap(px); g_stDirty = true; ReleaseSRWLockExclusive(&g_stLock);
    }
}

#ifndef FPC_TESTE_MENU
// Thread principal.
static void VrStatusUpdate() {
    if (!g_statusOn) return;
    bool vr = g_vrEnabled && g_xrRunning && g_vrHead && g_phase == RUNNING;
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!vr || now - last < 1000) return;
    last = now;
    std::string txt = ReadModsFile("fpc_status.txt");          // o pedido anterior já gravou
    SendServerCommand("fpc.status");
    if (!txt.empty() && txt != g_stLast) { g_stLast = txt; StatusRender(txt); }
}

// Thread de render: relógio no pulso esquerdo quando ele está virado para o rosto.
static XrSwapchain g_stSwap = XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_stImgs;
static ID3D11Texture2D* g_stTex = nullptr;
static XrCompositionLayerQuad g_stLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
static int VrStatusLayers(const XrCompositionLayerBaseHeader** out) {
    if (!g_statusOn || g_stLast.empty()) return 0;
    CtrlState c; HmdPose hp;
    AcquireSRWLockShared(&g_hmdLock); c = g_ctrl[0]; hp = g_hmdPose; ReleaseSRWLockShared(&g_hmdLock);
    if (!c.gripValid || !hp.valid) { g_stVisible = false; return 0; }
    const float* q = &c.grip.orientation.x;
    float gx[3], gz[3]; { const float x0[3] = { 1, 0, 0 }, z0[3] = { 0, 0, 1 }; QuatRotateXR(q, x0, gx); QuatRotateXR(q, z0, gz); }
    float pos[3] = { c.grip.position.x + gx[0] * 0.045f + gz[0] * 0.07f, c.grip.position.y + gx[1] * 0.045f + gz[1] * 0.07f, c.grip.position.z + gx[2] * 0.045f + gz[2] * 0.07f };
    float toHead[3] = { hp.pos[0] - pos[0], hp.pos[1] - pos[1], hp.pos[2] - pos[2] }; V3Norm(toHead);
    float hf[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(hp.quat, f0, hf); }
    float facing = V3Dot(gx, toHead);                          // costas da mão (esquerda: +X) viradas para o rosto
    float look = -V3Dot(hf, toHead);                           // olhando para o pulso
    bool vis = facing > 0.55f && look > 0.75f;
    if (vis != g_stVisible) { g_stVisible = vis; if (vis) XrHaptic(0, 0.1f, 0.02f); }
    if (!vis) return 0;
    DXGI_FORMAT fmt = PickSwapFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!g_stTex) {
        D3D11_TEXTURE2D_DESC td = {}; td.Width = ST_W; td.Height = ST_H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_stTex))) g_stTex = nullptr;
        if (g_stTex && !MakeSwap(g_stSwap, g_stImgs, ST_W, ST_H, fmt)) { g_stTex->Release(); g_stTex = nullptr; }
        Log("relogio: camada %s", g_stTex ? "pronta" : "FALHOU");
    }
    if (!g_stTex) return 0;
    if (g_stDirty) {
        AcquireSRWLockExclusive(&g_stLock);
        ID3D11DeviceContext* ctx = nullptr; g_gameDevice->GetImmediateContext(&ctx);
        if (ctx && !g_stPixels.empty()) ctx->UpdateSubresource(g_stTex, 0, nullptr, g_stPixels.data(), ST_W * 4, 0);
        if (ctx) ctx->Release();
        g_stDirty = false;
        ReleaseSRWLockExclusive(&g_stLock);
    }
    if (!CopyIntoSwap(g_stSwap, g_stImgs, g_stTex, false, ST_W, ST_H)) return 0;
    // painel: normal = costas da mão (+X), "cima" do painel = para os dedos (-Z do grip)
    float z[3] = { gx[0], gx[1], gx[2] };
    float y[3] = { -gz[0], -gz[1], -gz[2] }; float d = V3Dot(y, z); y[0] -= z[0] * d; y[1] -= z[1] * d; y[2] -= z[2] * d; V3Norm(y);
    float x[3]; V3Cross(y, z, x);
    g_stLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    g_stLayer.space = g_localSpace; g_stLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    g_stLayer.subImage.swapchain = g_stSwap; g_stLayer.subImage.imageRect = { { 0, 0 }, { ST_W, ST_H } };
    g_stLayer.pose.position = { pos[0], pos[1], pos[2] }; g_stLayer.pose.orientation = QuatFromAxes(x, y, z);
    g_stLayer.size = { 0.16f, 0.16f * ST_H / ST_W };
    out[0] = (const XrCompositionLayerBaseHeader*)&g_stLayer;
    return 1;
}

static void VrStatusLoadConfig(const char* path) {
    g_statusOn = GetPrivateProfileIntA("fpc_rig", "relogio", 1, path) != 0;
    Log("config: relogio=%d", g_statusOn);
}
#endif
