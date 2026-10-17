// fpc_rig.cpp v9: câmera em primeira pessoa estilo FPS para o TS4 (fase 1, monitor).
//
// NÃO TESTADO além do v2. Baseado na análise estática do TS4_x64.exe 1.118.257.1020
// (veja MAPA_CAMERA.md). As funções são localizadas por assinatura (AOB).
//
// O problema do design original (confirmado no código):
//   - a ROTAÇÃO da câmera vem do mouse (Impl+0x1d0), aplicada por Look_ApplyEachFrame;
//   - a POSIÇÃO vem do osso da cabeça; a cabeça gira para o lado em que você olha,
//     então a câmera orbita o pescoço e pula quando a cabeça bate no limite.
//
// O que o v3 faz:
//   1. eye_weight = 0 todo frame (a rotação não é puxada para os olhos).
//   2. Na ativação da FPC, troca o osso de fixação para b__Spine2__ (peito), usando
//      as próprias funções do jogo (as mesmas do comando camera.fpc.set_joint).
//   3. Calibra a altura dos olhos: (posição da cabeça) − (posição do peito), só no eixo Y.
//   4. Posição final = peito + altura dos olhos (+ um pequeno avanço na direção do olhar),
//      filtrada. Ela não depende mais da rotação da cabeça.
//
// Teclas: F7 log de 10 s · F8 congela posição · F9 eye_weight 0/1 · F10 filtro on/off
//         F11 recalibrar a altura dos olhos · modo FPS = tecla de fpc_rig.ini (padrao INSERT)
//
// Dependência: MinHook (embutido em minhook\). Compile com build.bat.

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include "MinHook.h"

// ---------------------------------------------------------------- configuração
static const char* g_anchorJoint    = "b__Spine2__";
static float g_smoothHorizontal     = 25.0f;  // 1/s; maior = segue o peito mais de perto
static float g_smoothVertical       = 6.0f;   // 1/s; menor = menos balanço vertical
static float g_forwardOffset        = 0.06f;  // metros na direção do olhar (evita ver o pescoço)
static float g_eyeUpMin = 0.05f, g_eyeUpMax = 0.70f;   // limites da calibração (metros)
static float g_eyeUpFallback        = 0.35f;  // usado se a calibração sair fora dos limites
static const int UP = 1;                      // eixo vertical: Y (confirmado no log do teste 1)

// ---------------------------------------------------------------- assinaturas (assinaturas_AOB.txt)
static const char* SIG_FPS_UPDATE    = "48 8B C4 48 89 58 08 57 48 83 EC 70 0F 28 05 ?? ?? ?? ??";
static const char* SIG_SET_POSE      = "48 8B C4 53 48 81 EC 90 00 00 00 41 0F 10 00 48 8B D9";
static const char* SIG_FPS_SETPARAM  = "83 FA 06 0F 87 ?? ?? ?? ?? 48 63 C2 4C 8D 05 ?? ?? ?? ??";
static const char* SIG_SIMPLE_MODE   = "48 89 5C 24 08 57 48 83 EC 20 48 8B D9 84 D2 74 ?? F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 05";
static const char* SIG_SET_JOINT     = "4C 8B DC 53 48 81 EC 90 00 00 00 48 8B 59 28 48 8B CA";
static const char* SIG_SET_OFFSETS   = "4C 8B DC 48 81 EC 98 00 00 00 48 8B 49 28 48 8B 81 F0 00 00 00 F3 0F 10 81 48 01 00 00";
static const char* SIG_LOOK_UP       = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 0F 28 05 ?? ?? ?? ?? 0F 57 C9";
static const char* SIG_SERVER_CONN   = "48 83 EC 28 E8 ?? ?? ?? ?? 48 8B 80 98 00 00 00";
static const char* SIG_SERVER_CMD    = "40 57 48 81 EC C0 00 00 00 48 C7 44 24 40 FE FF FF FF";
static const char* SIG_SET_OPACITY   = "48 83 EC 38 45 33 C9 F3 0F 11 5C 24 20 E8 ?? ?? ?? ?? 48 83 C4 38 C3";
static const char* SIG_GET_COMPONENT = "8B 41 10 4C 8B C1 85 C2 74 ?? FF CA 23 D0 8B C2";
static const char* SIG_CLIENT_OBJMGR = "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 8B 02 48 89 41 08 48 8B C1";
static const char* SIG_FPS_SIM_VALID = "40 53 48 83 EC 30 48 85 C9 74 ?? 8B 15 ?? ?? ?? ??";
static const char* SIG_FPS_DEACTIVATE = "48 89 5C 24 08 57 48 83 EC 20 48 8B 59 28 48 8B F9 48 8B CB 33 D2 E8 ?? ?? ?? ??";

// ---------------------------------------------------------------- modo FPS (andar com WASD)
// Liga/desliga com g_walkToggleKey. Precisa do script mod fpc_walk.ts4script na pasta Mods.
static int   g_walkToggleKey   = VK_INSERT; // padrão; troque em fpc_rig.ini (tecla_modo_fps=...)
static int   g_hideBodyKey     = VK_END;    // padrão; troque em fpc_rig.ini (tecla_esconder_corpo=...)
static bool  g_bodyHidden      = false;
static int   g_runKey          = VK_LMENU;  // Alt esquerdo; troque em fpc_rig.ini (tecla_correr=...)
static HMODULE g_self          = nullptr;
static float g_walkResend      = 0.30f;   // s entre comandos de andar com a tecla segurada
static float g_walkTurnResend  = 0.26f;   // rad (~15°): mudou a direção, reenvia na hora
static float g_faceInterval    = 0.25f;   // s entre comandos de virar o corpo
static float g_faceMinAngle    = 0.26f;   // rad (~15°)
static float g_strafeSign      = 1.0f;    // se A e D estiverem invertidos, troque para -1
// Locomoção: 1 = pela ROTA do jogo (fpc.walk: o Sim anda com animação, contorna objetos, passa por portas
// e escadas; a câmera segue o corpo). 0 = antiga (teleporte em passos + câmera à frente).
static bool  g_routeWalk       = false;   // locomocao_rota (padrão: antiga)
static float g_stickDead       = 0.20f;   // zona morta dos analógicos do VR

// ---------------------------------------------------------------- tipos (x64 MS ABI)
using FpsUpdate_t   = void (__fastcall*)(void* ctrl, float dt, char flag);
using SetPose_t     = void (__fastcall*)(void* cam, const float* pos, const float* up, const float* fwd);
using FpsSetParam_t = void (__fastcall*)(void* ctrl, int index, float value);
using SetJoint_t    = void (__fastcall*)(void* ctrl, const char* jointName);
using SetOffsets_t  = void (__fastcall*)(void* ctrl, const float* offset, const float* up, const float* fwd);
using LookUp_t      = void (__fastcall*)(void* look);          // look = Impl+0x1a0

using ServerConn_t  = void* (__fastcall*)();
using ServerCmd_t   = void (__fastcall*)(void* conn, const char* command);

static ServerConn_t  p_ServerConn  = nullptr;
static ServerCmd_t   p_ServerCmd   = nullptr;
static LookUp_t      o_LookUp      = nullptr;
static uint8_t*      g_impl        = nullptr;                   // CameraControllerFPSImpl
static float g_maxPitchSin         = 0.97f;                     // limita o pitch (~76°) para não virar

static FpsUpdate_t   o_FpsUpdate   = nullptr;
static SetPose_t     o_SetPose     = nullptr;
static FpsSetParam_t p_FpsSetParam = nullptr;
static SetJoint_t    p_SetJoint    = nullptr;
static SetOffsets_t  p_SetOffsets  = nullptr;
static float*        p_eyeWeight   = nullptr;

// ---------------------------------------------------------------- estado
enum Phase { CAPTURE_HEAD, SWITCHED_WAIT, CAPTURE_ANCHOR, RUNNING };
static Phase  g_phase        = CAPTURE_HEAD;
static void*  g_lastCtrl     = nullptr;
static void*  g_fpsCam       = nullptr;
static thread_local bool t_inFpsUpdate = false;
static int    g_poseCalls    = 0;
static float  g_dt           = 1.0f / 60.0f;
static float  g_headPos[4]   = {};
static float  g_eyeUp        = 0.35f;
static float  g_out[4]       = {};        // posição final deste frame
static float  g_filtered[4]  = {};
static bool   g_haveFiltered = false;
static bool   g_freeze       = false;
static float  g_frozen[4]    = {};
static float  g_eyeTarget    = 0.0f;
static bool   g_filterOn     = true;
static int    g_burstFrames  = 0;
static unsigned g_frame      = 0;
static FILE*  g_log          = nullptr;

// ---------------------------------------------------------------- utilitários
static uint8_t* FindPattern(const char* sig) {
    std::vector<int> pat;
    for (const char* p = sig; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        pat.push_back((int)strtoul(p, nullptr, 16)); p += 2;
    }
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* start = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        for (size_t i = 0; i + pat.size() <= size; ++i) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || start[i + k] == (uint8_t)pat[k])) ++k;
            if (k == pat.size()) return start + i;
        }
    }
    return nullptr;
}

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static volatile bool g_cfgOpen = false;                       // menu de configurações aberto (vr_config.inl)
static bool Pressed(int vk) {                                 // borda de descida
    static bool down[256] = {};
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0 && !g_cfgOpen;   // com as configurações abertas, as teclas são delas
    bool edge = now && !down[vk];
    down[vk] = now;
    return edge;
}

static void SwitchToAnchor(void* ctrl) {
    if (!p_SetJoint || !p_SetOffsets) return;
    uint8_t* impl = *(uint8_t**)((uint8_t*)ctrl + 0x28);
    alignas(16) float zero[4] = { 0, 0, 0, 0 };
    alignas(16) float up[4], fwd[4];                          // mantém os vetores locais atuais
    memcpy(up, impl + 0x120, 16);
    memcpy(fwd, impl + 0x130, 16);
    p_SetJoint(ctrl, g_anchorJoint);
    p_SetOffsets(ctrl, zero, up, fwd);
    if (p_FpsSetParam) p_FpsSetParam(ctrl, 0, 0.0f);         // idletime = 0: sem retorno para a frente
    Log("osso de fixacao -> %s; idletime=0", g_anchorJoint);
}

// Direção do olhar = estado do mouse (Impl+0x1d0), normalizada e com pitch limitado.
// O limite é gravado de volta para o mouse não "acumular" além do limite.
static bool MouseLookDir(float out[4]) {
    if (!g_impl) return false;
    float* look = (float*)(g_impl + 0x1d0);
    float x = look[0], y = look[1], z = look[2];
    float len = std::sqrt(x * x + y * y + z * z);
    if (len < 1e-4f) return false;
    x /= len; y /= len; z /= len;
    if (std::fabs(y) > g_maxPitchSin) {
        float h = std::sqrt(x * x + z * z);
        float s = std::sqrt(1.0f - g_maxPitchSin * g_maxPitchSin) / (h > 1e-4f ? h : 1.0f);
        x *= s; z *= s; y = (y > 0 ? g_maxPitchSin : -g_maxPitchSin);
        if (h <= 1e-4f) { x = 0; z = 1; }
        look[0] = x; look[1] = y; look[2] = z;
    }
    out[0] = x; out[1] = y; out[2] = z; out[3] = 0.0f;
    return true;
}

// O jogo recalcula o "up" do olhar a partir da rotação do osso; trocamos pelo up do mundo,
// assim o mouse gira em volta de um eixo vertical e a câmera não inclina com o tronco.
static void __fastcall hk_LookUp(void* look) {
    o_LookUp(look);
    float* up = (float*)((uint8_t*)look + 0x40);
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f; up[3] = 0.0f;
}

// ---------------------------------------------------------------- modo FPS: estado
// LIVRE:     a câmera é do jogador. A origem do rig anda suave no cliente (WASD) e o
//            servidor (fpc_walk.ts4script) move o Sim atrás, validando no navmesh.
// OCUPADO:   o Sim está numa interação; a câmera segue o corpo (peito + altura dos olhos).
// O estado vem do Python via "camera.fpc.idletime -1000/-1001" (interceptado em hk_FpsSetParam).
static bool  g_walkMode    = false;
static bool  g_busy        = true;        // até o Python dizer que está livre
static bool  g_haveOrigin  = false;
static float g_origin[2]   = {};          // origem do rig (x, z) no mundo
static float g_anchor[4]   = {};          // posição do peito neste frame (1ª chamada do SetPose)
static float g_groundY     = 0.0f;        // altura do peito filtrada (só vertical)
static bool  g_moving      = false;
static float g_moveTimer   = 0.0f, g_faceTimer = 0.0f, g_blockedTimer = 0.0f;
static float g_lastFaceDir[2] = {};

static float g_walkSpeed   = 1.6f;        // m/s andando
static float g_runSpeed    = 3.6f;        // m/s correndo (Shift)
static float g_maxLead     = 0.80f;       // m: a partir daqui a câmera desacelera (coleira elástica)
static float g_hardLead    = 1.00f;       // m: limite; acima disso a câmera volta suave até ele (sem pulo)
// Sonda da câmera: o Python testa a linha Sim -> câmera no navmesh (fpc.cam, ~10/s). Se cruzar uma
// parede/objeto, a câmera (incluindo o deslocamento da cabeça no VR) é puxada suave para perto do corpo.
static bool  g_camProbeOn  = true;        // sonda_camera no ini
static volatile bool g_camBlocked = false;
static float g_camPull     = 0.0f;        // 0 = câmera livre, 1 = puxada até g_camKeep do corpo
static float g_camKeep     = 0.10f;       // m
static float g_camWanted[2] = {};         // posição da câmera antes do puxão (o que a sonda testa)
static float g_probeTimer  = 0.0f;
static float g_moveInterval = 1.0f / 120.0f;  // s entre "fpc.move" (envios_por_segundo, padrão 120)
static float g_turnRate     = 10.0f;      // giro_corpo_velocidade (1/s): o quão rápido o corpo alcança o olhar
static float g_turnMaxDeg   = 360.0f;     // graus/s, limite do giro do corpo
static float g_bodyDir[2]   = { 0.0f, 0.0f };   // direção (suave) do corpo, enviada ao servidor
static float g_driftSnap   = 0.40f;       // m: parado, se o corpo estiver mais longe que isso, a câmera vai até ele

static bool FpsFree() { return g_walkMode && !g_busy && g_phase == RUNNING; }

// ---------------------------------------------------------------- bolha de snap (conforto)
// Com o modo FPS ligado e o Sim OCUPADO (interação, porta, escada): a animação roda, mas a
// câmera fica parada dentro de uma bolha em volta do ponto dos olhos do corpo. Só quando esse
// ponto sai da bolha a câmera dá um snap (pulo instantâneo) e a bolha recentra.
// A rotação nunca segue a animação (já é assim: vem só do mouse / headset).
static bool  g_bubbleOn      = true;
static float g_bubbleRadiusH = 0.30f;     // m
static float g_bubbleRadiusV = 0.20f;     // m
static float g_bubbleMinGap  = 0.25f;     // s mínimo entre snaps
static int   g_bubbleMaxSnaps = 3;        // mais que isso em 2 s -> modo de reserva (segue suave)
static bool  g_bubbleActive  = false;
static float g_bubbleCam[3]  = {};
static float g_bubbleSinceSnap = 0.0f, g_bubbleCalm = 0.0f;
static float g_snapTimes[8]  = {};
static int   g_snapHead      = 0;
static bool  g_bubbleFallback = false;
static bool  g_cameFromBubble = false;

static int RecentSnaps(float now) {
    int n = 0;
    for (float t : g_snapTimes) if (t > 0.0f && now - t <= 2.0f) ++n;
    return n;
}

// target = ponto dos olhos do corpo neste frame. Escreve a posição da câmera em out[0..2].
static void BubbleUpdate(const float* target, float* out) {
    static float clock = 0.0f;
    clock += g_dt; g_bubbleSinceSnap += g_dt;
    if (!g_bubbleActive) {                                  // entrou agora: começa onde a câmera estava
        if (!g_haveFiltered) { g_bubbleCam[0] = target[0]; g_bubbleCam[1] = target[1]; g_bubbleCam[2] = target[2]; }
        else { g_bubbleCam[0] = g_out[0]; g_bubbleCam[1] = g_out[1]; g_bubbleCam[2] = g_out[2]; }
        g_bubbleActive = true; g_bubbleFallback = false; g_bubbleSinceSnap = 1.0f; g_bubbleCalm = 0.0f;
    }
    float dx = target[0] - g_bubbleCam[0], dz = target[2] - g_bubbleCam[2], dy = target[UP] - g_bubbleCam[UP];
    bool outside = std::sqrt(dx * dx + dz * dz) > g_bubbleRadiusH || std::fabs(dy) > g_bubbleRadiusV;

    if (g_bubbleFallback) {                                 // animação agitada demais: segue suave
        float a = 1.0f - std::exp(-6.0f * g_dt);
        for (int i = 0; i < 3; ++i) g_bubbleCam[i] += (target[i] - g_bubbleCam[i]) * a;
        g_bubbleCalm = outside ? 0.0f : g_bubbleCalm + g_dt;
        if (g_bubbleCalm > 1.0f) { g_bubbleFallback = false; Log("bolha: animacao acalmou, voltando aos snaps"); }
    } else if (outside && g_bubbleSinceSnap >= g_bubbleMinGap) {
        g_bubbleCam[0] = target[0]; g_bubbleCam[1] = target[1]; g_bubbleCam[2] = target[2];
        g_bubbleSinceSnap = 0.0f;
        g_snapTimes[g_snapHead] = clock; g_snapHead = (g_snapHead + 1) % 8;
        if (g_burstFrames > 0) Log("bolha: snap (%.2f m)", std::sqrt(dx * dx + dz * dz + dy * dy));
        if (RecentSnaps(clock) > g_bubbleMaxSnaps) {
            g_bubbleFallback = true; g_bubbleCalm = 0.0f;
            Log("bolha: %d snaps em 2 s -> modo de reserva (segue suave)", RecentSnaps(clock));
        }
    }
    out[0] = g_bubbleCam[0]; out[1] = g_bubbleCam[1]; out[2] = g_bubbleCam[2];
}

// Calcula a posição final uma vez por frame (na 1ª chamada do SetPose).
static void ComputeOutput(const float* raw, const float* fwd) {
    memcpy(g_anchor, raw, 16);
    float target[4] = { raw[0], raw[1], raw[2], raw[3] };
    float fx = fwd[0], fz = fwd[2], len = std::sqrt(fx * fx + fz * fz);
    if (len > 1e-4f) { fx /= len; fz /= len; } else { fx = 0; fz = 0; }

    if (FpsFree()) {
        // Câmera do jogador: horizontal = origem do rig (já suave), vertical = peito bem filtrado.
        if (!g_haveOrigin) {
            if (g_cameFromBubble) {                             // saindo da bolha: começa onde a câmera está
                g_origin[0] = g_out[0] - fx * g_forwardOffset; g_origin[1] = g_out[2] - fz * g_forwardOffset;
                g_groundY = g_out[UP] - g_eyeUp;
            } else { g_origin[0] = raw[0]; g_origin[1] = raw[2]; g_groundY = raw[UP]; }
            g_haveOrigin = true;
        }
        g_bubbleActive = false; g_cameFromBubble = false;
        g_groundY += (raw[UP] - g_groundY) * (1.0f - std::exp(-g_smoothVertical * g_dt));
        g_out[0] = g_origin[0] + fx * g_forwardOffset;
        g_out[2] = g_origin[1] + fz * g_forwardOffset;
        g_out[UP] = g_groundY + g_eyeUp;
        g_out[3] = raw[3];
        memcpy(g_filtered, g_out, 16);                         // transição suave ao sair do modo livre
        if (g_freeze) memcpy(g_out, g_frozen, 16);
        return;
    }

    g_haveOrigin = false;
    g_groundY = raw[UP];
    if (g_phase == RUNNING) {
        target[UP] += g_eyeUp;
        target[0] += fx * g_forwardOffset; target[2] += fz * g_forwardOffset;
    }
    if (g_bubbleOn && g_walkMode && g_phase == RUNNING) {    // ocupado com o modo FPS: bolha de snap
        BubbleUpdate(target, g_out);
        g_out[3] = target[3];
        memcpy(g_filtered, g_out, 16); g_haveFiltered = true;
        g_cameFromBubble = true;
        if (g_freeze) memcpy(g_out, g_frozen, 16);
        return;
    }
    g_bubbleActive = false;
    if (!g_haveFiltered) { memcpy(g_filtered, target, 16); g_haveFiltered = true; }
    for (int i = 0; i < 3; ++i) {
        float k = (i == UP) ? g_smoothVertical : g_smoothHorizontal;
        float a = 1.0f - std::exp(-k * g_dt);
        g_filtered[i] += (target[i] - g_filtered[i]) * a;
    }
    g_filtered[3] = target[3];
    const float* use = g_freeze ? g_frozen : (g_filterOn ? g_filtered : target);
    memcpy(g_out, use, 16);
}

// ---------------------------------------------------------------- modo FPS: ponte para o Python
static bool GameHasFocus() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// ---------------------------------------------------------------- VR (fases V0 e V1)
#include "xr_session.inl"
static bool AvatarServerXZ(float& x, float& z);   // vr_avatar.inl

// Envia um comando para a simulação (Python), igual a digitar "|comando" no console.
static void SendServerCommand(const char* fmt, ...) {
    if (!p_ServerConn || !p_ServerCmd) return;
    char buf[160];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    void* conn = p_ServerConn();
    if (!conn) { Log("sem conexao com o servidor: %s", buf); return; }
    p_ServerCmd(conn, buf);
    if (g_burstFrames > 0) Log("-> %s", buf);
}

// Comandos que o cliente manda para a simulação passam por aqui (os nossos e os do jogo). A roda de
// opções é pedida com "interactions.choices ..." e a escolha volta com "interactions.select ...":
// a roda é mostrada no mundo (VR) não importa se o clique veio do laser, do mouse ou do centro da visão.
static ServerCmd_t o_ServerCmd = nullptr;
static volatile bool g_pieFromGame = false, g_pieSelectFromGame = false;
static void __fastcall hk_ServerCmd(void* conn, const char* cmd) {
    if (cmd) {
        if (strncmp(cmd, "interactions.choices", 20) == 0) g_pieFromGame = true;
        else if (strncmp(cmd, "interactions.select", 19) == 0) g_pieSelectFromGame = true;
        static int logged = 0;
        if (strncmp(cmd, "interactions.", 13) == 0 && logged++ < 20) Log("jogo -> simulacao: %.120s", cmd);
    }
    o_ServerCmd(conn, cmd);
}

static float AngleBetween2D(const float a[2], const float b[2]) {
    float d = a[0] * b[0] + a[1] * b[1];
    d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
    return std::acos(d);
}

// Roda depois de cada frame da FPC, com o modo FPS ligado.
static void UpdateFpsMode() {
    g_motionWalk = 0.0f;                                       // (recalculado abaixo se estiver andando)
    float look[4];
    if (!GetViewDir(look)) return;
    float fx = look[0], fz = look[2], len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-4f) return;
    fx /= len; fz /= len;
    static LARGE_INTEGER qpf = {}, last = {};
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float mdt = last.QuadPart ? (float)((double)(now.QuadPart - last.QuadPart) / (double)qpf.QuadPart) : g_dt;
    last = now;
    if (mdt <= 0.0f || mdt > 0.1f) mdt = g_dt;
    g_moveTimer += mdt; g_faceTimer += mdt;
    if (g_camProbeOn && FpsFree()) {
        g_probeTimer += mdt;
        if (g_probeTimer >= 0.1f) { g_probeTimer = 0.0f; SendServerCommand("fpc.cam %.4f %.4f", g_camWanted[0], g_camWanted[1]); }
    } else g_camBlocked = false;
    if (g_blockedTimer > 0.0f) g_blockedTimer -= mdt;

    // Ocupado: só mantém o Python atualizando o estado (ele avisa quando ficar livre).
    if (!FpsFree()) {
        if (g_faceTimer >= 0.5f) { SendServerCommand("fpc.face %.4f %.4f", fx, fz); g_faceTimer = 0.0f; }
        g_moving = false;
        return;
    }

    float rx = -fz * g_strafeSign, rz = fx * g_strafeSign;   // direita = cross(frente, up) [I]
    bool focus = GameHasFocus();
    auto down = [&](int vk) { return focus && !g_cfgOpen && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    float f = (down('W') ? 1.0f : 0.0f) - (down('S') ? 1.0f : 0.0f);
    float r = (down('D') ? 1.0f : 0.0f) - (down('A') ? 1.0f : 0.0f);
    // VR: analógico esquerdo = andar (proporcional), clique nele = alterna correr.
    float stickMag = 0.0f;
    static bool stickRun = false, clickWas = false;
    if (g_vrHead) {
        CtrlState lc; AcquireSRWLockShared(&g_hmdLock); lc = g_ctrl[0]; ReleaseSRWLockShared(&g_hmdLock);
        float m = std::sqrt(lc.sx * lc.sx + lc.sy * lc.sy);
        if (lc.stickClick && !clickWas) { stickRun = !stickRun; Log("analogico: correr=%d", stickRun); }
        clickWas = lc.stickClick;
        static bool walking = false;
        if ((m > g_stickDead) != walking) { walking = m > g_stickDead; Log("analogico esquerdo: %s (%.2f, %.2f)", walking ? "andando" : "parado", lc.sx, lc.sy); }
        if (m > g_stickDead) {
            float k = (std::min)(1.0f, (m - g_stickDead) / (1.0f - g_stickDead)) / m;
            f += lc.sy * k; r += lc.sx * k;
            stickMag = (std::min)(1.0f, m);
        } else stickRun = false;                               // soltou o analógico: volta a andar
    }
    g_motionWalk = (std::min)(1.0f, std::sqrt(f * f + r * r));   // vinheta de conforto (xr_vignette.inl)
    float dir[2] = { fx * f + rx * r, fz * f + rz * r };
    float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    float analog = (dl > 1.0f) ? 1.0f : dl;                     // teclado = 1; analógico = o quanto empurrou

    if (g_routeWalk) {
        // A câmera anda com o corpo (a rota é animada no cliente, suave); só filtra o balanço.
        float a = 1.0f - std::exp(-18.0f * mdt);
        g_origin[0] += (g_anchor[0] - g_origin[0]) * a;
        g_origin[1] += (g_anchor[2] - g_origin[1]) * a;
        bool run = down(g_runKey) || (stickRun && stickMag > 0.0f);
        static float sentDir[2] = { 0, 0 }; static bool sentRun = false, walking = false;
        static float resend = 0.0f;
        resend += mdt;
        if (dl > 0.15f) {
            float nx = dir[0] / dl, nz = dir[1] / dl;
            bool changed = !walking || (nx * sentDir[0] + nz * sentDir[1]) < 0.97f || run != sentRun;
            if (changed || resend >= 0.25f) {
                SendServerCommand("fpc.walk %.4f %.4f %d", nx, nz, run ? 1 : 0);
                sentDir[0] = nx; sentDir[1] = nz; sentRun = run; walking = true; resend = 0.0f;
            }
        } else if (walking) {
            SendServerCommand("fpc.walk 0 0 0");
            walking = false; resend = 0.0f;
            g_faceTimer = 0.0f; g_lastFaceDir[0] = fx; g_lastFaceDir[1] = fz;
        } else {
            // parado: o corpo vira para onde você olha só quando a diferença é grande (em VR a cabeça é livre)
            float face[2] = { fx, fz };
            float minAng = g_vrHead ? 0.9f : g_faceMinAngle;   // ~50° em VR
            if (g_faceTimer >= 0.6f && AngleBetween2D(face, g_lastFaceDir) > minAng) {
                SendServerCommand("fpc.face %.4f %.4f", fx, fz);
                g_lastFaceDir[0] = fx; g_lastFaceDir[1] = fz; g_faceTimer = 0.0f;
            }
        }
        g_moving = walking;
        return;
    }

    // Vetor corpo -> câmera (horizontal).
    // Referência da coleira: o corpo real. Com o avatar no cliente, o corpo visível é nosso: usa o servidor.
    float ax = g_anchor[0], az = g_anchor[2];
    AvatarServerXZ(ax, az);
    float lx = g_origin[0] - ax, lz = g_origin[1] - az;
    float lead = std::sqrt(lx * lx + lz * lz);

    // Giro contínuo do corpo: a direção enviada ao servidor gira suave até o olhar (sem degraus de 15°).
    {
        if (g_bodyDir[0] == 0.0f && g_bodyDir[1] == 0.0f) { g_bodyDir[0] = fx; g_bodyDir[1] = fz; }
        float cur = std::atan2(g_bodyDir[0], g_bodyDir[1]), want = std::atan2(fx, fz);
        float d = want - cur;
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        float step = d * (1.0f - std::exp(-g_turnRate * mdt));
        float maxStep = g_turnMaxDeg * 0.0174533f * mdt;
        if (step > maxStep) step = maxStep; else if (step < -maxStep) step = -maxStep;
        cur += step;
        g_bodyDir[0] = std::sin(cur); g_bodyDir[1] = std::cos(cur);
    }
    const float bfx = g_bodyDir[0], bfz = g_bodyDir[1];

    if (dl > 1e-3f) {
        dir[0] /= dl; dir[1] /= dl;
        if (g_blockedTimer <= 0.0f) {                          // bloqueado: não avança
            float speed = (down(g_runKey) || (stickRun && stickMag > 0.0f)) ? g_runSpeed : g_walkSpeed;
            speed *= (std::max)(0.25f, analog);
            // Coleira elástica: só a parte do passo que AFASTA do corpo é freada depois de g_maxLead.
            float ux = lead > 1e-3f ? lx / lead : 0.0f, uz = lead > 1e-3f ? lz / lead : 0.0f;
            float away = dir[0] * ux + dir[1] * uz;
            float k = 1.0f;
            if (away > 0.0f && lead > g_maxLead)
                k = (std::max)(0.0f, 1.0f - (lead - g_maxLead) / (g_hardLead - g_maxLead));
            float sx = dir[0] * speed * mdt, sz = dir[1] * speed * mdt;
            float a = (sx * ux + sz * uz) * (away > 0.0f ? (1.0f - k) : 0.0f);   // parte que afasta, a frear
            g_origin[0] += sx - ux * a;
            g_origin[1] += sz - uz * a;
        }
        lx = g_origin[0] - ax; lz = g_origin[1] - az; lead = std::sqrt(lx * lx + lz * lz);
        if (lead > g_hardLead) {                               // além do limite: volta suave, sem pulo
            float t = 1.0f - std::exp(-8.0f * mdt), want = lead - (lead - g_hardLead) * t;
            g_origin[0] = ax + lx / lead * want; g_origin[1] = az + lz / lead * want;
        }
        {
            static float leadLog = 0.0f;
            leadLog += mdt;
            if (leadLog > 2.0f) { leadLog = 0.0f; Log("andar: camera a %.2f m a frente do corpo real (dt %.4f s)", lead, mdt); }
        }
        if (g_moveTimer >= g_moveInterval) {
            SendServerCommand("fpc.move %.4f %.4f %.4f %.4f", g_origin[0], g_origin[1], bfx, bfz);
            g_moveTimer = 0.0f;
        }
        g_moving = true;
        return;
    }

    if (g_moving) {                                            // soltou as teclas: último passo
        SendServerCommand("fpc.move %.4f %.4f %.4f %.4f", g_origin[0], g_origin[1], bfx, bfz);
        g_moving = false; g_moveTimer = 0.0f; g_faceTimer = 0.0f;
        g_lastFaceDir[0] = bfx; g_lastFaceDir[1] = bfz;
        return;
    }
    // Parado: se o corpo foi parar longe (o jogo moveu o Sim), a câmera vai até ele, suave.
    if (lead > g_driftSnap) {
        float a = 1.0f - std::exp(-8.0f * g_dt);
        g_origin[0] -= lx * a; g_origin[1] -= lz * a;
    }
    // O corpo acompanha a direção da câmera em giro contínuo: passos pequenos (~1,5°), até 30 por segundo.
    float face[2] = { bfx, bfz };
    if (g_faceTimer >= 1.0f / 30.0f && AngleBetween2D(face, g_lastFaceDir) > 0.026f) {
        SendServerCommand("fpc.face %.4f %.4f", bfx, bfz);
        g_lastFaceDir[0] = bfx; g_lastFaceDir[1] = bfz;
        g_faceTimer = 0.0f;
    }
}

// Sinais do Python chegam como "camera.fpc.idletime <código>" -> FPS_SetParam(ctrl, 0, código).
static FpsSetParam_t o_FpsSetParam = nullptr;
static void __fastcall hk_FpsSetParam(void* ctrl, int index, float value) {
    if (index == 0 && value <= -999.5f) {
        int code = (int)std::lround(value);
        if (code == -1000 || code == -1001) {
            bool busy = (code == -1001);
            if (busy != g_busy) Log("estado: %s", busy ? "OCUPADO (camera segue o corpo)" : "livre (camera do jogador)");
            g_busy = busy;
        } else if (code == -1002) {
            g_blockedTimer = 0.15f;
            if (g_burstFrames > 0) Log("bloqueado");
        } else if (code == -1011) { g_rayReady = true;          // Fase 3b: resultado do raycast do laser
        } else if (code == -1009) { g_objGrabReady = true;      // Fase 5: o Python escolheu um objeto
        } else if (code == -1010) { g_objGrabNone = true;       // nada para agarrar
        } else if (code == -1007) {                            // Fase 3: opções do menu VR gravadas
            g_menuPending = true;
            Log("menu VR: opcoes prontas (Python)");
        } else if (code == -1005) {                            // a roda de opções foi criada (Python)
            g_pieOpen = true; g_pieCloseAt = 0; g_pieOpenedAt = GetTickCount64(); g_pieLockPending = true;
            Log("roda de opcoes: criada pelo jogo");
        } else if (code == -1006) {                            // uma opção foi escolhida
            g_pieCloseAt = GetTickCount64() + 300;
            Log("roda de opcoes: opcao escolhida");
        } else if (code == -1003 || code == -1004) {
            bool b = (code == -1003);
            if (b != g_camBlocked) Log("sonda: camera %s", b ? "ATRAS DE PAREDE/OBJETO -> puxando para o corpo" : "livre");
            g_camBlocked = b;
        }
        return;                                                 // o jogo nunca vê esses valores
    }
    o_FpsSetParam(ctrl, index, value);
}

// ---------------------------------------------------------------- esconder o corpo (só no cliente)
// Usa a mesma função que o cliente executa ao receber um SetOpacity do servidor, mas sem passar
// pela simulação: o Python e o resto do jogo continuam vendo o Sim normal.
// A FPS sai sozinha se o alfa do Sim for 0 (FPS_IsSimValidForFPC); por isso esse teste é interceptado.
using SetOpacity_t    = void (__fastcall*)(void* comp, float opacity, float duration, float unk);
using GetComponent_t  = void* (__fastcall*)(void* obj, uint32_t type);
using ClientObjMgr_t  = void* (__fastcall*)();
using SimValid_t      = bool (__fastcall*)(void* obj);
static SetOpacity_t   p_SetOpacity     = nullptr;
static GetComponent_t p_GetComponent   = nullptr;
static ClientObjMgr_t p_ClientObjMgr   = nullptr;
static uint32_t*      p_visualCompType = nullptr;
static SimValid_t     o_SimValidForFpc = nullptr;
static float          g_bodyTimer      = 0.0f;

// Objeto do Sim seguido pela FPC, no cliente (mesmo caminho do jogo em FPSImpl_AttachTransform).
static void* GetSimObject() {
    if (!g_impl || !p_ClientObjMgr) return nullptr;
    uint64_t id = *(uint64_t*)(g_impl + 0xf0);
    if (!id) return nullptr;
    void* mgr = p_ClientObjMgr();
    if (!mgr) return nullptr;
    void* sub = ((void* (__fastcall*)(void*))(*(void***)mgr)[0x28 / 8])(mgr);
    if (!sub) return nullptr;
    return ((void* (__fastcall*)(void*, uint64_t))(*(void***)sub)[0x18 / 8])(sub, id);
}

static void* GetVisualComponent() {
    if (!p_GetComponent || !p_visualCompType) return nullptr;
    void* obj = GetSimObject();
    return obj ? p_GetComponent(obj, *p_visualCompType) : nullptr;
}

static bool SetBodyVisible(bool visible) {
    void* comp = GetVisualComponent();
    if (!comp || !p_SetOpacity) return false;
    p_SetOpacity(comp, visible ? 1.0f : 0.0f, 0.0f, 0.0f);
    return true;
}

// Mantém o corpo escondido: o servidor pode reenviar a opacidade (roupa, animação...).
static void KeepBodyHidden() {
    if (!g_bodyHidden) return;
    g_bodyTimer -= g_dt;
    if (g_bodyTimer > 0.0f) return;
    g_bodyTimer = 0.5f;
    void* comp = GetVisualComponent();
    if (comp && *(float*)((uint8_t*)comp + 0x7c) > 0.01f) SetBodyVisible(false);
}

static bool __fastcall hk_SimValidForFpc(void* obj) {
    if (g_bodyHidden && obj) return true;                     // escondido por nós: a FPS continua
    return o_SimValidForFpc(obj);
}

// ---------------------------------------------------------------- congelar o corpo (pose do esqueleto)
// Pipeline de animação: ... EvaluateControllers -> ApplyBoneDeltas -> IkTargetOverrides -> FocusPosing
// -> LimbPoseIk -> ApplyBoneMultipliers -> EndPosingState (finaliza a pose; roda para todo ator).
// Antes do EndPosingState do nosso Sim, reaplicamos uma "foto" das juntas tirada ao entrar no modo
// livre. Juntas: 0x30 bytes cada (posição, rotação quaternion, escala), quantidade em pose+0x5e.
// Também é o ponto de entrada que o IK dos braços (VR) vai usar.
static const char* SIG_END_POSING  = "48 89 5C 24 18 56 48 83 EC 20 48 8B CA 41 8B D9";
static const char* SIG_ACTOR_BY_ID = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 10 48 8B C8 4C 8B 42 20 48 8B D3";
static const char* SIG_ACTOR_POSE  = "40 55 56 57 48 83 EC 40 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 68 48 8B F9";
static const char* SIG_POSE_JOINTS = "48 8B 41 38 48 85 C0 75 ?? 48 8D 51 60 4C 8D 82 80 25 00 00";
static const char* SIG_ACTOR_LOCK  = "40 53 48 83 EC 20 48 8B D9 48 89 11 48 8D 8A C8 00 00 00";
static const char* SIG_ACTOR_UNLOCK = "48 8B 09 48 81 C1 C8 00 00 00 48 FF 25 ?? ?? ?? ??";

using EndPosing_t  = void (__fastcall*)(void* self, void* actor, void* a3, uint32_t flags);
using ActorById_t  = void* (__fastcall*)(const uint64_t* id);
using ActorPose_t  = void* (__fastcall*)(void* actor, char create);
using PoseJoints_t = float* (__fastcall*)(void* pose);
using ActorLock_t  = void* (__fastcall*)(void* lock, void* actor, int mode);
using ActorUnlock_t = void (__fastcall*)(void* lock);
static EndPosing_t   o_EndPosing    = nullptr;
static ActorById_t   p_ActorById    = nullptr;
static ActorPose_t   p_ActorPose    = nullptr;
static PoseJoints_t  p_PoseJoints   = nullptr;
static ActorLock_t   p_ActorLock    = nullptr;
static ActorUnlock_t p_ActorUnlock  = nullptr;

static bool  g_freezeBodyCfg = true;               // congelar_corpo no ini
static void* volatile g_simActor = nullptr;        // ator de animação do nosso Sim (atualizado no update)
static volatile bool g_wantFreeze = false;
static bool  g_haveSnapshot = false;
static int   g_snapJoints   = 0;
static float g_snapshot[256 * 12];                 // até 256 juntas x 12 floats
static bool  g_loggedActor  = false;
static volatile ULONGLONG g_lastFpsTick = 0;      // último update da FPC (proteção: sem FPC, sem congelar)

static int ParseKey(const char* s);
#include "vr_rig.inl"                               // VR: braços, mãos e cabeça seguem os controles/headset
#include "vr_grab.inl"                              // VR: agarrar outros Sims (modelo HIGGS/PLANCK)
#include "vr_laser.inl"                             // VR: laser da mão direita (cursor + cliques)
#include "vr_avatar.inl"                            // Fase 1: o cliente posiciona o Sim (corpo liso)
#include "vr_menu.inl"                              // Fase 3: menu radial 3D próprio
#include "vr_status.inl"                            // Fase 4: relógio de pulso
#include "vr_objgrab.inl"                           // Fase 5: agarrar objetos
static int ParseKey(const char* s);
#include "vr_config.inl"                            // menu de configurações (DEL / segurar ≡)

// Pose_Commit (0x140894080, chamado pelo processador #18, logo antes do EndPosingState): copia as juntas
// da pose de trabalho (ator+0x380) para a pose definitiva do ator (ator+0xf0). Escrever DEPOIS disso
// (no EndPosingState, como até a v19) não tinha efeito nenhum: a pose de trabalho é descartada ali.
// O chamador já segura o lock do ator.
static const char* SIG_POSE_COMMIT = "48 85 D2 0F 84 C8 00 00 00 48 89 5C 24 10 57 48 83 EC 20 48 8B 01 48 8B FA 48 8B D9";
using PoseCommit_t = void (__fastcall*)(void* actor, void* pose);
static PoseCommit_t o_PoseCommit = nullptr;
static void __fastcall hk_PoseCommit(void* actor, void* pose) {
    if (actor && pose && actor == g_simActor && p_PoseJoints) {
        if (!g_loggedActor) { g_loggedActor = true; Log("pose: Pose_Commit recebeu o ator do nosso Sim (%p)", actor); }
        int n = *(uint16_t*)((uint8_t*)pose + 0x5e);
        float* joints = p_PoseJoints(pose);
        bool fresh = GetTickCount64() - g_lastFpsTick < 250;
        if (joints && n > 0 && n <= 256) {
            // Movimento da animação neste frame (maior giro de junta), para saber quando ela assentou.
            static float prev[256 * 12]; static int prevN = 0;
            float maxAng = 0.0f;
            if (prevN == n) {
                for (int i = 0; i < n; ++i) {
                    const float* a = joints + i * 12 + 4; const float* b = prev + i * 12 + 4;
                    float d = std::fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
                    float ang = 2.0f * std::acos(d > 1.0f ? 1.0f : d);
                    if (ang > maxAng) maxAng = ang;
                }
            }
            memcpy(prev, joints, (size_t)n * 0x30); prevN = n;
            static ULONGLONG wantSince = 0, calmSince = 0;
            static bool releasedByAnim = false;                   // solto por animação nova: sem limite de 2,5 s
            ULONGLONG now = GetTickCount64();
            if (!(g_wantFreeze && fresh)) wantSince = 0;
            if (g_wantFreeze && fresh) {
                if (!g_haveSnapshot || g_snapJoints != n) {
                    // Não tira a foto no instante em que fica livre (pode ser o fim de "levantar" ainda
                    // sentado): deixa a animação rodar até o corpo parar (0,3 s calmo) ou 2,5 s no máximo.
                    if (!wantSince) { wantSince = now; calmSince = 0; }
                    bool calm = maxAng < 0.02f;                  // < ~1,1 grau por frame em todas as juntas
                    if (!calm) calmSince = 0; else if (!calmSince) calmSince = now;
                    bool settled = (now - wantSince > 500 && calmSince && now - calmSince > 300) || (!releasedByAnim && now - wantSince > 2500);
                    if (settled) {
                        memcpy(g_snapshot, joints, (size_t)n * 0x30);   // foto da pose atual (já assentada)
                        g_snapJoints = n; g_haveSnapshot = true; releasedByAnim = false;
                        Log("pose: corpo congelado (%d juntas) depois de %llu ms (movimento %.3f rad/frame)", n, now - wantSince, maxAng);
                    }
                } else {
                    // Uma animação "de verdade" começou por baixo (tronco/quadril se mexendo muito em relação
                    // à foto)? Solta o corpo para ela rodar; congela de novo só quando ela assentar.
                    static ULONGLONG devSince = 0;
                    bool big = false;
                    if (g_rigIdxOk) {
                        int ids[2] = { g_rigIdx[B_ROOTBIND], g_rigIdx[B_SPINE2] };
                        for (int k = 0; k < 2; ++k) {
                            int j = ids[k]; if (j < 0 || j >= n) continue;
                            const float* a2 = joints + j * 12; const float* b2 = g_snapshot + j * 12;
                            float dq = std::fabs(a2[4] * b2[4] + a2[5] * b2[5] + a2[6] * b2[6] + a2[7] * b2[7]);
                            float ang = 2.0f * std::acos(dq > 1.0f ? 1.0f : dq);
                            float dp = std::sqrt((a2[0] - b2[0]) * (a2[0] - b2[0]) + (a2[1] - b2[1]) * (a2[1] - b2[1]) + (a2[2] - b2[2]) * (a2[2] - b2[2]));
                            if (ang > 0.35f || dp > 0.10f) big = true;
                        }
                    }
                    if (!big) devSince = 0; else if (!devSince) devSince = now;
                    if (devSince && now - devSince > 250) {
                        g_haveSnapshot = false; wantSince = now; calmSince = 0; devSince = 0; releasedByAnim = true;
                        Log("pose: animacao nova por baixo do corpo congelado -> solto ate ela assentar");
                    } else {
                        memcpy(joints, g_snapshot, (size_t)n * 0x30);   // mantém a foto (a idle não anda)
                    }
                }
            } else if (g_haveSnapshot) {
                g_haveSnapshot = false;
                Log("pose: corpo liberado (animacao normal)");
            }
            if (fresh) VrRigApply(joints, n);                     // VR por cima de tudo (animação ou foto)
        }
    }
    else if (actor && pose && p_PoseJoints && g_grabEnabled && g_vrHead) {   // outros atores: rastreio + agarrar
        int n = *(uint16_t*)((uint8_t*)pose + 0x5e);
        float* joints = p_PoseJoints(pose);
        GrabTrackActor(actor);
        if (joints && n > 0 && n <= 256) VrGrabApply(actor, joints, n);
    }
    o_PoseCommit(actor, pose);
}

// Chamado no update da FPC (thread principal).
static void UpdateFreezeState() {
    if (g_impl && p_ActorById) {
        uint64_t id = *(uint64_t*)(g_impl + 0xf0);
        g_simActor = id ? p_ActorById(&id) : nullptr;
    }
    g_wantFreeze = g_freezeBodyCfg && FpsFree();
    if (g_simActor) ResolveBones(g_simActor);                  // índices dos ossos (também para soltar o congelamento)
    g_lastFpsTick = GetTickCount64();
    VrRigUpdate(g_simActor, g_walkMode, g_busy);             // liga com o modo FPS (INSERT)
    VrGrabUpdate();                                          // antes do laser: perto de alguém, o grip agarra
    VrConfigUpdate();                                        // configurações (antes de tudo: silencia a entrada)
    VrMenuUpdate();                                          // Fase 3: menu VR (antes do laser, que o usa)
    VrObjGrabUpdate();                                       // Fase 5: objeto na mão
    VrStatusUpdate();                                        // Fase 4: dados do relógio
    VrLaserUpdate();

}

// ---------------------------------------------------------------- hooks
static void __fastcall hk_FpsUpdate(void* ctrl, float dt, char flag) {
    g_fpsCam = *(void**)((uint8_t*)ctrl + 0x18);              // câmera do engine
    g_projCam = g_fpsCam;
    g_impl   = *(uint8_t**)((uint8_t*)ctrl + 0x28);           // CameraControllerFPSImpl
    if (dt > 0.0f && dt < 0.5f) g_dt = dt;

    // Reentrada: o controlador é o mesmo objeto toda vez, mas ao sair e voltar (Shift+Tab)
    // o jogo prende a câmera na cabeça de novo. Uma pausa nos updates indica reentrada.
    static ULONGLONG lastTick = 0;
    ULONGLONG now = GetTickCount64();
    if (lastTick != 0 && now - lastTick > 300) { g_lastCtrl = nullptr; Log("FPC reativada (pausa de %llu ms)", now - lastTick); }
    lastTick = now;

    if (ctrl != g_lastCtrl) {                                 // FPC (re)ativada
        g_haveOrigin = false; g_moving = false;
        g_vrNeedRecenter = true;
        g_busy = true; SendServerCommand("fpc.sync");        // estado livre/ocupado + recuperação
        g_bodyTimer = 0.0f;                                    // reaplica o corpo escondido logo
        g_lastCtrl = ctrl; g_phase = CAPTURE_HEAD; g_haveFiltered = false;
        Log("FPS ctrl=%p cam=%p dt=%f", ctrl, g_fpsCam, dt);
    }
    if (Pressed(VK_F7))  { g_burstFrames = 600; Log("F7: gravando frames"); }
    if (Pressed(VK_F8))  { g_freeze = !g_freeze; memcpy(g_frozen, g_out, 16); Log("F8: congelar=%d", g_freeze); }
    if (Pressed(VK_F9))  { g_eyeTarget = (g_eyeTarget == 0.0f) ? 1.0f : 0.0f; Log("F9: eye_weight=%f", g_eyeTarget); }
    if (Pressed(VK_F10)) { g_filterOn = !g_filterOn; Log("F10: filtro=%d", g_filterOn); }
    if (Pressed(VK_F11)) { g_lastCtrl = nullptr; Log("F11: recalibrar no proximo frame"); }
    if (Pressed(g_hideBodyKey) && GameHasFocus()) {
        g_bodyHidden = !g_bodyHidden;
        bool ok = SetBodyVisible(!g_bodyHidden);
        Log("corpo %s (cliente)%s", g_bodyHidden ? "escondido" : "visivel", ok ? "" : " FALHOU: componente visual nao encontrado");
    }
    if (Pressed(g_walkToggleKey) && GameHasFocus()) {
        g_walkMode = !g_walkMode;
        g_busy = true; g_haveOrigin = false; g_moving = false;  // espera o Python confirmar "livre"
        g_lastFaceDir[0] = g_lastFaceDir[1] = 0.0f;
        g_faceTimer = 0.0f;
        if (g_walkMode) SendServerCommand("fpc.sync");      // Python reenvia livre/ocupado na hora
        Log("modo FPS (WASD) = %d", g_walkMode);
    }

    if (p_eyeWeight) *p_eyeWeight = g_eyeTarget;
    else if (p_FpsSetParam) p_FpsSetParam(ctrl, 1, g_eyeTarget);

    UpdateVrHead();
    g_poseCalls = 0;
    t_inFpsUpdate = true;
    o_FpsUpdate(ctrl, dt, flag);
    t_inFpsUpdate = false;
    ++g_frame;

    // Avança a máquina de estados depois do frame (fora do update do jogo).
    if (g_phase == CAPTURE_HEAD)        { SwitchToAnchor(ctrl); g_phase = SWITCHED_WAIT; }
    else if (g_phase == SWITCHED_WAIT)  { g_phase = CAPTURE_ANCHOR; }
    else if (g_phase == RUNNING && g_walkMode) UpdateFpsMode();
    VrAvatarUpdate();                                         // Fase 1: o cliente posiciona o Sim
    if (g_phase == RUNNING) KeepBodyHidden();
    UpdateFreezeState();
}

static void __fastcall hk_SetPose(void* cam, const float* pos, const float* up, const float* fwd) {
    if (!t_inFpsUpdate || cam != g_fpsCam) { o_SetPose(cam, pos, up, fwd); return; }

    if (++g_poseCalls == 1) {                                  // 1ª chamada: pos = osso de fixação
        if (g_phase == CAPTURE_HEAD) memcpy(g_headPos, pos, 16);
        if (g_phase == CAPTURE_ANCHOR) {
            static bool calibrated = false;
            float up_ = g_headPos[UP] - pos[UP];
            bool ok = (up_ >= g_eyeUpMin && up_ <= g_eyeUpMax);
            if (ok) { g_eyeUp = up_; calibrated = true; }
            else if (!calibrated) g_eyeUp = g_eyeUpFallback;      // senão mantém a última boa
            Log("calibracao: cabeca.y=%.3f peito.y=%.3f -> altura_olhos=%.3f%s",
                g_headPos[UP], pos[UP], g_eyeUp, ok ? "" : " (medida fora dos limites; mantendo valor anterior/padrao)");
            g_phase = RUNNING; g_haveFiltered = false;
        }
        V3 bo = BodyOffApplied();
        float pc[4] = { pos[0] - bo.x, pos[1] - bo.y, pos[2] - bo.z, pos[3] };   // y: agachar já vem do headset
        ComputeOutput(pc, fwd);
        static ULONGLONG lastBody = 0;
        if (FpsFree() && GetTickCount64() - lastBody > 2000) {
            lastBody = GetTickCount64();
            float rx = g_origin[0] - pos[0], rz = g_origin[1] - pos[2], tx = g_origin[0] - pc[0], tz = g_origin[1] - pc[2];
            Log("corpo: peito visivel a %.3f m da camera (real a %.3f m; deslocamento aplicado %.3f m)%s",
                std::sqrt(rx * rx + rz * rz), std::sqrt(tx * tx + tz * tz), std::sqrt(bo.x * bo.x + bo.z * bo.z),
                g_camPull > 0.01f ? " [camera puxada pela sonda]" : "");
        }
    }

    if (g_burstFrames > 0 || (g_frame % 120) == 0) {
        if (g_burstFrames > 0 && g_poseCalls == 1) --g_burstFrames;
        Log("f=%u c=%d ph=%d livre=%d dt=%.4f raw=(%.3f %.3f %.3f) out=(%.3f %.3f %.3f) fwd=(%.3f %.3f %.3f)",
            g_frame, g_poseCalls, g_phase, (int)FpsFree(), g_dt, pos[0], pos[1], pos[2],
            g_out[0], g_out[1], g_out[2], fwd[0], fwd[1], fwd[2]);
    }
    if (g_phase != RUNNING && !g_freeze) { o_SetPose(cam, pos, up, fwd); return; }

    // Rotação: direção do mouse sem o filtro de 120 amostras do jogo, up do mundo (sem roll).
    alignas(16) float newPos[4]; memcpy(newPos, g_out, 16);
    alignas(16) float newFwd[4];
    alignas(16) float worldUp[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    if (g_vrHead) {                                            // VR: a cabeça manda (com roll e posição)
        memcpy(newFwd, g_vrFwd, 16); memcpy(worldUp, g_vrUp, 16);
        newPos[0] += g_vrOffset[0]; newPos[1] += g_vrOffset[1]; newPos[2] += g_vrOffset[2];
    } else if (!MouseLookDir(newFwd)) memcpy(newFwd, fwd, 16);
    if (FpsFree()) {
        if (g_poseCalls == 1) {
            g_camWanted[0] = newPos[0]; g_camWanted[1] = newPos[2];
            float tgt = g_camBlocked ? 1.0f : 0.0f;
            float rate = g_camBlocked ? 10.0f : 3.0f;          // entra rápido, sai devagar
            g_camPull += (tgt - g_camPull) * (1.0f - std::exp(-rate * g_dt));
        }
        if (g_camPull > 0.001f) {                               // corpo real (g_anchor) -> câmera, encurtado
            float vx = newPos[0] - g_anchor[0], vz = newPos[2] - g_anchor[2];
            float l = std::sqrt(vx * vx + vz * vz);
            if (l > g_camKeep) {
                float nl = l + ((std::min)(l, g_camKeep) - l) * g_camPull;
                newPos[0] = g_anchor[0] + vx / l * nl; newPos[2] = g_anchor[2] + vz / l * nl;
            }
        }
    } else g_camPull = 0.0f;
    if (g_vrHead) {                                            // estéreo síncrono parte daqui
        memcpy(g_centerCam, newPos, 16); memcpy(g_centerFwd, newFwd, 16); memcpy(g_centerUp, worldUp, 16);
        g_syncReady = true;
        if (g_gameCamMode != GCAM_HEAD) {                      // câmera do jogo no laser / travada (os olhos não mudam)
            memcpy(newPos, g_gameCamW[0], 16); memcpy(newFwd, g_gameCamW[1], 16); memcpy(worldUp, g_gameCamW[2], 16);
        }
    }
    memcpy(g_restoreCam[0], newPos, 16); memcpy(g_restoreCam[1], newFwd, 16); memcpy(g_restoreCam[2], worldUp, 16);
    o_SetPose(cam, newPos, worldUp, newFwd);
}

// Saindo da FPC (slot 8 do CameraControllerFPS): se o corpo estiver escondido, mostra de novo,
// senão o Sim ficaria invisível na câmera normal. Ao voltar para a FPC ele é escondido outra vez.
using FpsDeactivate_t = void (__fastcall*)(void* ctrl, void* a2, void* a3, void* a4);
static FpsDeactivate_t o_FpsDeactivate = nullptr;
static void __fastcall hk_FpsDeactivate(void* ctrl, void* a2, void* a3, void* a4) {
    if (g_bodyHidden) { SetBodyVisible(true); Log("saiu da FPC: corpo visivel de novo"); }
    g_lastCtrl = nullptr;                                     // a próxima entrada recalibra
    g_wantFreeze = false;                                     // o corpo volta a animar fora da FPC
    o_FpsDeactivate(ctrl, a2, a3, a4);
}

// ---------------------------------------------------------------- proteção: salvamento da conta
// UserAccountDataService (slot 10) -> 0x140130b70 usa o armazenamento em this+0x18 sem checar se é
// nulo. Com o VR assumindo o headset (provável perda de foco da janela), isso derrubou o jogo duas
// vezes (violação de acesso em TS4_x64.exe+0x130bc0). Aqui: armazenamento nulo = "não salvou".
static const char* SIG_ACCOUNT_SAVE = "48 8B C4 57 48 83 EC 70 48 C7 40 C8 FE FF FF FF 48 89 58 18 48 89 70 20 49 8B F8 48 8B DA 48 8B F1";
using AccountSave_t = bool (__fastcall*)(void* self, void* a2, void* a3);
static AccountSave_t o_AccountSave = nullptr;
static bool __fastcall hk_AccountSave(void* self, void* a2, void* a3) {
    if (self && *(void**)((uint8_t*)self + 0x18) == nullptr) {
        static int n = 0;
        if (n++ < 3) Log("protecao: salvamento da conta ignorado (armazenamento nulo; o jogo teria caido)");
        return false;
    }
    return o_AccountSave(self, a2, a3);
}

// ---------------------------------------------------------------- configuração (fpc_rig.ini)
// Lê fpc_rig.ini (mesma pasta da DLL). Aceita: F1..F24, INSERT, HOME, END, PAGEUP, PAGEDOWN,
// CAPSLOCK, PAUSE, SCROLLLOCK, NUMPAD0..NUMPAD9, uma letra/número (ex.: G) ou código hex (ex.: 0x2D).
static int ParseKey(const char* s) {
    struct { const char* n; int vk; } named[] = {
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"DEL", VK_DELETE}, {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR},
        {"PAGEDOWN", VK_NEXT}, {"CAPSLOCK", VK_CAPITAL}, {"PAUSE", VK_PAUSE}, {"SCROLLLOCK", VK_SCROLL},
        {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"SPACE", VK_SPACE}, {"TAB", VK_TAB},
        {"VIRGULA", VK_OEM_COMMA}, {"COMMA", VK_OEM_COMMA}, {",", VK_OEM_COMMA}, {"PONTO", VK_OEM_PERIOD}, {".", VK_OEM_PERIOD} };
    for (auto& k : named) if (_stricmp(s, k.n) == 0) return k.vk;
    // Ç do teclado ABNT2 = VK_OEM_1. Aceita o nome, ou o próprio Ç (ANSI 0xC7/0xE7 ou UTF-8 C3 87/C3 A7).
    if (_stricmp(s, "CEDILHA") == 0 || _stricmp(s, "C_CEDILHA") == 0) return VK_OEM_1;
    { const unsigned char* u = (const unsigned char*)s;
      if ((u[0] == 0xC7 || u[0] == 0xE7) && !u[1]) return VK_OEM_1;
      if (u[0] == 0xC3 && (u[1] == 0x87 || u[1] == 0xA7) && !u[2]) return VK_OEM_1; }
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') { int n = atoi(s + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (_strnicmp(s, "NUMPAD", 6) == 0 && s[6] >= '0' && s[6] <= '9' && !s[7]) return VK_NUMPAD0 + (s[6] - '0');
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, nullptr, 16);
    if (s[0] && !s[1]) { char c = (char)toupper((unsigned char)s[0]); if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c; }
    return 0;
}

static void LoadConfig() {
    CfgBuild();                                                // tabela do menu de configurações (padrões = valores do código)
    char path[MAX_PATH], key[32] = {};
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "fpc_rig.ini");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {   // cria um ini de exemplo
        WritePrivateProfileStringA("fpc_rig", "tecla_modo_fps", "INSERT", path);
        WritePrivateProfileStringA("fpc_rig", "inverter_A_D", "0", path);
        WritePrivateProfileStringA("fpc_rig", "tecla_esconder_corpo", "END", path);
        WritePrivateProfileStringA("fpc_rig", "tecla_correr", "LALT", path);
    }
    GetPrivateProfileStringA("fpc_rig", "tecla_modo_fps", "INSERT", key, sizeof(key), path);
    if (int vk = ParseKey(key)) g_walkToggleKey = vk;
    char hide[32] = {};
    GetPrivateProfileStringA("fpc_rig", "tecla_esconder_corpo", "END", hide, sizeof(hide), path);
    if (int vk = ParseKey(hide)) g_hideBodyKey = vk;
    Log("config: tecla_esconder_corpo=%s (vk=0x%02X)", hide, g_hideBodyKey);
    char f[32];
    g_bubbleOn = GetPrivateProfileIntA("fpc_rig", "bolha_ligada", 1, path) != 0;
    g_routeWalk = GetPrivateProfileIntA("fpc_rig", "locomocao_rota", 0, path) != 0;
    {
        char fm[32];
        GetPrivateProfileStringA("fpc_rig", "envios_por_segundo", "120", fm, sizeof(fm), path);
        float hz = (float)atof(fm); if (hz < 10.0f || hz > 240.0f) hz = 120.0f; g_moveInterval = 1.0f / hz;
        GetPrivateProfileStringA("fpc_rig", "giro_corpo_velocidade", "10", fm, sizeof(fm), path);
        g_turnRate = (float)atof(fm); if (g_turnRate < 1.0f || g_turnRate > 60.0f) g_turnRate = 10.0f;
        Log("config: envios_por_segundo=%.0f giro_corpo_velocidade=%.1f", hz, g_turnRate);
    }
    // Locomoção antiga: corpo congelado por padrão (com a rota ele precisa animar as pernas).
    g_freezeBodyCfg = GetPrivateProfileIntA("fpc_rig", "congelar_corpo", g_routeWalk ? 0 : 1, path) != 0;
    Log("config: locomocao_rota=%d", g_routeWalk);
    g_vrEnabled = GetPrivateProfileIntA("fpc_rig", "vr", 0, path) != 0;
    char rc[32] = {};
    GetPrivateProfileStringA("fpc_rig", "tecla_recentrar", "VIRGULA", rc, sizeof(rc), path);
    if (int vk = ParseKey(rc)) g_recenterKey = vk;
    g_recenterKeyXR = g_recenterKey;
    { char pk[32] = {}; GetPrivateProfileStringA("fpc_rig", "tecla_painel_vr", "PONTO", pk, sizeof(pk), path);
      if (int vk = ParseKey(pk)) g_panelKey = vk; Log("config: tecla_painel_vr=%s (vk=0x%02X)", pk, g_panelKey); }
    g_vrHeadTracking = GetPrivateProfileIntA("fpc_rig", "vr_rastrear_cabeca", 1, path) != 0;
    { int mode = GetPrivateProfileIntA("fpc_rig", "vr_estereo", 2, path);
      g_vrStereo = (mode == 1); g_vrStereoSync = (mode == 2); }
    { char f3[32]; GetPrivateProfileStringA("fpc_rig", "vr_escala_mundo", "1.0", f3, sizeof(f3), path); g_worldScale = (float)atof(f3); }
    Log("config: vr_estereo=%s vr_escala_mundo=%.2f", g_vrStereoSync ? "2 (sincrono)" : g_vrStereo ? "1 (AER)" : "0 (tela)", g_worldScale);
    {
        char f2[32];
        GetPrivateProfileStringA("fpc_rig", "vr_tela_largura", "2.4", f2, sizeof(f2), path); g_screenWidth = (float)atof(f2);
        GetPrivateProfileStringA("fpc_rig", "vr_tela_distancia", "2.2", f2, sizeof(f2), path); g_screenDist = (float)atof(f2);
    }
    Log("config: vr_rastrear_cabeca=%d tela %.1f m a %.1f m", g_vrHeadTracking, g_screenWidth, g_screenDist);
    Log("config: tecla_recentrar=%s (vk=0x%02X)", rc, g_recenterKey);
    Log("config: vr=%d", g_vrEnabled);
    Log("config: congelar_corpo=%d", g_freezeBodyCfg);
    VrRigLoadConfig(path);
    VrLaserLoadConfig(path);
    VrGrabLoadConfig(path);
    VrAvatarLoadConfig(path);
    VrMenuLoadConfig(path);
    VrStatusLoadConfig(path);
    VrObjGrabLoadConfig(path);
    {
        char fu[32];
        g_uiSeparate = GetPrivateProfileIntA("fpc_rig", "ui_separada", 1, path) != 0;
        g_laserVisible = GetPrivateProfileIntA("fpc_rig", "laser_visivel", 1, path) != 0;
        GetPrivateProfileStringA("fpc_rig", "ui_painel_largura", "0.34", fu, sizeof(fu), path); g_panelWidth = (float)atof(fu);
        GetPrivateProfileStringA("fpc_rig", "ui_painel_altura", "0.12", fu, sizeof(fu), path); g_panelLift = (float)atof(fu);
        GetPrivateProfileStringA("fpc_rig", "ui_painel_olhar", "40", fu, sizeof(fu), path); g_panelLookDeg = (float)atof(fu);
        g_worldUi = GetPrivateProfileIntA("fpc_rig", "ui_mundo", 1, path) != 0;
        GetPrivateProfileStringA("fpc_rig", "ui_mundo_distancia", "2.0", fu, sizeof(fu), path); g_worldUiDist = (float)atof(fu);
        GetPrivateProfileStringA("fpc_rig", "ui_mundo_regiao", "0.10 0.90 0.04 0.80", fu, sizeof(fu), path);
        { float a, b, c2, d2; if (sscanf_s(fu, "%f %f %f %f", &a, &b, &c2, &d2) == 4 && a < b && c2 < d2) { g_wuU0 = a; g_wuU1 = b; g_wuV0 = c2; g_wuV1 = d2; } }
        Log("config: ui_mundo=%d distancia=%.1f regiao u %.2f-%.2f v %.2f-%.2f", g_worldUi, g_worldUiDist, g_wuU0, g_wuU1, g_wuV0, g_wuV1);
        Log("config: ui_separada=%d ui_painel_largura=%.2f ui_painel_altura=%.2f ui_painel_olhar=%.0f laser_visivel=%d",
            g_uiSeparate, g_panelWidth, g_panelLift, g_panelLookDeg, g_laserVisible);
    }
    g_camProbeOn = GetPrivateProfileIntA("fpc_rig", "sonda_camera", 1, path) != 0;
    Log("config: sonda_camera=%d", g_camProbeOn);
    {
        char fz[32];
        g_symFov = GetPrivateProfileIntA("fpc_rig", "vr_fov_simetrico", 0, path) != 0;
        g_turnSnap = GetPrivateProfileIntA("fpc_rig", "vr_giro_passos", 0, path) != 0;
        GetPrivateProfileStringA("fpc_rig", "vr_giro_velocidade", "120", fz, sizeof(fz), path); g_turnSpeed = (float)atof(fz);
        GetPrivateProfileStringA("fpc_rig", "vr_giro_passo", "30", fz, sizeof(fz), path); g_turnSnapDeg = (float)atof(fz);
        GetPrivateProfileStringA("fpc_rig", "vr_zona_morta", "0.20", fz, sizeof(fz), path); g_stickDead = (float)atof(fz);
        if (g_stickDead < 0.05f || g_stickDead > 0.9f) g_stickDead = 0.2f;
        Log("config: vr_fov_simetrico=%d vr_giro_passos=%d vr_giro_velocidade=%.0f vr_giro_passo=%.0f vr_zona_morta=%.2f",
            g_symFov, g_turnSnap, g_turnSpeed, g_turnSnapDeg, g_stickDead);
    }
    g_cursorOn = GetPrivateProfileIntA("fpc_rig", "vr_cursor", 1, path) != 0;
    { char fc[32]; GetPrivateProfileStringA("fpc_rig", "vr_cursor_escala", "1.0", fc, sizeof(fc), path); g_cursorScale = (float)atof(fc); if (g_cursorScale < 0.25f) g_cursorScale = 1.0f; }
    Log("config: vr_cursor=%d vr_cursor_escala=%.2f", g_cursorOn, g_cursorScale);
    GetPrivateProfileStringA("fpc_rig", "bolha_raio", "0.30", f, sizeof(f), path); g_bubbleRadiusH = (float)atof(f);
    GetPrivateProfileStringA("fpc_rig", "bolha_raio_vertical", "0.20", f, sizeof(f), path); g_bubbleRadiusV = (float)atof(f);
    Log("config: bolha_ligada=%d raio=%.2f vertical=%.2f", g_bubbleOn, g_bubbleRadiusH, g_bubbleRadiusV);
    char run[32] = {};
    GetPrivateProfileStringA("fpc_rig", "tecla_correr", "LALT", run, sizeof(run), path);
    if (int vk = ParseKey(run)) g_runKey = vk;
    Log("config: tecla_correr=%s (vk=0x%02X)", run, g_runKey);
    if (GetPrivateProfileIntA("fpc_rig", "inverter_A_D", 0, path)) g_strafeSign = -1.0f;
    Log("config: %s tecla_modo_fps=%s (vk=0x%02X) inverter_A_D=%d", path, key, g_walkToggleKey, g_strafeSign < 0);
    VrConfigLoadAll(path);                                     // opções do menu de configurações (inclusive as novas)
}

// ---------------------------------------------------------------- inicialização
static DWORD WINAPI Init(LPVOID) {
    fopen_s(&g_log, "fpc_rig.log", "w");
    Log("fpc_rig v49 (menu de configuracoes: DEL; upscaling FSR 1.0/CAS; remapeamento; vinheta; IPD; altura)");
    auto fUpdate  = FindPattern(SIG_FPS_UPDATE);
    auto fPose    = FindPattern(SIG_SET_POSE);
    p_FpsSetParam = (FpsSetParam_t)FindPattern(SIG_FPS_SETPARAM);
    p_SetJoint    = (SetJoint_t)FindPattern(SIG_SET_JOINT);
    p_SetOffsets  = (SetOffsets_t)FindPattern(SIG_SET_OFFSETS);
    if (auto s = FindPattern(SIG_SIMPLE_MODE)) {
        uint8_t* ins = s + 0x19;                              // F3 0F 11 05 disp32 → eye_weight
        p_eyeWeight = (float*)(ins + 8 + *(int32_t*)(ins + 4));
    }
    p_ActorById   = (ActorById_t)FindPattern(SIG_ACTOR_BY_ID);
    p_ActorPose   = (ActorPose_t)FindPattern(SIG_ACTOR_POSE);
    p_PoseJoints  = (PoseJoints_t)FindPattern(SIG_POSE_JOINTS);
    p_ActorLock   = (ActorLock_t)FindPattern(SIG_ACTOR_LOCK);
    p_ActorUnlock = (ActorUnlock_t)FindPattern(SIG_ACTOR_UNLOCK);
    p_BoneHash    = (BoneHash_t)FindPattern(SIG_BONE_HASH);
    p_BoneFind    = (BoneFind_t)FindPattern(SIG_BONE_FIND);
    Log("rig: BoneHash=%p BoneFind=%p", p_BoneHash, p_BoneFind);
    uint8_t* fEndPosing = FindPattern(SIG_END_POSING);
    Log("pose: EndPosing=%p ActorById=%p GetPose=%p Joints=%p Lock=%p Unlock=%p", fEndPosing, p_ActorById, p_ActorPose, p_PoseJoints, p_ActorLock, p_ActorUnlock);
    p_SetOpacity  = (SetOpacity_t)FindPattern(SIG_SET_OPACITY);
    p_GetComponent = (GetComponent_t)FindPattern(SIG_GET_COMPONENT);
    p_ClientObjMgr = (ClientObjMgr_t)FindPattern(SIG_CLIENT_OBJMGR);
    uint8_t* fSimValid = nullptr;
    if (auto v = FindPattern(SIG_FPS_SIM_VALID)) {
        uint8_t* ins = v + 0x0b;                               // 8B 15 disp32 -> tipo do componente visual
        p_visualCompType = (uint32_t*)(ins + 6 + *(int32_t*)(ins + 2));
        fSimValid = v;
    }
    Log("SetOpacity=%p GetComponent=%p ObjMgr=%p tipoVisual@%p", p_SetOpacity, p_GetComponent, p_ClientObjMgr, p_visualCompType);
    p_ServerConn  = (ServerConn_t)FindPattern(SIG_SERVER_CONN);
    p_ServerCmd   = (ServerCmd_t)FindPattern(SIG_SERVER_CMD);
    Log("FPS_Update=%p SetPose=%p SetParam=%p SetJoint=%p SetOffsets=%p eye_weight@%p",
        fUpdate, fPose, p_FpsSetParam, p_SetJoint, p_SetOffsets, p_eyeWeight);
    Log("ServerConn=%p ServerCmd=%p", p_ServerConn, p_ServerCmd);
    LoadConfig();
    if (!fUpdate || !fPose) { Log("assinatura nao encontrada; abortando"); return 0; }

    if (MH_Initialize() != MH_OK) return 0;
    MH_CreateHook(fUpdate, &hk_FpsUpdate, (void**)&o_FpsUpdate);
    MH_CreateHook(fPose, &hk_SetPose, (void**)&o_SetPose);
    g_setPoseFn = (SetPoseFn_t)o_SetPose;                         // o estéreo usa o SetPose original
    if (fSimValid) MH_CreateHook(fSimValid, &hk_SimValidForFpc, (void**)&o_SimValidForFpc);
    if (auto fCommit = FindPattern(SIG_POSE_COMMIT)) { MH_CreateHook(fCommit, &hk_PoseCommit, (void**)&o_PoseCommit); Log("pose: Pose_Commit=%p", fCommit); }
    else Log("pose: Pose_Commit nao encontrado (sem congelar corpo / sem rig VR)");
    if (auto fDeact = FindPattern(SIG_FPS_DEACTIVATE)) MH_CreateHook(fDeact, &hk_FpsDeactivate, (void**)&o_FpsDeactivate);
    else Log("FPS_Deactivate nao encontrado (o corpo nao reaparece sozinho ao sair da FPC)");
    if (p_FpsSetParam) MH_CreateHook((void*)p_FpsSetParam, &hk_FpsSetParam, (void**)&o_FpsSetParam);
    if (auto fLookUp = FindPattern(SIG_LOOK_UP)) MH_CreateHook(fLookUp, &hk_LookUp, (void**)&o_LookUp);
    else Log("Look_UpdateUp nao encontrado (sem up do mundo no giro do mouse)");
    VrLaserInstallHooks();
    VrCursorInstallHooks();
    VrAvatarInstallHooks();
    VrMenuInstall();
    if (p_ServerCmd) MH_CreateHook((void*)p_ServerCmd, &hk_ServerCmd, (void**)&o_ServerCmd);
    if (auto fAcc = FindPattern(SIG_ACCOUNT_SAVE)) MH_CreateHook(fAcc, &hk_AccountSave, (void**)&o_AccountSave);
    else Log("protecao do salvamento da conta: assinatura nao encontrada");
    XrInstallIfEnabled();
    MH_EnableHook(MH_ALL_HOOKS);
    Log("hooks ativos");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
