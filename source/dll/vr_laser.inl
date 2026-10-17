// vr_laser.inl: laser da mão direita (VR; a mão é configurável em laser_mao). Incluído por fpc_rig.cpp.
//
// Segurar o grip direito mostra o laser. O gatilho é o clique esquerdo.
//   - Laser no painel da mão esquerda ou na roda de opções aberta: o cursor vai para o ponto da interface.
//   - Laser no mundo (só com o modo FPS, INSERT): a câmera do jogo vai para o raio do laser (xr_ui.inl),
//     então o brilho e a seleção do próprio jogo (centro da câmera) seguem o laser; o gatilho clica.
//     A roda de opções aparece na ponta do laser; com ela aberta a câmera do jogo trava e o laser escolhe.
// Como funciona: o jogo não usa entrada "raw"; na primeira pessoa ele recentraliza o cursor do Windows
// (SetCursorPos) e escolhe o objeto sob o cursor. Enquanto o laser controla o cursor, as chamadas do
// jogo a SetCursorPos/ClipCursor são ignoradas (hook), e o cursor fica onde o laser aponta.
// Não usa Shift nem o botão direito (o botão direito liga/desliga o "olhar com o mouse" na FPC).

static bool  g_laserEnabled = true;     // laser no ini
static int   g_laserHand = 1;           // laser_mao: 1 = direita (padrão), 0 = esquerda
static float g_laserMaxDist = 4.0f;     // m, quando o raio não aponta para o chão
static float g_laserSmooth  = 25.0f;    // 1/s, suavização do cursor no mundo
static float g_laserPx = -1, g_laserPy = -1;
static volatile bool g_blockGameCursor = false;

// ---- hooks: o jogo não move/prende o cursor enquanto o laser manda nele
using SetCursorPos_t = BOOL (WINAPI*)(int, int);
using ClipCursor_t   = BOOL (WINAPI*)(const RECT*);
static SetCursorPos_t o_SetCursorPos = nullptr;
static ClipCursor_t   o_ClipCursor = nullptr;
static BOOL WINAPI hk_SetCursorPos(int x, int y) { if (g_blockGameCursor) return TRUE; return o_SetCursorPos(x, y); }
static BOOL WINAPI hk_ClipCursor(const RECT* r) { if (g_blockGameCursor && r) return TRUE; return o_ClipCursor(r); }
static BOOL MySetCursorPos(int x, int y) { return o_SetCursorPos ? o_SetCursorPos(x, y) : SetCursorPos(x, y); }
static void VrLaserInstallHooks() {
    HMODULE u = GetModuleHandleA("user32.dll");
    void* a = u ? (void*)GetProcAddress(u, "SetCursorPos") : nullptr;
    void* b = u ? (void*)GetProcAddress(u, "ClipCursor") : nullptr;
    MH_STATUS s1 = a ? MH_CreateHook(a, &hk_SetCursorPos, (void**)&o_SetCursorPos) : MH_ERROR_FUNCTION_NOT_FOUND;
    MH_STATUS s2 = b ? MH_CreateHook(b, &hk_ClipCursor, (void**)&o_ClipCursor) : MH_ERROR_FUNCTION_NOT_FOUND;
    Log("laser: hooks SetCursorPos=%d ClipCursor=%d", (int)s1, (int)s2);
}

static void SendMouseBtn(int btn, bool down) {
    INPUT in = {}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = btn == 2 ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP) : (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
    SendInput(1, &in, sizeof(in));
}

// ---- cursor virtual do jogo (VirtualCursor@ClientApp): a "bolinha" e o ponto de seleção na 1ª pessoa
// FUN_140154080(vc, x, y) grava a posição em vc+0xb0/+0xb4 e, se o cursor não estiver travado
// (vc+0x17c == 0), manda um "mouse movido" para o jogo. VC_LockSet (0x140153a10) salva a posição em
// vc+0x180/+0x184 ao travar e volta para ela ao destravar. O Canvas (vtable 0x141b22860, entrada 38)
// converte cliente -> interface com escala/deslocamento (canvas+0xb4/+0xb8, +0xbc/+0xc0 se canvas+0x9c).
// Antes, o laser movia o cursor do Windows, o cursor virtual ia junto, e o jogo salvava/voltava para
// essa posição fora do centro: a seleção ficava presa num canto. Agora, em VR, fora do controle do laser
// o cursor virtual fica sempre no centro, e ao travar/destravar a posição salva vira o centro.
static const char* SIG_VC_SETPOS   = "48 8B C4 53 57 48 83 EC 68 48 89 68 10 48 8B D9 48 89 70 18 41 8B E8 4C";
static const char* SIG_VC_LOCKSET  = "40 53 48 83 EC 40 48 C7 44 24 20 FE FF FF FF 44 0F B6 C2 48 8B D9 38 91";
static const char* SIG_CANVAS_LOCK = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 49 8B F1 48 8B 89";
using VcSetPos_t    = void (__fastcall*)(void* vc, int x, int y);
using VcLock_t      = void (__fastcall*)(void* vc, char lock, void* a3, void* a4);
using CanvasLock_t  = void (__fastcall*)(void* canvas, char lock, void* a3, int* outX, int* outY);
static VcSetPos_t   o_VcSetPos = nullptr;
static VcLock_t     o_VcLock = nullptr;
static CanvasLock_t o_CanvasLock = nullptr;
static void* volatile g_vc = nullptr;
static void* volatile g_canvas = nullptr;
static volatile bool g_vcFree = false;          // o laser está mandando no cursor (painel/roda): pode andar

static bool VcKeepCenter() { return g_vrEnabled && g_xrRunning && g_vrHead && g_phase == RUNNING && !g_vcFree; }
// Onde o cursor virtual deve ficar fora do controle do laser: no centro com o grip (laser = câmera do
// jogo) ou estacionado fora da tela sem o grip (o olhar não destaca nem seleciona nada).
static bool VcCenter(int& x, int& y) {
    RECT rc; if (!g_gameHwnd || !GetClientRect(g_gameHwnd, &rc) || rc.right <= 0) return false;
    x = -20000; y = -20000; return true;                      // em VR a escolha é pelo raycast próprio do laser
    float cx = rc.right * 0.5f, cy = rc.bottom * 0.5f;
    uint8_t* c = (uint8_t*)g_canvas;
    if (c && *(char*)(c + 0x9c)) { cx = cx * *(float*)(c + 0xb4) + *(float*)(c + 0xbc); cy = cy * *(float*)(c + 0xb8) + *(float*)(c + 0xc0); }
    x = (int)cx; y = (int)cy; return true;
}
static void __fastcall hk_VcSetPos(void* vc, int x, int y) {
    g_vc = vc;
    if (VcKeepCenter()) VcCenter(x, y);
    o_VcSetPos(vc, x, y);
}
static void __fastcall hk_VcLock(void* vc, char lock, void* a3, void* a4) {
    g_vc = vc;
    int cx, cy;
    bool keep = VcKeepCenter() && VcCenter(cx, cy);
    if (keep && !lock) { *(int*)((uint8_t*)vc + 0x180) = cx; *(int*)((uint8_t*)vc + 0x184) = cy; }   // destravar volta ao centro
    o_VcLock(vc, lock, a3, a4);
    if (keep && lock) {                                       // travar salva o centro (não o canto)
        *(int*)((uint8_t*)vc + 0x180) = cx; *(int*)((uint8_t*)vc + 0x184) = cy;
        *(int*)((uint8_t*)vc + 0xb0) = cx;  *(int*)((uint8_t*)vc + 0xb4) = cy;
    }
}
static void __fastcall hk_CanvasLock(void* canvas, char lock, void* a3, int* outX, int* outY) {
    g_canvas = canvas;
    o_CanvasLock(canvas, lock, a3, outX, outY);
}
static void VcRecenterNow() {
    int cx, cy;
    if (!g_vc || !o_VcSetPos || !VcCenter(cx, cy)) return;
    o_VcSetPos(g_vc, cx, cy);
    *(int*)((uint8_t*)g_vc + 0x180) = cx; *(int*)((uint8_t*)g_vc + 0x184) = cy;
}
// Canvas (dono do cursor virtual) pelo getter do jogo: o tratador ActionScript "SendVirtualCursorClick"
// (0x140d23840) é "call getter; call [canvas+0x240]".
static const char* SIG_AS_VCLICK = "48 83 EC 28 E8 ?? ?? ?? ?? 48 8B C8 48 8B 10 FF 92 40 02 00 00 B0 01";
using CanvasGet_t = void* (__fastcall*)();
static CanvasGet_t p_CanvasGet = nullptr;
static void* GetCanvas() { if (!g_canvas && p_CanvasGet) g_canvas = p_CanvasGet(); return g_canvas; }
static void VrCursorInstallHooks() {
    if (uint8_t* h = FindPattern(SIG_AS_VCLICK)) p_CanvasGet = (CanvasGet_t)(h + 9 + *(int32_t*)(h + 5));
    Log("cursor virtual: getter do Canvas=%p", p_CanvasGet);
    uint8_t* a = FindPattern(SIG_VC_SETPOS); uint8_t* b = FindPattern(SIG_VC_LOCKSET); uint8_t* c = FindPattern(SIG_CANVAS_LOCK);
    if (a) MH_CreateHook(a, &hk_VcSetPos, (void**)&o_VcSetPos);
    if (b) MH_CreateHook(b, &hk_VcLock, (void**)&o_VcLock);
    if (c) MH_CreateHook(c, &hk_CanvasLock, (void**)&o_CanvasLock);
    Log("cursor virtual: SetPos=%p LockSet=%p Canvas::Lock=%p", a, b, c);
}

// ---- Fase 3b: hover pelo raycast próprio + brilho nativo do jogo
// Brilho: o tratador "MouseOver" da interface (0x14022d580) faz FUN_14094d550(0x11 acende / 0x12 apaga,
// objeto_do_cliente, estilo 2 = MouseOver). Aqui fazemos o mesmo com o alvo do nosso raycast.
static const char* SIG_CLIENT_EVENT = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 63 D9 48 8D 05";
using ClientEvent_t = void (__fastcall*)(int evt, void* obj, uint64_t arg);
static ClientEvent_t p_ClientEvent = nullptr;
struct Hover { uint64_t id; int kind; V3 pt; float dist; ULONGLONG t; };   // kind: 0 nada, 1 objeto, 2 chão, 3 Sim
static Hover g_hover = {};
static volatile float g_laserHitLen = 0.0f;                   // distância do impacto (m) para o raio desenhado
static void* ClientObjectById(uint64_t id);
static void SafeClientEvent(int evt, void* obj, uint64_t arg) {
    __try { p_ClientEvent(evt, obj, arg); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void HoverHighlight(uint64_t id, bool on) {
    if (!id || !p_ClientEvent) return;
    void* obj = ClientObjectById(id);
    if (obj) SafeClientEvent(on ? 0x11 : 0x12, obj, 2);
}
static std::string ReadModsFile(const char* name);
static void VrHoverUpdate(bool want, const CtrlState& c) {
    static bool resolved = false;
    if (!resolved) { resolved = true; p_ClientEvent = (ClientEvent_t)FindPattern(SIG_CLIENT_EVENT); Log("laser: eventos do cliente (brilho) = %p", p_ClientEvent); }
    static ULONGLONG lastRay = 0;
    ULONGLONG now = GetTickCount64();
    if (!want) {
        if (g_hover.id) HoverHighlight(g_hover.id, false);
        g_hover = {}; g_laserHitLen = 0.0f; g_laserHitLenXR = 0.0f;
        return;
    }
    V3 ap; Q aq; CtrlWorld(c.aim, ap, aq);
    V3 dir = Norm(QRot(aq, v3(0, 0, -1)));
    if (now - lastRay >= 66) {                                // ~15 raios por segundo
        lastRay = now;
        SendServerCommand("fpc.ray %.4f %.4f %.4f %.5f %.5f %.5f", ap.x, ap.y, ap.z, dir.x, dir.y, dir.z);
    }
    if (g_rayReady) {
        g_rayReady = false;
        std::string s = ReadModsFile("fpc_hit.txt");
        unsigned long long id = 0; char kind[16] = {}; float x = 0, y = 0, z = 0;
        if (sscanf_s(s.c_str(), "%llu %15s %f %f %f", &id, kind, (unsigned)sizeof(kind), &x, &y, &z) == 5) {
            int k = strcmp(kind, "sim") == 0 ? 3 : strcmp(kind, "chao") == 0 ? 2 : strcmp(kind, "objeto") == 0 ? 1 : 0;
            if (id != g_hover.id) {
                if (g_hover.id) HoverHighlight(g_hover.id, false);
                if (id) { HoverHighlight(id, true); XrHaptic(g_laserHand, 0.08f, 0.015f); }
            }
            g_hover.id = id; g_hover.kind = k; g_hover.pt = v3(x, y, z); g_hover.t = now;
            g_hover.dist = Len(g_hover.pt - ap);
            g_laserHitLen = k ? g_hover.dist : 0.0f;
            g_laserHitLenXR = g_laserHitLen * (g_worldScale > 0.05f ? g_worldScale : 1.0f);
        }
    }
}

static void VrLaserUpdate() {
    static bool trigDown = false, active = false, bWas = false;
    static bool worldTrig = false;                            // um aperto do gatilho só vale uma vez (menu OU mundo)
    CtrlState c; AcquireSRWLockShared(&g_hmdLock); c = g_ctrl[g_laserHand]; ReleaseSRWLockShared(&g_hmdLock);
    bool vr = g_vrEnabled && g_xrRunning && g_vrHead && g_phase == RUNNING && g_laserEnabled && GameHasFocus();
    if (g_cfgOpen) {                                          // configurações abertas: o laser só aponta para o menu
        g_laserOn = vr; VrHoverUpdate(false, c); g_cursorForce = false;
        return;
    }
    // grip segurado = laser (histerese)
    if (!active && vr && c.squeeze > 0.5f && !g_handGrabbing[g_laserHand]) active = true;   // agarrar tem prioridade
    else if (active && (!vr || c.squeeze < 0.35f)) active = false;
    if (active && g_handGrabbing[g_laserHand]) active = false;   // a mão pegou algo: o laser some
    if (active != g_laserOn) Log("laser: %s", active ? "ativo (grip)" : "inativo");
    g_laserOn = active;
    if (c.b && !bWas && g_pieOpen) { g_pieCloseAt = GetTickCount64(); Log("ui: roda de opcoes fechada (botao B)"); }
    if (c.b && !bWas) VrMenuBack();                            // Fase 3: B volta/fecha o menu VR
    bWas = c.b;

    float px = 0, py = 0;
    bool control = false, onUi = false;
    float hu, hv;
    bool onMenu = false;
    float mu, mv;
    if (active && c.aimValid && VrMenuHitNow(mu, mv)) {        // Fase 3: laser no menu VR
        onMenu = true;
        VrMenuHover(mu, mv);
        static bool mTrig = false;
        if (!mTrig && c.trigger > 0.7f) { mTrig = true; worldTrig = true; VrMenuClick(); }   // o menu fecha: o mesmo aperto não reabre a roda
        else if (mTrig && c.trigger < 0.4f) mTrig = false;
    }
    if (active && c.aimValid && !onMenu) {
        if (UiHitNow(hu, hv)) {                                // painel da mão ou roda de opções
            RECT rc; GetClientRect(g_gameHwnd, &rc);
            px = hu * rc.right; py = hv * rc.bottom;
            g_laserPx = px; g_laserPy = py;
            control = true; onUi = true;
        }
    }
    // ---- câmera do jogo: travada (roda aberta), no laser (mundo, com INSERT) ou na cabeça
    {
        int mode = GCAM_HEAD;
        if (g_pieLockPending) {                                 // a roda acabou de abrir: trava onde está
            g_pieLockPending = false;
            if (g_gameCamMode == GCAM_HEAD && g_vrHead) {       // estava na cabeça: trava a pose da cabeça
                memcpy(g_gameCamW[0], g_centerCam, 16); memcpy(g_gameCamW[1], g_centerFwd, 16); memcpy(g_gameCamW[2], g_centerUp, 16);
                HmdPose hp; AcquireSRWLockShared(&g_hmdLock); hp = g_hmdPose; ReleaseSRWLockShared(&g_hmdLock);
                XrPosef xp; xp.position = { hp.pos[0], hp.pos[1], hp.pos[2] }; xp.orientation = { hp.quat[0], hp.quat[1], hp.quat[2], hp.quat[3] };
                AcquireSRWLockExclusive(&g_gameCamLock); g_gameCamXR = xp; ReleaseSRWLockExclusive(&g_gameCamLock);
            }
        }
        if (g_pieOpen && vr) mode = GCAM_LOCKED;
        else if (false) {                                      // (antigo: câmera do jogo no laser; substituído pelo raycast)
            mode = GCAM_LASER;
            V3 ap; Q aq; CtrlWorld(c.aim, ap, aq);
            V3 f = Norm(QRot(aq, v3(0, 0, -1)));
            V3 up = v3(0, 1, 0); if (std::fabs(f.y) > 0.98f) up = Norm(QRot(aq, v3(0, 1, 0)));
            V3 r = Norm(Cross(f, up)); V3 u = Cross(r, f);
            float pw[4] = { ap.x, ap.y, ap.z, 1.0f }, fw[4] = { f.x, f.y, f.z, 0.0f }, uw[4] = { u.x, u.y, u.z, 0.0f };
            memcpy(g_gameCamW[0], pw, 16); memcpy(g_gameCamW[1], fw, 16); memcpy(g_gameCamW[2], uw, 16);
            // a mesma pose no espaço do headset (sem rolar, como no mundo)
            const float* q = &c.aim.orientation.x;
            float fx[3]; { const float f0[3] = { 0, 0, -1 }; QuatRotateXR(q, f0, fx); }
            float ux[3] = { 0, 1, 0 }; if (std::fabs(fx[1]) > 0.98f) { const float y0[3] = { 0, 1, 0 }; QuatRotateXR(q, y0, ux); }
            float zx[3] = { -fx[0], -fx[1], -fx[2] };
            float xx[3]; V3Cross(ux, zx, xx); V3Norm(xx);
            float yx[3]; V3Cross(zx, xx, yx);
            XrPosef xp; xp.position = c.aim.position; xp.orientation = QuatFromAxes(xx, yx, zx);
            AcquireSRWLockExclusive(&g_gameCamLock); g_gameCamXR = xp; ReleaseSRWLockExclusive(&g_gameCamLock);
        }
        if (mode != g_gameCamMode) Log("camera do jogo: %s", mode == GCAM_LASER ? "no laser" : mode == GCAM_LOCKED ? "travada (roda aberta)" : "na cabeca");
        g_gameCamMode = mode;
    }
    // ---- Fase 3b: raycast próprio do laser (Python testa o raio 3D contra Sims/objetos/chão)
    VrHoverUpdate(active && c.aimValid && !onUi && !onMenu && g_walkMode, c);
    g_blockGameCursor = control;
    g_cursorForce = control && onUi;                          // desenhar o cursor no painel mesmo se o jogo o esconde
    static bool hadControl = false;
    auto centerCursor = [&]() {
        RECT rc; if (!g_gameHwnd || !GetClientRect(g_gameHwnd, &rc)) return;
        POINT c0 = { rc.right / 2, rc.bottom / 2 }; ClientToScreen(g_gameHwnd, &c0); MySetCursorPos(c0.x, c0.y);
    };
    if (!control) {
        g_laserPx = -1;
        // O jogo escolhe pelo cursor (que ele mantém no centro). Ao soltar o cursor, devolve ao centro;
        // com a câmera do jogo no laser, mantém no centro (senão a seleção sai do raio do laser).
        if (hadControl) { centerCursor(); g_vcFree = false; VcRecenterNow(); hadControl = false; Log("cursor virtual: de volta ao centro"); }
        g_vcFree = false;
        static bool wasOn = false;                            // grip ligou/desligou: centro <-> estacionado
        if (g_laserOn != wasOn) { wasOn = g_laserOn; VcRecenterNow(); }
        // laser no mundo: o jogo escolhe pelo centro da câmera dele (que está no laser); só clicar
        // Gatilho com um alvo do raycast: o servidor monta as opções dele e elas vão para o menu VR.
        if (active && !onMenu && !worldTrig && c.trigger > 0.7f) {
            worldTrig = true;
            if (g_hover.id || g_hover.kind == 2) {
                const char* kinds[] = { "nada", "objeto", "chao", "sim" };
                SendServerCommand("fpc.choices %llu %s %.4f %.4f %.4f", (unsigned long long)g_hover.id, kinds[g_hover.kind], g_hover.pt.x, g_hover.pt.y, g_hover.pt.z);
                XrHaptic(g_laserHand, 0.4f, 0.03f);
                Log("laser: opcoes de %s %llu", kinds[g_hover.kind], (unsigned long long)g_hover.id);
            }
        } else if (worldTrig && c.trigger < 0.4f) worldTrig = false;
        if (trigDown) { SendMouseBtn(1, false); trigDown = false; }
        return;
    }
    hadControl = true;
    g_vcFree = true;                                          // laser no painel/roda: o cursor virtual acompanha
    POINT pt = { (LONG)g_laserPx, (LONG)g_laserPy };
    ClientToScreen(g_gameHwnd, &pt);
    MySetCursorPos(pt.x, pt.y);

    // gatilho = clique esquerdo (histerese)
    if (!trigDown && c.trigger > 0.7f) {
        SendMouseBtn(1, true); trigDown = true;
        bool pie = onUi && UiHitIsPie();
        if (pie) g_pieCloseAt = GetTickCount64() + 800;        // clicou na roda: ela fecha (se abrir outra, continua)
        Log("laser: clique %s em (%ld, %ld)", pie ? "na roda" : onUi ? "no painel" : "no mundo (centro da camera do jogo)", pt.x, pt.y);
    } else if (trigDown && c.trigger < 0.4f) { SendMouseBtn(1, false); trigDown = false; }
}

static void VrLaserLoadConfig(const char* path) {
    char f[32];
    g_laserEnabled = GetPrivateProfileIntA("fpc_rig", "laser", 1, path) != 0;
    GetPrivateProfileStringA("fpc_rig", "laser_mao", "DIREITA", f, sizeof(f), path); g_laserHand = (_stricmp(f, "ESQUERDA") == 0) ? 0 : 1;
    GetPrivateProfileStringA("fpc_rig", "laser_distancia", "4.0", f, sizeof(f), path); g_laserMaxDist = (float)atof(f);
    Log("config: laser=%d laser_mao=%s laser_distancia=%.1f", g_laserEnabled, g_laserHand ? "DIREITA" : "ESQUERDA", g_laserMaxDist);
}

