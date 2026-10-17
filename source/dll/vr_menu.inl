// vr_menu.inl: Fase 3, menu radial 3D próprio (substitui a roda do Scaleform em VR).
// Incluído por fpc_rig.cpp (depois de vr_avatar.inl).
//
// Fluxo:
//  1. o laser seleciona o objeto (câmera do jogo no raio da mão) e o gatilho clica; o servidor monta a
//     roda em create_pie_menu_message; o Python (fpc_walk v6) grava as opções em Mods/fpc_menu.txt,
//     esvazia a roda do Scaleform e avisa (-1007);
//  2. aqui: cada texto é resolvido pelo serviço de tradução do próprio jogo
//     (FUN_140257050(servico, hash, &string_eastl, tokens, n, 0); servico = FUN_140254ec0()),
//     os tokens (nomes/números) entram no lugar de {N.SimFirstName}/{N.String}/{N.Number}...;
//  3. o menu é desenhado com GDI+ numa textura e mostrado como camada OpenXR na frente da mão;
//  4. laser no menu: o botão acende; gatilho escolhe (categoria -> submenu; opção -> "fpc.select <id>");
//     B volta/fecha.
#include <gdiplus.h>
#include <string>

static bool g_vrMenuOn = true;            // menu_vr no ini
static const char* SIG_LOC_FORMAT = "48 8B C4 89 50 10 48 89 48 08 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 60";
static const char* SIG_LOC_LOOKUP = "48 89 5C 24 08 48 89 6C 24 10 56 57 41 56 48 83 EC 50 8B F2";
using LocSvc_t    = void* (__fastcall*)();
using LocLookup_t = bool (__fastcall*)(void* svc, uint32_t hash, void* outStr, void* tokens, char nTokens, char flag);
static LocSvc_t    p_LocSvc = nullptr;
static LocLookup_t p_LocLookup = nullptr;

static std::wstring Utf8ToW(const char* s, int n = -1) {
    if (!s) return L"";
    int m = MultiByteToWideChar(CP_UTF8, 0, s, n, nullptr, 0);
    std::wstring w(m > 0 ? m : 0, L'\0');
    if (m > 0) MultiByteToWideChar(CP_UTF8, 0, s, n, &w[0], m);
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

static bool SafeLocLookup(void* svc, uint32_t hash, uint8_t* str) {
    __try { p_LocLookup(svc, hash, str, nullptr, 0, 0); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Texto traduzido do próprio jogo para um hash (sem tokens; eles são trocados depois).
static std::wstring LocText(uint32_t hash) {
    if (!hash || !p_LocSvc || !p_LocLookup) return L"";
    void* svc = p_LocSvc();
    if (!svc) return L"";
    alignas(16) uint8_t str[16] = {}; str[15] = 0x0f;          // eastl::string vazia (SSO, 15 livres)
    if (!SafeLocLookup(svc, hash, str)) return L"";
    const char* data; int len;
    if (str[15] & 0x80) { data = *(const char**)str; len = (int)*(uint32_t*)(str + 8); }
    else { data = (const char*)str; len = 15 - str[15]; }
    if (!data || len <= 0 || len > 4096) return L"";
    return Utf8ToW(data, len);
}

// Troca os marcadores de token do TS4 pelos valores: {0.SimFirstName}, {0.String}, {0.Number}...;
// {M0.ele}{F0.ela} -> primeira variante. tokens: "s:texto" | "h:hash" | "n:numero".
static std::wstring ApplyTokens(const std::wstring& in, const std::vector<std::string>& toks) {
    auto tokStr = [&](int i, const std::wstring& field) -> std::wstring {
        if (i < 0 || i >= (int)toks.size()) return L"";
        const std::string& t = toks[i];
        if (t.size() < 2) return L"";
        std::wstring v = t[0] == 'h' ? LocText((uint32_t)strtoul(t.c_str() + 2, nullptr, 16)) : Utf8ToW(t.c_str() + 2);
        if (t[0] == 's' && (field == L"SimFirstName")) { size_t sp = v.find(L' '); if (sp != std::wstring::npos) v = v.substr(0, sp); }
        if (t[0] == 's' && (field == L"SimLastName")) { size_t sp = v.find(L' '); v = sp != std::wstring::npos ? v.substr(sp + 1) : L""; }
        return v;
    };
    std::wstring out; size_t i = 0;
    while (i < in.size()) {
        if (in[i] == L'{') {
            size_t j = in.find(L'}', i);
            if (j == std::wstring::npos) { out += in.substr(i); break; }
            std::wstring tag = in.substr(i + 1, j - i - 1);
            if (!tag.empty() && (tag[0] == L'F' || tag[0] == L'M') && tag.size() > 1 && iswdigit(tag[1])) {
                // gênero: usa só a variante masculina/neutra ({M...}) e descarta a {F...} seguinte
                size_t dot = tag.find(L'.');
                if (tag[0] == L'M' && dot != std::wstring::npos) out += tag.substr(dot + 1);
            } else if (!tag.empty() && iswdigit(tag[0])) {
                size_t dot = tag.find(L'.');
                int idx = _wtoi(tag.c_str());
                out += tokStr(idx, dot != std::wstring::npos ? tag.substr(dot + 1) : L"");
            }
            i = j + 1;
        } else out += in[i++];
    }
    return out;
}

// ---- estado do menu
struct MenuItem { uint64_t id; std::wstring text; bool disabled; uint64_t cat; int prio; };
struct MenuCat  { uint64_t key; std::wstring name; };
static std::vector<MenuItem> g_menuItems;
static std::vector<MenuCat>  g_menuCats;
static volatile bool g_menuOpen = false;
static uint64_t g_menuCurCat = 0;          // 0 = topo
static int      g_menuPage = 0;
static int      g_menuHover = -1;
struct MenuButton { std::wstring text; bool disabled; int kind; uint64_t val; float cx, cy, w, h; };   // kind: 0 opção, 1 categoria, 2 mais, 3 voltar
static std::vector<MenuButton> g_menuButtons;
static SRWLOCK g_menuLock = SRWLOCK_INIT;
static const int MENU_TEX = 1024;
static std::vector<uint32_t> g_menuPixels;   // RGBA pré-multiplicado
static volatile bool g_menuDirty = false;
static XrPosef g_menuPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
static float g_menuSize = 0.55f;             // menu_tamanho (m)
struct MenuHit { bool valid; float u, v; ULONGLONG t; };
static MenuHit g_menuHit = {};
static ULONG_PTR g_gdipToken = 0;

static void MenuBuildButtons() {
    g_menuButtons.clear();
    std::vector<MenuButton> list;
    if (g_menuCurCat == 0) {
        for (auto& c : g_menuCats) list.push_back({ c.name.empty() ? L"More options" : c.name, false, 1, c.key });
        for (auto& it : g_menuItems) if (it.cat == 0) list.push_back({ it.text, it.disabled, 0, it.id });
    } else {
        for (auto& it : g_menuItems) if (it.cat == g_menuCurCat) list.push_back({ it.text, it.disabled, 0, it.id });
    }
    const int perPage = 8;
    int pages = (int)((list.size() + perPage - 1) / perPage);
    if (g_menuPage >= pages) g_menuPage = 0;
    int from = g_menuPage * perPage, to = (std::min)((int)list.size(), from + perPage);
    int n = to - from + (pages > 1 ? 1 : 0);
    float R = 330.0f;
    for (int i = from; i < to; ++i) {
        int k = i - from;
        float a = -1.5707963f + 6.2831853f * k / (std::max)(1, n);
        MenuButton b = list[i]; b.cx = 512 + R * std::cos(a); b.cy = 512 + R * std::sin(a); b.w = 300; b.h = 104;
        g_menuButtons.push_back(b);
    }
    if (pages > 1) {
        int k = to - from; float a = -1.5707963f + 6.2831853f * k / n;
        g_menuButtons.push_back({ L"More…", false, 2, 0, 512 + R * std::cos(a), 512 + R * std::sin(a), 220, 90 });
    }
    // centro: título / voltar
    std::wstring title = L"Options";
    if (g_menuCurCat) for (auto& c : g_menuCats) if (c.key == g_menuCurCat) title = c.name;
    g_menuButtons.push_back({ g_menuCurCat ? L"◀ " + title : title, false, 3, 0, 512, 512, 230, 230 });
}

static void MenuRender() {
    if (!g_gdipToken) { Gdiplus::GdiplusStartupInput si; Gdiplus::GdiplusStartup(&g_gdipToken, &si, nullptr); }
    Gdiplus::Bitmap bmp(MENU_TEX, MENU_TEX, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font font(&ff, 25, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font fontC(&ff, 30, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat sf; sf.SetAlignment(Gdiplus::StringAlignmentCenter); sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
        for (int i = 0; i < (int)g_menuButtons.size(); ++i) {
            const MenuButton& b = g_menuButtons[i];
            bool hov = i == g_menuHover && !b.disabled;
            Gdiplus::Color fill = b.kind == 3 ? Gdiplus::Color(225, 20, 70, 50) : hov ? Gdiplus::Color(240, 70, 170, 110) : b.disabled ? Gdiplus::Color(200, 60, 60, 60) : Gdiplus::Color(225, 24, 40, 56);
            Gdiplus::SolidBrush br(fill);
            Gdiplus::Pen pen(hov ? Gdiplus::Color(255, 190, 255, 210) : Gdiplus::Color(200, 120, 200, 160), hov ? 5.0f : 3.0f);
            Gdiplus::RectF r(b.cx - b.w / 2, b.cy - b.h / 2, b.w, b.h);
            if (b.kind == 3) { g.FillEllipse(&br, r); g.DrawEllipse(&pen, r); }
            else {
                Gdiplus::GraphicsPath path; float rr = 26;
                path.AddArc(r.X, r.Y, rr * 2, rr * 2, 180, 90); path.AddArc(r.X + r.Width - rr * 2, r.Y, rr * 2, rr * 2, 270, 90);
                path.AddArc(r.X + r.Width - rr * 2, r.Y + r.Height - rr * 2, rr * 2, rr * 2, 0, 90); path.AddArc(r.X, r.Y + r.Height - rr * 2, rr * 2, rr * 2, 90, 90);
                path.CloseFigure();
                g.FillPath(&br, &path); g.DrawPath(&pen, &path);
            }
            Gdiplus::SolidBrush tb(b.disabled ? Gdiplus::Color(255, 150, 150, 150) : Gdiplus::Color(255, 240, 248, 240));
            Gdiplus::RectF tr(r.X + 12, r.Y + 6, r.Width - 24, r.Height - 12);
            g.DrawString(b.text.c_str(), -1, b.kind == 3 ? &fontC : &font, tr, &sf, &tb);
        }
    }
    Gdiplus::BitmapData bd; Gdiplus::Rect rc(0, 0, MENU_TEX, MENU_TEX);
    if (bmp.LockBits(&rc, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Gdiplus::Ok) {
        std::vector<uint32_t> px(MENU_TEX * MENU_TEX);
        for (int y = 0; y < MENU_TEX; ++y) {
            const uint32_t* row = (const uint32_t*)((uint8_t*)bd.Scan0 + y * bd.Stride);
            for (int x = 0; x < MENU_TEX; ++x) {
                uint32_t p = row[x];                              // BGRA pré-multiplicado -> RGBA
                px[y * MENU_TEX + x] = (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff);
            }
        }
        bmp.UnlockBits(&bd);
        AcquireSRWLockExclusive(&g_menuLock); g_menuPixels.swap(px); g_menuDirty = true; ReleaseSRWLockExclusive(&g_menuLock);
    }
}

static bool MenuLoadFile() {
    char path[MAX_PATH];
    if (!SHGetSpecialFolderPathA(nullptr, path, CSIDL_PERSONAL, FALSE)) return false;
    strcat_s(path, "\\Electronic Arts\\The Sims 4\\Mods\\fpc_menu.txt");
    FILE* f = nullptr; fopen_s(&f, path, "rb");
    if (!f) { Log("menu VR: nao achei %s", path); return false; }
    std::vector<MenuItem> items; std::vector<MenuCat> cats;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string L(line); while (!L.empty() && (L.back() == '\n' || L.back() == '\r')) L.pop_back();
        if (L.rfind("ref|", 0) == 0) continue;
        std::vector<std::string> p; size_t a = 0;
        for (int k = 0; k < 6; ++k) { size_t b = L.find('|', a); if (b == std::string::npos) break; p.push_back(L.substr(a, b - a)); a = b + 1; }
        if (p.size() < 6) continue;
        std::string tokStr = L.substr(a);
        std::vector<std::string> toks; size_t c = 0;
        while (c <= tokStr.size() && !tokStr.empty()) { size_t d = tokStr.find(';', c); toks.push_back(tokStr.substr(c, d == std::string::npos ? std::string::npos : d - c)); if (d == std::string::npos) break; c = d + 1; }
        MenuItem it;
        it.id = _strtoui64(p[0].c_str(), nullptr, 10);
        uint32_t h = (uint32_t)strtoul(p[1].c_str(), nullptr, 16);
        it.disabled = p[2] == "1";
        it.cat = _strtoui64(p[3].c_str(), nullptr, 10);
        uint32_t ch = (uint32_t)strtoul(p[4].c_str(), nullptr, 16);
        it.prio = atoi(p[5].c_str());
        it.text = ApplyTokens(LocText(h), toks);
        if (it.text.empty()) { wchar_t b[32]; swprintf_s(b, L"option %08X", h); it.text = b; }
        items.push_back(it);
        if (it.cat) {
            bool have = false; for (auto& q : cats) if (q.key == it.cat) have = true;
            if (!have) cats.push_back({ it.cat, LocText(ch) });
        }
    }
    fclose(f);
    std::stable_sort(items.begin(), items.end(), [](const MenuItem& x, const MenuItem& y) { return x.prio > y.prio; });
    g_menuItems.swap(items); g_menuCats.swap(cats);
    Log("menu VR: %d opcoes, %d categorias", (int)g_menuItems.size(), (int)g_menuCats.size());
    return !g_menuItems.empty();
}

// (u, v) do laser no menu -> botão sob ele.
static int MenuButtonAt(float u, float v) {
    float px = u * MENU_TEX, py = v * MENU_TEX;
    for (int i = 0; i < (int)g_menuButtons.size(); ++i) {
        const MenuButton& b = g_menuButtons[i];
        if (b.kind == 3) { float dx = px - b.cx, dy = py - b.cy; if (dx * dx + dy * dy <= (b.w / 2) * (b.w / 2)) return i; }
        else if (std::fabs(px - b.cx) <= b.w / 2 && std::fabs(py - b.cy) <= b.h / 2) return i;
    }
    return -1;
}

#ifndef FPC_TESTE_MENU
// Coloca o menu na frente da mão do laser, virado para a cabeça.
static void MenuPlace() {
    CtrlState c; HmdPose hp;
    AcquireSRWLockShared(&g_hmdLock); c = g_ctrl[1]; hp = g_hmdPose; ReleaseSRWLockShared(&g_hmdLock);
    float head[3] = { hp.pos[0], hp.pos[1], hp.pos[2] }, pos[3];
    if (c.aimValid) {
        float dir[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(&c.aim.orientation.x, f0, dir); }
        for (int k = 0; k < 3; ++k) pos[k] = (&c.aim.position.x)[k] + dir[k] * 0.75f;
    } else {
        float hf[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(hp.quat, f0, hf); }
        for (int k = 0; k < 3; ++k) pos[k] = head[k] + hf[k] * 0.9f;
    }
    float z[3] = { head[0] - pos[0], head[1] - pos[1], head[2] - pos[2] }; V3Norm(z);
    const float wy[3] = { 0, 1, 0 };
    float x[3]; V3Cross(wy, z, x); V3Norm(x);
    float y[3]; V3Cross(z, x, y);
    XrPosef p; p.position = { pos[0], pos[1], pos[2] }; p.orientation = QuatFromAxes(x, y, z);
    AcquireSRWLockExclusive(&g_menuLock); g_menuPose = p; ReleaseSRWLockExclusive(&g_menuLock);
}

static void MenuClose(const char* why) {
    if (!g_menuOpen) return;
    g_menuOpen = false; g_menuHover = -1;
    Log("menu VR: fechado (%s)", why);
}

// Thread principal, a cada frame.
static void VrMenuUpdate() {
    if (!g_vrMenuOn) return;
    static ULONGLONG lastAnnounce = 0;
    bool vr = g_vrEnabled && g_xrRunning && g_vrHead && g_phase == RUNNING;
    ULONGLONG now = GetTickCount64();
    if (vr && now - lastAnnounce > 5000) { lastAnnounce = now; SendServerCommand("fpc.vrmenu 1"); }
    if (g_menuPending) {
        g_menuPending = false;
        if (vr && MenuLoadFile()) {
            g_menuCurCat = 0; g_menuPage = 0; g_menuHover = -1;
            MenuBuildButtons(); MenuRender(); MenuPlace();
            g_menuOpen = true;
        }
    }
    if (!vr) MenuClose("fora do VR");
}

static void VrMenuHover(float u, float v) {
    int h = MenuButtonAt(u, v);
    if (h != g_menuHover) { g_menuHover = h; MenuRender(); if (h >= 0) XrHaptic(1, 0.15f, 0.02f); }
}
static void VrMenuClick() {
    if (g_menuHover < 0 || g_menuHover >= (int)g_menuButtons.size()) { MenuClose("clique fora"); return; }
    MenuButton b = g_menuButtons[g_menuHover];
    XrHaptic(1, 0.5f, 0.04f);
    if (b.kind == 0) {
        if (b.disabled) return;
        SendServerCommand("fpc.select %llu", (unsigned long long)b.val);
        Log("menu VR: escolheu '%ls' (%llu)", b.text.c_str(), (unsigned long long)b.val);
        MenuClose("escolha");
    } else if (b.kind == 1) { g_menuCurCat = b.val; g_menuPage = 0; g_menuHover = -1; MenuBuildButtons(); MenuRender(); }
    else if (b.kind == 2) { g_menuPage++; g_menuHover = -1; MenuBuildButtons(); MenuRender(); }
    else if (b.kind == 3) {
        if (g_menuCurCat) { g_menuCurCat = 0; g_menuPage = 0; g_menuHover = -1; MenuBuildButtons(); MenuRender(); }
        else MenuClose("centro");
    }
}
static void VrMenuBack() {
    if (!g_menuOpen) return;
    if (g_menuCurCat) { g_menuCurCat = 0; g_menuPage = 0; g_menuHover = -1; MenuBuildButtons(); MenuRender(); }
    else MenuClose("botao B");
}
static bool VrMenuHitNow(float& u, float& v) {
    if (!g_menuOpen) return false;
    MenuHit h; AcquireSRWLockShared(&g_menuLock); h = g_menuHit; ReleaseSRWLockShared(&g_menuLock);
    if (!h.valid || GetTickCount64() - h.t > 200) return false;
    u = h.u; v = h.v; return true;
}

// ---- thread de render: textura -> swapchain -> camada; acerto do laser
static XrSwapchain g_menuSwap = XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_menuImgs;
static ID3D11Texture2D* g_menuTex = nullptr;
static XrCompositionLayerQuad g_menuLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
static int VrMenuLayers(const XrCompositionLayerBaseHeader** out) {
    MenuHit hit = {};
    int n = 0;
    if (g_menuOpen) {
        DXGI_FORMAT fmt = PickSwapFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!g_menuTex) {
            D3D11_TEXTURE2D_DESC td = {}; td.Width = MENU_TEX; td.Height = MENU_TEX; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_menuTex))) g_menuTex = nullptr;
            if (g_menuTex && !MakeSwap(g_menuSwap, g_menuImgs, MENU_TEX, MENU_TEX, fmt)) { g_menuTex->Release(); g_menuTex = nullptr; }
            Log("menu VR: camada %s", g_menuTex ? "pronta" : "FALHOU");
        }
        if (g_menuTex) {
            if (g_menuDirty) {
                AcquireSRWLockExclusive(&g_menuLock);
                if (!g_menuPixels.empty()) {
                    ID3D11DeviceContext* ctx = nullptr; g_gameDevice->GetImmediateContext(&ctx);
                    if (ctx) { ctx->UpdateSubresource(g_menuTex, 0, nullptr, g_menuPixels.data(), MENU_TEX * 4, 0); ctx->Release(); }
                }
                g_menuDirty = false;
                ReleaseSRWLockExclusive(&g_menuLock);
            }
            if (CopyIntoSwap(g_menuSwap, g_menuImgs, g_menuTex, false, MENU_TEX, MENU_TEX)) {
                XrPosef mp; AcquireSRWLockShared(&g_menuLock); mp = g_menuPose; ReleaseSRWLockShared(&g_menuLock);
                g_menuLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                g_menuLayer.space = g_localSpace; g_menuLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                g_menuLayer.subImage.swapchain = g_menuSwap;
                g_menuLayer.subImage.imageRect = { { 0, 0 }, { MENU_TEX, MENU_TEX } };
                g_menuLayer.pose = mp; g_menuLayer.size = { g_menuSize, g_menuSize };
                out[n++] = (const XrCompositionLayerBaseHeader*)&g_menuLayer;
                // acerto do laser (mão direita)
                CtrlState c; AcquireSRWLockShared(&g_hmdLock); c = g_ctrl[1]; ReleaseSRWLockShared(&g_hmdLock);
                if (g_laserOn && c.aimValid) {
                    float dir[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(&c.aim.orientation.x, f0, dir); }
                    float t, u, v;
                    if (HitQuad(mp, g_menuSize, g_menuSize, &c.aim.position.x, dir, t, u, v)) hit = { true, u, v, GetTickCount64() };
                }
            }
        }
    }
    AcquireSRWLockExclusive(&g_menuLock); g_menuHit = hit; ReleaseSRWLockExclusive(&g_menuLock);
    return n;
}

#endif  // FPC_TESTE_MENU
#ifndef FPC_TESTE_MENU
static void VrMenuInstall() {
    if (uint8_t* f = FindPattern(SIG_LOC_FORMAT)) if (f[0x31] == 0xE8) p_LocSvc = (LocSvc_t)(f + 0x36 + *(int32_t*)(f + 0x32));
    p_LocLookup = (LocLookup_t)FindPattern(SIG_LOC_LOOKUP);
    if (!p_LocSvc || !p_LocLookup) g_vrMenuOn = false;
    Log("menu VR: servico de traducao=%p busca=%p (%s)", p_LocSvc, p_LocLookup, g_vrMenuOn ? "ok" : "DESLIGADO");
}
static void VrMenuLoadConfig(const char* path) {
    g_vrMenuOn = GetPrivateProfileIntA("fpc_rig", "menu_vr", 1, path) != 0;
    char f[32]; GetPrivateProfileStringA("fpc_rig", "menu_tamanho", "0.55", f, sizeof(f), path); g_menuSize = (float)atof(f);
    Log("config: menu_vr=%d menu_tamanho=%.2f", g_vrMenuOn, g_menuSize);
}
#endif  // FPC_TESTE_MENU
