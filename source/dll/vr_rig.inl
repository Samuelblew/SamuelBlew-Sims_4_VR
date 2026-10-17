// vr_rig.inl: o esqueleto do Sim segue o VR (fase V6). Incluído por fpc_rig.cpp, antes do hk_EndPosing.
//
// - Braços: IK de dois ossos (UpperArm/Forearm) até o pulso do controle; a mão copia a rotação do
//   controle; metade da torção vai para o ForearmTwist.
// - Dedos: gatilho fecha o indicador, grip fecha médio/anelar/mindinho.
// - Cabeça: Neck + Head giram para a orientação do headset (limitado em relação ao tronco).
//
// Divisão por threads:
//   thread principal (update da FPC): índices dos ossos, transformações no mundo dos pais fixos
//     (Spine2, Clavicles) via ator->vf[0x48], alvos dos controles e da cabeça -> g_rigIn (com lock);
//   thread da animação (EndPosingState): FK a partir desses pais com as juntas locais deste frame,
//     IK e escrita das rotações locais na pose.
// Nada aqui depende dos eixos internos dos ossos: as direções locais são medidas nas próprias
// juntas (posições dos filhos), e o plano do cotovelo vem da própria animação.

// ---------------------------------------------------------------- matemática (quaternions x,y,z,w)
struct V3 { float x, y, z; };
static inline V3 v3(float x, float y, float z) { return { x, y, z }; }
static inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }
static inline V3 Norm(V3 a) { float l = Len(a); return l > 1e-8f ? a * (1.0f / l) : v3(0, 0, 0); }

struct Q { float x, y, z, w; };
static inline Q QMul(Q a, Q b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static inline Q QConj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
static inline Q QNorm(Q a) { float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w); return l > 1e-8f ? Q{ a.x / l, a.y / l, a.z / l, a.w / l } : Q{ 0, 0, 0, 1 }; }
static inline V3 QRot(Q q, V3 v) {
    V3 u = { q.x, q.y, q.z };
    V3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}
static inline Q QAxisAngle(V3 axis, float ang) { V3 a = Norm(axis); float s = std::sin(ang * 0.5f); return { a.x * s, a.y * s, a.z * s, std::cos(ang * 0.5f) }; }
static inline float QAngle(Q q) { float w = std::fabs(q.w); if (w > 1.0f) w = 1.0f; return 2.0f * std::acos(w); }
static Q QSlerp(Q a, Q b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0f) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
    if (d > 0.9995f) return QNorm({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1.0f - t) * th) / s, wb = std::sin(t * th) / s;
    return { a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
}
// Menor rotação que leva a direção a em b.
static Q QFromTo(V3 a, V3 b) {
    a = Norm(a); b = Norm(b);
    float d = Dot(a, b);
    if (d < -0.9999f) {
        V3 ax = Cross(v3(1, 0, 0), a);
        if (Len(ax) < 1e-4f) ax = Cross(v3(0, 1, 0), a);
        return QAxisAngle(ax, 3.14159265f);
    }
    V3 c = Cross(a, b);
    return QNorm({ c.x, c.y, c.z, 1.0f + d });
}
// Quaternion de uma matriz de rotação dada por colunas (c0, c1, c2).
static Q QFromCols(V3 c0, V3 c1, V3 c2) {
    float m00 = c0.x, m11 = c1.y, m22 = c2.z, tr = m00 + m11 + m22;
    Q q;
    if (tr > 0.0f) { float s = std::sqrt(tr + 1.0f) * 2.0f; q = { (c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, 0.25f * s }; }
    else if (m00 > m11 && m00 > m22) { float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f; q = { 0.25f * s, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s }; }
    else if (m11 > m22) { float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f; q = { (c1.x + c0.y) / s, 0.25f * s, (c2.y + c1.z) / s, (c2.x - c0.z) / s }; }
    else { float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f; q = { (c2.x + c0.z) / s, (c2.y + c1.z) / s, 0.25f * s, (c0.y - c1.x) / s }; }
    return QNorm(q);
}
// Base ortonormal a partir de uma direção principal e uma secundária.
static void Basis(V3 a, V3 b, V3& e0, V3& e1, V3& e2) {
    e0 = Norm(a);
    e2 = Norm(Cross(e0, b));
    e1 = Cross(e2, e0);
}
// Rotação que leva o par (a0, b0) ao par (a1, b1): a0 -> a1 exato, b0 -> b1 no possível.
static Q QFromPairs(V3 a0, V3 b0, V3 a1, V3 b1) {
    V3 x0, y0, z0, x1, y1, z1;
    Basis(a0, b0, x0, y0, z0); Basis(a1, b1, x1, y1, z1);
    Q r0 = QFromCols(x0, y0, z0), r1 = QFromCols(x1, y1, z1);
    return QNorm(QMul(r1, QConj(r0)));
}
// Componente de torção de q em volta do eixo (unitário).
static Q QTwist(Q q, V3 axis) {
    V3 r = { q.x, q.y, q.z };
    V3 p = axis * Dot(r, axis);
    Q t = { p.x, p.y, p.z, q.w };
    float l = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z + t.w * t.w);
    return l > 1e-6f ? Q{ t.x / l, t.y / l, t.z / l, t.w / l } : Q{ 0, 0, 0, 1 };
}

// Junta: 0x30 bytes = posição (vec4) · rotação (quat) · escala (vec4). Igual para o local (pose) e o
// mundo (ator->vf[0x48]).
struct alignas(16) Xf { float p[4]; float q[4]; float s[4]; };
static inline V3 XP(const Xf& t) { return { t.p[0], t.p[1], t.p[2] }; }
static inline Q  XQ(const Xf& t) { return { t.q[0], t.q[1], t.q[2], t.q[3] }; }
struct WX { V3 p; Q q; V3 s; };
static inline WX WFrom(const Xf& t) { return { XP(t), XQ(t), { t.s[0], t.s[1], t.s[2] } }; }
// mundo(filho) = mundo(pai) ∘ local(filho)
static inline WX WCompose(const WX& par, const float* loc) {
    V3 lp = { loc[0] * par.s.x, loc[1] * par.s.y, loc[2] * par.s.z };
    WX o;
    o.p = par.p + QRot(par.q, lp);
    o.q = QNorm(QMul(par.q, Q{ loc[4], loc[5], loc[6], loc[7] }));
    o.s = { par.s.x * loc[8], par.s.y * loc[9], par.s.z * loc[10] };
    return o;
}
static inline Q  LQ(const float* loc) { return { loc[4], loc[5], loc[6], loc[7] }; }
static inline V3 LP(const float* loc) { return { loc[0], loc[1], loc[2] }; }
static inline void SetLQ(float* loc, Q q) { loc[4] = q.x; loc[5] = q.y; loc[6] = q.z; loc[7] = q.w; }

// ---------------------------------------------------------------- configuração (fpc_rig.ini)
static int   g_rigArms      = 1;      // vr_bracos: 0 = não, 1 = fora de interações, 2 = sempre
static bool  g_rigHead      = true;   // vr_cabeca
static bool  g_rigFingers   = true;   // vr_dedos
static float g_rigWristBack = 0.07f;  // vr_mao_recuo (m): pulso atrás do centro do grip
static float g_rigHandPitch = 0.0f;   // vr_mao_inclinacao (graus): gira a mão em volta do eixo lateral do controle
static float g_rigFingerSign = 1.0f;  // vr_dedos_sinal: -1 se os dedos fecharem para trás
static float g_rigHeadLimit = 80.0f;  // vr_cabeca_limite (graus)
static float g_rigThumbSign = 1.0f;   // vr_polegar_sinal: -1 se o polegar fechar para fora

// Fase 2 (corpo inteiro)
static bool  g_legsOn = true, g_crouchOn = true, g_leanOn = true;   // vr_pernas, vr_agachar, vr_inclinar
static float g_stepDist = 0.18f;    // passo_distancia (m): pé longe do ideal mais que isso -> passo
static float g_stepLift = 0.08f;    // passo_altura (m)
static float g_leanMax  = 0.25f;    // m de inclinação; além disso o corpo vai junto (andar no quarto)

// ---------------------------------------------------------------- ossos
enum RigBone {
    B_ROOT, B_ROOTBIND, B_SPINE0, B_SPINE1,
    B_SPINE2, B_NECK, B_HEAD, B_GLASSES, B_LEYEAREA, B_REYEAREA,
    B_CLAV, B_UPPER = B_CLAV + 2, B_FORE = B_UPPER + 2, B_HAND = B_FORE + 2, B_TWIST = B_HAND + 2,
    B_FINGER = B_TWIST + 2,            // [mão][dedo 0..4: Thumb Index Mid Ring Pinky][junta 0..2]
    B_PELVIS = B_FINGER + 2 * 5 * 3,
    B_THIGH = B_PELVIS + 1, B_CALF = B_THIGH + 2, B_FOOT = B_CALF + 2,
    B_COUNT = B_FOOT + 2
};
static const char* kFingerNames[5] = { "Thumb", "Index", "Mid", "Ring", "Pinky" };
static int FingerBone(int h, int f, int j) { return B_FINGER + (h * 5 + f) * 3 + j; }

using BoneHash_t  = uint32_t (__fastcall*)(const char* name);
using BoneFind_t  = uint32_t (__fastcall*)(void* skeleton, uint32_t hash);
static const char* SIG_BONE_HASH = "48 85 C9 74 ?? 0F B6 01 48 8D 51 01 41 B8 C5 9D 1C 81";
static const char* SIG_BONE_FIND = "48 83 EC 18 44 8B C2 48 B8 C5 4E EC C4 4E EC C4 4E";
static BoneHash_t p_BoneHash = nullptr;
static BoneFind_t p_BoneFind = nullptr;

static void*  g_rigActor = nullptr;            // ator para o qual os índices valem
static int    g_rigIdx[B_COUNT];
static bool   g_rigIdxOk = false;

// Índices dos ossos usados pelo rig num ator qualquer (o nosso Sim ou outro). true = esqueleto de Sim completo.
static bool ResolveBonesFor(void* actor, int* idx, bool quiet) {
    if (!actor || !p_BoneHash || !p_BoneFind) return false;
    void* rig = ((void* (__fastcall*)(void*))(*(void***)actor)[0x20 / 8])(actor);
    if (!rig) return false;
    void* skel = (uint8_t*)rig + 0x48;
    char name[64];
    auto find = [&](int slot, const char* n) {
        uint32_t i = p_BoneFind(skel, p_BoneHash(n));
        idx[slot] = (i == 0xffffffffu) ? -1 : (int)i;
        if (idx[slot] < 0 && !quiet) Log("rig: osso %s nao encontrado", n);
    };
    find(B_ROOT, "b__ROOT__"); find(B_ROOTBIND, "b__ROOT_bind__"); find(B_SPINE0, "b__Spine0__"); find(B_SPINE1, "b__Spine1__");
    find(B_SPINE2, "b__Spine2__"); find(B_PELVIS, "b__Pelvis__"); find(B_NECK, "b__Neck__"); find(B_HEAD, "b__Head__");
    find(B_GLASSES, "b__CAS_Glasses__"); find(B_LEYEAREA, "b__CAS_L_EyeArea__"); find(B_REYEAREA, "b__CAS_R_EyeArea__");
    for (int h = 0; h < 2; ++h) {
        const char* s = h == 0 ? "L" : "R";
        snprintf(name, sizeof(name), "b__%s_Clavicle__", s);    find(B_CLAV + h, name);
        snprintf(name, sizeof(name), "b__%s_UpperArm__", s);    find(B_UPPER + h, name);
        snprintf(name, sizeof(name), "b__%s_Forearm__", s);     find(B_FORE + h, name);
        snprintf(name, sizeof(name), "b__%s_Hand__", s);        find(B_HAND + h, name);
        snprintf(name, sizeof(name), "b__%s_ForearmTwist__", s); find(B_TWIST + h, name);
        for (int f = 0; f < 5; ++f) for (int j = 0; j < 3; ++j) {
            snprintf(name, sizeof(name), "b__%s_%s%d__", s, kFingerNames[f], j);
            find(FingerBone(h, f, j), name);
        }
        snprintf(name, sizeof(name), "b__%s_Thigh__", s); find(B_THIGH + h, name);
        snprintf(name, sizeof(name), "b__%s_Calf__", s);  find(B_CALF + h, name);
        snprintf(name, sizeof(name), "b__%s_Foot__", s);  find(B_FOOT + h, name);
    }
    int must[] = { B_ROOT, B_ROOTBIND, B_SPINE0, B_SPINE1, B_NECK, B_SPINE2, B_CLAV, B_CLAV + 1, B_UPPER, B_UPPER + 1, B_FORE, B_FORE + 1, B_HAND, B_HAND + 1 };
    for (int m : must) if (idx[m] < 0) return false;
    return true;
}
static bool ResolveBones(void* actor) {
    if (actor == g_rigActor) return g_rigIdxOk;
    g_rigActor = actor;
    g_rigIdxOk = ResolveBonesFor(actor, g_rigIdx, false);
    Log("rig: ossos resolvidos (ok=%d) Spine2=%d Neck=%d Head=%d L: Clav=%d Upper=%d Fore=%d Hand=%d R: Clav=%d Upper=%d Fore=%d Hand=%d",
        g_rigIdxOk, g_rigIdx[B_SPINE2], g_rigIdx[B_NECK], g_rigIdx[B_HEAD], g_rigIdx[B_CLAV], g_rigIdx[B_UPPER], g_rigIdx[B_FORE],
        g_rigIdx[B_HAND], g_rigIdx[B_CLAV + 1], g_rigIdx[B_UPPER + 1], g_rigIdx[B_FORE + 1], g_rigIdx[B_HAND + 1]);
    return g_rigIdxOk;
}

static bool BoneWorld(void* actor, int idx, WX& out) {
    if (idx < 0) return false;
    Xf t = {};
    ((void (__fastcall*)(void*, uint32_t, Xf*))(*(void***)actor)[0x48 / 8])(actor, (uint32_t)idx, &t);
    out = WFrom(t);
    float ql = out.q.x * out.q.x + out.q.y * out.q.y + out.q.z * out.q.z + out.q.w * out.q.w;
    return ql > 0.5f && ql < 1.5f;
}

// ---------------------------------------------------------------- entrada da thread principal
struct RigInput {
    bool active;                   // aplicar algo neste frame
    float weight;                  // 0..1 (entrada/saída suave)
    int   idx[B_COUNT];
    WX    spine2, clav[2];         // mundo dos pais fixos (montado na thread da animação)
    V3    wpSpine2, wpNeck, wpClav[2], wpUpper[2];   // posições no mundo (vf[0x48]), da thread principal
    V3*   hingeLocal;              // eixo do cotovelo aprendido (null = o do nosso Sim)
    bool* hingeOk;
    bool  handQuatOn[2]; Q handQuat[2];   // rotação da mão no mundo, direta (outros atores)
    bool  quiet;                   // sem diagnóstico (outros atores)
    bool  arm[2];                  // braço com alvo válido
    V3    wrist[2];                // alvo do pulso (mundo)
    V3    handFwd[2], handThumb[2];  // rotação da mão como base no mundo (frente dos dedos, lado do polegar)
    float curlIndex[2], curlOthers[2], curlThumb[2];
    bool  head;
    V3    headFwd, headLeft;       // direções da cabeça (mundo)
    V3    bodyFwd;                 // frente do corpo (horizontal), para o polo do cotovelo
    bool  wpOk;                    // posições wp* medidas neste frame
    V3    bodyOff;                 // deslocamento visual do corpo (mundo, horizontal) a aplicar
    V3    measOff;                 // deslocamento que estava aplicado quando wp* foi medido
    // Fase 2: corpo inteiro
    bool  wpLower; V3 wpRoot, wpSpine0, wpThigh[2];   // encaixe pelo corpo de baixo (a inclinação não mexe)
    V3    lean;                    // deslocamento horizontal da cabeça em relação ao corpo (mundo), já limitado
    bool  legs;                    // pés plantados + passos procedurais
    WX    pelvis; Q fitR; V3 rootW;   // saídas do encaixe
    ULONGLONG tick;
};
// Corpo deslizando sob a câmera (modo FPS livre): o servidor continua movendo o Sim de verdade em
// passos (fpc.move, validados no navmesh), e o cliente desloca a raiz do esqueleto para o corpo
// aparecer exatamente embaixo da câmera. Em interações (portas, escadas, objetos) o deslocamento volta
// a zero em ~0,25 s e a animação do jogo roda na posição real.
static bool  g_bodySmooth = true;              // corpo_suave no ini
static volatile bool g_avatarActive = false;   // Fase 1: o cliente posiciona o Sim (vr_avatar.inl); sem deslocamento visual
static float g_bodyBlend = 0.0f;               // 0..1
static V3    g_bodyOffMain = { 0, 0, 0 };      // último alvo (thread principal)
static SRWLOCK g_bodyLock = SRWLOCK_INIT;
static V3    g_bodyOffApplied = { 0, 0, 0 };   // o que a última Pose_Commit aplicou (thread da animação)
static V3 BodyOffApplied() { V3 v; AcquireSRWLockShared(&g_bodyLock); v = g_bodyOffApplied; ReleaseSRWLockShared(&g_bodyLock); return v; }
static RigInput g_rigIn = {};
static SRWLOCK  g_rigLock = SRWLOCK_INIT;

// Pose de um controle no mundo do jogo (mesma transformação dos olhos no estéreo síncrono).
static bool CtrlWorld(const XrPosef& p, V3& pos, Q& rot) {
    float d[3] = { (p.position.x - g_centerHead[0]) * g_centerScale,
                   (p.position.y - g_centerHead[1]) * g_centerScale,
                   (p.position.z - g_centerHead[2]) * g_centerScale };
    float dw[3]; RotYXR(g_centerYaw, d, dw);
    pos = { g_centerCam[0] + dw[0], g_centerCam[1] + dw[1], g_centerCam[2] + dw[2] };
    Q yaw = QAxisAngle(v3(0, 1, 0), g_centerYaw);
    rot = QNorm(QMul(yaw, Q{ p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w }));
    return true;
}

static float g_rigWeight = 0.0f;
static int  g_rigTestKey = VK_OEM_1; // tecla_teste_bracos (padrao CEDILHA = Ç do ABNT2)
static bool g_rigTest = false;      // pose de teste (braços esticados para a frente), sem precisar do VR

// Thread principal, depois do update da FPC (a câmera central deste frame já está em g_centerCam).
static void VrRigUpdate(void* actor, bool walkMode, bool busy) {
    RigInput in = {};
    bool testKey = Pressed(g_rigTestKey);
    if (g_rigTestKey == VK_NEXT && Pressed(VK_NUMPAD3)) testKey = true;
    if (testKey && GameHasFocus()) { g_rigTest = !g_rigTest; Log("pose de teste dos bracos = %d", g_rigTest); }
    bool vrOk = g_vrEnabled && g_xrRunning && g_vrHead && g_phase == RUNNING && actor;
    // Liga junto com o modo FPS (INSERT). vr_bracos=1: só fora de interações; 2: também nas interações.
    bool wantArms = (vrOk && walkMode && g_rigArms > 0 && (!busy || g_rigArms == 2)) || (g_rigTest && g_phase == RUNNING && actor);
    bool wantHead = vrOk && walkMode && g_rigHead;
    float target = (wantArms || wantHead) ? 1.0f : 0.0f;
    float step = g_dt / 0.25f;
    g_rigWeight = target > g_rigWeight ? (std::min)(target, g_rigWeight + step) : (std::max)(target, g_rigWeight - step);

    // Deslocamento visual do corpo: alvo = origem da câmera - posição real do peito (já sem o deslocamento).
    {
        bool want = g_bodySmooth && FpsFree() && actor && !g_avatarActive;
        g_bodyBlend = want ? (std::min)(1.0f, g_bodyBlend + g_dt / 0.25f) : (std::max)(0.0f, g_bodyBlend - g_dt / 0.25f);
        if (want) {
            V3 tgt = { g_origin[0] - g_anchor[0], 0.0f, g_origin[1] - g_anchor[2] };
            float l = Len(tgt), lim = 1.0f;
            if (l > lim) tgt = tgt * (lim / l);
            g_bodyOffMain = tgt;
        }
        if (g_bodyBlend <= 0.0f) g_bodyOffMain = { 0, 0, 0 };
        in.bodyOff = g_bodyOffMain * g_bodyBlend;
        if (g_crouchOn && g_vrHead && FpsFree()) {               // agachar: altura do headset vs. em pé (recentrar)
            float ws = g_worldScale > 0.05f ? 1.0f / g_worldScale : 1.0f;
            float c = (g_hmdRefPos[1] - g_hmdPose.pos[1]) * ws;
            c = c < 0.04f ? 0.0f : (std::min)(0.75f, c - 0.04f);
            static float cs = 0.0f; cs += (c - cs) * (1.0f - std::exp(-12.0f * g_dt));
            in.bodyOff.y = -cs;
        }
        in.measOff = BodyOffApplied();
    }
    bool bodyOn = Len(in.bodyOff) > 1e-4f || Len(in.measOff) > 1e-4f;
    if ((g_rigWeight > 0.0f || bodyOn) && actor && ResolveBones(actor)) {
        memcpy(in.idx, g_rigIdx, sizeof(in.idx));
        WX t0, t1, t2, t3, t4, t5;
        bool ok = BoneWorld(actor, g_rigIdx[B_SPINE2], t0) && BoneWorld(actor, g_rigIdx[B_NECK], t1) &&
                  BoneWorld(actor, g_rigIdx[B_CLAV], t2) && BoneWorld(actor, g_rigIdx[B_CLAV + 1], t3) &&
                  BoneWorld(actor, g_rigIdx[B_UPPER], t4) && BoneWorld(actor, g_rigIdx[B_UPPER + 1], t5);
        in.wpSpine2 = t0.p; in.wpNeck = t1.p; in.wpClav[0] = t2.p; in.wpClav[1] = t3.p; in.wpUpper[0] = t4.p; in.wpUpper[1] = t5.p;
        in.wpOk = ok;
        {   // Fase 2: referências do corpo de baixo
            WX r0, r1, r2, r3;
            in.wpLower = ok && g_rigIdx[B_PELVIS] >= 0 && BoneWorld(actor, g_rigIdx[B_ROOT], r0) && BoneWorld(actor, g_rigIdx[B_SPINE0], r1) &&
                         BoneWorld(actor, g_rigIdx[B_THIGH], r2) && BoneWorld(actor, g_rigIdx[B_THIGH + 1], r3);
            if (in.wpLower) { in.wpRoot = r0.p; in.wpSpine0 = r1.p; in.wpThigh[0] = r2.p; in.wpThigh[1] = r3.p; }
        }
        in.clav[0].p = t2.p; in.clav[1].p = t3.p; in.spine2.p = t0.p;   // (diagnóstico / teste antes da FK)
        float lookDir[4] = { 0, 0, 1, 0 };
        if (g_vrHead) memcpy(lookDir, g_vrFwd, 16); else MouseLookDir(lookDir);
        V3 bf = { lookDir[0], 0.0f, lookDir[2] };
        in.bodyFwd = Len(bf) > 1e-3f ? Norm(bf) : v3(0, 0, 1);
        CtrlState c[2];
        AcquireSRWLockShared(&g_hmdLock); c[0] = g_ctrlRaw[0]; c[1] = g_ctrlRaw[1]; ReleaseSRWLockShared(&g_hmdLock);   // dedos: botões físicos
        if (ok && wantArms && g_rigTest) {
            for (int h = 0; h < 2; ++h) {
                in.wrist[h] = in.clav[h].p + in.bodyFwd * 0.55f + v3(0, -0.05f, 0);
                in.handFwd[h] = in.bodyFwd; in.handThumb[h] = v3(0, 1, 0);
                in.curlIndex[h] = in.curlOthers[h] = in.curlThumb[h] = 0.0f;
                in.arm[h] = true;
            }
        } else if (ok && wantArms) {
            for (int h = 0; h < 2; ++h) {
                if (!c[h].gripValid || !c[h].aimValid) continue;
                V3 gp, ap; Q gq, aq;
                CtrlWorld(c[h].grip, gp, gq);
                CtrlWorld(c[h].aim, ap, aq);
                if (g_rigHandPitch != 0.0f) aq = QNorm(QMul(aq, QAxisAngle(v3(1, 0, 0), g_rigHandPitch * 0.0174533f)));
                in.handFwd[h]   = QRot(aq, v3(0, 0, -1));
                in.handThumb[h] = QRot(aq, v3(0, 1, 0));
                in.wrist[h]     = gp - in.handFwd[h] * g_rigWristBack;
                in.curlIndex[h] = g_rigFingers ? c[h].trigger : 0.0f;
                in.curlOthers[h] = g_rigFingers ? c[h].squeeze : 0.0f;
                {   // polegar: encostou = fecha (suavizado, ~0,1 s)
                    static float thumb[2] = {};
                    float tgt = (g_rigFingers && c[h].thumbTouch) ? 1.0f : 0.0f;
                    thumb[h] += (tgt - thumb[h]) * (std::min)(1.0f, g_dt / 0.08f);
                    in.curlThumb[h] = thumb[h];
                }
                in.arm[h] = true;
            }
        }
        if (ok && wantHead) {
            float hq[4] = { 0, 0, 0, 1 }; bool hv;
            AcquireSRWLockShared(&g_hmdLock); memcpy(hq, g_hmdPose.quat, 16); hv = g_hmdPose.valid; ReleaseSRWLockShared(&g_hmdLock);
            if (hv) {
                Q q = QNorm(QMul(QAxisAngle(v3(0, 1, 0), g_centerYaw), Q{ hq[0], hq[1], hq[2], hq[3] }));
                in.headFwd = QRot(q, v3(0, 0, -1));
                in.headLeft = QRot(q, v3(-1, 0, 0));
                in.head = true;
            }
        }
        // Fase 2: inclinar / andar no quarto / pernas (só no modo FPS livre, em VR)
        if (ok && vrOk && walkMode && !busy) {
            V3 off = { g_vrOffset[0], 0.0f, g_vrOffset[2] };       // cabeça em relação ao corpo (mundo)
            float d = Len(off);
            if (d > g_leanMax && g_avatarActive) {                  // andar no quarto: o corpo vai junto
                V3 ex = off * ((d - g_leanMax) / d);
                g_origin[0] += ex.x; g_origin[1] += ex.z;
                float exr[3] = { ex.x, 0.0f, ex.z }, exx[3];
                RotYXR(-g_vrYaw, exr, exx);                         // mundo -> espaço do headset
                float ws = g_worldScale > 0.05f ? g_worldScale : 1.0f;
                g_hmdRefPos[0] += exx[0] * ws; g_hmdRefPos[2] += exx[2] * ws;
                off = off * (g_leanMax / d);
            } else if (d > g_leanMax) off = off * (g_leanMax / d);
            if (g_leanOn) in.lean = off;
            in.legs = g_legsOn && in.wpLower;
        }
        in.active = ok && g_rigWeight > 0.0f && (in.arm[0] || in.arm[1] || in.head || in.legs || Len(in.lean) > 0.01f);
        static ULONGLONG lastD = 0;
        if (GetTickCount64() - lastD > 3000) {
            lastD = GetTickCount64();
            Log("rig: update ok=%d ativo=%d peso=%.2f bracos(E=%d D=%d) cabeca=%d teste=%d camera=(%.3f %.3f %.3f) pulsoE=(%.3f %.3f %.3f)",
                ok, in.active, g_rigWeight, in.arm[0], in.arm[1], in.head, g_rigTest, g_centerCam[0], g_centerCam[1], g_centerCam[2],
                in.wrist[0].x, in.wrist[0].y, in.wrist[0].z);
        }
    }
    in.weight = g_rigWeight;
    in.tick = GetTickCount64();
    AcquireSRWLockExclusive(&g_rigLock); g_rigIn = in; ReleaseSRWLockExclusive(&g_rigLock);
}

// ---------------------------------------------------------------- thread da animação
static float* JointAt(float* joints, int n, int idx) { return (idx >= 0 && idx < n) ? joints + idx * 12 : nullptr; }

// Escreve a rotação local nova, misturando com a da animação pelo peso.
static void WriteLQ(float* loc, Q q, float w) {
    Q a = LQ(loc);
    SetLQ(loc, w >= 0.999f ? QNorm(q) : QSlerp(a, QNorm(q), w));
}

static ULONGLONG g_armDiagT[2] = {};
static bool ArmDiag(int h) { ULONGLONG now = GetTickCount64(); if (now - g_armDiagT[h] < 3000) return false; g_armDiagT[h] = now; return true; }

static V3  g_hingeLocal[2] = {};   // eixo do cotovelo no espaço local do UpperArm (aprendido da animação)
static bool g_hingeOk[2] = { false, false };

static void SolveArm(const RigInput& in, int h, float* joints, int n) {
    V3* hingeLocal = in.hingeLocal ? in.hingeLocal : g_hingeLocal;
    bool* hingeOk = in.hingeOk ? in.hingeOk : g_hingeOk;
    float* lU = JointAt(joints, n, in.idx[B_UPPER + h]);
    float* lF = JointAt(joints, n, in.idx[B_FORE + h]);
    float* lH = JointAt(joints, n, in.idx[B_HAND + h]);
    if (!lU || !lF || !lH) return;
    // FK da animação deste frame.
    WX wU = WCompose(in.clav[h], lU), wF = WCompose(wU, lF), wH = WCompose(wF, lH);
    V3 S = wU.p, E0 = wF.p, T0 = wH.p;
    float L1 = Len(E0 - S), L2 = Len(T0 - E0);
    bool diag = !in.quiet && ArmDiag(h);
    if (diag) Log("rig[%c]: clav=(%.3f %.3f %.3f) escala=(%.2f %.2f %.2f) ombro=(%.3f %.3f %.3f) L1=%.3f L2=%.3f alvo=(%.3f %.3f %.3f) dist=%.3f | localU p=(%.3f %.3f %.3f) s=(%.2f %.2f %.2f)",
        h ? 'D' : 'E', in.clav[h].p.x, in.clav[h].p.y, in.clav[h].p.z, in.clav[h].s.x, in.clav[h].s.y, in.clav[h].s.z,
        S.x, S.y, S.z, L1, L2, in.wrist[h].x, in.wrist[h].y, in.wrist[h].z, Len(in.wrist[h] - S),
        lU[0], lU[1], lU[2], lU[8], lU[9], lU[10]);
    if (L1 < 0.05f || L2 < 0.05f) { if (diag) Log("rig[%c]: ossos curtos demais (escala?) -> IK ignorado", h ? 'D' : 'E'); return; }

    // Eixo do cotovelo da animação (aprendido quando o braço está dobrado o bastante).
    V3 u0 = Norm(E0 - S), v0 = Norm(T0 - E0);
    V3 n0 = Cross(u0, v0);
    if (Len(n0) > 0.17f) { hingeLocal[h] = Norm(QRot(QConj(wU.q), n0)); hingeOk[h] = true; }
    V3 hinge0 = hingeOk[h] ? QRot(wU.q, hingeLocal[h]) : v3(0, 0, 0);

    // Alvo e posição do cotovelo (lei dos cossenos, plano escolhido pelo polo).
    V3 T = in.wrist[h];
    V3 dv = T - S; float d = Len(dv);
    float dmax = (L1 + L2) * 0.999f, dmin = std::fabs(L1 - L2) + 0.01f;
    if (d < 1e-4f) return;
    if (d > dmax) { dv = dv * (dmax / d); d = dmax; T = S + dv; }
    if (d < dmin) { dv = dv * (dmin / d); d = dmin; T = S + dv; }
    V3 nd = dv * (1.0f / d);
    V3 side = Norm(v3(S.x - in.spine2.p.x, 0.0f, S.z - in.spine2.p.z));
    V3 pole = v3(0, -1, 0) + side * 0.6f - in.bodyFwd * 0.4f;
    V3 pp = pole - nd * Dot(pole, nd);
    if (Len(pp) < 1e-4f) pp = side; pp = Norm(pp);
    float a = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d);
    float hh = std::sqrt((std::max)(0.0f, L1 * L1 - a * a));
    V3 E = S + nd * a + pp * hh;
    V3 u1 = Norm(E - S), v1 = Norm(T - E);
    V3 n1 = Cross(u1, v1);
    if (Len(n1) < 1e-4f) n1 = Cross(u1, pp);
    n1 = Norm(n1);

    // UpperArm: leva (direção do braço, eixo do cotovelo) da animação aos novos.
    Q rU = (hingeOk[h] && Len(hinge0) > 0.5f) ? QFromPairs(u0, hinge0, u1, n1) : QFromTo(u0, u1);
    Q qU = QNorm(QMul(rU, wU.q));
    // Forearm: depois do UpperArm, gira no plano até apontar para o alvo.
    Q qF1 = QNorm(QMul(rU, wF.q));
    V3 fdir = QRot(rU, v0);
    Q qF = QNorm(QMul(QFromTo(fdir, v1), qF1));
    // Hand: base (frente = direção dos dedos, polegar) medida nas juntas locais dos filhos.
    Q qH = wH.q;
    float* lMid = JointAt(joints, n, in.idx[FingerBone(h, 2, 0)]);
    float* lIdx = JointAt(joints, n, in.idx[FingerBone(h, 1, 0)]);
    float* lPky = JointAt(joints, n, in.idx[FingerBone(h, 4, 0)]);
    V3 fLoc = { 0, 0, 0 }, tLoc = { 0, 0, 0 };
    if (lMid && lIdx && lPky) {
        fLoc = Norm(LP(lMid));
        tLoc = Norm(LP(lIdx) - LP(lPky));
        Q rl = QFromPairs(fLoc, tLoc, in.handFwd[h], in.handThumb[h]);   // leva a base local ao mundo
        qH = rl;                                                          // (a base local já está no espaço da mão)
    }
    if (in.handQuatOn[h]) qH = QNorm(in.handQuat[h]);

    float w = in.weight;
    Q lUq = QNorm(QMul(QConj(in.clav[h].q), qU));
    Q lFq = QNorm(QMul(QConj(qU), qF));
    Q lHq = QNorm(QMul(QConj(qF), qH));
    // Torção: metade da diferença de torção da mão (em volta do antebraço) vai para o ForearmTwist.
    if (float* lT = JointAt(joints, n, in.idx[B_TWIST + h])) {
        V3 ax = Norm(LP(lH));
        if (Len(ax) > 0.5f) {
            Q tw = QMul(QTwist(lHq, ax), QConj(QTwist(LQ(lH), ax)));
            WriteLQ(lT, QMul(QSlerp(Q{ 0, 0, 0, 1 }, QNorm(tw), 0.5f), LQ(lT)), w);
        }
    }
    if (diag) {
        Q a = LQ(lU);
        Log("rig[%c]: cotovelo=(%.3f %.3f %.3f) eixo_aprendido=%d | UpperArm local antes=(%.3f %.3f %.3f %.3f) depois=(%.3f %.3f %.3f %.3f) peso=%.2f",
            h ? 'D' : 'E', E.x, E.y, E.z, (int)hingeOk[h], a.x, a.y, a.z, a.w, lUq.x, lUq.y, lUq.z, lUq.w, w);
    }
    WriteLQ(lU, lUq, w);
    WriteLQ(lF, lFq, w);
    WriteLQ(lH, lHq, w);

    // Dedos: gira cada junta em volta do eixo lateral da mão (no espaço da mão, levado junta a junta).
    if (lMid && lIdx && lPky && Len(fLoc) > 0.5f) {
        V3 curlAxis = tLoc * ((h == 0 ? -1.0f : 1.0f) * g_rigFingerSign);
        // Polegar: gira em volta da direção dos dedos (atravessa a palma) e dobra as juntas de cima.
        V3 thumbAxis = fLoc * ((h == 0 ? 1.0f : -1.0f) * g_rigFingerSign * g_rigThumbSign);
        for (int f = 0; f < 5; ++f) {
            float c = f == 0 ? in.curlThumb[h] : f == 1 ? in.curlIndex[h] : in.curlOthers[h];
            if (c <= 0.01f) continue;
            const float maxAngF[3] = { 1.2f, 1.4f, 1.0f };  // rad por junta, mão fechada
            const float maxAngT[3] = { 0.55f, 0.6f, 0.7f };  // polegar
            const float* maxAng = f == 0 ? maxAngT : maxAngF;
            V3 axisUse = f == 0 ? thumbAxis : curlAxis;
            Q acc = { 0, 0, 0, 1 };                           // espaço da mão -> espaço do pai da junta j
            for (int j = 0; j < 3; ++j) {
                float* lj = JointAt(joints, n, in.idx[FingerBone(h, f, j)]);
                if (!lj) break;
                V3 axLocal = QRot(QConj(acc), axisUse);       // eixo no espaço do pai desta junta
                WriteLQ(lj, QMul(QAxisAngle(axLocal, c * maxAng[j]), LQ(lj)), w);
                acc = QNorm(QMul(acc, LQ(lj)));               // inclui a rotação já escrita
            }
        }
    }
}

static void SolveHead(const RigInput& in, float* joints, int n) {
    float* lN = JointAt(joints, n, in.idx[B_NECK]);
    float* lHd = JointAt(joints, n, in.idx[B_HEAD]);
    float* lG = JointAt(joints, n, in.idx[B_GLASSES]);
    float* lL = JointAt(joints, n, in.idx[B_LEYEAREA]);
    float* lR = JointAt(joints, n, in.idx[B_REYEAREA]);
    if (!lN || !lHd || !lG || !lL || !lR) return;
    WX wN = WCompose(in.spine2, lN), wHd = WCompose(wN, lHd);
    V3 fLoc = Norm(LP(lG) - (LP(lL) + LP(lR)) * 0.5f);   // frente da cabeça (espaço da cabeça)
    V3 sLoc = Norm(LP(lL) - LP(lR));                     // esquerda da cabeça
    if (Len(fLoc) < 0.5f || Len(sLoc) < 0.5f) return;
    Q want = QFromPairs(fLoc, sLoc, in.headFwd, in.headLeft);
    // Limita o giro em relação à cabeça da animação (evita torcer o pescoço além do humano).
    Q delta = QNorm(QMul(want, QConj(wHd.q)));
    float ang = QAngle(delta), lim = g_rigHeadLimit * 0.0174533f;
    if (ang > lim) { delta = QSlerp(Q{ 0, 0, 0, 1 }, delta, lim / ang); want = QNorm(QMul(delta, wHd.q)); }
    // 40% no pescoço, o resto na cabeça.
    Q neckW = QNorm(QMul(QSlerp(Q{ 0, 0, 0, 1 }, delta, 0.4f), wN.q));
    WriteLQ(lN, QNorm(QMul(QConj(in.spine2.q), neckW)), in.weight);
    WriteLQ(lHd, QNorm(QMul(QConj(neckW), want)), in.weight);
}

// Chamado no hk_EndPosing (lock do ator já pego), depois do congelamento.
// FK desde a raiz com as juntas deste frame (espaço do modelo) e encaixe no mundo pelas posições:
// lateral = clavícula D - clavícula E, cima = pescoço - Spine2. Preenche in.spine2 e in.clav[].
static bool BuildWorldFrames(RigInput& in, float* joints, int n, Q* rOut = nullptr) {
    float* L[7];
    int chain[7] = { B_ROOT, B_ROOTBIND, B_SPINE0, B_SPINE1, B_SPINE2, B_NECK, B_CLAV };
    for (int i = 0; i < 6; ++i) if (!(L[i] = JointAt(joints, n, in.idx[chain[i]]))) return false;
    float* lc[2] = { JointAt(joints, n, in.idx[B_CLAV]), JointAt(joints, n, in.idx[B_CLAV + 1]) };
    if (!lc[0] || !lc[1]) return false;
    WX id = { { 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 } };
    WX m = WCompose(id, L[0]);
    m = WCompose(m, L[1]); m = WCompose(m, L[2]); m = WCompose(m, L[3]);
    WX mS2 = WCompose(m, L[4]);
    WX mN = WCompose(mS2, L[5]);
    WX mC[2] = { WCompose(mS2, lc[0]), WCompose(mS2, lc[1]) };
    float* lu[2] = { JointAt(joints, n, in.idx[B_UPPER]), JointAt(joints, n, in.idx[B_UPPER + 1]) };
    if (!lu[0] || !lu[1]) return false;
    WX mU[2] = { WCompose(mC[0], lu[0]), WCompose(mC[1], lu[1]) };
    V3 latM = mU[1].p - mU[0].p, upM = mN.p - mS2.p;
    V3 latW = in.wpUpper[1] - in.wpUpper[0], upW = in.wpNeck - in.wpSpine2;
    // Fase 2: encaixe por raiz, Spine0 e coxas (não mudam quando a coluna inclina)
    WX mRoot = WCompose(id, L[0]), mBind = WCompose(mRoot, L[1]), mS0 = WCompose(mBind, L[2]);
    WX mPel = mBind, mTh[2] = { mBind, mBind };
    bool lowerOk = false;
    if (in.wpLower) {
        float* lp = JointAt(joints, n, in.idx[B_PELVIS]);
        float* lt[2] = { JointAt(joints, n, in.idx[B_THIGH]), JointAt(joints, n, in.idx[B_THIGH + 1]) };
        if (lp && lt[0] && lt[1]) {
            mPel = WCompose(mBind, lp); mTh[0] = WCompose(mPel, lt[0]); mTh[1] = WCompose(mPel, lt[1]);
            // lateral = coxas; cima = vertical (o Sim fica sempre de pé no espaço do objeto). Agachar e
            // deslocar o corpo mexem no ROOT_bind (as duas coxas juntas) e não mudam nenhum dos dois.
            V3 lm = mTh[1].p - mTh[0].p, lw = in.wpThigh[1] - in.wpThigh[0];
            if (Len(lm) > 0.05f && Len(lw) > 0.05f) { latM = lm; upM = v3(0, 1, 0); latW = lw; upW = v3(0, 1, 0); lowerOk = true; }
        }
    }
    if (Len(latM) < 0.05f || Len(upM) < 0.05f || Len(latW) < 0.05f || Len(upW) < 0.05f) {
        static int f = 0;
        if (f++ < 5) Log("rig: FK: referencias curtas lat_modelo=%.3f cima_modelo=%.3f lat_mundo=%.3f cima_mundo=%.3f",
                         Len(latM), Len(upM), Len(latW), Len(upW));
        return false;
    }
    Q R = QFromPairs(upM, latM, upW, latW);
    V3 t = lowerOk ? in.wpRoot - QRot(R, mRoot.p) : in.wpSpine2 - QRot(R, mS2.p);
    auto toW = [&](const WX& a) { WX o; o.p = QRot(R, a.p) + t; o.q = QNorm(QMul(R, a.q)); o.s = a.s; return o; };
    in.spine2 = toW(mS2); in.clav[0] = toW(mC[0]); in.clav[1] = toW(mC[1]);
    in.pelvis = toW(mPel); in.fitR = R; in.rootW = toW(mRoot).p;
    if (rOut) { *rOut = R; return true; }
    static ULONGLONG last = 0;
    if (GetTickCount64() - last > 3000) {
        last = GetTickCount64();
        Log("rig: encaixe modelo->mundo: erro clavE=%.3f clavD=%.3f pescoco=%.3f ombroE=%.3f ombroD=%.3f m (perto de 0 = bom)",
            Len(in.clav[0].p - in.wpClav[0]), Len(in.clav[1].p - in.wpClav[1]), Len(QRot(R, mN.p) + t - in.wpNeck),
            Len(QRot(R, mU[0].p) + t - in.wpUpper[0]), Len(QRot(R, mU[1].p) + t - in.wpUpper[1]));
    }
    return true;
}

// ================================================================ Fase 2: corpo inteiro

// Inclina a coluna (Spine0 e Spine1, metade cada) para o lado em que a cabeça foi.
static bool SolveLean(RigInput& in, float* joints, int n) {
    float* l0 = JointAt(joints, n, in.idx[B_SPINE0]); float* l1 = JointAt(joints, n, in.idx[B_SPINE1]);
    float* lb = JointAt(joints, n, in.idx[B_ROOTBIND]); float* lr = JointAt(joints, n, in.idx[B_ROOT]);
    if (!l0 || !l1 || !lb || !lr) return false;
    V3 L = in.lean; float d = Len(L);
    if (d < 0.01f) return false;
    float ang = std::atan2(d, 0.45f);                       // coluna ~0,45 m até o pescoço
    V3 axis = Norm(Cross(v3(0, 1, 0), L * (1.0f / d)));
    Q half = QAxisAngle(axis, ang * 0.5f * in.weight);
    // mundo de cada junta (encaixe atual) -> gira no mundo -> volta para local
    WX wRoot = { in.rootW, QNorm(QMul(in.fitR, LQ(lr))), { 1, 1, 1 } };
    WX wBind = WCompose(wRoot, lb);
    WX w0 = WCompose(wBind, l0);
    Q n0 = QNorm(QMul(half, w0.q));
    SetLQ(l0, QNorm(QMul(QConj(wBind.q), n0)));
    WX w0n = { w0.p, n0, w0.s };
    WX w1 = WCompose(w0n, l1);
    Q n1 = QNorm(QMul(half, w1.q));
    SetLQ(l1, QNorm(QMul(QConj(n0), n1)));
    return true;
}

// Pés plantados e passos procedurais (thread da animação; estado entre frames).
struct FootState { bool init; V3 plant; float yaw; bool stepping; V3 from; float fromYaw; float t, dur; };
static FootState g_feet[2] = {};
static V3 g_prevRoot = { 0, 0, 0 }; static bool g_prevRootOk = false;
static float g_ankleH = 0.11f;
static V3  g_legHinge[2] = {}; static bool g_legHingeOk[2] = { false, false };

static float YawOfDir(V3 f) { return std::atan2(f.x, f.z); }

// IK de dois ossos genérico (coxa/canela/pé), mesma matemática do braço.
static void SolveLeg(const RigInput& in, int s, float* joints, int n, V3 T, Q footW, V3 pole) {
    float* lU = JointAt(joints, n, in.idx[B_THIGH + s]); float* lF = JointAt(joints, n, in.idx[B_CALF + s]); float* lH = JointAt(joints, n, in.idx[B_FOOT + s]);
    if (!lU || !lF || !lH) return;
    WX wU = WCompose(in.pelvis, lU), wF = WCompose(wU, lF), wH = WCompose(wF, lH);
    V3 S = wU.p, E0 = wF.p, T0 = wH.p;
    float L1 = Len(E0 - S), L2 = Len(T0 - E0);
    if (L1 < 0.1f || L2 < 0.1f) return;
    V3 u0 = Norm(E0 - S), v0 = Norm(T0 - E0), n0 = Cross(u0, v0);
    if (Len(n0) > 0.05f) { g_legHinge[s] = Norm(QRot(QConj(wU.q), n0)); g_legHingeOk[s] = true; }
    V3 hinge0 = g_legHingeOk[s] ? QRot(wU.q, g_legHinge[s]) : v3(0, 0, 0);
    V3 dv = T - S; float d = Len(dv);
    float dmax = (L1 + L2) * 0.998f, dmin = std::fabs(L1 - L2) + 0.02f;
    if (d < 1e-4f) return;
    if (d > dmax) { dv = dv * (dmax / d); d = dmax; T = S + dv; }
    if (d < dmin) { dv = dv * (dmin / d); d = dmin; T = S + dv; }
    V3 nd = dv * (1.0f / d);
    V3 pp = pole - nd * Dot(pole, nd); if (Len(pp) < 1e-4f) pp = pole; pp = Norm(pp);
    float a = (L1 * L1 - L2 * L2 + d * d) / (2.0f * d), hh = std::sqrt((std::max)(0.0f, L1 * L1 - a * a));
    V3 E = S + nd * a + pp * hh;
    V3 u1 = Norm(E - S), v1 = Norm(T - E), n1 = Cross(u1, v1);
    if (Len(n1) < 1e-4f) n1 = Cross(u1, pp);
    n1 = Norm(n1);
    Q rU = (g_legHingeOk[s] && Len(hinge0) > 0.5f) ? QFromPairs(u0, hinge0, u1, n1) : QFromTo(u0, u1);
    Q qU = QNorm(QMul(rU, wU.q));
    Q qF = QNorm(QMul(QFromTo(QRot(rU, v0), v1), QNorm(QMul(rU, wF.q))));
    float w = in.weight;
    WriteLQ(lU, QNorm(QMul(QConj(in.pelvis.q), qU)), w);
    WriteLQ(lF, QNorm(QMul(QConj(qU), qF)), w);
    WriteLQ(lH, QNorm(QMul(QConj(qF), footW)), w);
}

static void SolveLegs(const RigInput& in, float* joints, int n) {
    float* lt[2] = { JointAt(joints, n, in.idx[B_THIGH]), JointAt(joints, n, in.idx[B_THIGH + 1]) };
    float* lc[2] = { JointAt(joints, n, in.idx[B_CALF]), JointAt(joints, n, in.idx[B_CALF + 1]) };
    float* lf[2] = { JointAt(joints, n, in.idx[B_FOOT]), JointAt(joints, n, in.idx[B_FOOT + 1]) };
    if (!lt[0] || !lt[1] || !lc[0] || !lc[1] || !lf[0] || !lf[1]) return;
    WX wT[2], wC[2], wF[2];
    for (int s = 0; s < 2; ++s) { wT[s] = WCompose(in.pelvis, lt[s]); wC[s] = WCompose(wT[s], lc[s]); wF[s] = WCompose(wC[s], lf[s]); }
    float ground = in.rootW.y;
    V3 lat = wT[0].p - wT[1].p;                                // direita -> esquerda
    V3 fwd = Norm(Cross(v3(lat.x, 0, lat.z), v3(0, 1, 0)));    // esquerda x cima = frente
    if (Len(fwd) < 0.5f) return;
    float bodyYaw = YawOfDir(fwd);
    // velocidade do corpo (para o passo antecipar)
    float dt = (std::max)(0.001f, (std::min)(0.1f, g_dt));
    V3 vel = { 0, 0, 0 };
    if (g_prevRootOk) { vel = (in.rootW - g_prevRoot) * (1.0f / dt); vel.y = 0; if (Len(vel) > 6.0f) vel = v3(0, 0, 0); }
    g_prevRoot = in.rootW; g_prevRootOk = true;
    float speed = Len(vel);
    V3 ideal[2];
    for (int s = 0; s < 2; ++s) {
        ideal[s] = v3(wT[s].p.x, ground, wT[s].p.z) + vel * 0.12f;
        if (!g_feet[s].init) {                                 // começa onde a animação põe o pé
            g_feet[s].init = true; g_feet[s].plant = v3(wF[s].p.x, ground, wF[s].p.z); g_feet[s].yaw = bodyYaw; g_feet[s].stepping = false;
            g_ankleH = (std::max)(0.05f, (std::min)(0.2f, wF[s].p.y - ground));
        }
    }
    // começa um passo (um pé por vez; correndo, o outro pode começar na metade)
    for (int pass = 0; pass < 1; ++pass) {
        int busy = g_feet[0].stepping ? 0 : g_feet[1].stepping ? 1 : -1;
        bool allowSecond = busy >= 0 && g_feet[busy].t > 0.5f && speed > 0.8f;
        if (busy >= 0 && !allowSecond) break;
        int pick = -1; float worst = 0;
        for (int s = 0; s < 2; ++s) {
            if (g_feet[s].stepping) continue;
            V3 e = ideal[s] - g_feet[s].plant; e.y = 0;
            float yawErr = std::fabs(std::remainder(bodyYaw - g_feet[s].yaw, 6.2831853f));
            float score = Len(e) / g_stepDist + yawErr / 0.6f;
            if (score > 1.0f && score > worst) { worst = score; pick = s; }
        }
        if (pick >= 0) {
            FootState& f = g_feet[pick];
            f.stepping = true; f.from = f.plant; f.fromYaw = f.yaw; f.t = 0;
            f.dur = (std::max)(0.18f, (std::min)(0.32f, 0.32f - speed * 0.05f));
        }
    }
    for (int s = 0; s < 2; ++s) {
        FootState& f = g_feet[s];
        V3 pos = f.plant; float yaw = f.yaw;
        if (f.stepping) {
            f.t += dt / f.dur;
            float k = (std::min)(1.0f, f.t), sm = k * k * (3 - 2 * k);
            float dy = std::remainder(bodyYaw - f.fromYaw, 6.2831853f);
            pos = f.from + (ideal[s] - f.from) * sm;
            yaw = f.fromYaw + dy * sm;
            pos.y = ground + std::sin(3.14159265f * k) * g_stepLift;
            if (f.t >= 1.0f) { f.stepping = false; f.plant = v3(ideal[s].x, ground, ideal[s].z); f.yaw = bodyYaw; pos = f.plant; yaw = f.yaw; }
        }
        V3 ankle = v3(pos.x, pos.y + g_ankleH, pos.z);
        // pé: rotação da animação girada em volta do eixo vertical para o "yaw" do pé plantado
        Q footW = QNorm(QMul(QAxisAngle(v3(0, 1, 0), std::remainder(yaw - bodyYaw, 6.2831853f)), wF[s].q));
        V3 side = s == 0 ? Norm(lat) : Norm(lat * -1.0f);
        V3 pole = fwd + side * 0.15f;                          // joelho para frente, um pouco para fora
        SolveLeg(in, s, joints, n, ankle, footW, pole);
    }
}

static void VrRigApply(float* joints, int n) {
    RigInput in;
    AcquireSRWLockShared(&g_rigLock); in = g_rigIn; ReleaseSRWLockShared(&g_rigLock);
    if (GetTickCount64() - in.tick > 250) return;
    bool rigOn = in.active && in.weight > 0.0f;
    bool bodyOn = in.wpOk && (Len(in.bodyOff) > 1e-4f || Len(in.measOff) > 1e-4f);
    V3 applied = { 0, 0, 0 };
    if (in.wpOk) {
        // As posições medidas incluem o deslocamento antigo: troca pelo novo.
        V3 d = in.bodyOff - in.measOff;
        in.wpSpine2 = in.wpSpine2 + d; in.wpNeck = in.wpNeck + d;
        for (int h = 0; h < 2; ++h) { in.wpClav[h] = in.wpClav[h] + d; in.wpUpper[h] = in.wpUpper[h] + d; in.wpThigh[h] = in.wpThigh[h] + d; }
        in.wpSpine0 = in.wpSpine0 + d;                         // (a raiz não se mexe: o deslocamento é no ROOT_bind)
    }
    if (bodyOn && Len(in.bodyOff) > 1e-4f) {
        Q R;
        float* lr = JointAt(joints, n, in.idx[B_ROOT]);
        float* lb = JointAt(joints, n, in.idx[B_ROOTBIND]);
        if (lr && lb && BuildWorldFrames(in, joints, n, &R)) {
            // O jogo parece não usar a translação do b__ROOT__ (v24 não teve efeito): desloca o
            // b__ROOT_bind__, pai da pélvis e da coluna. Espaço dele = espaço do ROOT (rotação R * qROOT).
            Q qr = QNorm(QMul(R, LQ(lr)));
            V3 sc = { lr[8] != 0.0f ? lr[8] : 1.0f, lr[9] != 0.0f ? lr[9] : 1.0f, lr[10] != 0.0f ? lr[10] : 1.0f };
            V3 dm = QRot(QConj(qr), in.bodyOff);              // mundo -> espaço do pai do ROOT_bind
            lb[0] += dm.x / sc.x; lb[1] += dm.y / sc.y; lb[2] += dm.z / sc.z;
            applied = in.bodyOff;
            static int lg = 0;
            if (lg++ < 2) Log("corpo: deslizando sob a camera (deslocamento %.3f m)", Len(in.bodyOff));
        }
    }
    AcquireSRWLockExclusive(&g_bodyLock); g_bodyOffApplied = applied; ReleaseSRWLockExclusive(&g_bodyLock);
    if (!rigOn) return;
    static bool logged = false;
    if (!logged) { logged = true; Log("rig: aplicando VR na pose (%d juntas; bracos E=%d D=%d cabeca=%d)", n, in.arm[0], in.arm[1], in.head); }
    if (!BuildWorldFrames(in, joints, n)) {
        static int f = 0; if (f++ < 3) Log("rig: nao consegui montar a cadeia da raiz (FK)");
        return;
    }
    if (Len(in.lean) > 0.01f && SolveLean(in, joints, n)) BuildWorldFrames(in, joints, n);
    if (in.head) SolveHead(in, joints, n);
    for (int h = 0; h < 2; ++h) if (in.arm[h]) SolveArm(in, h, joints, n);
    if (in.legs) SolveLegs(in, joints, n);
}

static void VrRigLoadConfig(const char* path) {
    char f[32];
    g_rigArms = GetPrivateProfileIntA("fpc_rig", "vr_bracos", 1, path);
    g_legsOn = GetPrivateProfileIntA("fpc_rig", "vr_pernas", 1, path) != 0;
    g_crouchOn = GetPrivateProfileIntA("fpc_rig", "vr_agachar", 1, path) != 0;
    g_leanOn = GetPrivateProfileIntA("fpc_rig", "vr_inclinar", 1, path) != 0;
    { char fx[32]; GetPrivateProfileStringA("fpc_rig", "passo_distancia", "0.18", fx, sizeof(fx), path); g_stepDist = (float)atof(fx);
      GetPrivateProfileStringA("fpc_rig", "passo_altura", "0.08", fx, sizeof(fx), path); g_stepLift = (float)atof(fx); }
    Log("config: vr_pernas=%d vr_agachar=%d vr_inclinar=%d passo_distancia=%.2f passo_altura=%.2f", g_legsOn, g_crouchOn, g_leanOn, g_stepDist, g_stepLift);
    g_bodySmooth = GetPrivateProfileIntA("fpc_rig", "corpo_suave", 1, path) != 0;   // só tem efeito na locomoção antiga
    Log("config: corpo_suave=%d", g_bodySmooth);
    GetPrivateProfileStringA("fpc_rig", "tecla_teste_bracos", "CEDILHA", f, sizeof(f), path);
    if (int vk = ParseKey(f)) g_rigTestKey = vk;
    Log("config: tecla_teste_bracos=%s (vk=0x%02X)", f, g_rigTestKey);
    g_rigHead = GetPrivateProfileIntA("fpc_rig", "vr_cabeca", 1, path) != 0;
    g_rigFingers = GetPrivateProfileIntA("fpc_rig", "vr_dedos", 1, path) != 0;
    GetPrivateProfileStringA("fpc_rig", "vr_mao_recuo", "0.07", f, sizeof(f), path); g_rigWristBack = (float)atof(f);
    GetPrivateProfileStringA("fpc_rig", "vr_mao_inclinacao", "0", f, sizeof(f), path); g_rigHandPitch = (float)atof(f);
    GetPrivateProfileStringA("fpc_rig", "vr_dedos_sinal", "1", f, sizeof(f), path); g_rigFingerSign = (float)atof(f) < 0 ? -1.0f : 1.0f;
    GetPrivateProfileStringA("fpc_rig", "vr_cabeca_limite", "80", f, sizeof(f), path); g_rigHeadLimit = (float)atof(f);
    GetPrivateProfileStringA("fpc_rig", "vr_polegar_sinal", "1", f, sizeof(f), path); g_rigThumbSign = (float)atof(f) < 0 ? -1.0f : 1.0f;
    Log("config: vr_bracos=%d vr_cabeca=%d vr_dedos=%d vr_mao_recuo=%.2f vr_mao_inclinacao=%.0f vr_dedos_sinal=%.0f vr_cabeca_limite=%.0f",
        g_rigArms, g_rigHead, g_rigFingers, g_rigWristBack, g_rigHandPitch, g_rigFingerSign, g_rigHeadLimit);
}
