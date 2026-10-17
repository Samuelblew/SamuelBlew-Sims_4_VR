// vr_grab.inl: agarrar outros Sims com as mãos (VR). Incluído por fpc_rig.cpp, depois do vr_rig.inl.
//
// Modelo: HIGGS / PLANCK (Skyrim VR), adaptados ao TS4, que não tem física de ragdoll para Sims:
//  - seleção por esfera a partir da palma (HIGGS NearCast): o osso mais próximo entre mão, antebraço e
//    cabeça dos outros Sims, dentro de um raio;
//  - restrição "mole" mão -> osso (HIGGS grab constraint com tau de ator): no instante do agarrar guarda o
//    deslocamento entre a sua mão e o osso; a cada frame o alvo segue a sua mão com uma mola (tau), e o
//    membro do outro Sim vai até ele por IK (o mesmo do nosso Sim), no Pose_Commit dele;
//  - entrada/saída suaves entre a animação do jogo e a pose agarrada (PLANCK blend in/out);
//  - o agarrar tem prioridade sobre o laser (mão direita) e a telinha (mão esquerda) quando há alguém perto;
//  - vibração no controle ao agarrar e soltar.
// É só visual (cliente): a simulação continua mandando no outro Sim (posição, interações).

static bool  g_grabEnabled = true;     // agarrar
static float g_grabRadius  = 0.14f;    // agarrar_raio (m), esfera da palma
static float g_grabTau     = 0.12f;    // agarrar_mola (s): quanto maior, mais "mole" o membro segue a mão
static float g_grabPalmFwd = 0.04f;    // m à frente do centro do grip (palma)

enum GrabPart { GP_NONE = 0, GP_ARM_L, GP_ARM_R, GP_HEAD };

// ---- atores de Sims vistos no Pose_Commit (thread da animação)
struct SeenActor { void* actor; int idx[B_COUNT]; ULONGLONG seen; bool sim; };
static SeenActor g_seen[64];
static int       g_seenN = 0;
static SRWLOCK   g_seenLock = SRWLOCK_INIT;

static void GrabTrackActor(void* actor) {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_seenLock);
    for (int i = 0; i < g_seenN; ++i) if (g_seen[i].actor == actor) { g_seen[i].seen = now; ReleaseSRWLockExclusive(&g_seenLock); return; }
    // novo: substitui o mais antigo se a lista estiver cheia
    int slot = g_seenN < 64 ? g_seenN++ : 0;
    if (slot == 0 && g_seenN == 64) { for (int i = 1; i < 64; ++i) if (g_seen[i].seen < g_seen[slot].seen) slot = i; }
    SeenActor a = {}; a.actor = actor; a.seen = now;
    a.sim = ResolveBonesFor(actor, a.idx, true) && a.idx[B_HEAD] >= 0;
    g_seen[slot] = a;
    ReleaseSRWLockExclusive(&g_seenLock);
}

// Posição de um osso no mundo, protegida (o ator pode ter sumido entre frames).
static bool SafeBonePos(void* actor, int idx, V3& out) {
    if (!actor || idx < 0) return false;
    __try {
        Xf t = {};
        ((void (__fastcall*)(void*, uint32_t, Xf*))(*(void***)actor)[0x48 / 8])(actor, (uint32_t)idx, &t);
        out = { t.p[0], t.p[1], t.p[2] };
        return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- estado de cada mão (0 = esquerda, 1 = direita)
struct GrabState {
    bool  active;          // segurando
    void* actor;
    int   idx[B_COUNT];
    int   part;
    float weight;          // 0..1 (entrada/saída)
    bool  releasing;
    // thread principal -> animação
    V3    handPos; Q handRot;     // pose da nossa mão (palma) no mundo
    V3    wpSpine2, wpNeck, wpClav[2], wpUpper[2];
    V3    theirFwd;
    ULONGLONG tick;
    // só na thread da animação
    bool  started; V3 relPos; Q relRot; V3 target; bool targetInit;
    V3    hingeLocal[2]; bool hingeOk[2];
};
static GrabState g_grab[2] = {};
static SRWLOCK   g_grabLock = SRWLOCK_INIT;

static const char* PartName(int p) { return p == GP_ARM_L ? "braco esquerdo" : p == GP_ARM_R ? "braco direito" : p == GP_HEAD ? "cabeca" : "?"; }

// Thread principal, a cada update da FPC.
static void VrGrabUpdate() {
    static bool gripWas[2] = { false, false };
    bool vr = g_grabEnabled && g_vrEnabled && g_xrRunning && g_vrHead && g_phase == RUNNING;
    CtrlState c[2]; AcquireSRWLockShared(&g_hmdLock); c[0] = g_ctrl[0]; c[1] = g_ctrl[1]; ReleaseSRWLockShared(&g_hmdLock);
    ULONGLONG now = GetTickCount64();
    for (int h = 0; h < 2; ++h) {
        GrabState gs; AcquireSRWLockShared(&g_grabLock); gs = g_grab[h]; ReleaseSRWLockShared(&g_grabLock);
        bool grip = vr && c[h].gripValid && c[h].squeeze > 0.6f;
        bool gripOff = !vr || !c[h].gripValid || c[h].squeeze < 0.35f;
        V3 gp; Q gq;
        if (c[h].gripValid) {
            CtrlWorld(c[h].grip, gp, gq);
            gp = gp + QRot(gq, v3(0, 0, -1)) * g_grabPalmFwd;   // palma, um pouco à frente do centro do grip
        }
        // ---- começar: borda do grip, perto de um osso de outro Sim (e a mão não está em outro uso)
        bool otherUse = (h == 1 && g_laserOn) || (h == 0 && g_uiPanelVisible);
        if (!gs.active && grip && !gripWas[h] && !otherUse && c[h].gripValid) {
            float best = g_grabRadius; SeenActor pick = {}; int pickPart = GP_NONE;
            AcquireSRWLockShared(&g_seenLock);
            int nseen = g_seenN; static SeenActor list[64]; memcpy(list, g_seen, sizeof(SeenActor) * nseen);
            ReleaseSRWLockShared(&g_seenLock);
            for (int i = 0; i < nseen; ++i) {
                const SeenActor& a = list[i];
                if (!a.sim || a.actor == g_simActor || now - a.seen > 300) continue;
                struct { int bone; int part; float r; } cand[] = {
                    { a.idx[B_HAND], GP_ARM_L, 1.0f }, { a.idx[B_HAND + 1], GP_ARM_R, 1.0f },
                    { a.idx[B_FORE], GP_ARM_L, 1.0f }, { a.idx[B_FORE + 1], GP_ARM_R, 1.0f },
                    { a.idx[B_HEAD], GP_HEAD, 1.3f } };
                for (auto& k : cand) {
                    V3 bp; if (!SafeBonePos(a.actor, k.bone, bp)) continue;
                    float d = Len(bp - gp) / k.r;
                    if (d < best) { best = d; pick = a; pickPart = k.part; }
                }
            }
            if (pickPart == GP_NONE && g_objReqHand < 0) {      // ninguém perto: tenta um objeto (Fase 5)
                g_objReqPalm[0] = gp.x; g_objReqPalm[1] = gp.y; g_objReqPalm[2] = gp.z; g_objReqHand = h;
            }
            if (pickPart != GP_NONE) {
                GrabState ng = {}; ng.active = true; ng.actor = pick.actor; memcpy(ng.idx, pick.idx, sizeof(ng.idx));
                ng.part = pickPart; ng.weight = 0.0f;
                AcquireSRWLockExclusive(&g_grabLock); g_grab[h] = ng; ReleaseSRWLockExclusive(&g_grabLock);
                gs = ng; g_handGrabbing[h] = true;
                XrHaptic(h, 0.6f, 0.06f);
                Log("agarrar: mao %s pegou o %s de outro Sim (%p) a %.2f m", h ? "direita" : "esquerda", PartName(pickPart), pick.actor, best);
            }
        }
        gripWas[h] = grip;
        if (!gs.active) { g_handGrabbing[h] = false; continue; }

        // ---- soltar: grip solto (sai suave), ator sumiu, ou a mão foi longe demais (quebra, como no HIGGS)
        V3 shoulder;
        bool alive = false;
        { AcquireSRWLockShared(&g_seenLock); for (int i = 0; i < g_seenN; ++i) if (g_seen[i].actor == gs.actor && now - g_seen[i].seen < 300) alive = true; ReleaseSRWLockShared(&g_seenLock); }
        if (!gs.releasing && (gripOff || !alive)) { gs.releasing = true; XrHaptic(h, 0.3f, 0.04f); Log("agarrar: mao %s soltou", h ? "direita" : "esquerda"); }
        if (!gs.releasing && gs.part != GP_HEAD) {
            int side = gs.part == GP_ARM_R ? 1 : 0;
            if (SafeBonePos(gs.actor, gs.idx[B_UPPER + side], shoulder) && Len(gp - shoulder) > 1.1f) {
                gs.releasing = true; XrHaptic(h, 0.8f, 0.08f); Log("agarrar: mao %s puxou longe demais e soltou", h ? "direita" : "esquerda");
            }
        }
        float step = g_dt / 0.25f;
        gs.weight = gs.releasing ? gs.weight - step : (std::min)(1.0f, gs.weight + step);
        if (gs.weight <= 0.0f && gs.releasing) {
            AcquireSRWLockExclusive(&g_grabLock); g_grab[h] = {}; ReleaseSRWLockExclusive(&g_grabLock);
            g_handGrabbing[h] = false;
            continue;
        }
        // ---- entrada para a animação
        gs.handPos = gp; gs.handRot = gq;
        bool ok = SafeBonePos(gs.actor, gs.idx[B_SPINE2], gs.wpSpine2) && SafeBonePos(gs.actor, gs.idx[B_NECK], gs.wpNeck) &&
                  SafeBonePos(gs.actor, gs.idx[B_CLAV], gs.wpClav[0]) && SafeBonePos(gs.actor, gs.idx[B_CLAV + 1], gs.wpClav[1]) &&
                  SafeBonePos(gs.actor, gs.idx[B_UPPER], gs.wpUpper[0]) && SafeBonePos(gs.actor, gs.idx[B_UPPER + 1], gs.wpUpper[1]);
        if (!ok && !gs.releasing) { gs.releasing = true; }
        V3 tf = v3(g_centerCam[0] - gs.wpSpine2.x, 0.0f, g_centerCam[2] - gs.wpSpine2.z);   // o outro Sim "olha" para nós
        gs.theirFwd = Len(tf) > 1e-3f ? Norm(tf) : v3(0, 0, 1);
        gs.tick = now;
        g_handGrabbing[h] = true;
        AcquireSRWLockExclusive(&g_grabLock);
        GrabState& dst = g_grab[h];
        // mantém o que é da thread da animação (deslocamento, mola, eixo do cotovelo)
        GrabState keep = dst;
        dst = gs;
        dst.started = keep.started; dst.relPos = keep.relPos; dst.relRot = keep.relRot;
        dst.target = keep.target; dst.targetInit = keep.targetInit;
        memcpy(dst.hingeLocal, keep.hingeLocal, sizeof(dst.hingeLocal)); memcpy(dst.hingeOk, keep.hingeOk, sizeof(dst.hingeOk));
        ReleaseSRWLockExclusive(&g_grabLock);
    }
}

// Thread da animação: Pose_Commit de um ator que não é o nosso Sim.
static void VrGrabApply(void* actor, float* joints, int n) {
    if (GetTickCount64() - g_grab[0].tick > 300 && GetTickCount64() - g_grab[1].tick > 300) return;
    for (int h = 0; h < 2; ++h) {
        AcquireSRWLockExclusive(&g_grabLock);
        GrabState& g = g_grab[h];
        if (!g.active || g.actor != actor || g.weight <= 0.0f || GetTickCount64() - g.tick > 300) { ReleaseSRWLockExclusive(&g_grabLock); continue; }
        RigInput in = {};
        memcpy(in.idx, g.idx, sizeof(in.idx));
        in.wpSpine2 = g.wpSpine2; in.wpNeck = g.wpNeck;
        for (int s = 0; s < 2; ++s) { in.wpClav[s] = g.wpClav[s]; in.wpUpper[s] = g.wpUpper[s]; }
        in.wpOk = true; in.quiet = true; in.weight = g.weight; in.bodyFwd = g.theirFwd;
        in.hingeLocal = g.hingeLocal; in.hingeOk = g.hingeOk;
        if (!BuildWorldFrames(in, joints, n)) { ReleaseSRWLockExclusive(&g_grabLock); continue; }
        if (g.part == GP_ARM_L || g.part == GP_ARM_R) {
            int s = g.part == GP_ARM_R ? 1 : 0;
            float* lU = JointAt(joints, n, g.idx[B_UPPER + s]); float* lF = JointAt(joints, n, g.idx[B_FORE + s]); float* lH = JointAt(joints, n, g.idx[B_HAND + s]);
            if (!lU || !lF || !lH) { ReleaseSRWLockExclusive(&g_grabLock); continue; }
            WX wU = WCompose(in.clav[s], lU), wF = WCompose(wU, lF), wH = WCompose(wF, lH);
            if (!g.started) {                      // deslocamento mão -> pulso dele no instante do agarrar
                g.relPos = QRot(QConj(g.handRot), wH.p - g.handPos);
                g.relRot = QNorm(QMul(QConj(g.handRot), wH.q));
                g.target = wH.p; g.targetInit = true; g.started = true;
            }
            V3 want = g.handPos + QRot(g.handRot, g.relPos);
            float a = 1.0f - std::exp(-g_dt / (std::max)(0.01f, g_grabTau));   // mola (tau)
            g.target = g.target + (want - g.target) * a;
            in.arm[s] = true; in.wrist[s] = g.target;
            in.handQuatOn[s] = true; in.handQuat[s] = QNorm(QMul(g.handRot, g.relRot));
            ReleaseSRWLockExclusive(&g_grabLock);
            SolveArm(in, s, joints, n);
        } else if (g.part == GP_HEAD) {
            float* lN = JointAt(joints, n, g.idx[B_NECK]); float* lHd = JointAt(joints, n, g.idx[B_HEAD]);
            if (!lN || !lHd) { ReleaseSRWLockExclusive(&g_grabLock); continue; }
            WX wN = WCompose(in.spine2, lN), wHd = WCompose(wN, lHd);
            if (!g.started) { g.relPos = QRot(QConj(g.handRot), wHd.p - g.handPos); g.target = wHd.p; g.started = true; }
            V3 want = g.handPos + QRot(g.handRot, g.relPos);
            float a = 1.0f - std::exp(-g_dt / (std::max)(0.01f, g_grabTau));
            g.target = g.target + (want - g.target) * a;
            V3 tgt = g.target; float w = g.weight;
            ReleaseSRWLockExclusive(&g_grabLock);
            // gira o pescoço para levar a cabeça em direção ao alvo (até 35°)
            V3 cur = wHd.p - wN.p, des = tgt - wN.p;
            if (Len(cur) > 1e-3f && Len(des) > 1e-3f) {
                Q r = QFromTo(cur, des);
                float ang = QAngle(r), lim = 35.0f * 0.0174533f;
                if (ang > lim) r = QSlerp(Q{ 0, 0, 0, 1 }, r, lim / ang);
                Q neckW = QNorm(QMul(r, wN.q));
                WriteLQ(lN, QNorm(QMul(QConj(in.spine2.q), neckW)), w);
            }
        } else ReleaseSRWLockExclusive(&g_grabLock);
    }
}

static void VrGrabLoadConfig(const char* path) {
    char f[32];
    g_grabEnabled = GetPrivateProfileIntA("fpc_rig", "agarrar", 1, path) != 0;
    GetPrivateProfileStringA("fpc_rig", "agarrar_raio", "0.14", f, sizeof(f), path); g_grabRadius = (float)atof(f);
    GetPrivateProfileStringA("fpc_rig", "agarrar_mola", "0.12", f, sizeof(f), path); g_grabTau = (float)atof(f);
    Log("config: agarrar=%d agarrar_raio=%.2f agarrar_mola=%.2f", g_grabEnabled, g_grabRadius, g_grabTau);
}
