// vr_avatar.inl: Fase 1, o Sim do jogador é posicionado pelo CLIENTE, quadro a quadro.
// Incluído por fpc_rig.cpp (depois de vr_grab.inl).
//
// Como o cliente do TS4 posiciona objetos (Op_SetLocation 0x1401f4cd0, tratador da operação de rede
// "SetLocation" que o servidor manda):
//   comp = GetComponent(objeto, TIPO_LOCALIZACAO)          (0x140939280; tipo lido em 0x1401f4d1b)
//   Loc_Begin(comp)                                         (0x1409412e0: abre o lote, comp+0x90 |= 1)
//   Loc_SetTransform(comp, {quat x,y,z,w ; pos x,y,z,w})    (0x140942ae0: comp+0x10 quat, comp+0x30 pos)
//   Loc_Commit(comp)                                        (0x1409421a0: fecha e avisa o cliente -> ator/cena)
// Modo avatar (modo FPS livre): a cada quadro o mod faz essa mesma sequência com a posição da câmera e o
// giro suave do corpo -> o corpo anda liso, sem passos. O servidor continua recebendo fpc.move (validação
// de paredes, posição da simulação); as posições que ele manda de volta para o NOSSO Sim viram só a
// "posição do servidor" (referência), a não ser que sejam um salto grande (escada, porta, o jogo te
// movendo), que é aceito e ressincroniza.

static bool  g_avatarEnabled = true;      // avatar_cliente no ini
static float g_avatarResync  = 1.0f;      // m: diferença acima da qual a posição do servidor é aceita
static const char* SIG_OP_SETLOCATION = "48 8B C4 55 57 41 54 41 56 41 57 48 8D 68 A1 48 81 EC C0 00 00 00 48 C7 45 07";
static const char* SIG_LOC_BEGIN      = "80 89 90 00 00 00 01 C3 CC CC CC CC";
static const char* SIG_LOC_SETXFORM   = "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 0F 10 52";
static const char* SIG_LOC_COMMIT     = "40 53 48 83 EC 20 80 A1 90 00 00 00";
using OpSetLoc_t   = void (__fastcall*)(void* op);
using LocBegin_t   = void (__fastcall*)(void* comp);
using LocSetXf_t   = void (__fastcall*)(void* comp, float* xf);
using LocCommit_t  = void (__fastcall*)(void* comp);
static OpSetLoc_t  o_OpSetLoc = nullptr;
static LocBegin_t  p_LocBegin = nullptr;
static LocSetXf_t  o_LocSetXf = nullptr;      // trampolim (a função é interceptada)
static LocCommit_t p_LocCommit = nullptr;
static uint32_t*   p_locType = nullptr;
static thread_local bool t_inSetLocOp = false;

static void* volatile g_myLocComp = nullptr;  // componente de localização do nosso Sim (cliente)
static volatile bool  g_avatarResyncPending = false;
static float g_serverPos[3] = {}, g_serverQuat[4] = { 0, 0, 0, 1 };
static volatile bool g_haveServerPos = false;
static float g_clientXf[8] = { 0, 0, 0, 1, 0, 0, 0, 1 };   // última transformação que o cliente aplicou
static SRWLOCK g_avatarLock = SRWLOCK_INIT;

static void __fastcall hk_OpSetLoc(void* op) {
    t_inSetLocOp = true;
    o_OpSetLoc(op);
    t_inSetLocOp = false;
}
static void __fastcall hk_LocSetXf(void* comp, float* xf) {
    if (t_inSetLocOp && IsHeldComp(comp)) return;             // Fase 5: objeto na mão, o cliente manda nele
    if (t_inSetLocOp && comp && comp == g_myLocComp && xf) {
        AcquireSRWLockExclusive(&g_avatarLock);
        memcpy(g_serverQuat, xf, 16); memcpy(g_serverPos, xf + 4, 12); g_haveServerPos = true;
        bool active = g_avatarActive;
        float dx = xf[4] - g_clientXf[4], dz = xf[6] - g_clientXf[6], dy = xf[5] - g_clientXf[5];
        bool bigJump = std::sqrt(dx * dx + dz * dz) > g_avatarResync || std::fabs(dy) > 0.5f;
        float mine[8]; memcpy(mine, g_clientXf, 32);
        ReleaseSRWLockExclusive(&g_avatarLock);
        if (active && !bigJump) {
            mine[5] = xf[5];                                  // altura (chão/andar) sempre do servidor
            o_LocSetXf(comp, mine);                           // o corpo fica onde o cliente o pôs
            return;
        }
        if (active && bigJump) {
            static int n = 0;
            if (n++ < 20) Log("avatar: o servidor moveu o Sim %.2f m (aceito e ressincronizado)", std::sqrt(dx * dx + dz * dz));
            g_avatarResyncPending = true;
        }
    }
    o_LocSetXf(comp, xf);
}

static void VrAvatarInstallHooks() {
    uint8_t* op = FindPattern(SIG_OP_SETLOCATION);
    p_LocBegin  = (LocBegin_t)FindPattern(SIG_LOC_BEGIN);
    uint8_t* sx = FindPattern(SIG_LOC_SETXFORM);
    p_LocCommit = (LocCommit_t)FindPattern(SIG_LOC_COMMIT);
    if (op && op[0x4b] == 0x8B && op[0x4c] == 0x15)          // mov edx, [rip+X] -> tipo do componente
        p_locType = (uint32_t*)(op + 0x51 + *(int32_t*)(op + 0x4d));
    bool ok = op && p_LocBegin && sx && p_LocCommit && p_locType && p_GetComponent;
    if (ok) { MH_CreateHook(op, &hk_OpSetLoc, (void**)&o_OpSetLoc); MH_CreateHook(sx, &hk_LocSetXf, (void**)&o_LocSetXf); }
    else g_avatarEnabled = false;
    Log("avatar: Op_SetLocation=%p Begin=%p SetTransform=%p Commit=%p tipo@%p (%s)", op, p_LocBegin, sx, p_LocCommit, p_locType, ok ? "ok" : "DESLIGADO");
}

// Thread principal, depois do UpdateFpsMode (a origem da câmera e a direção do corpo deste quadro).
static void VrAvatarUpdate() {
    bool want = g_avatarEnabled && FpsFree() && g_haveOrigin;
    void* obj = want ? GetSimObject() : nullptr;
    void* comp = (obj && p_locType) ? p_GetComponent(obj, *p_locType) : nullptr;
    // O componente é resolvido de novo a cada quadro: o Sim pode ser recriado (viagem, carregamento,
    // troca de Sim) e um ponteiro de um quadro anterior pode já estar liberado (era a causa dos crashes).
    if (!comp && !want && g_avatarActive) { void* o2 = GetSimObject(); comp = (o2 && p_locType) ? p_GetComponent(o2, *p_locType) : nullptr; }
    g_myLocComp = comp;
    if (!want || !comp) {
        if (g_avatarActive) {                                 // saindo: volta para a posição do servidor
            g_avatarActive = false;
            if (comp && g_haveServerPos) {                    // só se o Sim ainda existe neste quadro
                float xf[8]; AcquireSRWLockShared(&g_avatarLock); memcpy(xf, g_serverQuat, 16); memcpy(xf + 4, g_serverPos, 12); ReleaseSRWLockShared(&g_avatarLock);
                xf[7] = 1.0f;
                p_LocBegin(comp); o_LocSetXf(comp, xf); p_LocCommit(comp);
            }
            Log("avatar: cliente devolveu o Sim ao servidor");
        }
        return;
    }
    // posição atual do componente (antes de mexer): base para a altura e para entrar suave
    const float* cq = (const float*)((uint8_t*)comp + 0x10);
    const float* cp = (const float*)((uint8_t*)comp + 0x30);
    if (!g_avatarActive) {
        g_avatarActive = true; g_avatarResyncPending = false;
        Log("avatar: cliente assumiu a posicao do Sim (pos %.2f %.2f %.2f)", cp[0], cp[1], cp[2]);
    }
    if (g_avatarResyncPending) {                              // o servidor moveu o Sim (portal/escada): a câmera vai junto
        g_avatarResyncPending = false;
        g_origin[0] = g_serverPos[0]; g_origin[1] = g_serverPos[2];
    }
    float y = g_haveServerPos ? g_serverPos[1] : cp[1];
    float a = std::atan2(g_bodyDir[0], g_bodyDir[1]);         // mesma convenção de sims4.math.vector3_angle
    float xf[8] = { 0.0f, std::sin(a * 0.5f), 0.0f, std::cos(a * 0.5f), g_origin[0], y, g_origin[1], 1.0f };
    if (g_bodyDir[0] == 0.0f && g_bodyDir[1] == 0.0f) memcpy(xf, cq, 16);
    AcquireSRWLockExclusive(&g_avatarLock); memcpy(g_clientXf, xf, 32); ReleaseSRWLockExclusive(&g_avatarLock);
    p_LocBegin(comp);
    o_LocSetXf(comp, xf);
    p_LocCommit(comp);
}

// Para a coleira/paredes: onde o servidor acha que o Sim está (com o avatar, o corpo visível é do cliente).
static bool AvatarServerXZ(float& x, float& z) {
    if (!g_avatarActive || !g_haveServerPos) return false;
    AcquireSRWLockShared(&g_avatarLock); x = g_serverPos[0]; z = g_serverPos[2]; ReleaseSRWLockShared(&g_avatarLock);
    return true;
}

static void VrAvatarLoadConfig(const char* path) {
    g_avatarEnabled = GetPrivateProfileIntA("fpc_rig", "avatar_cliente", 1, path) != 0;
    char f[32]; GetPrivateProfileStringA("fpc_rig", "avatar_ressinc", "1.0", f, sizeof(f), path); g_avatarResync = (float)atof(f);
    Log("config: avatar_cliente=%d avatar_ressinc=%.2f", g_avatarEnabled, g_avatarResync);
}
