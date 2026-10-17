// xr_input.inl: controles do Quest (OpenXR actions). Incluído por xr_session.inl.
// Perfil: /interaction_profiles/oculus/touch_controller. O estado é lido a cada frame do headset
// (thread de render, dentro do frame) e guardado em g_ctrl[] (0 = esquerda, 1 = direita).
struct CtrlState {
    XrPosef grip, aim; bool gripValid, aimValid;
    float trigger, squeeze, sx, sy;
    bool a, b, stickClick, menu;          // a/b = A/B (direita) ou X/Y (esquerda); menu só na esquerda
    bool thumbTouch;                      // polegar encostado em A/B (X/Y), no analógico ou no apoio
};
static CtrlState g_ctrl[2] = {};               // estado lógico (depois do remapeamento): o que as funções do mod usam
static CtrlState g_ctrlRaw[2] = {};            // estado físico (poses e dedos do rig, menu de configurações)

// ---- remapeamento (aba "Controles VR" das configurações): cada entrada lógica (o lugar onde o mod lê
// um botão) recebe um botão físico. Gatilho e grip são analógicos; botões físicos valem 0 ou 1.
enum { PI_NONE = 0, PI_LT, PI_LG, PI_X, PI_Y, PI_LS, PI_MENU, PI_RT, PI_RG, PI_A, PI_B, PI_RS, PI_COUNT };
static const char* const kPhysKey[PI_COUNT] = { "NADA", "GATILHO_E", "GRIP_E", "X", "Y", "ANALOGICO_E", "MENU",
                                                "GATILHO_D", "GRIP_D", "A", "B", "ANALOGICO_D" };
enum { SL_LT = 0, SL_LG, SL_X, SL_Y, SL_LS, SL_MENU, SL_RT, SL_RG, SL_A, SL_B, SL_RS, SL_COUNT };
static const int kBindDefault[SL_COUNT] = { PI_LT, PI_LG, PI_X, PI_Y, PI_LS, PI_MENU, PI_RT, PI_RG, PI_A, PI_B, PI_RS };
static int  g_bind[SL_COUNT] = { PI_LT, PI_LG, PI_X, PI_Y, PI_LS, PI_MENU, PI_RT, PI_RG, PI_A, PI_B, PI_RS };
static bool g_swapSticks = false;              // trocar_analogicos: andar no direito, girar no esquerdo
static volatile bool g_inputMuted = false;     // configurações abertas: o jogo não recebe botões nem analógicos
static float PhysValue(const CtrlState* st, int pi) {
    switch (pi) {
    case PI_LT: return st[0].trigger;   case PI_LG: return st[0].squeeze;
    case PI_X:  return st[0].a ? 1.f : 0.f;  case PI_Y: return st[0].b ? 1.f : 0.f;
    case PI_LS: return st[0].stickClick ? 1.f : 0.f; case PI_MENU: return st[0].menu ? 1.f : 0.f;
    case PI_RT: return st[1].trigger;   case PI_RG: return st[1].squeeze;
    case PI_A:  return st[1].a ? 1.f : 0.f;  case PI_B: return st[1].b ? 1.f : 0.f;
    case PI_RS: return st[1].stickClick ? 1.f : 0.f;
    default: return 0.0f;
    }
}
static void ApplyRemap(const CtrlState raw[2], CtrlState out[2]) {
    out[0] = raw[0]; out[1] = raw[1];                                  // poses, validade, polegar
    auto v = [&](int slot) { return PhysValue(raw, g_bind[slot]); };
    out[0].trigger = v(SL_LT); out[0].squeeze = v(SL_LG);
    out[0].a = v(SL_X) > 0.5f; out[0].b = v(SL_Y) > 0.5f; out[0].stickClick = v(SL_LS) > 0.5f; out[0].menu = v(SL_MENU) > 0.5f;
    out[1].trigger = v(SL_RT); out[1].squeeze = v(SL_RG);
    out[1].a = v(SL_A) > 0.5f; out[1].b = v(SL_B) > 0.5f; out[1].stickClick = v(SL_RS) > 0.5f; out[1].menu = false;
    if (g_swapSticks) { std::swap(out[0].sx, out[1].sx); std::swap(out[0].sy, out[1].sy); }
    if (g_inputMuted) for (int h = 0; h < 2; ++h) {
        out[h].trigger = out[h].squeeze = out[h].sx = out[h].sy = 0.0f;
        out[h].a = out[h].b = out[h].stickClick = out[h].menu = false;
    }
}
static XrActionSet g_actSet = XR_NULL_HANDLE;
static XrAction a_grip = XR_NULL_HANDLE, a_aim = XR_NULL_HANDLE, a_trig = XR_NULL_HANDLE, a_squeeze = XR_NULL_HANDLE,
                a_stick = XR_NULL_HANDLE, a_stickClick = XR_NULL_HANDLE, a_btnA = XR_NULL_HANDLE, a_btnB = XR_NULL_HANDLE,
                a_menu = XR_NULL_HANDLE, a_thumbTouch = XR_NULL_HANDLE, a_haptic = XR_NULL_HANDLE;
static XrPath   g_handPath[2] = {};
static XrSpace  g_gripSpace[2] = {}, g_aimSpace[2] = {};
static bool     g_inputReady = false;
static bool     g_trigActive[2] = {};

static PFN_xrStringToPath                      xrStringToPath_ = nullptr;
static PFN_xrCreateActionSet                   xrCreateActionSet_ = nullptr;
static PFN_xrCreateAction                      xrCreateAction_ = nullptr;
static PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings_ = nullptr;
static PFN_xrAttachSessionActionSets           xrAttachSessionActionSets_ = nullptr;
static PFN_xrCreateActionSpace                 xrCreateActionSpace_ = nullptr;
static PFN_xrSyncActions                       xrSyncActions_ = nullptr;
static PFN_xrGetActionStateFloat               xrGetActionStateFloat_ = nullptr;
static PFN_xrGetActionStateVector2f            xrGetActionStateVector2f_ = nullptr;
static PFN_xrGetActionStateBoolean             xrGetActionStateBoolean_ = nullptr;
static PFN_xrApplyHapticFeedback               xrApplyHapticFeedback_ = nullptr;

static XrPath XrPathOf(const char* s) { XrPath p = XR_NULL_PATH; xrStringToPath_(g_xrInstance, s, &p); return p; }

// Chamado na thread do VR, depois de criar a sessão e os espaços de referência.
static bool XrInputInit(XrSession session) {
    XR_LOAD(g_xrInstance, xrStringToPath); XR_LOAD(g_xrInstance, xrCreateActionSet); XR_LOAD(g_xrInstance, xrCreateAction);
    XR_LOAD(g_xrInstance, xrSuggestInteractionProfileBindings); XR_LOAD(g_xrInstance, xrAttachSessionActionSets);
    XR_LOAD(g_xrInstance, xrCreateActionSpace); XR_LOAD(g_xrInstance, xrSyncActions);
    XR_LOAD(g_xrInstance, xrGetActionStateFloat); XR_LOAD(g_xrInstance, xrGetActionStateVector2f);
    XR_LOAD(g_xrInstance, xrGetActionStateBoolean); XR_LOAD(g_xrInstance, xrApplyHapticFeedback);

    g_handPath[0] = XrPathOf("/user/hand/left");
    g_handPath[1] = XrPathOf("/user/hand/right");
    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy_s(asci.actionSetName, "ts4vr"); strcpy_s(asci.localizedActionSetName, "TS4 VR");
    XrResult r = xrCreateActionSet_(g_xrInstance, &asci, &g_actSet);
    if (XR_FAILED(r)) { Log("vr: xrCreateActionSet falhou: %s", XrStr(r)); return false; }

    auto mk = [&](XrAction& a, const char* name, XrActionType type, bool bothHands) {
        XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
        strcpy_s(ci.actionName, name); strcpy_s(ci.localizedActionName, name);
        ci.actionType = type;
        if (bothHands) { ci.countSubactionPaths = 2; ci.subactionPaths = g_handPath; }
        XrResult rr = xrCreateAction_(g_actSet, &ci, &a);
        if (XR_FAILED(rr)) Log("vr: acao %s falhou: %s", name, XrStr(rr));
    };
    mk(a_grip, "grip_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    mk(a_aim, "aim_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    mk(a_trig, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    mk(a_squeeze, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
    mk(a_stick, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    mk(a_stickClick, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    mk(a_btnA, "button_a_x", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    mk(a_btnB, "button_b_y", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    mk(a_menu, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    mk(a_thumbTouch, "thumb_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    mk(a_haptic, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);

    std::vector<XrActionSuggestedBinding> b;
    const char* hands[2] = { "left", "right" };
    char buf[128];
    for (int h = 0; h < 2; ++h) {
        auto add = [&](XrAction a, const char* sub) {
            snprintf(buf, sizeof(buf), "/user/hand/%s/%s", hands[h], sub);
            b.push_back({ a, XrPathOf(buf) });
        };
        add(a_grip, "input/grip/pose"); add(a_aim, "input/aim/pose");
        add(a_trig, "input/trigger/value"); add(a_squeeze, "input/squeeze/value");
        add(a_stick, "input/thumbstick"); add(a_stickClick, "input/thumbstick/click");
        add(a_btnA, h == 0 ? "input/x/click" : "input/a/click");
        add(a_btnB, h == 0 ? "input/y/click" : "input/b/click");
        // várias fontes para a mesma ação booleana: o runtime faz o OU entre elas
        add(a_thumbTouch, h == 0 ? "input/x/touch" : "input/a/touch");
        add(a_thumbTouch, h == 0 ? "input/y/touch" : "input/b/touch");
        add(a_thumbTouch, "input/thumbstick/touch");
        add(a_thumbTouch, "input/thumbrest/touch");
        add(a_haptic, "output/haptic");
    }
    b.push_back({ a_menu, XrPathOf("/user/hand/left/input/menu/click") });
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    sb.interactionProfile = XrPathOf("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = (uint32_t)b.size(); sb.suggestedBindings = b.data();
    r = xrSuggestInteractionProfileBindings_(g_xrInstance, &sb);
    if (XR_FAILED(r)) { Log("vr: bindings do Touch falharam: %s", XrStr(r)); return false; }

    XrSessionActionSetsAttachInfo ai = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    ai.countActionSets = 1; ai.actionSets = &g_actSet;
    r = xrAttachSessionActionSets_(session, &ai);
    if (XR_FAILED(r)) { Log("vr: xrAttachSessionActionSets falhou: %s", XrStr(r)); return false; }

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.subactionPath = g_handPath[h]; sci.poseInActionSpace.orientation.w = 1.0f;
        sci.action = a_grip; xrCreateActionSpace_(session, &sci, &g_gripSpace[h]);
        sci.action = a_aim;  xrCreateActionSpace_(session, &sci, &g_aimSpace[h]);
    }
    g_inputReady = true;
    Log("vr: controles prontos (perfil oculus/touch_controller)");
    return true;
}

// Vibração no controle h (0 esq, 1 dir): amplitude 0..1, duração em segundos.
static void XrHaptic(int h, float amp, float seconds) {
    if (!g_inputReady || !xrApplyHapticFeedback_ || g_xrSession == XR_NULL_HANDLE || !g_xrRunning) return;
    XrHapticVibration v = { XR_TYPE_HAPTIC_VIBRATION };
    v.amplitude = amp; v.duration = (XrDuration)(seconds * 1e9); v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi = { XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = a_haptic; hi.subactionPath = g_handPath[h];
    xrApplyHapticFeedback_(g_xrSession, &hi, (const XrHapticBaseHeader*)&v);
}

static bool LocatePose(XrSpace s, XrTime t, XrPosef& out) {
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace_(s, g_localSpace, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    out = loc.pose;
    return true;
}

// Chamado dentro do frame do headset (thread de render), com o tempo previsto do frame.
static void XrInputSync(XrTime t) {
    if (!g_inputReady) return;
    XrActiveActionSet aas = { g_actSet, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1; si.activeActionSets = &aas;
    XrResult r = xrSyncActions_(g_xrSession, &si);
    if (XR_FAILED(r)) return;
    CtrlState st[2] = {};
    for (int h = 0; h < 2; ++h) {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.subactionPath = g_handPath[h];
        XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
        gi.action = a_trig;    if (XR_SUCCEEDED(xrGetActionStateFloat_(g_xrSession, &gi, &f))) { g_trigActive[h] = f.isActive != 0; if (f.isActive) st[h].trigger = f.currentState; }
        gi.action = a_squeeze; if (XR_SUCCEEDED(xrGetActionStateFloat_(g_xrSession, &gi, &f)) && f.isActive) st[h].squeeze = f.currentState;
        XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
        gi.action = a_stick;   if (XR_SUCCEEDED(xrGetActionStateVector2f_(g_xrSession, &gi, &v)) && v.isActive) { st[h].sx = v.currentState.x; st[h].sy = v.currentState.y; }
        XrActionStateBoolean bb = { XR_TYPE_ACTION_STATE_BOOLEAN };
        gi.action = a_stickClick; if (XR_SUCCEEDED(xrGetActionStateBoolean_(g_xrSession, &gi, &bb)) && bb.isActive) st[h].stickClick = bb.currentState != 0;
        gi.action = a_btnA;    if (XR_SUCCEEDED(xrGetActionStateBoolean_(g_xrSession, &gi, &bb)) && bb.isActive) st[h].a = bb.currentState != 0;
        gi.action = a_btnB;    if (XR_SUCCEEDED(xrGetActionStateBoolean_(g_xrSession, &gi, &bb)) && bb.isActive) st[h].b = bb.currentState != 0;
        gi.action = a_thumbTouch; if (XR_SUCCEEDED(xrGetActionStateBoolean_(g_xrSession, &gi, &bb)) && bb.isActive) st[h].thumbTouch = bb.currentState != 0;
        st[h].gripValid = LocatePose(g_gripSpace[h], t, st[h].grip);
        st[h].aimValid  = LocatePose(g_aimSpace[h], t, st[h].aim);
    }
    {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = a_menu;
        XrActionStateBoolean bb = { XR_TYPE_ACTION_STATE_BOOLEAN };
        if (XR_SUCCEEDED(xrGetActionStateBoolean_(g_xrSession, &gi, &bb)) && bb.isActive) st[0].menu = bb.currentState != 0;
    }
    CtrlState lg[2]; ApplyRemap(st, lg);
    AcquireSRWLockExclusive(&g_hmdLock);
    g_ctrlRaw[0] = st[0]; g_ctrlRaw[1] = st[1];
    g_ctrl[0] = lg[0]; g_ctrl[1] = lg[1];
    ReleaseSRWLockExclusive(&g_hmdLock);
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last > 5000) {
        last = now;
        Log("vr: gatilho ativo E=%d D=%d", g_trigActive[0], g_trigActive[1]);
        Log("vr: controles E(pose=%d trig=%.2f grip=%.2f stick=%.2f,%.2f polegar=%d) D(pose=%d trig=%.2f grip=%.2f stick=%.2f,%.2f polegar=%d)",
            st[0].gripValid && st[0].aimValid, st[0].trigger, st[0].squeeze, st[0].sx, st[0].sy, st[0].thumbTouch,
            st[1].gripValid && st[1].aimValid, st[1].trigger, st[1].squeeze, st[1].sx, st[1].sy, st[1].thumbTouch);
    }
}
