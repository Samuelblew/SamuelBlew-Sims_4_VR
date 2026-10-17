// teste_config.cpp: desenha o menu de configurações (vr_config.inl) fora do jogo, em PNG, uma imagem por aba,
// e confere o registro das opções (nomes do ini, ida e volta dos valores). Sem D3D/OpenXR.
#define FPC_TESTE_CONFIG 1
#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#pragma comment(lib, "gdiplus.lib")

static void Log(const char* f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
// ---- stubs do resto do mod
struct XrPosef { struct { float x, y, z, w; } orientation; struct { float x, y, z; } position; };
struct CtrlState { float trigger, squeeze, sx, sy; bool a, b, stickClick, menu; };
static SRWLOCK g_hmdLock = SRWLOCK_INIT;
static CtrlState g_ctrlRaw[2] = {};
enum { PI_NONE = 0, PI_LT, PI_LG, PI_X, PI_Y, PI_LS, PI_MENU, PI_RT, PI_RG, PI_A, PI_B, PI_RS, PI_COUNT };
static const char* const kPhysKey[PI_COUNT] = { "NADA", "GATILHO_E", "GRIP_E", "X", "Y", "ANALOGICO_E", "MENU", "GATILHO_D", "GRIP_D", "A", "B", "ANALOGICO_D" };
enum { SL_LT = 0, SL_LG, SL_X, SL_Y, SL_LS, SL_MENU, SL_RT, SL_RG, SL_A, SL_B, SL_RS, SL_COUNT };
static const int kBindDefault[SL_COUNT] = { PI_LT, PI_LG, PI_X, PI_Y, PI_LS, PI_MENU, PI_RT, PI_RG, PI_A, PI_B, PI_RS };
static int g_bind[SL_COUNT] = { PI_LT, PI_LG, PI_X, PI_Y, PI_LS, PI_MENU, PI_RT, PI_RG, PI_A, PI_B, PI_RS };
static bool g_swapSticks = false; static volatile bool g_inputMuted = false, g_cfgOpen = false, g_blockGameCursor = false, g_cfgRecenter = false;
static float PhysValue(const CtrlState*, int) { return 0; }
static ULONG_PTR g_gdipToken = 0;
static HWND g_gameHwnd = nullptr;
static unsigned g_eyeWidth = 2080, g_eyeHeight = 2096, g_eyeW = 2080, g_eyeH = 2096, g_uiW = 1920, g_uiH = 1009;
static float g_centerScale = 1, g_worldScale = 1, g_moveInterval = 1.0f / 120, g_turnRate = 10, g_ipdScale = 1, g_heightOffset = 0;
static int g_recenterKey = VK_OEM_COMMA, g_recenterKeyXR = VK_OEM_COMMA, g_walkToggleKey = VK_INSERT, g_panelKey = VK_OEM_PERIOD, g_hideBodyKey = VK_END, g_runKey = VK_LMENU, g_rigTestKey = VK_OEM_1;
enum { UP_OFF = 0, UP_BILINEAR = 1, UP_FSR = 2, UP_CAS = 3 };
static int g_upMethod = UP_FSR, g_vignette = 0, g_laserHand = 1, g_rigArms = 1;
static float g_outScale = 1, g_upSharp = 0.6f, g_rigHeadLimit = 80, g_screenDist = 2.2f, g_screenWidth = 2.4f, g_turnSnapDeg = 30, g_turnSpeed = 120, g_stickDead = 0.2f;
static float g_panelWidth = 0.34f, g_panelLift = 0.12f, g_panelLookDeg = 40, g_worldUiDist = 2, g_menuSize = 0.55f, g_cursorScale = 1, g_rigWristBack = 0.07f, g_rigHandPitch = 0;
static float g_stepDist = 0.18f, g_stepLift = 0.08f, g_grabRadius = 0.14f, g_grabTau = 0.12f, g_avatarResync = 1;
static bool g_symFov = false, g_vrHeadTracking = true, g_turnSnap = false, g_worldUi = true, g_statusOn = true, g_laserVisible = true, g_cursorOn = true;
static bool g_rigFingers = true, g_rigHead = true, g_legsOn = true, g_crouchOn = true, g_leanOn = true, g_grabEnabled = true, g_objGrabOn = true, g_avatarEnabled = true;
static int ParseKey(const char* s) { if (!_stricmp(s, "DELETE")) return VK_DELETE; if (!_stricmp(s, "INSERT")) return VK_INSERT; return s[0] && !s[1] ? toupper(s[0]) : 0; }
static void CfgClose(const char*) {}

#include "../vr_config.inl"

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0; Gdiplus::GetImageEncodersSize(&num, &size); if (!size) return -1;
    std::vector<BYTE> buf(size); auto* info = (Gdiplus::ImageCodecInfo*)buf.data(); Gdiplus::GetImageEncoders(num, size, info);
    for (UINT j = 0; j < num; ++j) if (!wcscmp(info[j].MimeType, format)) { *pClsid = info[j].Clsid; return (int)j; }
    return -1;
}
static void SavePng(const wchar_t* name) {
    Gdiplus::Bitmap o(CFG_W, CFG_H, PixelFormat32bppARGB);
    Gdiplus::BitmapData b; Gdiplus::Rect r(0, 0, CFG_W, CFG_H);
    o.LockBits(&r, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &b);
    for (int y = 0; y < CFG_H; ++y) for (int x = 0; x < CFG_W; ++x) { uint32_t q = g_cfgPixels[(size_t)y * CFG_W + x];
        ((uint32_t*)((uint8_t*)b.Scan0 + y * b.Stride))[x] = (q & 0xff00ff00) | ((q & 0xff) << 16) | ((q >> 16) & 0xff); }
    o.UnlockBits(&b); CLSID png; GetEncoderClsid(L"image/png", &png); o.Save(name, &png, nullptr);
}

int main() {
    int fails = 0;
    CfgBuild();
    printf("opcoes: %d\n", (int)g_cfg.size());
    // ida e volta: valor -> texto do ini -> valor
    for (auto& it : g_cfg) {
        if (!it.key) continue;
        std::string s = CfgIniValue(it);
        float before = CfgGet(it), v;
        if (it.ui == CT_BIND) { v = -1; for (int i = 0; i < PI_COUNT; ++i) if (s == kPhysKey[i]) v = (float)i; }
        else if (it.ui == CT_KEY) v = before;   // (o ParseKey real está no fpc_rig.cpp)
        else if (it.ui == CT_ENUM && !it.iniNames.empty()) { v = -1; for (int i = 0; i < (int)it.iniNames.size(); ++i) if (s == it.iniNames[i]) v = (float)i; }
        else v = (float)atof(s.c_str());
        if (std::fabs(v - before) > 0.002f) { printf("FALHA ida e volta: %s = %s (%.3f != %.3f)\n", it.key, s.c_str(), v, before); ++fails; }
    }
    // chaves repetidas
    for (size_t i = 0; i < g_cfg.size(); ++i) for (size_t j = i + 1; j < g_cfg.size(); ++j)
        if (g_cfg[i].key && g_cfg[j].key && !strcmp(g_cfg[i].key, g_cfg[j].key)) { printf("FALHA chave repetida: %s\n", g_cfg[i].key); ++fails; }
    // uma imagem por aba, com o ponteiro numa linha
    for (int t = 0; t < CFG_TABS; ++t) {
        g_cfgTab = t; g_cfgPx = g_cfgPy = -1; g_cfgPtrVr = (t % 2) == 1;
        CfgRender();
        CfgMoveTo(900, ROW_Y0 + ROW_H * 1.5f, (t % 2) == 1);
        CfgRender();
        wchar_t name[64]; swprintf_s(name, L"config_aba%d.png", t); SavePng(name);
        if (g_cfgHots.empty()) { printf("FALHA aba %d sem areas clicaveis\n", t); ++fails; }
    }
    // clique: alterna um bool e avança um enum
    g_cfgTab = 3; CfgRender();
    bool w0 = g_worldUi;
    for (auto& h : g_cfgHots) if (h.part == HP_TOGGLE && g_cfg[h.item].ptr == &g_worldUi) { CfgPress(h.x + 5, h.y + 5); break; }
    if (g_worldUi == w0) { printf("FALHA clique no interruptor\n"); ++fails; }
    g_cfgTab = 0; CfgRender();
    int u0 = g_upMethod;
    for (auto& h : g_cfgHots) if (h.part == HP_NEXT && g_cfg[h.item].ptr == &g_upMethod) { CfgPress(h.x + 5, h.y + 5); break; }
    if (g_upMethod != (u0 + 1) % 4) { printf("FALHA enum do upscaler (%d)\n", g_upMethod); ++fails; }
    // slider
    for (auto& h : g_cfgHots) if (h.part == HP_SLIDER && g_cfg[h.item].ptr == &g_upSharp) { CfgPress(h.x + h.w * 0.0f + 6, h.y + 5); CfgRelease(); break; }
    if (g_upSharp > 0.05f) { printf("FALHA slider da nitidez (%.2f)\n", g_upSharp); ++fails; }
    printf("\n%s (%d falhas)\n", fails ? "HA FALHAS" : "TUDO OK", fails);
    return fails;
}
