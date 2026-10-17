// teste_rig.cpp: testa o IK do vr_rig.inl fora do jogo, com o esqueleto real do Sim (body.blend).
// Compila com teste\build_teste.bat. Imprime o erro da mão, do cotovelo e da cabeça para vários alvos.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>
#include <openxr/openxr.h>

// ---- stubs do que o vr_rig.inl usa do resto do mod
static void Log(const char* fmt, ...) { va_list a; va_start(a, fmt); printf("  [log] "); vprintf(fmt, a); printf("\n"); va_end(a); }
static bool Pressed(int) { return false; }
static bool GameHasFocus() { return true; }
static bool MouseLookDir(float o[4]) { o[0] = 0; o[1] = 0; o[2] = 1; o[3] = 0; return true; }
static int ParseKey(const char*) { return 0; }
static bool g_vrEnabled = true, g_vrHead = true; static volatile bool g_xrRunning = true;
enum Phase { RUNNING = 3 }; static Phase g_phase = RUNNING;
static float g_dt = 1.0f / 60.0f;
static bool FpsFree() { return false; }
static float g_vrOffset[3] = {}, g_vrYaw = 0.0f, g_hmdRefPos[3] = {}, g_worldScale = 1.0f;
static float g_origin[2] = {}, g_anchor[4] = {};
struct CtrlState { XrPosef grip, aim; bool gripValid, aimValid; float trigger, squeeze, sx, sy; bool a, b, stickClick, menu, thumbTouch; };
static CtrlState g_ctrl[2] = {};
static CtrlState g_ctrlRaw[2] = {};
static SRWLOCK g_hmdLock = SRWLOCK_INIT;
static float g_vrFwd[4] = { 0, 0, 1, 0 };
struct HmdPose { float pos[3]; float quat[4]; bool valid; };
static HmdPose g_hmdPose = {};
static float g_centerHead[3] = {}, g_centerScale = 1.0f, g_centerYaw = 0.0f, g_centerCam[4] = {};
static void RotYXR(float th, const float v[3], float o[3]) { float c = std::cos(th), s = std::sin(th); o[0] = v[0] * c + v[2] * s; o[1] = v[1]; o[2] = -v[0] * s + v[2] * c; }

#include "../vr_rig.inl"
static bool g_laserOn = false; static volatile bool g_uiPanelVisible = false;
static volatile bool g_handGrabbing[2] = { false, false };
static void* g_simActor = nullptr;
static void XrHaptic(int, float, float) {}
static volatile int g_objReqHand = -1; static float g_objReqPalm[3] = {};
#include "../vr_grab.inl"
#include "esqueleto_teste.h"

static int Find(const char* n) { for (int i = 0; i < kSkN; ++i) if (!strcmp(kSk[i].name, n)) return i; printf("FALTA %s\n", n); return -1; }

// FK completa (modelo) e aplicação da transformação do mundo.
static std::vector<WX> FK(const float* joints, Q wq, V3 wt) {
    std::vector<WX> w(kSkN);
    for (int i = 0; i < kSkN; ++i) {
        const float* l = joints + i * 12;
        WX par = kSk[i].parent < 0 ? WX{ wt, wq, { 1, 1, 1 } } : w[kSk[i].parent];
        w[i] = WCompose(par, l);
    }
    return w;
}

int main() {
    std::vector<float> joints(kSkN * 12), rest(kSkN * 12);
    for (int i = 0; i < kSkN; ++i) {
        float* l = &rest[i * 12];
        l[0] = kSk[i].p[0]; l[1] = kSk[i].p[1]; l[2] = kSk[i].p[2]; l[3] = 1;
        memcpy(l + 4, kSk[i].q, 16);
        l[8] = l[9] = l[10] = 1; l[11] = 0;
    }
    // dobra um pouco os cotovelos na "animação" (para o eixo do cotovelo ser aprendido)
    RigInput in = {};
    const char* names[B_COUNT] = {};
    in.idx[B_ROOT] = Find("b__ROOT__"); in.idx[B_ROOTBIND] = Find("b__ROOT_bind__"); in.idx[B_SPINE0] = Find("b__Spine0__");
    in.idx[B_SPINE1] = Find("b__Spine1__"); in.idx[B_SPINE2] = Find("b__Spine2__"); in.idx[B_NECK] = Find("b__Neck__");
    in.idx[B_HEAD] = Find("b__Head__"); in.idx[B_GLASSES] = Find("b__CAS_Glasses__");
    in.idx[B_LEYEAREA] = Find("b__CAS_L_EyeArea__"); in.idx[B_REYEAREA] = Find("b__CAS_R_EyeArea__");
    char nm[64];
    for (int h = 0; h < 2; ++h) {
        const char* s = h ? "R" : "L";
        snprintf(nm, 64, "b__%s_Clavicle__", s); in.idx[B_CLAV + h] = Find(nm);
        snprintf(nm, 64, "b__%s_UpperArm__", s); in.idx[B_UPPER + h] = Find(nm);
        snprintf(nm, 64, "b__%s_Forearm__", s); in.idx[B_FORE + h] = Find(nm);
        snprintf(nm, 64, "b__%s_Hand__", s); in.idx[B_HAND + h] = Find(nm);
        snprintf(nm, 64, "b__%s_ForearmTwist__", s); in.idx[B_TWIST + h] = Find(nm);
        for (int f = 0; f < 5; ++f) for (int j = 0; j < 3; ++j) { snprintf(nm, 64, "b__%s_%s%d__", s, kFingerNames[f], j); in.idx[FingerBone(h, f, j)] = Find(nm); }
    }
    for (int h = 0; h < 2; ++h) {   // cotovelo da "animação" dobrado ~25° (em volta do eixo lateral do modelo)
        float* lf = &rest[in.idx[B_FORE + h] * 12];
        float* lh = &rest[in.idx[B_HAND + h] * 12];
        V3 dir = Norm(QRot(LQ(lf), LP(lh)));                    // antebraço no espaço do UpperArm
        V3 ax = Norm(Cross(dir, v3(0, 1, 0)));                   // perpendicular: dobra de verdade
        Q b = QMul(QAxisAngle(ax, h ? 0.6f : -0.6f), LQ(lf));
        // (rotaciona no espaço do pai; o eixo exato não importa, só precisa dobrar)
        SetLQ(lf, QNorm(b));
    }

    // mundo do Sim: girado 37° e deslocado (como no jogo, ~205, 158.5, 210)
    Q wq = QAxisAngle(v3(0, 1, 0), 0.65f); V3 wt = { 205.0f, 158.5f, 210.0f };
    V3 fwd = QRot(wq, v3(0, 0, 1)), up = { 0, 1, 0 }, left = QRot(wq, v3(1, 0, 0));   // Sim olha para +Z do modelo; esquerda = +X

    struct Case { const char* nome; V3 off[2]; V3 hf; V3 ht; } cases[] = {
        { "maos na frente do peito", { fwd * 0.35f + left * 0.10f - up * 0.25f, fwd * 0.35f - left * 0.10f - up * 0.25f }, fwd, up },
        { "bracos esticados p/ frente", { fwd * 0.60f + left * 0.16f, fwd * 0.60f - left * 0.16f }, fwd, up },
        { "maos para cima", { up * 0.55f + left * 0.20f, up * 0.55f - left * 0.20f }, up, fwd * -1.0f },
        { "maos ao lado do quadril", { left * 0.25f - up * 0.50f, left * -0.25f - up * 0.50f }, up * -1.0f, fwd },
        { "alvo fora do alcance", { fwd * 1.50f, fwd * 1.50f }, fwd, up },
    };
    int fails = 0;
    for (auto& c : cases) {
        joints = rest;
        std::vector<WX> w0 = FK(joints.data(), wq, wt);
        in.wpSpine2 = w0[in.idx[B_SPINE2]].p; in.wpNeck = w0[in.idx[B_NECK]].p;
        for (int h = 0; h < 2; ++h) { in.wpClav[h] = w0[in.idx[B_CLAV + h]].p; in.wpUpper[h] = w0[in.idx[B_UPPER + h]].p; }
        in.bodyFwd = fwd;
        for (int h = 0; h < 2; ++h) {
            in.arm[h] = true;
            V3 sh = w0[in.idx[B_UPPER + h]].p;
            in.wrist[h] = sh + c.off[h];
            in.handFwd[h] = Norm(c.hf); in.handThumb[h] = Norm(c.ht);
            in.curlIndex[h] = 0.0f; in.curlOthers[h] = 0.0f;
        }
        // cabeça: olhar 30° para a esquerda e 20° para baixo
        Q hq = QNorm(QMul(QAxisAngle(up, 0.52f), QAxisAngle(left, 0.35f)));
        in.headFwd = QRot(hq, fwd); in.headLeft = QRot(hq, left); in.head = true;
        in.weight = 1.0f; in.active = true; in.tick = GetTickCount64();
        g_rigIn = in;
        g_armDiagT[0] = g_armDiagT[1] = 0;
        { std::vector<float> tmp = joints; VrRigApply(tmp.data(), kSkN); }   // 1a passada: aprende o eixo do cotovelo
        g_armDiagT[0] = g_armDiagT[1] = 0;
        VrRigApply(joints.data(), kSkN);
        std::vector<WX> w1 = FK(joints.data(), wq, wt);
        printf("== %s (eixo do cotovelo aprendido: E=%d D=%d)\n", c.nome, (int)g_hingeOk[0], (int)g_hingeOk[1]);
        for (int h = 0; h < 2; ++h) {
            V3 S = w1[in.idx[B_UPPER + h]].p, E = w1[in.idx[B_FORE + h]].p, H = w1[in.idx[B_HAND + h]].p;
            float reach = Len(in.wrist[h] - S), maxr = Len(w0[in.idx[B_FORE + h]].p - w0[in.idx[B_UPPER + h]].p) + Len(w0[in.idx[B_HAND + h]].p - w0[in.idx[B_FORE + h]].p);
            V3 want = reach > maxr ? S + Norm(in.wrist[h] - S) * (maxr * 0.999f) : in.wrist[h];
            float errH = Len(H - want);
            V3 midDir = Norm(w1[in.idx[FingerBone(h, 2, 0)]].p - H);
            float angF = std::acos((std::max)(-1.0f, (std::min)(1.0f, Dot(midDir, in.handFwd[h])))) * 57.3f;
            V3 thumbSide = Norm(w1[in.idx[FingerBone(h, 1, 0)]].p - w1[in.idx[FingerBone(h, 4, 0)]].p);
            V3 tproj = Norm(in.handThumb[h] - in.handFwd[h] * Dot(in.handThumb[h], in.handFwd[h]));
            V3 sproj = Norm(thumbSide - midDir * Dot(thumbSide, midDir));
            float angT = std::acos((std::max)(-1.0f, (std::min)(1.0f, Dot(sproj, tproj)))) * 57.3f;
            float l1 = Len(E - S), l2 = Len(H - E);
            // cotovelo: deve ficar para baixo/fora (não para cima/dentro)
            V3 mid = (S + want) * 0.5f, eo = E - mid;
            float elbowUp = Dot(eo, up);
            bool ok = errH < 0.01f && angF < 8.0f && angT < 15.0f && std::fabs(l1 - 0.321f) < 0.01f;
            if (!ok) ++fails;
            printf("   %s: erro_mao=%.4f m  dedos=%.1f°  polegar=%.1f°  braco=%.3f antebraco=%.3f  cotovelo_vertical=%+.3f  %s\n",
                h ? "D" : "E", errH, angF, angT, l1, l2, elbowUp, ok ? "OK" : "FALHA");
        }
        V3 hpos = w1[in.idx[B_HEAD]].p;
        V3 hfw = Norm(w1[in.idx[B_GLASSES]].p - (w1[in.idx[B_LEYEAREA]].p + w1[in.idx[B_REYEAREA]].p) * 0.5f);
        float angHead = std::acos((std::max)(-1.0f, (std::min)(1.0f, Dot(hfw, in.headFwd)))) * 57.3f;
        printf("   cabeca: erro de direcao=%.1f°  %s\n", angHead, angHead < 5.0f ? "OK" : "FALHA");
        if (angHead >= 5.0f) ++fails;
        (void)hpos;
    }
    // dedos: gatilho fecha o indicador para o lado da palma
    {
        joints = rest;
        std::vector<WX> w0 = FK(joints.data(), wq, wt);
        in.wpSpine2 = w0[in.idx[B_SPINE2]].p; in.wpNeck = w0[in.idx[B_NECK]].p;
        for (int h = 0; h < 2; ++h) { in.wpClav[h] = w0[in.idx[B_CLAV + h]].p; in.wpUpper[h] = w0[in.idx[B_UPPER + h]].p; }
        for (int h = 0; h < 2; ++h) { in.wrist[h] = w0[in.idx[B_UPPER + h]].p + fwd * 0.45f - up * 0.15f; in.handFwd[h] = fwd; in.handThumb[h] = up; in.curlIndex[h] = 1.0f; in.curlOthers[h] = 1.0f; in.curlThumb[h] = 1.0f; }
        in.head = false; g_rigIn = in; g_rigIn.tick = GetTickCount64();
        VrRigApply(joints.data(), kSkN);
        std::vector<WX> w1 = FK(joints.data(), wq, wt);
        // referência: mesma mão sem fechar nada
        std::vector<float> open_ = rest;
        for (int h = 0; h < 2; ++h) { g_rigIn.curlIndex[h] = g_rigIn.curlOthers[h] = g_rigIn.curlThumb[h] = 0.0f; }
        g_rigIn.tick = GetTickCount64();
        VrRigApply(open_.data(), kSkN);
        std::vector<WX> w2 = FK(open_.data(), wq, wt);
        printf("== dedos fechados\n");
        for (int h = 0; h < 2; ++h) {
            V3 palm = h == 0 ? QRot(wq, v3(-1, 0, 0)) : QRot(wq, v3(1, 0, 0));
            V3 t1 = w1[in.idx[FingerBone(h, 0, 2)]].p, t0 = w2[in.idx[FingerBone(h, 0, 2)]].p;
            float side = Dot(t1 - t0, palm), down = Dot(t1 - t0, v3(0, -1, 0));
            bool ok = side > 0.005f;
            if (!ok) ++fails;
            printf("   %s: ponta do polegar moveu: para a palma=%+.3f m, para baixo=%+.3f m  %s\n", h ? "D" : "E", side, down, ok ? "OK" : "FALHA");
        }
        for (int h = 0; h < 2; ++h) {
            V3 H = w1[in.idx[B_HAND + h]].p, tip = w1[in.idx[FingerBone(h, 1, 2)]].p;
            V3 palm = h == 0 ? QRot(wq, v3(-1, 0, 0)) : QRot(wq, v3(1, 0, 0));   // palma virada para dentro (polegar para cima)
            float side = Dot(tip - H, palm), along = Dot(tip - H, fwd);
            bool ok = side > 0.02f;
            if (!ok) ++fails;
            printf("   %s: ponta do indicador: para a palma=%+.3f m, para a frente=%+.3f m  %s\n", h ? "D" : "E", side, along, ok ? "OK" : "FALHA");
        }
    }
    // corpo deslizando: desloca o corpo 0,25 m para o lado e 0,15 m para a frente; a medição (wp*) veio de um
    // frame em que já havia 0,10 m aplicados. O peito deve ir para (real + desloc.) e as mãos continuar no alvo.
    {
        joints = rest;
        std::vector<WX> w0 = FK(joints.data(), wq, wt);            // posição REAL (sem deslocamento)
        V3 measOff = left * 0.10f, bodyOff = left * 0.25f + fwd * 0.15f;
        in.wpSpine2 = w0[in.idx[B_SPINE2]].p + measOff; in.wpNeck = w0[in.idx[B_NECK]].p + measOff;
        for (int h = 0; h < 2; ++h) { in.wpClav[h] = w0[in.idx[B_CLAV + h]].p + measOff; in.wpUpper[h] = w0[in.idx[B_UPPER + h]].p + measOff; }
        in.wpOk = true; in.measOff = measOff; in.bodyOff = bodyOff;
        for (int h = 0; h < 2; ++h) {
            in.arm[h] = true;
            in.wrist[h] = w0[in.idx[B_UPPER + h]].p + bodyOff + fwd * 0.35f - up * 0.2f;
            in.handFwd[h] = fwd; in.handThumb[h] = up; in.curlIndex[h] = in.curlOthers[h] = in.curlThumb[h] = 0.0f;
        }
        in.head = false; in.active = true; in.weight = 1.0f; in.tick = GetTickCount64();
        g_rigIn = in;
        VrRigApply(joints.data(), kSkN);
        std::vector<WX> w1 = FK(joints.data(), wq, wt);
        V3 gotS2 = w1[in.idx[B_SPINE2]].p, wantS2 = w0[in.idx[B_SPINE2]].p + bodyOff;
        float e = Len(gotS2 - wantS2);
        printf("== corpo deslizando\n   peito: erro=%.4f m  %s\n", e, e < 0.005f ? "OK" : "FALHA");
        if (e >= 0.005f) ++fails;
        for (int h = 0; h < 2; ++h) {
            float eh = Len(w1[in.idx[B_HAND + h]].p - in.wrist[h]);
            printf("   %s: mao no alvo: erro=%.4f m  %s\n", h ? "D" : "E", eh, eh < 0.01f ? "OK" : "FALHA");
            if (eh >= 0.01f) ++fails;
        }
        V3 ap = BodyOffApplied();
        printf("   deslocamento registrado como aplicado: (%.3f %.3f %.3f)  %s\n", ap.x, ap.y, ap.z, Len(ap - bodyOff) < 1e-4f ? "OK" : "FALHA");
        if (Len(ap - bodyOff) >= 1e-4f) ++fails;
    }
    // agarrar: pega o pulso esquerdo de "outro Sim" e move a mão do jogador 20 cm para cima/frente
    {
        joints = rest;
        std::vector<WX> w0 = FK(joints.data(), wq, wt);
        GrabState& g = g_grab[1];
        g = {}; g.active = true; g.actor = (void*)0x1234; memcpy(g.idx, in.idx, sizeof(g.idx)); g.part = GP_ARM_L; g.weight = 1.0f;
        g.wpSpine2 = w0[in.idx[B_SPINE2]].p; g.wpNeck = w0[in.idx[B_NECK]].p;
        for (int k = 0; k < 2; ++k) { g.wpClav[k] = w0[in.idx[B_CLAV + k]].p; g.wpUpper[k] = w0[in.idx[B_UPPER + k]].p; }
        g.theirFwd = fwd;
        V3 wrist0 = w0[in.idx[B_HAND]].p;
        g.handPos = wrist0 + v3(0.02f, 0.01f, 0.0f); g.handRot = Q{ 0, 0, 0, 1 };
        V3 move = up * 0.15f + fwd * 0.12f;
        float err = 0;
        for (int fr = 0; fr < 90; ++fr) {                     // 1,5 s a 60 fps
            joints = rest;
            if (fr == 1) g.handPos = g.handPos + move;          // a mão do jogador se move depois do 1º frame
            g.tick = GetTickCount64();
            VrGrabApply((void*)0x1234, joints.data(), kSkN);
        }
        std::vector<WX> w1 = FK(joints.data(), wq, wt);
        V3 want = wrist0 + move;
        err = Len(w1[in.idx[B_HAND]].p - want);
        printf("== agarrar\n   pulso do outro Sim seguiu a mao: erro=%.4f m (moveu %.2f m)  %s\n", err, Len(move), err < 0.01f ? "OK" : "FALHA");
        if (err >= 0.01f) ++fails;
        // puxar além do alcance: o braço estica e não passa do comprimento
        g.handPos = g.handPos + fwd * 1.0f;
        for (int fr = 0; fr < 120; ++fr) { joints = rest; g.tick = GetTickCount64(); VrGrabApply((void*)0x1234, joints.data(), kSkN); }
        w1 = FK(joints.data(), wq, wt);
        float reach = Len(w1[in.idx[B_HAND]].p - w1[in.idx[B_UPPER]].p);
        printf("   puxando longe: alcance do braco = %.3f m (max ~0.608)  %s\n", reach, reach < 0.62f ? "OK" : "FALHA");
        if (reach >= 0.62f) ++fails;
        g = {};
    }
    // ================= Fase 2
    {
        auto fill = [&](RigInput& r, const std::vector<WX>& w) {
            r.wpSpine2 = w[in.idx[B_SPINE2]].p; r.wpNeck = w[in.idx[B_NECK]].p;
            for (int k = 0; k < 2; ++k) { r.wpClav[k] = w[in.idx[B_CLAV + k]].p; r.wpUpper[k] = w[in.idx[B_UPPER + k]].p; r.wpThigh[k] = w[in.idx[B_THIGH + k]].p; }
            r.wpRoot = w[in.idx[B_ROOT]].p; r.wpSpine0 = w[in.idx[B_SPINE0]].p;
            r.wpOk = true; r.wpLower = true;
        };
        in.idx[B_PELVIS] = Find("b__Pelvis__");
        for (int k = 0; k < 2; ++k) {
            const char* sd = k ? "R" : "L"; char nm2[64];
            snprintf(nm2, 64, "b__%s_Thigh__", sd); in.idx[B_THIGH + k] = Find(nm2);
            snprintf(nm2, 64, "b__%s_Calf__", sd);  in.idx[B_CALF + k] = Find(nm2);
            snprintf(nm2, 64, "b__%s_Foot__", sd);  in.idx[B_FOOT + k] = Find(nm2);
        }
        // --- andar: o corpo avança a 1,8 m/s por 1 s e para
        g_feet[0] = {}; g_feet[1] = {}; g_prevRootOk = false;
        V3 wt0 = wt; int steps = 0; bool wasStep[2] = { false, false }; float minFoot = 9;
        std::vector<WX> w1;
        for (int fr = 0; fr < 120; ++fr) {
            V3 wtf = wt0 + fwd * (0.03f * (std::min)(fr, 60));
            joints = rest;
            std::vector<WX> w0 = FK(joints.data(), wq, wtf);
            RigInput r = {}; memcpy(r.idx, in.idx, sizeof(r.idx)); fill(r, w0);
            r.legs = true; r.weight = 1.0f; r.active = true; r.bodyFwd = fwd; r.tick = GetTickCount64();
            g_rigIn = r;
            VrRigApply(joints.data(), kSkN);
            w1 = FK(joints.data(), wq, wtf);
            for (int k = 0; k < 2; ++k) {
                if (g_feet[k].stepping && !wasStep[k]) ++steps;
                wasStep[k] = g_feet[k].stepping;
                float fy = w1[in.idx[B_FOOT + k]].p.y - wtf.y;
                if (fy < minFoot) minFoot = fy;
            }
        }
        V3 wtEnd = wt0 + fwd * (0.03f * 60);
        float off0 = Len(v3(w1[in.idx[B_FOOT]].p.x - w1[in.idx[B_THIGH]].p.x, 0, w1[in.idx[B_FOOT]].p.z - w1[in.idx[B_THIGH]].p.z));
        float off1 = Len(v3(w1[in.idx[B_FOOT + 1]].p.x - w1[in.idx[B_THIGH + 1]].p.x, 0, w1[in.idx[B_FOOT + 1]].p.z - w1[in.idx[B_THIGH + 1]].p.z));
        bool okWalk = steps >= 4 && minFoot > 0.09f && off0 < 0.2f && off1 < 0.2f;
        printf("== pernas (andar 1,8 m e parar)\n   passos=%d  altura minima do tornozelo=%.3f m  pe sob o quadril no fim: E %.3f D %.3f m  %s\n",
               steps, minFoot, off0, off1, okWalk ? "OK" : "FALHA");
        if (!okWalk) ++fails;
        // --- agachar 30 cm: pés no chão, quadril mais baixo
        {
            joints = rest;
            std::vector<WX> w0 = FK(joints.data(), wq, wtEnd);
            RigInput r = {}; memcpy(r.idx, in.idx, sizeof(r.idx)); fill(r, w0);
            r.legs = true; r.weight = 1.0f; r.active = true; r.tick = GetTickCount64();
            r.bodyOff = v3(0, -0.30f, 0); r.measOff = v3(0, 0, 0);
            g_rigIn = r;
            VrRigApply(joints.data(), kSkN);
            std::vector<WX> w2 = FK(joints.data(), wq, wtEnd);
            float pelDrop = w0[in.idx[B_PELVIS]].p.y - w2[in.idx[B_PELVIS]].p.y;
            float footY = (std::min)(w2[in.idx[B_FOOT]].p.y, w2[in.idx[B_FOOT + 1]].p.y) - wtEnd.y;
            float knee = Len(w2[in.idx[B_CALF]].p - w2[in.idx[B_THIGH]].p);
            bool ok = std::fabs(pelDrop - 0.30f) < 0.01f && std::fabs(footY - g_ankleH) < 0.02f && std::fabs(knee - 0.483f) < 0.01f;
            printf("== agachar 30 cm\n   quadril desceu %.3f m  tornozelo a %.3f m do chao  coxa %.3f m  %s\n", pelDrop, footY, knee, ok ? "OK" : "FALHA");
            if (!ok) ++fails;
        }
        // --- inclinar: cabeça 20 cm para frente
        {
            joints = rest;
            std::vector<WX> w0 = FK(joints.data(), wq, wtEnd);
            V3 feet0 = w0[in.idx[B_FOOT]].p;
            RigInput r = {}; memcpy(r.idx, in.idx, sizeof(r.idx)); fill(r, w0);
            r.legs = true; r.weight = 1.0f; r.active = true; r.tick = GetTickCount64();
            r.lean = fwd * 0.20f;
            g_rigIn = r;
            VrRigApply(joints.data(), kSkN);
            std::vector<WX> w2 = FK(joints.data(), wq, wtEnd);
            float neckFwd = Dot(w2[in.idx[B_NECK]].p - w0[in.idx[B_NECK]].p, fwd);
            float footMove = Len(w2[in.idx[B_FOOT]].p - g_feet[0].plant - v3(0, g_ankleH, 0));
            bool ok = neckFwd > 0.12f && neckFwd < 0.3f;
            printf("== inclinar 20 cm para frente\n   pescoco foi %.3f m para frente  %s\n", neckFwd, ok ? "OK" : "FALHA");
            if (!ok) ++fails;
            (void)feet0; (void)footMove;
        }
    }
    printf("\n%s (%d falhas)\n", fails ? "HA FALHAS" : "TUDO OK", fails);
    return fails ? 1 : 0;
}
