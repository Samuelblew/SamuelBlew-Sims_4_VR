// vr_objgrab.inl: Fase 5, agarrar objetos pequenos com as mãos (VR). Incluído por fpc_rig.cpp
// (depois de vr_avatar.inl: usa o mesmo caminho de posicionamento do cliente).
//
// Grip perto de algo (e sem Sim perto, que tem prioridade em vr_grab.inl): a DLL pede "fpc.grabobj x y z";
// o Python escolhe o objeto pequeno/carregável mais próximo da palma e grava o id (-1009) ou avisa que não
// há nada (-1010). O cliente segura o objeto: a cada frame, Begin/SetTransform/Commit no componente de
// localização dele com a pose da mão vezes o deslocamento do instante do agarrar (as posições que o
// servidor mandar para ele são ignoradas enquanto estiver na mão). Soltou: "fpc.dropobj" e o servidor
// pousa o objeto no chão do andar, com o giro em volta do eixo vertical.

static bool g_objGrabOn = true;     // agarrar_objetos
struct HeldObj { bool active; uint64_t id; void* comp; V3 relPos; Q relRot; Q lastRot; V3 lastPos; };
static HeldObj g_held[2] = {};
static int  g_objPendingHand = -1;
static ULONGLONG g_objPendingT = 0;

// Objeto do cliente pelo id (mesmo caminho do GetSimObject).
static void* ClientObjectById(uint64_t id) {
    if (!id || !p_ClientObjMgr) return nullptr;
    void* mgr = p_ClientObjMgr();
    if (!mgr) return nullptr;
    void* sub = ((void* (__fastcall*)(void*))(*(void***)mgr)[0x28 / 8])(mgr);
    if (!sub) return nullptr;
    return ((void* (__fastcall*)(void*, uint64_t))(*(void***)sub)[0x18 / 8])(sub, id);
}
static bool IsHeldComp(void* comp) {
    return comp && ((g_held[0].active && g_held[0].comp == comp) || (g_held[1].active && g_held[1].comp == comp));
}

// Thread principal (chamado no VrGrabUpdate: mesma borda de grip).
static void ObjGrabRequest(int h, V3 palm) {
    if (!g_objGrabOn || g_held[h].active || g_objPendingHand >= 0) return;
    g_objPendingHand = h; g_objPendingT = GetTickCount64(); g_objGrabReady = g_objGrabNone = false;
    SendServerCommand("fpc.grabobj %.4f %.4f %.4f", palm.x, palm.y, palm.z);
}

static void VrObjGrabUpdate() {
    if (g_objReqHand >= 0) {                                  // pedido feito pelo VrGrabUpdate nesta borda de grip
        int h = g_objReqHand; g_objReqHand = -1;
        if (g_objGrabOn) ObjGrabRequest(h, v3(g_objReqPalm[0], g_objReqPalm[1], g_objReqPalm[2]));
    }
    if (!g_objGrabOn) return;
    CtrlState c[2]; AcquireSRWLockShared(&g_hmdLock); c[0] = g_ctrl[0]; c[1] = g_ctrl[1]; ReleaseSRWLockShared(&g_hmdLock);
    ULONGLONG now = GetTickCount64();
    // resposta do Python
    if (g_objPendingHand >= 0) {
        int h = g_objPendingHand;
        if (g_objGrabNone || now - g_objPendingT > 800) { g_objPendingHand = -1; g_objGrabNone = false; }
        else if (g_objGrabReady) {
            g_objGrabReady = false; g_objPendingHand = -1;
            std::string s = ReadModsFile("fpc_grab.txt");
            uint64_t id = _strtoui64(s.c_str(), nullptr, 10);
            void* obj = ClientObjectById(id);
            void* comp = (obj && p_locType) ? p_GetComponent(obj, *p_locType) : nullptr;
            if (comp && c[h].gripValid && c[h].squeeze > 0.35f) {
                V3 gp; Q gq; CtrlWorld(c[h].grip, gp, gq);
                const float* oq = (const float*)((uint8_t*)comp + 0x10); const float* op = (const float*)((uint8_t*)comp + 0x30);
                HeldObj& H = g_held[h];
                H.active = true; H.id = id; H.comp = comp;
                H.relPos = QRot(QConj(gq), v3(op[0], op[1], op[2]) - gp);
                H.relRot = QNorm(QMul(QConj(gq), Q{ oq[0], oq[1], oq[2], oq[3] }));
                g_handGrabbing[h] = true;
                XrHaptic(h, 0.5f, 0.05f);
                Log("agarrar: mao %s pegou o objeto %llu", h ? "direita" : "esquerda", (unsigned long long)id);
            }
        }
    }
    // segurando: o objeto segue a mão
    for (int h = 0; h < 2; ++h) {
        HeldObj& H = g_held[h];
        if (!H.active) continue;
        // o objeto pode ter sido apagado/recriado pelo jogo: resolve pelo id a cada quadro
        void* obj = ClientObjectById(H.id);
        void* comp = (obj && p_locType) ? p_GetComponent(obj, *p_locType) : nullptr;
        if (!comp) {
            Log("agarrar: o objeto %llu sumiu da mao %s (apagado pelo jogo)", (unsigned long long)H.id, h ? "direita" : "esquerda");
            H = {}; g_handGrabbing[h] = false;
            continue;
        }
        H.comp = comp;
        bool release = !c[h].gripValid || c[h].squeeze < 0.35f || !g_vrHead;
        if (!release) {
            V3 gp; Q gq; CtrlWorld(c[h].grip, gp, gq);
            H.lastPos = gp + QRot(gq, H.relPos);
            H.lastRot = QNorm(QMul(gq, H.relRot));
            float xf[8] = { H.lastRot.x, H.lastRot.y, H.lastRot.z, H.lastRot.w, H.lastPos.x, H.lastPos.y, H.lastPos.z, 1.0f };
            p_LocBegin(H.comp); o_LocSetXf(H.comp, xf); p_LocCommit(H.comp);
        } else {
            // giro só em volta do eixo vertical (o objeto pousa de pé)
            V3 f = QRot(H.lastRot, v3(0, 0, 1)); float a = std::atan2(f.x, f.z);
            SendServerCommand("fpc.dropobj %.4f %.4f %.4f %.5f %.5f", H.lastPos.x, H.lastPos.y, H.lastPos.z, std::sin(a * 0.5f), std::cos(a * 0.5f));
            XrHaptic(h, 0.3f, 0.04f);
            Log("agarrar: mao %s soltou o objeto %llu", h ? "direita" : "esquerda", (unsigned long long)H.id);
            H = {};
            g_handGrabbing[h] = false;
        }
    }
}

static void VrObjGrabLoadConfig(const char* path) {
    g_objGrabOn = GetPrivateProfileIntA("fpc_rig", "agarrar_objetos", 1, path) != 0;
    Log("config: agarrar_objetos=%d", g_objGrabOn);
}
