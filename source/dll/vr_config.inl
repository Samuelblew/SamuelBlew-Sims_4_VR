// vr_config.inl: menu de configurações do mod (DEL no teclado, ou segurar o botão de menu do controle
// esquerdo). Incluído por fpc_rig.cpp (por último: usa as variáveis de todos os módulos).
//
// Inspirado no que os mods de VR costumam oferecer (UEVR, VRPerfKit, Skyrim VR, REAL VR...):
//   Imagem        resolução do jogo por olho (com presets estilo FSR), upscaler (FSR 1.0 / CAS / bilinear),
//                 resolução da imagem no headset (supersampling), nitidez, FOV simétrico;
//   Mundo/câmera  escala do mundo, separação dos olhos (IPD), altura da vista, rastreio da cabeça, tela virtual;
//   Conforto      giro suave/em degraus, ângulo, velocidade, zona morta, vinheta em movimento...;
//   Interface     painel da mão, interface no mundo, menus, relógio, laser, cursor;
//   Corpo e mãos  braços, dedos, cabeça, pernas, agachar, inclinar, mãos, passos, agarrar, avatar;
//   Controles VR  qual botão físico faz cada ação (remapeamento) + trocar analógicos;
//   Teclado       teclas do mod (clique e aperte a nova tecla).
// Cada mudança vale na hora e é gravada no fpc_rig.ini. O menu aparece:
//   no headset: um painel na sua frente (laser da mão direita aponta, gatilho clica, analógico rola);
//   no monitor: por cima da imagem do jogo (mouse; Esc fecha). Enquanto aberto, o jogo não recebe as
//   teclas, os cliques nem os botões do controle.

#include <windowsx.h>

enum { CT_BOOL, CT_FLOAT, CT_ENUM, CT_KEY, CT_BIND, CT_BUTTON, CT_INFO };
enum { CF_RESTART = 1, CF_PCT = 2 };
struct CfgItem {
    int tab, ui; char store;                 // store: 'b' bool, 'i' int, 'f' float
    const char* key; const wchar_t* label; const wchar_t* help;
    void* ptr; float mn, mx, step;
    std::vector<const wchar_t*> names;       // opções de um enum
    std::vector<const char*> iniNames;       // como o enum é gravado no ini (vazio = número)
    int flags; const wchar_t* fmt;           // fmt: formato do valor (ex.: L"%.2f m")
    void (*changed)();
    std::wstring (*info)();
    float def;
};
static std::vector<CfgItem> g_cfg;
static const wchar_t* const kCfgTabs[] = { L"Image", L"World & camera", L"Comfort", L"Interface", L"Body & hands", L"VR Controls", L"Keyboard" };
static const int CFG_TABS = 7;
static const int CFG_W = 1280, CFG_H = 800;

static int   g_cfgKey = VK_DELETE;              // tecla_config
static float g_cfgSize = 1.1f;                  // config_tamanho (m, largura do painel no headset)
static int   g_gameResPreset = 0;               // vr_preset: 0 janela atual, 1..5 presets, 6 personalizado
static float g_gameResPct = 1.0f;               // vr_resolucao_jogo (x a resolução recomendada por olho)
static float g_cfgSendHz = 120.0f;              // envios_por_segundo (proxy de g_moveInterval)
static std::string g_cfgIni;

// ---------------------------------------------------------------- nomes de teclas e botões
struct KeyName { int vk; const char* ini; const wchar_t* show; };
static const KeyName kKeyNames[] = {
    { VK_INSERT, "INSERT", L"Insert" }, { VK_DELETE, "DELETE", L"Delete" }, { VK_HOME, "HOME", L"Home" }, { VK_END, "END", L"End" },
    { VK_PRIOR, "PAGEUP", L"Page Up" }, { VK_NEXT, "PAGEDOWN", L"Page Down" }, { VK_CAPITAL, "CAPSLOCK", L"Caps Lock" },
    { VK_PAUSE, "PAUSE", L"Pause" }, { VK_SCROLL, "SCROLLLOCK", L"Scroll Lock" }, { VK_LMENU, "LALT", L"Left Alt" },
    { VK_RMENU, "RALT", L"Right Alt" }, { VK_LCONTROL, "LCTRL", L"Ctrl esquerdo" }, { VK_RCONTROL, "RCTRL", L"Ctrl direito" },
    { VK_LSHIFT, "LSHIFT", L"Left Shift" }, { VK_RSHIFT, "RSHIFT", L"Right Shift" }, { VK_SPACE, "SPACE", L"Space" },
    { VK_TAB, "TAB", L"Tab" }, { VK_OEM_COMMA, "VIRGULA", L"Comma ( , )" }, { VK_OEM_PERIOD, "PONTO", L"Period ( . )" },
    { VK_OEM_1, "CEDILHA", L"Ç" } };
static std::string KeyIni(int vk) {
    for (auto& k : kKeyNames) if (k.vk == vk) return k.ini;
    char b[16];
    if (vk >= VK_F1 && vk <= VK_F24) { sprintf_s(b, "F%d", vk - VK_F1 + 1); return b; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { sprintf_s(b, "NUMPAD%d", vk - VK_NUMPAD0); return b; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { b[0] = (char)vk; b[1] = 0; return b; }
    sprintf_s(b, "0x%02X", vk); return b;
}
static std::wstring KeyShow(int vk) {
    for (auto& k : kKeyNames) if (k.vk == vk) return k.show;
    std::string s = KeyIni(vk); if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) s = "Num " + s.substr(6);
    return std::wstring(s.begin(), s.end());
}
static const wchar_t* const kPhysShow[PI_COUNT] = { L"(none)", L"Left trigger", L"Left grip", L"X", L"Y",
    L"Left stick click", L"Menu ≡ (left)", L"Right trigger", L"Right grip", L"A", L"B", L"Right stick click" };

// ---------------------------------------------------------------- resolução do jogo (tamanho da janela)
static const float kPresetPct[7] = { 0.0f, 1.00f, 0.77f, 0.67f, 0.59f, 0.50f, 0.0f };
static bool  g_haveOrigWin = false; static RECT g_origWin = {};
static std::wstring g_resNote;
static bool  g_resPending = false;
static float GameResPct() { return g_gameResPreset == 6 ? g_gameResPct : kPresetPct[g_gameResPreset]; }
static void ApplyGameResolution() {
    HWND hw = g_gameHwnd;
    if (!hw) { g_resPending = true; return; }
    g_resPending = false; g_resNote.clear();
    LONG st = GetWindowLongW(hw, GWL_STYLE), ex = GetWindowLongW(hw, GWL_EXSTYLE);
    if (g_gameResPreset == 0) {                                 // volta para o tamanho de antes
        if (g_haveOrigWin) {
            SetWindowPos(hw, nullptr, 0, 0, g_origWin.right - g_origWin.left, g_origWin.bottom - g_origWin.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            g_haveOrigWin = false; Log("config: janela do jogo de volta ao tamanho original");
        }
        return;
    }
    if (!g_eyeWidth || !g_eyeHeight) { g_resPending = true; g_resNote = L"(applies when the headset connects)"; return; }
    if (!(st & WS_CAPTION)) { g_resNote = L"The game is fullscreen or borderless: choose \"Windowed\" in the game's graphics options."; Log("config: resolucao do jogo: janela sem borda/tela cheia, nao da para mudar"); return; }
    float p = GameResPct();
    int w = (int)(g_eyeWidth * p + 0.5f) & ~1, h = (int)(g_eyeHeight * p + 0.5f) & ~1;
    if (!g_haveOrigWin) { GetWindowRect(hw, &g_origWin); g_haveOrigWin = true; }
    if (IsZoomed(hw)) ShowWindowAsync(hw, SW_RESTORE);
    RECT r = { 0, 0, w, h }; AdjustWindowRectEx(&r, st, FALSE, ex);
    SetWindowPos(hw, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    Log("config: resolucao do jogo por olho -> %dx%d (%.0f%% de %ux%u)", w, h, p * 100, g_eyeWidth, g_eyeHeight);
}

// ---------------------------------------------------------------- callbacks das opções
static void OnWorldScale() { g_centerScale = g_worldScale > 0.05f ? 1.0f / g_worldScale : 1.0f; }
static void OnRecenterKey() { g_recenterKeyXR = g_recenterKey; }
static void OnSendHz() { g_moveInterval = 1.0f / (std::max)(10.0f, g_cfgSendHz); }
static void OnPreset() { if (g_gameResPreset >= 1 && g_gameResPreset <= 5) g_gameResPct = kPresetPct[g_gameResPreset]; ApplyGameResolution(); }
static void OnResPct() { g_gameResPreset = 6; ApplyGameResolution(); }
static void OnRecenterNow() { g_cfgRecenter = true; }
static std::wstring InfoRes() {
    wchar_t b[256];
    swprintf_s(b, L"Game renders %u×%u per eye  →  headset gets %u×%u   (recommended %u×%u)",
               g_uiW, g_uiH, g_eyeW, g_eyeH, g_eyeWidth, g_eyeHeight);
    return g_resNote.empty() ? std::wstring(b) : std::wstring(b) + L"\n" + g_resNote;
}

// ---------------------------------------------------------------- registro das opções
static void CfgAdd(int tab, int ui, char store, const char* key, const wchar_t* label, const wchar_t* help, void* ptr,
                   float mn = 0, float mx = 1, float step = 0, std::vector<const wchar_t*> names = {}, std::vector<const char*> ini = {},
                   int flags = 0, const wchar_t* fmt = nullptr, void (*changed)() = nullptr, std::wstring (*info)() = nullptr) {
    CfgItem it; it.tab = tab; it.ui = ui; it.store = store; it.key = key; it.label = label; it.help = help; it.ptr = ptr;
    it.mn = mn; it.mx = mx; it.step = step; it.names = names; it.iniNames = ini; it.flags = flags; it.fmt = fmt;
    it.changed = changed; it.info = info; it.def = 0;
    g_cfg.push_back(it);
}
static float CfgGet(const CfgItem& it) {
    if (!it.ptr) return 0;
    switch (it.store) { case 'b': return *(bool*)it.ptr ? 1.0f : 0.0f; case 'i': return (float)*(int*)it.ptr; case 'f': return *(float*)it.ptr; }
    return 0;
}
static void CfgPut(const CfgItem& it, float v) {
    if (!it.ptr) return;
    switch (it.store) { case 'b': *(bool*)it.ptr = v > 0.5f; break; case 'i': *(int*)it.ptr = (int)std::lround(v); break; case 'f': *(float*)it.ptr = v; break; }
}
static std::string CfgIniValue(const CfgItem& it) {
    float v = CfgGet(it); char b[32];
    if (it.ui == CT_KEY) return KeyIni((int)v);
    if (it.ui == CT_BIND) return kPhysKey[(int)v];
    if (it.ui == CT_ENUM && !it.iniNames.empty()) return it.iniNames[(std::min)((int)it.iniNames.size() - 1, (std::max)(0, (int)v))];
    if (it.store == 'f') { sprintf_s(b, "%.3f", v); return b; }
    sprintf_s(b, "%d", (int)std::lround(v)); return b;
}
static void CfgSave(const CfgItem& it) {
    if (!it.key || g_cfgIni.empty()) return;
    WritePrivateProfileStringA("fpc_rig", it.key, CfgIniValue(it).c_str(), g_cfgIni.c_str());
}
static void CfgSetValue(CfgItem& it, float v, bool save) {
    if (it.ui == CT_FLOAT) { v = (std::max)(it.mn, (std::min)(it.mx, v)); if (it.step > 0) v = std::round((v - it.mn) / it.step) * it.step + it.mn; }
    if (it.ui == CT_ENUM) { int n = (int)it.names.size(); int i = (int)std::lround(v); v = (float)(((i % n) + n) % n); }
    if (CfgGet(it) == v && !save) return;
    CfgPut(it, v);
    if (it.changed) it.changed();
    if (save) CfgSave(it);
}

static void CfgBuild() {
    if (!g_cfg.empty()) return;
    g_cfgSendHz = g_moveInterval > 0 ? 1.0f / g_moveInterval : 120.0f;
    // ---- 0 Imagem
    CfgAdd(0, CT_INFO, 0, nullptr, L"Current resolution", nullptr, nullptr, 0, 0, 0, {}, {}, 0, nullptr, nullptr, &InfoRes);
    CfgAdd(0, CT_ENUM, 'i', "vr_preset", L"Game resolution (preset)",
           L"How much the game renders per eye, relative to what the headset asks for. Less = more FPS (the upscaler reconstructs). \"Current window\" leaves the window alone. The game must be in Windowed mode.",
           &g_gameResPreset, 0, 6, 1, { L"Current window (don't change)", L"Native (100%)", L"Ultra quality (77%)", L"Quality (67%)", L"Balanced (59%)", L"Performance (50%)", L"Custom" },
           {}, 0, nullptr, &OnPreset);
    CfgAdd(0, CT_FLOAT, 'f', "vr_resolucao_jogo", L"Game resolution (custom)", L"Per-axis scale of the headset's recommended resolution (used with the Custom preset).",
           &g_gameResPct, 0.30f, 1.50f, 0.01f, {}, {}, CF_PCT, L"%.0f%%", &OnResPct);
    CfgAdd(0, CT_ENUM, 'i', "vr_upscaler", L"Upscaler", L"How the game image becomes the headset image. FSR 1.0 (AMD) reconstructs edges and sharpens; CAS (AMD) applies adaptive sharpening; Off = direct copy at the window resolution (old behavior).",
           &g_upMethod, 0, 3, 1, { L"Off (direct copy)", L"Bilinear", L"FSR 1.0 (AMD)", L"CAS (AMD)" });
    CfgAdd(0, CT_FLOAT, 'f', "vr_supersampling", L"Headset image resolution", L"Multiplies the headset's recommended resolution (per axis). Above 100% = supersampling (sharper, heavier on the GPU).",
           &g_outScale, 0.50f, 2.00f, 0.05f, {}, {}, CF_PCT, L"%.0f%%");
    CfgAdd(0, CT_FLOAT, 'f', "vr_nitidez", L"Sharpness", L"Strength of the FSR (RCAS) or CAS sharpening filter. 0 = soft, 100% = maximum.",
           &g_upSharp, 0.0f, 1.0f, 0.05f, {}, {}, CF_PCT, L"%.0f%%");
    CfgAdd(0, CT_BOOL, 'b', "vr_fov_simetrico", L"Symmetric FOV", L"Uses the same field of view for both eyes (the crosshair and central UI match). Off is correct for the Quest.", &g_symFov);
    // ---- 1 Mundo e câmera
    CfgAdd(1, CT_FLOAT, 'f', "vr_escala_mundo", L"World scale", L"Apparent size of the world. Above 1 the world looks bigger (you feel smaller); below 1, the opposite.",
           &g_worldScale, 0.50f, 2.00f, 0.01f, {}, {}, 0, L"%.2f×", &OnWorldScale);
    CfgAdd(1, CT_FLOAT, 'f', "vr_ipd_escala", L"Eye separation (IPD)", L"Multiplies the distance between the eyes in the game image: higher = stronger depth and objects look smaller.",
           &g_ipdScale, 0.50f, 2.00f, 0.01f, {}, {}, 0, L"%.2f×");
    CfgAdd(1, CT_FLOAT, 'f', "vr_altura", L"View height", L"Raises or lowers the camera (useful for playing seated or matching the Sim's height).",
           &g_heightOffset, -0.60f, 0.60f, 0.01f, {}, {}, 0, L"%+.2f m");
    CfgAdd(1, CT_BUTTON, 0, nullptr, L"Recenter view now", L"Same as the recenter key/button.", nullptr, 0, 0, 0, {}, {}, 0, nullptr, &OnRecenterNow);
    CfgAdd(1, CT_BOOL, 'b', "vr_rastrear_cabeca", L"Head tracking", L"The game camera follows the headset. Off = virtual screen only.", &g_vrHeadTracking);
    CfgAdd(1, CT_FLOAT, 'f', "vr_cabeca_limite", L"Sim's maximum head turn", L"How many degrees the Sim's head follows yours before the body turns.",
           &g_rigHeadLimit, 30, 120, 5, {}, {}, 0, L"%.0f°");
    CfgAdd(1, CT_FLOAT, 'f', "vr_tela_distancia", L"Virtual screen distance", L"Screen used in the game's menus, build mode and loading screens.",
           &g_screenDist, 1.0f, 5.0f, 0.1f, {}, {}, 0, L"%.1f m");
    CfgAdd(1, CT_FLOAT, 'f', "vr_tela_largura", L"Virtual screen width", nullptr, &g_screenWidth, 1.0f, 6.0f, 0.1f, {}, {}, 0, L"%.1f m");
    // ---- 2 Movimento e conforto
    CfgAdd(2, CT_ENUM, 'b', "vr_giro_passos", L"Turn type", L"Right stick: smooth or snap turning (snap is more comfortable if you get motion sick).",
           &g_turnSnap, 0, 1, 1, { L"Smooth", L"Snap" });
    CfgAdd(2, CT_FLOAT, 'f', "vr_giro_passo", L"Snap angle", nullptr, &g_turnSnapDeg, 10, 90, 5, {}, {}, 0, L"%.0f°");
    CfgAdd(2, CT_FLOAT, 'f', "vr_giro_velocidade", L"Smooth turn speed", nullptr, &g_turnSpeed, 30, 360, 10, {}, {}, 0, L"%.0f°/s");
    CfgAdd(2, CT_ENUM, 'i', "vr_vinheta", L"Motion vignette", L"Darkens the edges of your view while walking or turning with the stick (reduces motion sickness).",
           &g_vignette, 0, 3, 1, { L"Off", L"Light", L"Medium", L"Strong" });
    CfgAdd(2, CT_FLOAT, 'f', "vr_zona_morta", L"Stick dead zone", nullptr, &g_stickDead, 0.05f, 0.60f, 0.01f, {}, {}, CF_PCT, L"%.0f%%");
    CfgAdd(2, CT_FLOAT, 'f', "envios_por_segundo", L"Movement updates per second", L"How many times per second the position is sent to the game while walking (more = smoother).",
           &g_cfgSendHz, 30, 240, 10, {}, {}, 0, L"%.0f /s", &OnSendHz);
    CfgAdd(2, CT_FLOAT, 'f', "giro_corpo_velocidade", L"How fast the body follows your direction", nullptr, &g_turnRate, 1, 60, 1, {}, {}, 0, L"%.0f");
    // ---- 3 Interface
    CfgAdd(3, CT_FLOAT, 'f', "ui_painel_largura", L"Hand panel width", nullptr, &g_panelWidth, 0.20f, 0.70f, 0.01f, {}, {}, 0, L"%.2f m");
    CfgAdd(3, CT_FLOAT, 'f', "ui_painel_altura", L"Panel height above the hand", nullptr, &g_panelLift, 0.0f, 0.30f, 0.01f, {}, {}, 0, L"%.2f m");
    CfgAdd(3, CT_FLOAT, 'f', "ui_painel_olhar", L"Angle to open the panel", L"How directly you need to look at your left hand for the grip to open the panel.",
           &g_panelLookDeg, 15, 90, 5, {}, {}, 0, L"%.0f°");
    CfgAdd(3, CT_BOOL, 'b', "ui_mundo", L"World UI", L"The game's speech bubbles, labels and pie menu stay in the world (projected like in the game).", &g_worldUi);
    CfgAdd(3, CT_FLOAT, 'f', "ui_mundo_distancia", L"World UI distance", nullptr, &g_worldUiDist, 0.5f, 5.0f, 0.1f, {}, {}, 0, L"%.1f m");
    CfgAdd(3, CT_FLOAT, 'f', "menu_tamanho", L"Options menu size (VR pie)", nullptr, &g_menuSize, 0.30f, 1.00f, 0.05f, {}, {}, 0, L"%.2f m");
    CfgAdd(3, CT_BOOL, 'b', "relogio", L"Wrist watch", L"Panel with needs, time and money when you turn your left wrist.", &g_statusOn);
    CfgAdd(3, CT_BOOL, 'b', "laser_visivel", L"Show the laser beam", nullptr, &g_laserVisible);
    CfgAdd(3, CT_ENUM, 'i', "laser_mao", L"Laser hand", nullptr, &g_laserHand, 0, 1, 1, { L"Left", L"Right" }, { "ESQUERDA", "DIREITA" });
    CfgAdd(3, CT_BOOL, 'b', "vr_cursor", L"Mouse cursor in the headset", nullptr, &g_cursorOn);
    CfgAdd(3, CT_FLOAT, 'f', "vr_cursor_escala", L"Cursor size", nullptr, &g_cursorScale, 0.5f, 3.0f, 0.1f, {}, {}, 0, L"%.1f×");
    CfgAdd(3, CT_FLOAT, 'f', "config_tamanho", L"Size of this menu in the headset", nullptr, &g_cfgSize, 0.6f, 1.8f, 0.05f, {}, {}, 0, L"%.2f m");
    // ---- 4 Corpo e mãos
    CfgAdd(4, CT_ENUM, 'i', "vr_bracos", L"Arms follow the controllers", nullptr, &g_rigArms, 0, 2, 1, { L"No", L"Outside interactions", L"Always" });
    CfgAdd(4, CT_BOOL, 'b', "vr_dedos", L"Fingers (trigger and grip)", nullptr, &g_rigFingers);
    CfgAdd(4, CT_BOOL, 'b', "vr_cabeca", L"Sim's head follows the headset", nullptr, &g_rigHead);
    CfgAdd(4, CT_BOOL, 'b', "vr_pernas", L"Legs and steps", nullptr, &g_legsOn);
    CfgAdd(4, CT_BOOL, 'b', "vr_agachar", L"Real crouching", nullptr, &g_crouchOn);
    CfgAdd(4, CT_BOOL, 'b', "vr_inclinar", L"Lean the torso", nullptr, &g_leanOn);
    CfgAdd(4, CT_FLOAT, 'f', "vr_mao_recuo", L"Wrist position", L"How far the wrist sits behind the center of the controller.", &g_rigWristBack, 0.0f, 0.20f, 0.005f, {}, {}, 0, L"%.3f m");
    CfgAdd(4, CT_FLOAT, 'f', "vr_mao_inclinacao", L"Hand tilt", nullptr, &g_rigHandPitch, -60, 60, 1, {}, {}, 0, L"%.0f°");
    CfgAdd(4, CT_FLOAT, 'f', "passo_distancia", L"Distance before taking a step", nullptr, &g_stepDist, 0.05f, 0.50f, 0.01f, {}, {}, 0, L"%.2f m");
    CfgAdd(4, CT_FLOAT, 'f', "passo_altura", L"Step height", nullptr, &g_stepLift, 0.0f, 0.20f, 0.01f, {}, {}, 0, L"%.2f m");
    CfgAdd(4, CT_BOOL, 'b', "agarrar", L"Grab other Sims", nullptr, &g_grabEnabled);
    CfgAdd(4, CT_FLOAT, 'f', "agarrar_raio", L"Hand grab reach", nullptr, &g_grabRadius, 0.05f, 0.40f, 0.01f, {}, {}, 0, L"%.2f m");
    CfgAdd(4, CT_FLOAT, 'f', "agarrar_mola", L"Grabbed limb softness", nullptr, &g_grabTau, 0.02f, 0.50f, 0.01f, {}, {}, 0, L"%.2f s");
    CfgAdd(4, CT_BOOL, 'b', "agarrar_objetos", L"Grab small objects", nullptr, &g_objGrabOn);
    CfgAdd(4, CT_BOOL, 'b', "avatar_cliente", L"Smooth body while walking (client-side avatar)", nullptr, &g_avatarEnabled);
    CfgAdd(4, CT_FLOAT, 'f', "avatar_ressinc", L"Body resync distance", nullptr, &g_avatarResync, 0.3f, 3.0f, 0.1f, {}, {}, 0, L"%.1f m");
    // ---- 5 Controles VR
    struct { int slot; const char* key; const wchar_t* label; } acts[] = {
        { SL_RG, "botao_laser", L"Laser (hold) and right-hand grab" },
        { SL_RT, "botao_selecionar", L"Select / click" },
        { SL_B, "botao_voltar", L"Back / close menu and pie" },
        { SL_RS, "botao_recentrar", L"Recenter view" },
        { SL_LG, "botao_painel", L"Hand panel and left-hand grab" },
        { SL_LS, "botao_correr", L"Run (toggle)" },
        { SL_MENU, "botao_menu", L"Virtual screen (tap) · Settings (hold)" } };
    for (auto& a : acts) {
        CfgAdd(5, CT_BIND, 'i', a.key, a.label, L"Click, then press the controller button that should perform this action.", &g_bind[a.slot]);
        g_cfg.back().def = (float)kBindDefault[a.slot];
    }
    CfgAdd(5, CT_BOOL, 'b', "trocar_analogicos", L"Swap sticks", L"Walk with the right stick and turn with the left.", &g_swapSticks);
    // ---- 6 Teclado
    CfgAdd(6, CT_KEY, 'i', "tecla_modo_fps", L"First-person mode (walk)", nullptr, &g_walkToggleKey); g_cfg.back().def = VK_INSERT;
    CfgAdd(6, CT_KEY, 'i', "tecla_config", L"Open/close these settings", nullptr, &g_cfgKey); g_cfg.back().def = VK_DELETE;
    CfgAdd(6, CT_KEY, 'i', "tecla_recentrar", L"Recenter view", nullptr, &g_recenterKey, 0, 0, 0, {}, {}, 0, nullptr, &OnRecenterKey); g_cfg.back().def = VK_OEM_COMMA;
    CfgAdd(6, CT_KEY, 'i', "tecla_painel_vr", L"Virtual screen (panel mode)", nullptr, &g_panelKey); g_cfg.back().def = VK_OEM_PERIOD;
    CfgAdd(6, CT_KEY, 'i', "tecla_esconder_corpo", L"Hide the body", nullptr, &g_hideBodyKey); g_cfg.back().def = VK_END;
    CfgAdd(6, CT_KEY, 'i', "tecla_correr", L"Run (hold)", nullptr, &g_runKey); g_cfg.back().def = VK_LMENU;
    CfgAdd(6, CT_KEY, 'i', "tecla_teste_bracos", L"Arm test", nullptr, &g_rigTestKey); g_cfg.back().def = VK_OEM_1;
    // padrões = valores que estão nas variáveis agora (antes de ler o ini), exceto os já definidos acima
    for (auto& it : g_cfg) if (it.ui != CT_BIND && it.ui != CT_KEY) it.def = CfgGet(it);
}

// Lê do ini as opções presentes (as ausentes mantêm o valor atual). Chamado no fim do LoadConfig.
static void VrConfigLoadAll(const char* path) {
    g_cfgIni = path;
    CfgBuild();
    int n = 0;
    for (auto& it : g_cfg) {
        if (!it.key) continue;
        char b[64] = {};
        GetPrivateProfileStringA("fpc_rig", it.key, "", b, sizeof(b), path);
        if (!b[0]) continue;
        float v;
        if (it.ui == CT_KEY) { int vk = ParseKey(b); if (!vk) continue; v = (float)vk; }
        else if (it.ui == CT_BIND) { int k = -1; for (int i = 0; i < PI_COUNT; ++i) if (_stricmp(b, kPhysKey[i]) == 0) k = i; if (k < 0) continue; v = (float)k; }
        else if (it.ui == CT_ENUM && !it.iniNames.empty()) { int k = -1; for (int i = 0; i < (int)it.iniNames.size(); ++i) if (_stricmp(b, it.iniNames[i]) == 0) k = i; if (k < 0) k = atoi(b); v = (float)k; }
        else v = (float)atof(b);
        if (it.ui == CT_FLOAT) v = (std::max)(it.mn, (std::min)(it.mx, v));
        CfgPut(it, v);
        if (it.changed && it.changed != &OnPreset && it.changed != &OnResPct) it.changed();
        ++n;
    }
    g_recenterKeyXR = g_recenterKey;
    if (g_gameResPreset >= 1 && g_gameResPreset <= 5) g_gameResPct = kPresetPct[g_gameResPreset];
    if (g_gameResPreset) g_resPending = true;                   // aplica quando a janela e o headset existirem
    Log("config: menu de configuracoes: %d opcoes (%d lidas do ini), tecla %s", (int)g_cfg.size(), n, KeyIni(g_cfgKey).c_str());
}

// ---------------------------------------------------------------- estado do menu
static int   g_cfgTab = 0;
static int   g_cfgScroll[CFG_TABS] = {};
static int   g_cfgHoverItem = -1, g_cfgHoverPart = 0;
static int   g_cfgDrag = -1;                 // opção (slider) sendo arrastada
static int   g_cfgCapKey = -1, g_cfgCapBind = -1;
static ULONGLONG g_cfgCapT = 0;
static bool  g_cfgCapPrev[256] = {}; static float g_cfgCapPhysPrev[PI_COUNT] = {};
static float g_cfgPx = -1, g_cfgPy = -1; static bool g_cfgPtrVr = false;
static volatile bool g_cfgDirty = true;
static std::vector<uint32_t> g_cfgPixels; static volatile bool g_cfgPixDirty = false;
static SRWLOCK g_cfgLock = SRWLOCK_INIT;
static XrPosef g_cfgPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
struct CfgHot { float x, y, w, h; int part, item; };
enum { HP_TAB = 1, HP_ROW, HP_TOGGLE, HP_SLIDER, HP_PREV, HP_NEXT, HP_KEY, HP_BIND, HP_BUTTON, HP_RESET, HP_CLOSE, HP_SCROLLUP, HP_SCROLLDN };
static std::vector<CfgHot> g_cfgHots;
static const float ROW_H = 58, ROW_Y0 = 92, CONTENT_X = 262, CTRL_X = 800, CTRL_W = 440;
static int CfgVisibleRows() { return (int)((CFG_H - 96 - ROW_Y0) / ROW_H); }
// entrada vinda da janela (thread da janela) e do VR (thread de render)
struct CfgMouseQ { int x, y; int downs, ups; int wheel; bool moved; };
static CfgMouseQ g_cfgMouse = {};
struct CfgVrHit { bool valid; float u, v; };
static CfgVrHit g_cfgVrHit = {};
struct CfgMonRect { int x, y, w, h; };
static CfgMonRect g_cfgMon = {};

static std::vector<int> CfgTabItems(int tab) { std::vector<int> v; for (int i = 0; i < (int)g_cfg.size(); ++i) if (g_cfg[i].tab == tab) v.push_back(i); return v; }

static std::wstring CfgValueText(const CfgItem& it) {
    float v = CfgGet(it); wchar_t b[64];
    switch (it.ui) {
    case CT_BOOL: return v > 0.5f ? L"On" : L"Off";
    case CT_ENUM: { int i = (int)v; return (i >= 0 && i < (int)it.names.size()) ? it.names[i] : L"?"; }
    case CT_KEY: return KeyShow((int)v);
    case CT_BIND: { int i = (int)v; return (i >= 0 && i < PI_COUNT) ? kPhysShow[i] : L"?"; }
    case CT_FLOAT: swprintf_s(b, it.fmt ? it.fmt : L"%.2f", (it.flags & CF_PCT) ? v * 100.0f : v); return b;
    }
    return L"";
}

// ---------------------------------------------------------------- desenho (GDI+, thread principal)
static void CfgRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    p.AddArc(x, y, r * 2, r * 2, 180, 90); p.AddArc(x + w - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90); p.AddArc(x, y + h - r * 2, r * 2, r * 2, 90, 90); p.CloseFigure();
}
static void CfgRender() {
    if (!g_gdipToken) { Gdiplus::GdiplusStartupInput si; Gdiplus::GdiplusStartup(&g_gdipToken, &si, nullptr); }
    using namespace Gdiplus;
    Bitmap bmp(CFG_W, CFG_H, PixelFormat32bppPARGB);
    std::vector<CfgHot> hots;
    {
        Graphics g(&bmp);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(255, 22, 26, 33));
        FontFamily ff(L"Segoe UI");
        Font fTitle(&ff, 30, FontStyleBold, UnitPixel), fTab(&ff, 22, FontStyleBold, UnitPixel), fRow(&ff, 22, FontStyleRegular, UnitPixel),
             fVal(&ff, 20, FontStyleBold, UnitPixel), fHelp(&ff, 18, FontStyleRegular, UnitPixel), fSmall(&ff, 17, FontStyleRegular, UnitPixel);
        StringFormat left; left.SetLineAlignment(StringAlignmentCenter); left.SetTrimming(StringTrimmingEllipsisCharacter); left.SetFormatFlags(StringFormatFlagsNoWrap);
        StringFormat center; center.SetAlignment(StringAlignmentCenter); center.SetLineAlignment(StringAlignmentCenter); center.SetTrimming(StringTrimmingEllipsisCharacter);
        StringFormat wrap; wrap.SetLineAlignment(StringAlignmentNear); wrap.SetTrimming(StringTrimmingEllipsisWord);
        SolidBrush white(Color(255, 236, 240, 245)), dim(Color(255, 150, 160, 175)), accent(Color(255, 90, 200, 140)), panel(Color(255, 32, 38, 48)),
                   hover(Color(255, 44, 54, 68)), track(Color(255, 60, 70, 86)), knob(Color(255, 236, 240, 245)), warn(Color(255, 240, 190, 90));
        // cabeçalho
        g.FillRectangle(&panel, RectF(0, 0, (REAL)CFG_W, 70));
        g.DrawString(L"VR Settings", -1, &fTitle, RectF(24, 0, 600, 70), &left, &white);
        std::wstring hint = L"Close: " + KeyShow(g_cfgKey) + L" · Esc · hold ≡";
        g.DrawString(hint.c_str(), -1, &fSmall, RectF(560, 0, 600, 70), &left, &dim);
        { RectF xr((REAL)CFG_W - 64, 13, 44, 44); SolidBrush xb(g_cfgHoverPart == HP_CLOSE ? Color(255, 200, 70, 70) : Color(255, 70, 40, 46));
          GraphicsPath p; CfgRoundRect(p, xr.X, xr.Y, xr.Width, xr.Height, 10); g.FillPath(&xb, &p);
          g.DrawString(L"✕", -1, &fTab, xr, &center, &white); hots.push_back({ xr.X, xr.Y, xr.Width, xr.Height, HP_CLOSE, -1 }); }
        // abas
        for (int t = 0; t < CFG_TABS; ++t) {
            RectF r(12, 84 + t * 62.0f, 232, 54);
            bool sel = t == g_cfgTab, hv = g_cfgHoverPart == HP_TAB && g_cfgHoverItem == t;
            SolidBrush b(sel ? Color(255, 40, 120, 85) : hv ? Color(255, 44, 54, 68) : Color(255, 30, 35, 44));
            GraphicsPath p; CfgRoundRect(p, r.X, r.Y, r.Width, r.Height, 12); g.FillPath(&b, &p);
            g.DrawString(kCfgTabs[t], -1, &fTab, RectF(r.X + 16, r.Y, r.Width - 20, r.Height), &left, &white);
            hots.push_back({ r.X, r.Y, r.Width, r.Height, HP_TAB, t });
        }
        // linhas da aba
        std::vector<int> items = CfgTabItems(g_cfgTab);
        int vis = CfgVisibleRows();
        int maxScroll = (std::max)(0, (int)items.size() - vis);
        g_cfgScroll[g_cfgTab] = (std::max)(0, (std::min)(maxScroll, g_cfgScroll[g_cfgTab]));
        int first = g_cfgScroll[g_cfgTab];
        for (int k = 0; k < vis && first + k < (int)items.size(); ++k) {
            int idx = items[first + k]; const CfgItem& it = g_cfg[idx];
            float y = ROW_Y0 + k * ROW_H;
            RectF row(CONTENT_X, y, (REAL)CFG_W - CONTENT_X - 16, ROW_H - 6);
            bool hv = g_cfgHoverItem == idx && g_cfgHoverPart != HP_TAB;
            if (hv) { GraphicsPath p; CfgRoundRect(p, row.X, row.Y, row.Width, row.Height, 10); g.FillPath(&hover, &p); }
            hots.push_back({ row.X, row.Y, row.Width, row.Height, HP_ROW, idx });
            if (it.ui == CT_INFO) {
                std::wstring s = it.info ? it.info() : L"";
                g.DrawString(s.c_str(), -1, &fSmall, RectF(row.X + 14, row.Y + 4, row.Width - 20, row.Height - 4), &wrap, &accent);
                continue;
            }
            std::wstring lab = it.label; if (it.flags & CF_RESTART) lab += L" *";
            g.DrawString(lab.c_str(), -1, &fRow, RectF(row.X + 14, row.Y, CTRL_X - row.X - 24, row.Height), &left, &white);
            RectF cr(CTRL_X, y + 7, CTRL_W, ROW_H - 20);
            float v = CfgGet(it);
            if (it.ui == CT_BOOL) {
                RectF sw(cr.X + cr.Width - 84, cr.Y + 2, 76, cr.Height - 4);
                SolidBrush sb(v > 0.5f ? Color(255, 60, 170, 110) : Color(255, 70, 76, 90));
                GraphicsPath p; CfgRoundRect(p, sw.X, sw.Y, sw.Width, sw.Height, sw.Height / 2); g.FillPath(&sb, &p);
                float kx = v > 0.5f ? sw.X + sw.Width - sw.Height + 3 : sw.X + 3;
                g.FillEllipse(&knob, RectF(kx, sw.Y + 3, sw.Height - 6, sw.Height - 6));
                g.DrawString(CfgValueText(it).c_str(), -1, &fVal, RectF(cr.X, cr.Y, cr.Width - 96, cr.Height), &left, &dim);
                hots.push_back({ cr.X, cr.Y, cr.Width, cr.Height, HP_TOGGLE, idx });
            } else if (it.ui == CT_FLOAT) {
                float tw = cr.Width - 130, t = (v - it.mn) / (it.mx - it.mn); t = t < 0 ? 0 : t > 1 ? 1 : t;
                RectF tr(cr.X + 6, cr.Y + cr.Height / 2 - 4, tw, 8);
                g.FillRectangle(&track, tr);
                g.FillRectangle(&accent, RectF(tr.X, tr.Y, tw * t, 8));
                g.FillEllipse(&knob, RectF(tr.X + tw * t - 11, cr.Y + cr.Height / 2 - 11, 22, 22));
                g.DrawString(CfgValueText(it).c_str(), -1, &fVal, RectF(cr.X + tw + 16, cr.Y, 120, cr.Height), &left, &white);
                hots.push_back({ cr.X - 6, cr.Y, tw + 24, cr.Height, HP_SLIDER, idx });
            } else if (it.ui == CT_ENUM) {
                RectF pr(cr.X, cr.Y, 44, cr.Height), nr(cr.X + cr.Width - 44, cr.Y, 44, cr.Height);
                SolidBrush ab(Color(255, 50, 60, 76));
                { GraphicsPath p; CfgRoundRect(p, pr.X, pr.Y, pr.Width, pr.Height, 8); g.FillPath(&ab, &p); }
                { GraphicsPath p; CfgRoundRect(p, nr.X, nr.Y, nr.Width, nr.Height, 8); g.FillPath(&ab, &p); }
                g.DrawString(L"◀", -1, &fVal, pr, &center, &white); g.DrawString(L"▶", -1, &fVal, nr, &center, &white);
                g.DrawString(CfgValueText(it).c_str(), -1, &fVal, RectF(cr.X + 48, cr.Y, cr.Width - 96, cr.Height), &center, &white);
                hots.push_back({ pr.X, pr.Y, pr.Width, pr.Height, HP_PREV, idx });
                hots.push_back({ nr.X, nr.Y, nr.Width, nr.Height, HP_NEXT, idx });
                hots.push_back({ cr.X + 48, cr.Y, cr.Width - 96, cr.Height, HP_NEXT, idx });
            } else if (it.ui == CT_KEY || it.ui == CT_BIND) {
                bool cap = (it.ui == CT_KEY && g_cfgCapKey == idx) || (it.ui == CT_BIND && g_cfgCapBind == idx);
                SolidBrush bb(cap ? Color(255, 150, 110, 30) : Color(255, 50, 60, 76));
                GraphicsPath p; CfgRoundRect(p, cr.X, cr.Y, cr.Width, cr.Height, 8); g.FillPath(&bb, &p);
                std::wstring s = cap ? (it.ui == CT_KEY ? L"Press a key…  (Esc cancels)" : L"Press a controller button…  (B cancels)") : CfgValueText(it);
                g.DrawString(s.c_str(), -1, &fVal, cr, &center, &white);
                hots.push_back({ cr.X, cr.Y, cr.Width, cr.Height, it.ui == CT_KEY ? HP_KEY : HP_BIND, idx });
            } else if (it.ui == CT_BUTTON) {
                SolidBrush bb(Color(255, 40, 120, 85));
                GraphicsPath p; CfgRoundRect(p, cr.X, cr.Y, cr.Width, cr.Height, 8); g.FillPath(&bb, &p);
                g.DrawString(L"Run", -1, &fVal, cr, &center, &white);
                hots.push_back({ cr.X, cr.Y, cr.Width, cr.Height, HP_BUTTON, idx });
            }
        }
        // rolagem
        if (maxScroll > 0) {
            RectF up((REAL)CFG_W - 60, ROW_Y0 - 2, 44, 30), dn((REAL)CFG_W - 60, ROW_Y0 + vis * ROW_H - 30, 44, 30);
            wchar_t sb[32]; swprintf_s(sb, L"%d–%d of %d", first + 1, (std::min)((int)items.size(), first + vis), (int)items.size());
            g.DrawString(sb, -1, &fSmall, RectF(CONTENT_X, CFG_H - 128.0f, 400, 26), &left, &dim);
            g.DrawString(L"▲", -1, &fVal, up, &center, first > 0 ? &white : &dim);
            g.DrawString(L"▼", -1, &fVal, dn, &center, first < maxScroll ? &white : &dim);
            hots.push_back({ up.X, up.Y, up.Width, up.Height, HP_SCROLLUP, -1 });
            hots.push_back({ dn.X, dn.Y, dn.Width, dn.Height, HP_SCROLLDN, -1 });
        }
        // rodapé: ajuda da opção sob o ponteiro + restaurar padrões
        g.FillRectangle(&panel, RectF(0, (REAL)CFG_H - 96, (REAL)CFG_W, 96));
        const wchar_t* help = (g_cfgHoverItem >= 0 && g_cfgHoverPart != HP_TAB && g_cfgHoverItem < (int)g_cfg.size()) ? g_cfg[g_cfgHoverItem].help : nullptr;
        g.DrawString(help ? help : L"Point at or hover an option to see what it does. Changes apply immediately and are saved to fpc_rig.ini.",
                     -1, &fHelp, RectF(24, (REAL)CFG_H - 88, CFG_W - 330.0f, 80), &wrap, &dim);
        { RectF rr((REAL)CFG_W - 290, (REAL)CFG_H - 74, 270, 52);
          SolidBrush rb(g_cfgHoverPart == HP_RESET ? Color(255, 120, 80, 40) : Color(255, 70, 56, 40));
          GraphicsPath p; CfgRoundRect(p, rr.X, rr.Y, rr.Width, rr.Height, 10); g.FillPath(&rb, &p);
          g.DrawString(L"Restore tab defaults", -1, &fVal, rr, &center, &white); hots.push_back({ rr.X, rr.Y, rr.Width, rr.Height, HP_RESET, -1 }); }
        // ponteiro
        if (g_cfgPx >= 0) {
            if (g_cfgPtrVr) { SolidBrush pb(Color(230, 120, 220, 255)); g.FillEllipse(&pb, RectF(g_cfgPx - 8, g_cfgPy - 8, 16, 16)); }
            else {
                PointF arrow[7] = { { 0, 0 }, { 0, 26 }, { 7, 20 }, { 12, 31 }, { 17, 29 }, { 12, 18 }, { 21, 18 } };
                for (auto& a : arrow) { a.X += g_cfgPx; a.Y += g_cfgPy; }
                SolidBrush ab(Color(255, 255, 255, 255)); Pen ap(Color(255, 0, 0, 0), 2.0f);
                g.FillPolygon(&ab, arrow, 7); g.DrawPolygon(&ap, arrow, 7);
            }
        }
    }
    g_cfgHots.swap(hots);
    BitmapData bd; Rect rc(0, 0, CFG_W, CFG_H);
    if (bmp.LockBits(&rc, ImageLockModeRead, PixelFormat32bppPARGB, &bd) == Ok) {
        std::vector<uint32_t> px((size_t)CFG_W * CFG_H);
        for (int y = 0; y < CFG_H; ++y) {
            const uint32_t* row = (const uint32_t*)((uint8_t*)bd.Scan0 + y * bd.Stride);
            for (int x = 0; x < CFG_W; ++x) { uint32_t p = row[x]; px[(size_t)y * CFG_W + x] = (p & 0xff00ff00) | ((p & 0xff) << 16) | ((p >> 16) & 0xff); }
        }
        bmp.UnlockBits(&bd);
        AcquireSRWLockExclusive(&g_cfgLock); g_cfgPixels.swap(px); g_cfgPixDirty = true; ReleaseSRWLockExclusive(&g_cfgLock);
    }
}

// ---------------------------------------------------------------- interação (thread principal)
static const CfgHot* CfgHotAt(float x, float y) {
    const CfgHot* best = nullptr;                               // o último desenhado (mais específico) ganha
    for (auto& h : g_cfgHots) if (x >= h.x && x < h.x + h.w && y >= h.y && y < h.y + h.h) best = &h;
    return best;
}
static void CfgSliderTo(int idx, float x) {
    CfgItem& it = g_cfg[idx];
    float tw = CTRL_W - 130, t = (x - (CTRL_X + 6)) / tw; t = t < 0 ? 0 : t > 1 ? 1 : t;
    CfgSetValue(it, it.mn + t * (it.mx - it.mn), false);
    g_cfgDirty = true;
}
static void CfgClose(const char* why);
static void CfgPress(float x, float y) {
    const CfgHot* h = CfgHotAt(x, y);
    if (!h) return;
    g_cfgCapKey = g_cfgCapBind = -1;
    switch (h->part) {
    case HP_TAB: g_cfgTab = h->item; break;
    case HP_CLOSE: CfgClose("botao fechar"); return;
    case HP_SCROLLUP: g_cfgScroll[g_cfgTab]--; break;
    case HP_SCROLLDN: g_cfgScroll[g_cfgTab]++; break;
    case HP_TOGGLE: { CfgItem& it = g_cfg[h->item]; CfgSetValue(it, CfgGet(it) > 0.5f ? 0.0f : 1.0f, true); break; }
    case HP_PREV: case HP_NEXT: { CfgItem& it = g_cfg[h->item]; CfgSetValue(it, CfgGet(it) + (h->part == HP_NEXT ? 1 : -1), true); break; }
    case HP_SLIDER: g_cfgDrag = h->item; CfgSliderTo(h->item, x); break;
    case HP_KEY: g_cfgCapKey = h->item; g_cfgCapT = GetTickCount64(); for (int k = 0; k < 256; ++k) g_cfgCapPrev[k] = (GetAsyncKeyState(k) & 0x8000) != 0; break;
    case HP_BIND: g_cfgCapBind = h->item; g_cfgCapT = GetTickCount64();
        { CtrlState r[2]; AcquireSRWLockShared(&g_hmdLock); r[0] = g_ctrlRaw[0]; r[1] = g_ctrlRaw[1]; ReleaseSRWLockShared(&g_hmdLock);
          for (int i = 0; i < PI_COUNT; ++i) g_cfgCapPhysPrev[i] = PhysValue(r, i); } break;
    case HP_BUTTON: if (g_cfg[h->item].changed) g_cfg[h->item].changed(); Log("config: %ls", g_cfg[h->item].label); break;
    case HP_RESET:
        for (auto& it : g_cfg) if (it.tab == g_cfgTab && it.ptr && it.key) CfgSetValue(it, it.def, true);
        Log("config: padroes restaurados na aba %d", g_cfgTab);
        break;
    }
    g_cfgDirty = true;
}
static void CfgRelease() {
    if (g_cfgDrag >= 0) { CfgSave(g_cfg[g_cfgDrag]); g_cfgDrag = -1; g_cfgDirty = true; }
}
static void CfgMoveTo(float x, float y, bool vr) {
    if (x == g_cfgPx && y == g_cfgPy && vr == g_cfgPtrVr) return;
    g_cfgPx = x; g_cfgPy = y; g_cfgPtrVr = vr; g_cfgDirty = true;
    if (g_cfgDrag >= 0) { CfgSliderTo(g_cfgDrag, x); return; }
    const CfgHot* h = CfgHotAt(x, y);
    int item = h ? (h->part == HP_TAB ? h->item : h->item) : -1, part = h ? h->part : 0;
    if (h && h->part == HP_ROW) part = HP_ROW;
    if (item != g_cfgHoverItem || part != g_cfgHoverPart) { g_cfgHoverItem = item; g_cfgHoverPart = part; }
}

#ifndef FPC_TESTE_CONFIG
// ---------------------------------------------------------------- janela: mouse/teclado do monitor
static WNDPROC g_cfgOrigProc = nullptr;
static HWND g_cfgProcHwnd = nullptr;
static LRESULT CALLBACK CfgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETMINMAXINFO) {
        LRESULT r = CallWindowProcW(g_cfgOrigProc, h, m, w, l);
        if (g_gameResPreset) { MINMAXINFO* mi = (MINMAXINFO*)l; mi->ptMaxTrackSize.x = mi->ptMaxTrackSize.y = 16384; }
        return r;
    }
    if (g_cfgOpen) {
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP: {
            AcquireSRWLockExclusive(&g_cfgLock);
            g_cfgMouse.x = GET_X_LPARAM(l); g_cfgMouse.y = GET_Y_LPARAM(l); g_cfgMouse.moved = true;
            if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) g_cfgMouse.downs++;
            if (m == WM_LBUTTONUP) g_cfgMouse.ups++;
            ReleaseSRWLockExclusive(&g_cfgLock);
            return 0; }
        case WM_MOUSEWHEEL:
            AcquireSRWLockExclusive(&g_cfgLock); g_cfgMouse.wheel += GET_WHEEL_DELTA_WPARAM(w); ReleaseSRWLockExclusive(&g_cfgLock);
            return 0;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_MOUSEHWHEEL:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
            return 0;                                           // o jogo não recebe nada disso com o menu aberto
        }
    }
    return CallWindowProcW(g_cfgOrigProc, h, m, w, l);
}
static void CfgHookWindow() {
    if (g_cfgOrigProc || !g_gameHwnd) return;
    g_cfgProcHwnd = g_gameHwnd;
    g_cfgOrigProc = (WNDPROC)SetWindowLongPtrW(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)&CfgWndProc);
    Log("config: janela do jogo %p (procedimento original %p)", g_gameHwnd, g_cfgOrigProc);
}

// ---------------------------------------------------------------- abrir / fechar
static void CfgPlaceVr() {
    HmdPose hp; AcquireSRWLockShared(&g_hmdLock); hp = g_hmdPose; ReleaseSRWLockShared(&g_hmdLock);
    if (!hp.valid) return;
    float hf[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(hp.quat, f0, hf); }
    float l = std::sqrt(hf[0] * hf[0] + hf[2] * hf[2]); if (l < 1e-3f) { hf[0] = 0; hf[2] = -1; l = 1; }
    float pos[3] = { hp.pos[0] + hf[0] / l * 0.95f, hp.pos[1] - 0.08f, hp.pos[2] + hf[2] / l * 0.95f };
    float z[3] = { hp.pos[0] - pos[0], 0, hp.pos[2] - pos[2] }; V3Norm(z);
    const float wy[3] = { 0, 1, 0 };
    float x[3]; V3Cross(wy, z, x); V3Norm(x);
    float y[3]; V3Cross(z, x, y);
    XrPosef p; p.position = { pos[0], pos[1], pos[2] }; p.orientation = QuatFromAxes(x, y, z);
    AcquireSRWLockExclusive(&g_cfgLock); g_cfgPose = p; ReleaseSRWLockExclusive(&g_cfgLock);
}
static void CfgOpen(const char* why) {
    if (g_cfgOpen) return;
    CfgBuild();
    g_cfgOpen = true; g_inputMuted = true; g_blockGameCursor = true;
    g_cfgDrag = g_cfgCapKey = g_cfgCapBind = -1; g_cfgPx = g_cfgPy = -1;
    AcquireSRWLockExclusive(&g_cfgLock); g_cfgMouse = {}; ReleaseSRWLockExclusive(&g_cfgLock);
    CfgPlaceVr();
    g_cfgDirty = true;
    Log("config: menu aberto (%s)", why);
}
static void CfgClose(const char* why) {
    if (!g_cfgOpen) return;
    if (g_cfgDrag >= 0) CfgSave(g_cfg[g_cfgDrag]);
    g_cfgOpen = false; g_inputMuted = false; g_blockGameCursor = false;
    g_cfgDrag = g_cfgCapKey = g_cfgCapBind = -1;
    Log("config: menu fechado (%s)", why);
}

// Thread principal, a cada frame (com o resto do VR).
static void VrConfigUpdate() {
    CfgHookWindow();
    if (g_resPending && g_gameHwnd && g_eyeWidth) ApplyGameResolution();
    CtrlState raw[2]; AcquireSRWLockShared(&g_hmdLock); raw[0] = g_ctrlRaw[0]; raw[1] = g_ctrlRaw[1]; ReleaseSRWLockShared(&g_hmdLock);
    // ---- captura de tecla / botão
    if (g_cfgOpen && g_cfgCapKey >= 0) {
        for (int vk = 8; vk < 255; ++vk) {
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LBUTTON && vk <= VK_XBUTTON2)) continue;
            bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (d && !g_cfgCapPrev[vk]) {
                CfgItem& it = g_cfg[g_cfgCapKey];
                if (vk != VK_ESCAPE) { CfgSetValue(it, (float)vk, true); Log("config: %s = %s", it.key, KeyIni(vk).c_str()); }
                g_cfgCapKey = -1; g_cfgDirty = true;
                for (int k = 0; k < 256; ++k) g_cfgCapPrev[k] = (GetAsyncKeyState(k) & 0x8000) != 0;
                break;
            }
            g_cfgCapPrev[vk] = d;
        }
        if (GetTickCount64() - g_cfgCapT > 10000) { g_cfgCapKey = -1; g_cfgDirty = true; }
    }
    if (g_cfgOpen && g_cfgCapBind >= 0) {
        for (int i = 1; i < PI_COUNT; ++i) {
            float v = PhysValue(raw, i);
            if (v > 0.7f && g_cfgCapPhysPrev[i] <= 0.7f) {
                // o gatilho da mão do laser só vale depois de 0,4 s (é ele que clicou para começar a captura)
                if (i == (g_laserHand ? PI_RT : PI_LT) && GetTickCount64() - g_cfgCapT < 400) break;
                CfgItem& it = g_cfg[g_cfgCapBind];
                CfgSetValue(it, (float)i, true); Log("config: %s = %s", it.key, kPhysKey[i]);
                g_cfgCapBind = -1; g_cfgDirty = true;
                break;
            }
            g_cfgCapPhysPrev[i] = v;
        }
        if (g_cfgCapBind >= 0 && GetTickCount64() - g_cfgCapT > 10000) { g_cfgCapBind = -1; g_cfgDirty = true; }
    }
    // ---- abrir/fechar: tecla, ou segurar o botão físico ligado à ação "menu"
    {
        static bool keyWas = false;
        bool k = (GetAsyncKeyState(g_cfgKey) & 0x8000) != 0 && GameHasFocus();
        if (k && !keyWas && g_cfgCapKey < 0) { if (g_cfgOpen) CfgClose("tecla"); else CfgOpen("tecla"); }
        keyWas = k;
        static ULONGLONG holdT = 0; static bool fired = false;
        bool held = PhysValue(raw, g_bind[SL_MENU]) > 0.5f && g_vrHead;
        if (held) { if (!holdT) holdT = GetTickCount64(); if (!fired && GetTickCount64() - holdT >= 600) { fired = true; if (g_cfgOpen) CfgClose("botao de menu"); else CfgOpen("botao de menu (segurar)"); } }
        else { holdT = 0; fired = false; }
        if (g_cfgOpen && (GetAsyncKeyState(VK_ESCAPE) & 0x8000) && g_cfgCapKey < 0 && GameHasFocus()) CfgClose("Esc");
    }
    if (!g_cfgOpen) return;
    // ---- B (físico) fecha a captura ou o menu
    {
        static bool bWas = false; bool b = raw[1].b;
        if (b && !bWas) { if (g_cfgCapBind >= 0) { g_cfgCapBind = -1; g_cfgDirty = true; } else if (g_cfgCapKey < 0) CfgClose("botao B"); }
        bWas = b;
        if (!g_cfgOpen) return;
    }
    // ---- mouse do monitor
    CfgMouseQ mq; CfgMonRect mr;
    AcquireSRWLockExclusive(&g_cfgLock); mq = g_cfgMouse; g_cfgMouse.downs = g_cfgMouse.ups = 0; g_cfgMouse.wheel = 0; g_cfgMouse.moved = false; mr = g_cfgMon; ReleaseSRWLockExclusive(&g_cfgLock);
    if (mr.w > 0 && (mq.moved || mq.downs || mq.ups)) {
        float x = (mq.x - mr.x) * (float)CFG_W / mr.w, y = (mq.y - mr.y) * (float)CFG_H / mr.h;
        CfgMoveTo(x, y, false);
        for (int i = 0; i < mq.downs; ++i) CfgPress(x, y);
        if (mq.ups) CfgRelease();
    }
    if (mq.wheel) { g_cfgScroll[g_cfgTab] -= mq.wheel / WHEEL_DELTA; g_cfgDirty = true; }
    // ---- laser no VR (gatilho físico da mão do laser = clique; analógico = rolar)
    CfgVrHit vh; AcquireSRWLockShared(&g_cfgLock); vh = g_cfgVrHit; ReleaseSRWLockShared(&g_cfgLock);
    {
        const CtrlState& lc = raw[g_laserHand];
        static bool trigWas = false;
        bool trig = lc.trigger > (trigWas ? 0.4f : 0.7f);
        if (vh.valid) {
            float x = vh.u * CFG_W, y = vh.v * CFG_H;
            CfgMoveTo(x, y, true);
            if (trig && !trigWas) { CfgPress(x, y); XrHaptic(g_laserHand, 0.3f, 0.02f); }
        }
        if (!trig && trigWas) CfgRelease();
        trigWas = trig;
        static ULONGLONG lastScroll = 0;
        if (std::fabs(lc.sy) > 0.6f && GetTickCount64() - lastScroll > 180) { g_cfgScroll[g_cfgTab] += lc.sy > 0 ? -1 : 1; lastScroll = GetTickCount64(); g_cfgDirty = true; }
    }
    // ---- redesenha quando algo mudou (no máximo ~40 vezes por segundo)
    static ULONGLONG lastDraw = 0;
    ULONGLONG now = GetTickCount64();
    if (g_cfgDirty && now - lastDraw >= 25) { g_cfgDirty = false; lastDraw = now; CfgRender(); }
}

// ---------------------------------------------------------------- thread de render
static ID3D11Texture2D* g_cfgTex = nullptr; static ID3D11ShaderResourceView* g_cfgSRV = nullptr;
static XrSwapchain g_cfgSwap = XR_NULL_HANDLE; static std::vector<ID3D11Texture2D*> g_cfgImgs;
static XrCompositionLayerQuad g_cfgLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };
static bool CfgUpload() {
    if (!g_cfgTex) {
        D3D11_TEXTURE2D_DESC td = {}; td.Width = CFG_W; td.Height = CFG_H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        if (FAILED(g_gameDevice->CreateTexture2D(&td, nullptr, &g_cfgTex)) || FAILED(g_gameDevice->CreateShaderResourceView(g_cfgTex, &sv, &g_cfgSRV))) {
            Log("config: textura do menu FALHOU"); if (g_cfgTex) g_cfgTex->Release(); g_cfgTex = nullptr; return false;
        }
    }
    if (g_cfgPixDirty) {
        AcquireSRWLockExclusive(&g_cfgLock);
        if (g_cfgPixels.size() == (size_t)CFG_W * CFG_H) {
            ID3D11DeviceContext* ctx = nullptr; g_gameDevice->GetImmediateContext(&ctx);
            if (ctx) { ctx->UpdateSubresource(g_cfgTex, 0, nullptr, g_cfgPixels.data(), CFG_W * 4, 0); ctx->Release(); }
        }
        g_cfgPixDirty = false;
        ReleaseSRWLockExclusive(&g_cfgLock);
    }
    return true;
}
static int VrConfigLayers(const XrCompositionLayerBaseHeader** out) {
    CfgVrHit hit = {};
    int n = 0;
    if (g_cfgOpen && CfgUpload()) {
        DXGI_FORMAT fmt = PickSwapFormat(DXGI_FORMAT_R8G8B8A8_UNORM);
        if (g_cfgSwap == XR_NULL_HANDLE && !MakeSwap(g_cfgSwap, g_cfgImgs, CFG_W, CFG_H, fmt)) Log("config: camada do menu FALHOU");
        if (g_cfgSwap != XR_NULL_HANDLE && CopyIntoSwap(g_cfgSwap, g_cfgImgs, g_cfgTex, false, CFG_W, CFG_H)) {
            XrPosef p; AcquireSRWLockShared(&g_cfgLock); p = g_cfgPose; ReleaseSRWLockShared(&g_cfgLock);
            float w = g_cfgSize, h = g_cfgSize * CFG_H / CFG_W;
            g_cfgLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            g_cfgLayer.space = g_localSpace; g_cfgLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            g_cfgLayer.subImage.swapchain = g_cfgSwap;
            g_cfgLayer.subImage.imageRect = { { 0, 0 }, { CFG_W, CFG_H } };
            g_cfgLayer.pose = p; g_cfgLayer.size = { w, h };
            out[n++] = (const XrCompositionLayerBaseHeader*)&g_cfgLayer;
            CtrlState c; AcquireSRWLockShared(&g_hmdLock); c = g_ctrlRaw[g_laserHand]; ReleaseSRWLockShared(&g_hmdLock);
            if (c.aimValid) {
                float dir[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(&c.aim.orientation.x, f0, dir); }
                float t, u, v;
                if (HitQuad(p, w, h, &c.aim.position.x, dir, t, u, v)) { hit = { true, u, v }; g_laserHitLenXR = t; }
                else g_laserHitLenXR = 0.0f;
            }
        }
    }
    AcquireSRWLockExclusive(&g_cfgLock); g_cfgVrHit = hit; ReleaseSRWLockExclusive(&g_cfgLock);
    return n;
}
// Estéreo: desenha depois da cópia para os olhos (só no monitor). Tela virtual / jogo plano: antes (assim
// ele também entra na tela virtual do headset).
static void VrConfigDrawMonitor(IDXGISwapChain* sc);
static void VrConfigPresent(IDXGISwapChain* sc, bool afterVr) { if (afterVr == (bool)g_uiRedirect) VrConfigDrawMonitor(sc); }
// No Present, antes de mostrar a imagem no monitor: o menu por cima (centrado, até 92% da tela).
static void VrConfigDrawMonitor(IDXGISwapChain* sc) {
    if (!g_cfgOpen || !g_gameDevice || !CfgUpload() || !g_cfgSRV) return;
    DXGI_SWAP_CHAIN_DESC d; if (FAILED(sc->GetDesc(&d))) return;
    int bw = (int)d.BufferDesc.Width, bh = (int)d.BufferDesc.Height;
    float s = (std::min)(1.0f, (std::min)(0.92f * bw / CFG_W, 0.92f * bh / CFG_H));
    CfgMonRect r = { (int)((bw - CFG_W * s) / 2), (int)((bh - CFG_H * s) / 2), (int)(CFG_W * s), (int)(CFG_H * s) };
    // o mouse vem em coordenadas da janela; se o backbuffer tiver outro tamanho, converte
    RECT cr; if (g_gameHwnd && GetClientRect(g_gameHwnd, &cr) && cr.right > 0 && cr.bottom > 0) {
        float kx = (float)cr.right / bw, ky = (float)cr.bottom / bh;
        CfgMonRect m = { (int)(r.x * kx), (int)(r.y * ky), (int)(r.w * kx), (int)(r.h * ky) };
        AcquireSRWLockExclusive(&g_cfgLock); g_cfgMon = m; ReleaseSRWLockExclusive(&g_cfgLock);
    }
    BlitToBackbuffer(sc, g_cfgSRV, r.x, r.y, r.w, r.h);
}
#endif  // FPC_TESTE_CONFIG
