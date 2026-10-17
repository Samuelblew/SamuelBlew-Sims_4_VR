// xr_session.inl: fase V0 do VR. Incluído em fpc_rig.cpp (mesma unidade: usa Log, FindPattern, MinHook).
//
// O que faz (só com vr=1 no fpc_rig.ini):
//   1. Hook em IDXGISwapChain::Present para pegar o ID3D11Device do próprio jogo.
//   2. Numa thread própria: carrega openxr_loader.dll (ao lado desta DLL), cria a instância com
//      XR_KHR_D3D11_enable, a sessão com o device do jogo, e roda o loop de frames do headset
//      (frames vazios: ainda não há imagem). O jogo não fica preso ao ritmo do headset.
//   3. Lê a pose da cabeça (espaço VIEW em LOCAL) a cada frame do headset e grava no log 1x/s.
//
// Nada daqui é usado pela câmera ainda (isso é a fase V1).

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#define XR_NO_PROTOTYPES
#include <d3d11_4.h>                                  // ID3D11Multithread
#include <dxgi1_2.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#pragma comment(lib, "d3d11.lib")

static bool g_vrEnabled = false;                      // vr=1 no ini
static int  g_recenterKeyXR = VK_HOME;                 // cópia da tecla de recentrar para a thread do VR

// ---------------------------------------------------------------- estado público (para a fase V1)
struct HmdPose { float pos[3]; float quat[4]; bool valid; };
static HmdPose        g_hmdPose = {};
static SRWLOCK        g_hmdLock = SRWLOCK_INIT;
static volatile bool  g_xrRunning = false;
static volatile bool  g_uiPanel = false;               // modo painel: tela virtual em vez do estéreo
static bool           g_laserOn = false;               // laser ligado (o mouse não gira o corpo)
static bool           g_symFov = false;                  // vr_fov_simetrico: mira/interface central iguais nos dois olhos

// ---- fase V4a (AER: um olho por frame do jogo, alternando)
struct EyeView { XrPosef pose; XrFovf fov; };
static EyeView        g_views[2] = {};                 // última pose/FOV de cada olho (LOCAL), via xrLocateViews
static bool           g_viewsValid = false;
static bool           g_vrStereo = false;              // modo 1 (AER)
static bool           g_vrStereoSync = false;          // modo 2 (síncrono: 2 renders por frame)
struct AerItem { int eye; EyeView v; bool sync; };
static AerItem        g_aerQueue[8];                   // qual olho cada frame do jogo desenhou (main -> render)
static volatile LONG  g_aerHead = 0, g_aerTail = 0;
static SRWLOCK        g_aerLock = SRWLOCK_INIT;
static volatile ULONGLONG g_aerLastPush = 0;           // último frame em modo estéreo (ms)
static float          g_projTan[4] = {};               // tan(esq), tan(dir), tan(cima), tan(baixo) do olho atual
static volatile bool  g_aerProjActive = false;         // aplicar a projeção do olho na câmera da FPC

static void QuatRotateXR(const float q[4], const float v[3], float o[3]) {
    float cx = q[1] * v[2] - q[2] * v[1] + q[3] * v[0];
    float cy = q[2] * v[0] - q[0] * v[2] + q[3] * v[1];
    float cz = q[0] * v[1] - q[1] * v[0] + q[3] * v[2];
    o[0] = v[0] + 2.0f * (q[1] * cz - q[2] * cy);
    o[1] = v[1] + 2.0f * (q[2] * cx - q[0] * cz);
    o[2] = v[2] + 2.0f * (q[0] * cy - q[1] * cx);
}
static void RotYXR(float th, const float v[3], float o[3]) {
    float c = std::cos(th), s = std::sin(th);
    float x = v[0] * c + v[2] * s, z = -v[0] * s + v[2] * c;
    o[0] = x; o[1] = v[1]; o[2] = z;
}
static void AerPush(int eye, const EyeView& v, bool sync = false) {
    AcquireSRWLockExclusive(&g_aerLock);
    if (g_aerHead - g_aerTail >= 4) g_aerTail = g_aerHead - 3;   // não deixa acumular (descompasso)
    g_aerQueue[g_aerHead % 8] = { eye, v, sync }; g_aerHead++;
    ReleaseSRWLockExclusive(&g_aerLock);
    g_aerLastPush = GetTickCount64();
}
static bool AerPop(AerItem& out) {
    AcquireSRWLockExclusive(&g_aerLock);
    bool ok = g_aerTail < g_aerHead;
    if (ok) { out = g_aerQueue[g_aerTail % 8]; g_aerTail++; }
    ReleaseSRWLockExclusive(&g_aerLock);
    return ok;
}
static uint32_t       g_eyeWidth = 0, g_eyeHeight = 0;  // resolução recomendada por olho
static uint32_t       g_eyeMaxW = 0, g_eyeMaxH = 0;      // resolução máxima por olho
// ---- câmera e conforto (configurações)
static float g_ipdScale     = 1.0f;     // vr_ipd_escala: separação dos olhos na imagem do jogo (x a real)
static float g_heightOffset = 0.0f;     // vr_altura (m): sobe/desce a vista
static int   g_vignette     = 0;        // vr_vinheta: 0 desligada, 1 leve, 2 média, 3 forte (só em movimento)
static volatile float g_motionWalk = 0.0f, g_motionTurn = 0.0f;   // 0..1: andando / girando com o analógico
static volatile bool  g_cfgRecenter = false;                      // botão "recentrar" das configurações
static void UpscaleForgetImages();                         // xr_upscale.inl

// ---------------------------------------------------------------- device do jogo via Present
using Present_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain* self, UINT sync, UINT flags);
static Present_t        o_Present = nullptr;
static ID3D11Device*    g_gameDevice = nullptr;
static volatile LONG    g_xrThreadStarted = 0;
static DWORD WINAPI XrThread(LPVOID);

using Present1_t = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p);
static Present1_t o_Present1 = nullptr;

static void CaptureDevice(IDXGISwapChain* self, const char* via) {
    if (!g_gameDevice && self) {
        ID3D11Device* dev = nullptr;
        if (SUCCEEDED(self->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
            g_gameDevice = dev;                                  // mantém a referência
            if (InterlockedCompareExchange(&g_xrThreadStarted, 1, 0) == 0)
                CreateThread(nullptr, 0, XrThread, nullptr, 0, nullptr);
            Log("vr: device capturado via %s", via);
        }
    }
}

// ---------------------------------------------------------------- fase V2: tela virtual no headset
// A cada Present do jogo, o backbuffer é copiado (na GPU) para um swapchain do OpenXR. A thread do
// VR envia esse swapchain como uma "quad layer" fixa no quarto. O acesso ao swapchain é protegido
// por g_xrLock (cópia na thread de render x xrEndFrame na thread do VR).
static CRITICAL_SECTION g_xrLock;
static XrSwapchain      g_quadSwap = XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_quadImgs;
static uint32_t         g_quadW = 0, g_quadH = 0;
static volatile bool    g_quadHasImage = false;
static volatile LONG    g_bbW = 0, g_bbH = 0, g_bbFmt = 0;   // backbuffer atual do jogo
static bool             g_vrHeadTracking = true;            // vr_rastrear_cabeca no ini
static float            g_screenWidth = 2.4f, g_screenDist = 2.2f;
static thread_local bool t_inPresent = false;

static PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage_ = nullptr;
static PFN_xrWaitSwapchainImage    xrWaitSwapchainImage_ = nullptr;
static PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage_ = nullptr;

// Todo o envio de frames ao headset roda aqui, na thread de render do jogo, dentro do Present:
// o runtime da Meta usa o contexto D3D11 do jogo no xrEndFrame, e isso não pode acontecer em
// paralelo com o jogo desenhando (foi o que derrubou a primeira versão da V2).
static bool XrRenderFrame(IDXGISwapChain* sc);   // true = não apresentar no monitor (olho esquerdo)
static volatile LONG g_xrFramesCalled = 0, g_xrFramesDone = 0;   // diagnóstico

// ---- diagnóstico: pilha de chamadas (para achar o loop de render do jogo no Ghidra)
#include <psapi.h>
static void LogStack(const char* tag) {
    void* frames[32];
    USHORT n = CaptureStackBackTrace(1, 32, frames, nullptr);
    char line[2048]; int pos = snprintf(line, sizeof(line), "pilha %s (thread %lu):", tag, GetCurrentThreadId());
    for (USHORT i = 0; i < n && pos < (int)sizeof(line) - 64; ++i) {
        HMODULE m = nullptr; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &m) && m) {
            GetModuleBaseNameA(GetCurrentProcess(), m, name, sizeof(name));
            if (m == GetModuleHandleA(nullptr))
                pos += snprintf(line + pos, sizeof(line) - pos, " TS4+%llx", (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)m) + 0x140000000ull);
            else
                pos += snprintf(line + pos, sizeof(line) - pos, " %s+%llx", name, (unsigned long long)((uint8_t*)frames[i] - (uint8_t*)m));
        } else {
            pos += snprintf(line + pos, sizeof(line) - pos, " %p", frames[i]);
        }
    }
    Log("%s", line);
}

// ExecuteCommandList do contexto imediato (vtable do ID3D11DeviceContext, índice 58).
using ExecCL_t = void (STDMETHODCALLTYPE*)(ID3D11DeviceContext* self, ID3D11CommandList* cl, BOOL restore);
static ExecCL_t o_ExecCL = nullptr;
static volatile LONG g_stackWanted = 0;                 // > 0: registrar as próximas N execuções
static void STDMETHODCALLTYPE hk_ExecCL(ID3D11DeviceContext* self, ID3D11CommandList* cl, BOOL restore) {
    if (g_stackWanted > 0 && InterlockedDecrement(&g_stackWanted) >= 0) LogStack("ExecuteCommandList");
    o_ExecCL(self, cl, restore);
}
static void InstallExecHook() {
    static bool done = false;
    if (done || !g_gameDevice) return;
    done = true;
    ID3D11DeviceContext* ctx = nullptr;
    g_gameDevice->GetImmediateContext(&ctx);
    if (!ctx) return;
    void* fn = (*(void***)ctx)[58];
    ctx->Release();
    MH_STATUS st = MH_CreateHook(fn, &hk_ExecCL, (void**)&o_ExecCL);
    if (st == MH_OK) st = MH_EnableHook(fn);
    Log("vr: hook de diagnostico em ExecuteCommandList (%p) status %d", fn, (int)st);
}

// ---- vtable sombra: só o swapchain do jogo passa a apontar para uma cópia da vtable com
// Present/Present1 trocados pelos nossos. Hooks no código do DXGI feitos depois (por outros
// programas) não tiram os nossos do caminho.
static void*            g_shadowVt[41];                      // IDXGISwapChain4 tem 41 métodos
static IDXGISwapChain*  g_vtSwap = nullptr;
static Present_t        o_PresentV = nullptr;
static Present1_t       o_Present1V = nullptr;
static volatile LONG    g_vtCalls = 0;
static void**           g_realVt = nullptr;                  // vtable original do swapchain do jogo
using Release_t = ULONG (STDMETHODCALLTYPE*)(IUnknown*);
static Release_t        o_ReleaseV = nullptr;
static Release_t        o_AddRefV = nullptr;
// Ao fechar o jogo, o swapchain era destruído ainda com a nossa vtable (crash dentro do DXGI ao sair).
// Na liberação que vai destruí-lo, a vtable original volta antes.
static ULONG STDMETHODCALLTYPE hk_ReleaseV(IUnknown* self) {
    o_AddRefV(self);
    ULONG before = o_ReleaseV(self);                          // contagem atual (sem alterar)
    if (before == 1 && g_realVt && *(void***)self == g_shadowVt) {
        *(void***)self = g_realVt;
        if (g_vtSwap == (IDXGISwapChain*)self) g_vtSwap = nullptr;
        Log("vr: swapchain do jogo sendo destruido; vtable original devolvida");
    }
    return o_ReleaseV(self);
}

static void VrConfigPresent(IDXGISwapChain* sc, bool afterVr);   // vr_config.inl: menu de configurações no monitor
static HRESULT STDMETHODCALLTYPE hk_PresentV(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (t_inPresent) return o_PresentV(self, sync, flags);
    t_inPresent = true;
    InterlockedIncrement(&g_vtCalls);
    VrConfigPresent(self, false);
    if (g_gameDevice && XrRenderFrame(self)) { t_inPresent = false; return S_OK; }
    VrConfigPresent(self, true);
    HRESULT hr = o_PresentV(self, sync, flags);
    t_inPresent = false;
    return hr;
}

static volatile LONG g_presentStacks = 3;               // registra a pilha dos primeiros Presents
static HRESULT STDMETHODCALLTYPE hk_Present1V(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (t_inPresent) return o_Present1V(self, sync, flags, p);
    t_inPresent = true;
    InterlockedIncrement(&g_vtCalls);
    if (g_presentStacks > 0 && InterlockedDecrement(&g_presentStacks) >= 0) LogStack("Present1");
    VrConfigPresent(self, false);
    if (g_gameDevice && XrRenderFrame(self)) { t_inPresent = false; return S_OK; }
    VrConfigPresent(self, true);
    HRESULT hr = o_Present1V(self, sync, flags, p);
    t_inPresent = false;
    return hr;
}

static void ApplyShadowVtable(IDXGISwapChain* sc) {
    void** vt = *(void***)sc;
    if (vt == g_shadowVt) return;
    if (g_vtSwap && g_vtSwap != sc) Log("vr: o jogo trocou de swapchain (%p -> %p); reaplicando a vtable", g_vtSwap, sc);
    memcpy(g_shadowVt, vt, sizeof(g_shadowVt));
    g_realVt = vt;
    o_AddRefV   = (Release_t)vt[1];
    o_ReleaseV  = (Release_t)vt[2];
    g_shadowVt[2] = (void*)&hk_ReleaseV;
    o_PresentV  = (Present_t)vt[8];
    o_Present1V = (Present1_t)vt[22];
    g_shadowVt[8]  = (void*)&hk_PresentV;
    g_shadowVt[22] = (void*)&hk_Present1V;
    *(void***)sc = g_shadowVt;
    g_vtSwap = sc;
    Log("vr: vtable sombra aplicada no swapchain do jogo (%p)", sc);
}

static HRESULT STDMETHODCALLTYPE hk_Present(IDXGISwapChain* self, UINT sync, UINT flags) {
    if (!t_inPresent) { CaptureDevice(self, "Present"); if (g_vtSwap != self || *(void***)self != g_shadowVt) ApplyShadowVtable(self); }
    return o_Present(self, sync, flags);
}

static HRESULT STDMETHODCALLTYPE hk_Present1(IDXGISwapChain1* self, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!t_inPresent) { CaptureDevice(self, "Present1"); if (g_vtSwap != self || *(void***)self != g_shadowVt) ApplyShadowVtable(self); }
    return o_Present1(self, sync, flags, p);
}

// Descobre o endereço de Present criando um swapchain descartável numa janela escondida.
static void* g_present1Addr = nullptr;


static void* FindPresentAddress() {
    WNDCLASSEXA wc = { sizeof(wc) }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "fpc_rig_dummy_dx";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    void* present = nullptr;
    if (SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx))) {
        present = (*(void***)sc)[8];                              // IDXGISwapChain::Present
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            g_present1Addr = (*(void***)sc1)[22];                 // IDXGISwapChain1::Present1
            sc1->Release();
        }
        sc->Release(); ctx->Release(); dev->Release();
    }
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return present;
}

// ---------------------------------------------------------------- OpenXR (funções carregadas na hora)
static PFN_xrGetInstanceProcAddr                  xrGetInstanceProcAddr_ = nullptr;
static PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties_ = nullptr;
static PFN_xrCreateInstance                       xrCreateInstance_ = nullptr;
static PFN_xrGetInstanceProperties                xrGetInstanceProperties_ = nullptr;
static PFN_xrGetSystem                            xrGetSystem_ = nullptr;
static PFN_xrGetSystemProperties                  xrGetSystemProperties_ = nullptr;
static PFN_xrEnumerateViewConfigurationViews      xrEnumerateViewConfigurationViews_ = nullptr;
static PFN_xrCreateSession                        xrCreateSession_ = nullptr;
static PFN_xrCreateReferenceSpace                 xrCreateReferenceSpace_ = nullptr;
static PFN_xrPollEvent                            xrPollEvent_ = nullptr;
static PFN_xrBeginSession                         xrBeginSession_ = nullptr;
static PFN_xrEndSession                           xrEndSession_ = nullptr;
static PFN_xrWaitFrame                            xrWaitFrame_ = nullptr;
static PFN_xrBeginFrame                           xrBeginFrame_ = nullptr;
static PFN_xrEndFrame                             xrEndFrame_ = nullptr;
static PFN_xrLocateSpace                          xrLocateSpace_ = nullptr;
static PFN_xrResultToString                       xrResultToString_ = nullptr;
static PFN_xrGetD3D11GraphicsRequirementsKHR      xrGetD3D11GraphicsRequirementsKHR_ = nullptr;
static PFN_xrEnumerateSwapchainFormats            xrEnumerateSwapchainFormats_ = nullptr;
static PFN_xrCreateSwapchain                      xrCreateSwapchain_ = nullptr;
static PFN_xrDestroySwapchain                     xrDestroySwapchain_ = nullptr;
static PFN_xrEnumerateSwapchainImages             xrEnumerateSwapchainImages_ = nullptr;
static PFN_xrLocateViews                          xrLocateViews_ = nullptr;

static XrInstance g_xrInstance = XR_NULL_HANDLE;
static XrSession  g_xrSession  = XR_NULL_HANDLE;
static XrSpace    g_localSpace = XR_NULL_HANDLE, g_viewSpace = XR_NULL_HANDLE;
static std::vector<int64_t> g_xrFormats;
static XrPosef    g_quadPose = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
static bool       g_quadPlaced = false;

static const char* XrStr(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (xrResultToString_ && g_xrInstance && XR_SUCCEEDED(xrResultToString_(g_xrInstance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult %d", (int)r);
    return buf;
}

#define XR_LOAD(inst, name) \
    if (XR_FAILED(xrGetInstanceProcAddr_(inst, #name, (PFN_xrVoidFunction*)&name##_))) { Log("vr: funcao %s indisponivel", #name); return false; }

static bool XrLoadLoader() {
    char path[MAX_PATH];
    GetModuleFileNameA(g_self, path, MAX_PATH);
    if (char* s = strrchr(path, '\\')) strcpy_s(s + 1, MAX_PATH - (s + 1 - path), "openxr_loader.dll");
    HMODULE m = LoadLibraryA(path);
    if (!m) { Log("vr: nao achei %s (copie openxr_loader.dll para a pasta Bin)", path); return false; }
    xrGetInstanceProcAddr_ = (PFN_xrGetInstanceProcAddr)GetProcAddress(m, "xrGetInstanceProcAddr");
    if (!xrGetInstanceProcAddr_) { Log("vr: openxr_loader.dll sem xrGetInstanceProcAddr"); return false; }
    XR_LOAD(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties);
    XR_LOAD(XR_NULL_HANDLE, xrCreateInstance);
    Log("vr: loader carregado (%s)", path);
    return true;
}

static bool XrLoadInstanceFunctions(XrInstance inst) {
    XR_LOAD(inst, xrGetInstanceProperties); XR_LOAD(inst, xrGetSystem); XR_LOAD(inst, xrGetSystemProperties);
    XR_LOAD(inst, xrEnumerateViewConfigurationViews); XR_LOAD(inst, xrCreateSession);
    XR_LOAD(inst, xrCreateReferenceSpace); XR_LOAD(inst, xrPollEvent); XR_LOAD(inst, xrBeginSession);
    XR_LOAD(inst, xrEndSession); XR_LOAD(inst, xrWaitFrame); XR_LOAD(inst, xrBeginFrame);
    XR_LOAD(inst, xrEndFrame); XR_LOAD(inst, xrLocateSpace); XR_LOAD(inst, xrResultToString);
    XR_LOAD(inst, xrGetD3D11GraphicsRequirementsKHR);
    XR_LOAD(inst, xrEnumerateSwapchainFormats); XR_LOAD(inst, xrCreateSwapchain); XR_LOAD(inst, xrDestroySwapchain);
    XR_LOAD(inst, xrEnumerateSwapchainImages); XR_LOAD(inst, xrAcquireSwapchainImage);
    XR_LOAD(inst, xrWaitSwapchainImage); XR_LOAD(inst, xrReleaseSwapchainImage);
    XR_LOAD(inst, xrLocateViews);
    return true;
}

static bool XrInputInit(XrSession session);
static DWORD WINAPI XrThread(LPVOID) {
    Log("vr: device do jogo capturado (%p); iniciando OpenXR", g_gameDevice);
    InstallExecHook();
    if (!XrLoadLoader()) return 0;

    // Extensão D3D11 disponível?
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties_(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties_(nullptr, n, &n, exts.data());
    bool hasD3D11 = false;
    for (auto& e : exts) if (strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    if (!hasD3D11) { Log("vr: o runtime nao tem %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); return 0; }

    const char* enabled[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    strcpy_s(ci.applicationInfo.applicationName, "TS4 FPC VR");
    strcpy_s(ci.applicationInfo.engineName, "fpc_rig");
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabled;
    XrResult r = xrCreateInstance_(&ci, &g_xrInstance);
    if (XR_FAILED(r)) { Log("vr: xrCreateInstance falhou: %d", (int)r); return 0; }
    if (!XrLoadInstanceFunctions(g_xrInstance)) return 0;

    XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
    xrGetInstanceProperties_(g_xrInstance, &ip);
    Log("vr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    // Headset (tenta de novo enquanto não estiver conectado).
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    for (int tries = 0; ; ++tries) {
        r = xrGetSystem_(g_xrInstance, &sgi, &system);
        if (XR_SUCCEEDED(r)) break;
        if (tries % 6 == 0) Log("vr: headset nao disponivel (%s); conecte o Quest pelo Link. Tentando de novo...", XrStr(r));
        Sleep(5000);
    }
    XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };
    xrGetSystemProperties_(g_xrInstance, system, &sp);
    Log("vr: headset '%s' (tracking de posicao=%d, rotacao=%d)", sp.systemName,
        sp.trackingProperties.positionTracking, sp.trackingProperties.orientationTracking);

    XrViewConfigurationView views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
    uint32_t vc = 0;
    xrEnumerateViewConfigurationViews_(g_xrInstance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &vc, views);
    g_eyeWidth = views[0].recommendedImageRectWidth; g_eyeHeight = views[0].recommendedImageRectHeight;
    g_eyeMaxW = views[0].maxImageRectWidth; g_eyeMaxH = views[0].maxImageRectHeight;
    Log("vr: resolucao recomendada por olho %ux%u (max %ux%u)", g_eyeWidth, g_eyeHeight,
        views[0].maxImageRectWidth, views[0].maxImageRectHeight);

    // Requisito obrigatório do D3D11: o headset precisa estar na mesma GPU do device do jogo.
    XrGraphicsRequirementsD3D11KHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
    xrGetD3D11GraphicsRequirementsKHR_(g_xrInstance, system, &req);
    LUID gameLuid = {};
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(g_gameDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
            DXGI_ADAPTER_DESC d; ad->GetDesc(&d); gameLuid = d.AdapterLuid;
            char name[128]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
            Log("vr: GPU do jogo: %s", name);
            ad->Release();
        }
        dxgiDev->Release();
    }
    bool sameGpu = gameLuid.LowPart == req.adapterLuid.LowPart && gameLuid.HighPart == req.adapterLuid.HighPart;
    Log("vr: GPU exigida pelo headset %s a do jogo", sameGpu ? "== (ok)" : "!= (PROBLEMA: o headset esta em outra GPU)");

    // O runtime pode usar o contexto D3D11 em outra thread: liga a proteção multithread do device.
    ID3D11Multithread* mt = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    g_gameDevice->GetImmediateContext(&ctx);
    if (ctx && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11Multithread), (void**)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    if (ctx) ctx->Release();

    XrGraphicsBindingD3D11KHR gb = { XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
    gb.device = g_gameDevice;
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &gb; sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    r = xrCreateSession_(g_xrInstance, &sci, &session);
    if (XR_FAILED(r)) { Log("vr: xrCreateSession falhou: %s", XrStr(r)); return 0; }
    Log("vr: sessao criada");

    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; xrCreateReferenceSpace_(session, &rs, &g_localSpace);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;  xrCreateReferenceSpace_(session, &rs, &g_viewSpace);

    uint32_t nf = 0;
    xrEnumerateSwapchainFormats_(session, 0, &nf, nullptr);
    g_xrFormats.resize(nf);
    xrEnumerateSwapchainFormats_(session, nf, &nf, g_xrFormats.data());
    g_xrSession = session;
    XrInputInit(session);

    // Esta thread só trata eventos; os frames são feitos no Present (XrRenderFrame).
    bool quit = false;
    while (!quit) {
        XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent_(g_xrInstance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = (XrEventDataSessionStateChanged*)&ev;
                Log("vr: estado da sessao = %d", (int)sc->state);
                if (sc->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    EnterCriticalSection(&g_xrLock);
                    r = xrBeginSession_(session, &bi);
                    g_xrRunning = XR_SUCCEEDED(r);
                    LeaveCriticalSection(&g_xrLock);
                    Log("vr: xrBeginSession: %s", XrStr(r));
                } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                    EnterCriticalSection(&g_xrLock);
                    g_xrRunning = false;
                    xrEndSession_(session);
                    LeaveCriticalSection(&g_xrLock);
                } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = true;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                quit = true;
            }
            ev = { XR_TYPE_EVENT_DATA_BUFFER };
        }
        static ULONGLONG lastBeat = 0;
        ULONGLONG now = GetTickCount64();
        if (g_xrRunning && now - lastBeat >= 5000) {
            lastBeat = now;
            const uint8_t* code = (const uint8_t*)g_present1Addr;
            Log("vr: frames chamados=%ld enviados=%ld | Present via vtable=%ld | vtable nossa=%d | inicio do Present1: %02X %02X %02X %02X %02X",
                g_xrFramesCalled, g_xrFramesDone, g_vtCalls,
                (g_vtSwap && *(void***)g_vtSwap == g_shadowVt) ? 1 : 0,
                code ? code[0] : 0, code ? code[1] : 0, code ? code[2] : 0, code ? code[3] : 0, code ? code[4] : 0);
        }
        Sleep(20);
    }
    EnterCriticalSection(&g_xrLock);
    g_xrRunning = false;
    LeaveCriticalSection(&g_xrLock);
    Log("vr: sessao encerrada");
    return 0;
}

// ---------------------------------------------------------------- frame do headset (thread de render)
static DXGI_FORMAT PickSwapFormat(DXGI_FORMAT bb) {
    auto supported = [](int64_t f) { for (auto x : g_xrFormats) if (x == f) return true; return false; };
    // O jogo grava cores já em sRGB num formato UNORM: o swapchain usa a variante _SRGB do mesmo grupo.
    DXGI_FORMAT srgb = bb == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                     : bb == DXGI_FORMAT_B8G8R8A8_UNORM ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : bb;
    if (supported(srgb)) return srgb;
    if (supported(bb)) return bb;
    return DXGI_FORMAT_UNKNOWN;
}

static void EnsureQuadSwapchain(uint32_t w, uint32_t h, DXGI_FORMAT bbFmt) {
    if (w == g_quadW && h == g_quadH) return;
    if (g_quadSwap != XR_NULL_HANDLE) { xrDestroySwapchain_(g_quadSwap); g_quadSwap = XR_NULL_HANDLE; g_quadImgs.clear(); }
    g_quadHasImage = false;
    g_quadW = w; g_quadH = h;
    DXGI_FORMAT fmt = PickSwapFormat(bbFmt);
    if (fmt == DXGI_FORMAT_UNKNOWN) { Log("vr: formato do backbuffer %d nao suportado pelo runtime", (int)bbFmt); return; }
    XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h;
    ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrResult cr = xrCreateSwapchain_(g_xrSession, &ci, &g_quadSwap);
    if (XR_FAILED(cr)) { Log("vr: xrCreateSwapchain falhou: %s", XrStr(cr)); g_quadSwap = XR_NULL_HANDLE; return; }
    uint32_t ni = 0;
    xrEnumerateSwapchainImages_(g_quadSwap, 0, &ni, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(ni, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    xrEnumerateSwapchainImages_(g_quadSwap, ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
    for (auto& im : imgs) g_quadImgs.push_back(im.texture);
    Log("vr: tela virtual criada %ux%u formato %d (backbuffer %d), %u imagens", w, h, (int)fmt, (int)bbFmt, ni);
}

// Swapchains dos olhos (mesmo tamanho/formato do backbuffer do jogo).
static XrSwapchain g_eyeSwap[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
static std::vector<ID3D11Texture2D*> g_eyeImgs[2];
static uint32_t    g_eyeW = 0, g_eyeH = 0;
static EyeView     g_eyeLast[2] = {};
static bool        g_eyeHas[2] = { false, false };

static void EnsureEyeSwapchains(uint32_t w, uint32_t h, DXGI_FORMAT bbFmt) {
    if (w == g_eyeW && h == g_eyeH) return;
    for (int e = 0; e < 2; ++e) {
        if (g_eyeSwap[e] != XR_NULL_HANDLE) { xrDestroySwapchain_(g_eyeSwap[e]); g_eyeSwap[e] = XR_NULL_HANDLE; }
        g_eyeImgs[e].clear(); g_eyeHas[e] = false;
    }
    g_eyeW = w; g_eyeH = h;
    UpscaleForgetImages();
    DXGI_FORMAT fmt = PickSwapFormat(bbFmt);
    if (fmt == DXGI_FORMAT_UNKNOWN) return;
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo ci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h;
        ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        XrResult cr = xrCreateSwapchain_(g_xrSession, &ci, &g_eyeSwap[e]);
        if (XR_FAILED(cr)) { Log("vr: swapchain do olho %d falhou: %s", e, XrStr(cr)); g_eyeSwap[e] = XR_NULL_HANDLE; continue; }
        uint32_t ni = 0;
        xrEnumerateSwapchainImages_(g_eyeSwap[e], 0, &ni, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> imgs(ni, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
        xrEnumerateSwapchainImages_(g_eyeSwap[e], ni, &ni, (XrSwapchainImageBaseHeader*)imgs.data());
        for (auto& im : imgs) g_eyeImgs[e].push_back(im.texture);
    }
    Log("vr: swapchains dos olhos criados %ux%u formato %d", w, h, (int)fmt);
}

#include "xr_cursor.inl"
static HWND g_gameHwnd = nullptr;
#include "xr_input.inl"
#include "xr_ui.inl"
#include "xr_upscale.inl"
#include "xr_vignette.inl"

// Copia o backbuffer para uma imagem de swapchain (acquire/wait/copy/release) e desenha o cursor nela.
// Olhos (eye = true): com upscaling ligado, a imagem tem a resolução do headset (xr_upscale.inl).
static bool CopyToSwapchain(XrSwapchain swap, std::vector<ID3D11Texture2D*>& imgs, ID3D11Texture2D* bb, bool eye = false) {
    if (swap == XR_NULL_HANDLE) return false;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    if (XR_FAILED(xrAcquireSwapchainImage_(swap, &ai, &idx))) return false;
    XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    swi.timeout = 100000000;                                     // 100 ms
    bool ok = false;
    if (XR_SUCCEEDED(xrWaitSwapchainImage_(swap, &swi)) && idx < imgs.size()) {
        ID3D11DeviceContext* ctx = nullptr;
        g_gameDevice->GetImmediateContext(&ctx);
        D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
        uint32_t ow = d.Width, oh = d.Height;
        if (eye) { ow = g_eyeW; oh = g_eyeH; }
        bool scaled = eye && (ow != d.Width || oh != d.Height || g_upMethod != UP_OFF) && UpscaleInto(imgs[idx], bb, ow, oh);
        if (!scaled && (ow != d.Width || oh != d.Height)) {   // tamanhos diferentes e o upscaling falhou:
            if (ctx) ctx->Release(); ctx = nullptr;             // volta para a cópia direta (os olhos são refeitos)
            if (g_upMethod != UP_OFF) { g_upMethod = UP_OFF; Log("vr: upscaling falhou; voltando para a copia direta"); }
        }
        if (ctx) { if (!scaled) ctx->CopyResource(imgs[idx], bb); ctx->Release(); ok = true; }
        else ok = scaled;
        if (ok && !(g_uiSeparate && g_uiRedirect)) DrawCursorOn(imgs[idx], ow, oh, g_gameHwnd);
    }
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage_(swap, &ri);
    return ok;
}

static bool XrRenderFrame(IDXGISwapChain* sc) {
    if (!g_xrRunning || g_xrSession == XR_NULL_HANDLE) return false;
    if (InterlockedIncrement(&g_xrFramesCalled) == 1)
        Log("vr: primeiro frame na thread de render (thread %lu)", GetCurrentThreadId());
    EnterCriticalSection(&g_xrLock);
    if (!g_xrRunning) { LeaveCriticalSection(&g_xrLock); return false; }

    ID3D11Texture2D* bb = nullptr;
    D3D11_TEXTURE2D_DESC d = {};
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) bb->GetDesc(&d);
    bool bbOk = bb && d.SampleDesc.Count == 1;
    if (!g_gameHwnd) { DXGI_SWAP_CHAIN_DESC scd = {}; if (SUCCEEDED(sc->GetDesc(&scd))) g_gameHwnd = scd.OutputWindow; }
    XrUiInstallD3DHook();
    if (bbOk) XrUiEnsureTarget(d.Width, d.Height);

    // Estéreo (AER): este frame do jogo desenhou um olho? (a fila diz qual)
    AerItem item;
    bool stereoFrame = bbOk && AerPop(item);
    static int leftStreak = 0;
    if (stereoFrame) {
        { uint32_t ow, oh; UpscaleOutSize(d.Width, d.Height, ow, oh); EnsureEyeSwapchains(ow, oh, d.Format); }
        if (CopyToSwapchain(g_eyeSwap[item.eye], g_eyeImgs[item.eye], bb, true)) {
            g_eyeLast[item.eye] = item.v; g_eyeHas[item.eye] = true;
        }
        // Só fecha um frame do headset depois do olho direito: o jogo roda a 2x a taxa do headset.
        if (item.eye == 0 && ++leftStreak < 3) { bb->Release(); LeaveCriticalSection(&g_xrLock); return item.sync; }
        leftStreak = 0;
    }

    static int fails = 0;
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrResult wr = xrWaitFrame_(g_xrSession, &wi, &fs);
    if (XR_FAILED(wr)) {
        if (fails++ < 5) Log("vr: xrWaitFrame falhou: %s", XrStr(wr));
        if (bb) bb->Release();
        LeaveCriticalSection(&g_xrLock); return false;
    }
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrResult br = xrBeginFrame_(g_xrSession, &fbi);
    if (XR_FAILED(br) && fails++ < 5) Log("vr: xrBeginFrame falhou: %s", XrStr(br));
    XrInputSync(fs.predictedDisplayTime);

    // Pose da cabeça, poses/FOV dos olhos, posição da tela virtual.
    static bool homeDown = false;
    static ULONGLONG lastLog = 0;
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_SUCCEEDED(xrLocateSpace_(g_viewSpace, g_localSpace, fs.predictedDisplayTime, &loc))) {
        bool valid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                     (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
        bool homeNow = (GetAsyncKeyState(g_recenterKeyXR) & 0x8000) != 0;
        bool homeEdge = homeNow && !homeDown; homeDown = homeNow;
        if (valid && (!g_quadPlaced || homeEdge)) {
            const float* q = &loc.pose.orientation.x;
            float fx = -2.0f * (q[0] * q[2] + q[3] * q[1]);
            float fz = -(1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]));
            float fl = std::sqrt(fx * fx + fz * fz);
            if (fl < 1e-4f) { fx = 0.0f; fz = -1.0f; fl = 1.0f; }
            fx /= fl; fz /= fl;
            float yaw = std::atan2(-fx, -fz);
            g_quadPose.position.x = loc.pose.position.x + fx * g_screenDist;
            g_quadPose.position.y = loc.pose.position.y;
            g_quadPose.position.z = loc.pose.position.z + fz * g_screenDist;
            g_quadPose.orientation = { 0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f) };
            g_quadPlaced = true;
            Log("vr: tela posicionada a %.1f m (yaw %.0f graus)", g_screenDist, yaw * 57.29578f);
        }
        XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime; vli.space = g_localSpace;
        XrViewState vs = { XR_TYPE_VIEW_STATE };
        XrView xv[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
        uint32_t nv = 0;
        bool viewsOk = XR_SUCCEEDED(xrLocateViews_(g_xrSession, &vli, &vs, 2, &nv, xv)) && nv == 2 &&
                       (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        AcquireSRWLockExclusive(&g_hmdLock);
        g_hmdPose.pos[0] = loc.pose.position.x; g_hmdPose.pos[1] = loc.pose.position.y; g_hmdPose.pos[2] = loc.pose.position.z;
        g_hmdPose.quat[0] = loc.pose.orientation.x; g_hmdPose.quat[1] = loc.pose.orientation.y;
        g_hmdPose.quat[2] = loc.pose.orientation.z; g_hmdPose.quat[3] = loc.pose.orientation.w;
        g_hmdPose.valid = valid;
        if (viewsOk) { for (int e = 0; e < 2; ++e) { g_views[e].pose = xv[e].pose; g_views[e].fov = xv[e].fov; } g_viewsValid = true; }
        ReleaseSRWLockExclusive(&g_hmdLock);
        ULONGLONG now = GetTickCount64();
        if (now - lastLog >= 5000) {
            lastLog = now;
            Log("vr: cabeca pos=(%.3f %.3f %.3f) olhos: E fov(%.1f %.1f %.1f %.1f) D fov(%.1f %.1f %.1f %.1f) graus | estereo=%d",
                loc.pose.position.x, loc.pose.position.y, loc.pose.position.z,
                xv[0].fov.angleLeft * 57.3f, xv[0].fov.angleRight * 57.3f, xv[0].fov.angleUp * 57.3f, xv[0].fov.angleDown * 57.3f,
                xv[1].fov.angleLeft * 57.3f, xv[1].fov.angleRight * 57.3f, xv[1].fov.angleUp * 57.3f, xv[1].fov.angleDown * 57.3f,
                (int)(GetTickCount64() - g_aerLastPush < 500));
        }
    }

    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pv[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
    const XrCompositionLayerBaseHeader* layers[12] = {};
    bool stereoRecent = GetTickCount64() - g_aerLastPush < 500;

    if (fs.shouldRender && stereoRecent && g_eyeHas[0] && g_eyeHas[1]) {
        for (int e = 0; e < 2; ++e) {
            pv[e].pose = g_eyeLast[e].pose; pv[e].fov = g_eyeLast[e].fov;
            pv[e].subImage.swapchain = g_eyeSwap[e];
            pv[e].subImage.imageRect.offset = { 0, 0 };
            pv[e].subImage.imageRect.extent = { (int32_t)g_eyeW, (int32_t)g_eyeH };
            pv[e].subImage.imageArrayIndex = 0;
        }
        proj.space = g_localSpace; proj.viewCount = 2; proj.views = pv;
        layers[0] = (const XrCompositionLayerBaseHeader*)&proj;
        int vg = VignetteLayer(layers + 1);                    // vinheta de conforto (acima do mundo, abaixo da interface)
        int extra = XrUiLayers(layers + 1 + vg, d.Format);     // painel da interface + laser + menus + configurações
        fei.layerCount = 1 + vg + extra; fei.layers = layers;
    } else if (fs.shouldRender && !stereoRecent && bbOk) {
        // Fora do estéreo (menus, construção...): tela virtual da V2.
        EnsureQuadSwapchain(d.Width, d.Height, d.Format);
        if (CopyToSwapchain(g_quadSwap, g_quadImgs, bb)) g_quadHasImage = true;
        if (g_quadHasImage && g_quadPlaced && g_quadSwap != XR_NULL_HANDLE) {
            quad.space = g_localSpace;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_quadSwap;
            quad.subImage.imageRect.offset = { 0, 0 };
            quad.subImage.imageRect.extent = { (int32_t)g_quadW, (int32_t)g_quadH };
            quad.subImage.imageArrayIndex = 0;
            quad.pose = g_quadPose;
            quad.size.width = g_screenWidth;
            quad.size.height = g_screenWidth * (float)g_quadH / (float)g_quadW;
            layers[0] = (const XrCompositionLayerBaseHeader*)&quad;
            fei.layerCount = 1; fei.layers = layers;
        }
    }
    XrResult er = xrEndFrame_(g_xrSession, &fei);
    if (XR_FAILED(er)) { if (fails++ < 5) Log("vr: xrEndFrame falhou: %s", XrStr(er)); }
    else InterlockedIncrement(&g_xrFramesDone);
    if (bb) bb->Release();
    LeaveCriticalSection(&g_xrLock);
    return false;
}

// ---------------------------------------------------------------- projeção do olho na câmera do jogo
// EngineCamera_BuildProjection (0x141163670) monta uma matriz PerspectiveOffCenterRH (vetor-linha,
// profundidade 0..1) a partir de multiplicadores em cam+0x270 (esq), +0x274 (cima), +0x278 (dir),
// +0x27c (baixo), com base = deslocamento (+0x1f0/+0x1f4) - 0,5 x tangente total (+0x14/+0x18).
// Trocamos os multiplicadores pelas tangentes do olho, chamamos o original e restauramos.
static const char* SIG_BUILD_PROJ = "48 8B C4 48 81 EC A8 00 00 00 83 79 10 00 4C 8B C1";
using BuildProj_t = void (__fastcall*)(uint8_t* cam);
static BuildProj_t o_BuildProj = nullptr;
static void* volatile g_projCam = nullptr;              // câmera da FPC (definida no update)

static void __fastcall hk_BuildProj(uint8_t* cam) {
    if (g_aerProjActive && cam && cam == g_projCam && *(int*)(cam + 0x10) == 0) {
        float tx = *(float*)(cam + 0x14), ty = *(float*)(cam + 0x18);
        float ox = *(float*)(cam + 0x1f0), oy = *(float*)(cam + 0x1f4);
        if (tx > 1e-4f && ty > 1e-4f) {
            float save[4]; memcpy(save, cam + 0x270, 16);
            float* m = (float*)(cam + 0x270);
            m[0] = (g_projTan[0] - ox) / tx + 0.5f;            // esquerda
            m[1] = (g_projTan[2] - oy) / ty + 0.5f;            // cima
            m[2] = (g_projTan[1] - ox) / tx + 0.5f;            // direita
            m[3] = (g_projTan[3] - oy) / ty + 0.5f;            // baixo
            o_BuildProj(cam);
            memcpy(cam + 0x270, save, 16);
            static int logged = 0;
            if (logged++ < 2) {
                const float* P = (const float*)(cam + 0x70);
                Log("vr: projecao do olho aplicada: tan(%.3f %.3f %.3f %.3f) -> m00=%.3f m11=%.3f m20=%.3f m21=%.3f m22=%.4f m23=%.1f m32=%.4f",
                    g_projTan[0], g_projTan[1], g_projTan[2], g_projTan[3], P[0], P[5], P[8], P[9], P[10], P[11], P[14]);
            }
            return;
        }
    }
    o_BuildProj(cam);
}

// ---------------------------------------------------------------- fase V4b: estéreo síncrono
// R_RenderView (0x1412d5cf0) atualiza efeitos com dt e chama FrameA (1 thread) ou FrameB (multithread)
// com (sistema de render, vista, dt). Para cada olho: câmera da cena (cena+0x60) na pose do olho,
// FOV/centro óptico do olho, desenhar. O 2º desenho recebe dt = 0 (nada avança duas vezes).
// Cada desenho termina num Present: o 1º (olho esquerdo) não aparece no monitor.
static const char* SIG_FRAME_A = "48 8B C4 55 57 41 56 48 8B EC 48 83 EC 60 48 C7 45 C8 FE FF FF FF 48 89 58 10 48 89 70 18 0F 29 70 D8";
static const char* SIG_FRAME_B = "48 8B C4 55 57 41 54 41 56 41 57 48 8D A8 48 FF FF FF 48 81 EC 90 01 00 00 48 C7 45 70 FE FF FF FF";
using Frame_t = void (__fastcall*)(uint8_t* rs, void* view, float dt);
static Frame_t o_FrameA = nullptr, o_FrameB = nullptr;
using SetPoseFn_t = void (__fastcall*)(void* cam, const float* pos, const float* up, const float* fwd);
static SetPoseFn_t g_setPoseFn = nullptr;               // o original (trampolim) do SetPose, vindo do fpc_rig
static volatile bool g_syncReady = false;               // a FPC calculou a câmera central neste frame
static float g_centerCam[4] = {};                       // posição da câmera central (mundo) deste frame
static float g_centerHead[3] = {};                      // posição da cabeça (XR) usada para ela
static float g_centerYaw = 0.0f, g_centerScale = 1.0f;
alignas(16) static float g_centerFwd[4] = { 0, 0, -1, 0 }, g_centerUp[4] = { 0, 1, 0, 0 };   // orientação da câmera central
alignas(16) static float g_gameCamW[3][4] = { { 0, 0, 0, 1 }, { 0, 0, -1, 0 }, { 0, 1, 0, 0 } };   // câmera do jogo (laser/travada): pos, frente, cima
alignas(16) static float g_restoreCam[3][4] = { { 0, 0, 0, 1 }, { 0, 0, -1, 0 }, { 0, 1, 0, 0 } }; // o que o SetPose recebeu (volta depois dos olhos)

static void EyeWorldPose(const EyeView& v, float pos[4], float up[4], float fwd[4]) {
    const float f0[3] = { 0.0f, 0.0f, -1.0f }, u0[3] = { 0.0f, 1.0f, 0.0f };
    float q[4] = { v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w };
    float f[3], u[3];
    QuatRotateXR(q, f0, f); QuatRotateXR(q, u0, u);
    RotYXR(g_centerYaw, f, fwd); fwd[3] = 0.0f;
    RotYXR(g_centerYaw, u, up);  up[3] = 0.0f;
    float d[3] = { (v.pose.position.x - g_centerHead[0]) * g_centerScale,
                   (v.pose.position.y - g_centerHead[1]) * g_centerScale,
                   (v.pose.position.z - g_centerHead[2]) * g_centerScale };
    float dw[3]; RotYXR(g_centerYaw, d, dw);
    pos[0] = g_centerCam[0] + dw[0]; pos[1] = g_centerCam[1] + dw[1]; pos[2] = g_centerCam[2] + dw[2]; pos[3] = g_centerCam[3];
}

static void StereoFrame(Frame_t orig, uint8_t* rs, void* view, float dt) {
    uint8_t* scene = rs ? *(uint8_t**)(rs + 0xa8) : nullptr;
    uint8_t* cam = scene ? *(uint8_t**)(scene + 0x60) : nullptr;
    EyeView ev[2]; bool vok;
    AcquireSRWLockShared(&g_hmdLock); vok = g_viewsValid; ev[0] = g_views[0]; ev[1] = g_views[1]; ReleaseSRWLockShared(&g_hmdLock);
    if (!g_vrStereoSync || g_uiPanel || !g_xrRunning || !g_syncReady || !vok || !cam || !g_setPoseFn || *(int*)(cam + 0x10) != 0) {
        g_uiRedirect = false;                                  // fora do estéreo: interface normal na imagem
        orig(rs, view, dt);
        return;
    }
    g_syncReady = false;
    static bool logged = false;
    if (!logged) { logged = true; Log("vr: estereo sincrono ativo (camera da cena %p, camera da FPC %p)", cam, g_fpsCam); }

    float save[5] = { *(float*)(cam + 0x14), *(float*)(cam + 0x18), *(float*)(cam + 0x20),
                      *(float*)(cam + 0x1f0), *(float*)(cam + 0x1f4) };
    for (int e = 0; e < 2; ++e) {
        if (g_symFov) {
            XrFovf& sf = ev[e].fov;
            float h = (std::max)(std::fabs(sf.angleLeft), std::fabs(sf.angleRight));
            float v = (std::max)(std::fabs(sf.angleUp), std::fabs(sf.angleDown));
            sf.angleLeft = -h; sf.angleRight = h; sf.angleUp = v; sf.angleDown = -v;
        }
        const XrFovf& f = ev[e].fov;
        float tl = std::tan(f.angleLeft), tr = std::tan(f.angleRight), tu = std::tan(f.angleUp), td = std::tan(f.angleDown);
        *(float*)(cam + 0x14)  = tr - tl;                     // largura do frustum (tangentes)
        *(float*)(cam + 0x18)  = tu - td;
        *(float*)(cam + 0x20)  = (tr - tl) / (tu - td);       // aspecto
        *(float*)(cam + 0x1f0) = (tr + tl) * 0.5f;            // centro óptico (projeção descentralizada)
        *(float*)(cam + 0x1f4) = (tu + td) * 0.5f;
        alignas(16) float pos[4], up[4], fwd[4];
        EyeView evs = ev[e];                                  // IPD: só a imagem do jogo (a camada usa a pose real)
        if (g_ipdScale != 1.0f) {
            float mx = (ev[0].pose.position.x + ev[1].pose.position.x) * 0.5f, my = (ev[0].pose.position.y + ev[1].pose.position.y) * 0.5f,
                  mz = (ev[0].pose.position.z + ev[1].pose.position.z) * 0.5f;
            evs.pose.position.x = mx + (evs.pose.position.x - mx) * g_ipdScale;
            evs.pose.position.y = my + (evs.pose.position.y - my) * g_ipdScale;
            evs.pose.position.z = mz + (evs.pose.position.z - mz) * g_ipdScale;
        }
        EyeWorldPose(evs, pos, up, fwd);
        pos[1] += g_heightOffset;
        g_setPoseFn(cam, pos, up, fwd);                       // pose + Commit (recalcula projeção e frustum)
        AerPush(e, ev[e], true);
        g_uiRedirect = true; InterlockedExchange(&g_uiClearPending, 1);
        orig(rs, view, e == 0 ? dt : 0.0f);
    }
    *(float*)(cam + 0x14) = save[0]; *(float*)(cam + 0x18) = save[1]; *(float*)(cam + 0x20) = save[2];
    *(float*)(cam + 0x1f0) = save[3]; *(float*)(cam + 0x1f4) = save[4];
    g_setPoseFn(cam, g_restoreCam[0], g_restoreCam[2], g_restoreCam[1]);   // câmera do jogo de volta (escolha/interface)
    static int n = 0;
    if (n++ < 2) {
        const float* P = (const float*)(cam + 0x70);
        Log("vr: [sincrono] projecao do olho direito: m00=%.3f m11=%.3f m20=%.3f m21=%.3f", P[0], P[5], P[8], P[9]);
    }
}
static void __fastcall hk_FrameA(uint8_t* rs, void* view, float dt) { StereoFrame(o_FrameA, rs, view, dt); }
static void __fastcall hk_FrameB(uint8_t* rs, void* view, float dt) { StereoFrame(o_FrameB, rs, view, dt); }

// Chamado no Init (depois do MH_Initialize).
static void XrInstallIfEnabled() {
    if (!g_vrEnabled) { Log("vr: desligado (vr=0 no ini)"); return; }
    InitializeCriticalSection(&g_xrLock);
    void* present = FindPresentAddress();
    if (!present) { Log("vr: nao consegui achar IDXGISwapChain::Present"); return; }
    if (g_vrStereo) {
        if (auto fp = FindPattern(SIG_BUILD_PROJ)) MH_CreateHook(fp, &hk_BuildProj, (void**)&o_BuildProj);
        else Log("vr: BuildProjection nao encontrada (sem estereo)");
    }
    if (g_vrStereoSync) {
        uint8_t* fa = FindPattern(SIG_FRAME_A); uint8_t* fb = FindPattern(SIG_FRAME_B);
        if (fa) MH_CreateHook(fa, &hk_FrameA, (void**)&o_FrameA);
        if (fb) MH_CreateHook(fb, &hk_FrameB, (void**)&o_FrameB);
        Log("vr: estereo sincrono: FrameA=%p FrameB=%p", fa, fb);
    }
    if (g_vrStereoSync) XrUiInstallHooks();
    MH_STATUS s1 = MH_CreateHook(present, &hk_Present, (void**)&o_Present);
    MH_STATUS s2 = g_present1Addr ? MH_CreateHook(g_present1Addr, &hk_Present1, (void**)&o_Present1) : MH_ERROR_NOT_EXECUTABLE;
    Log("vr: ligado; hooks Present=%p (status %d) Present1=%p (status %d); aguardando o jogo apresentar um frame",
        present, (int)s1, g_present1Addr, (int)s2);
}

// ================================================================ fase V1: head tracking
// A câmera usa a orientação do headset (com roll). O mouse na horizontal gira o "corpo" (yaw do
// espaço de jogo). A posição da cabeça (relativa ao recentrar) soma-se à câmera, girada pelo yaw.
// Eixos: OpenXR e o jogo são destros com Y para cima (o strafe A/D confirmou), então a conversão
// é só uma rotação em Y: mundo = RotY(g_vrYaw) * vetorXR.
static int   g_recenterKey    = VK_HOME;
static int   g_panelKey       = VK_OEM_PERIOD;  // tecla_painel_vr: liga/desliga o modo painel
static bool  g_vrHead         = false;      // head tracking ativo neste frame
static bool  g_vrNeedRecenter = true;
static float g_vrYaw          = 0.0f;
static float g_hmdRefPos[3]   = {};
static float g_vrFwd[4]       = {}, g_vrUp[4] = {}, g_vrOffset[3] = {};
static float g_lastMouseYaw   = 0.0f;
static bool  g_haveMouseYaw   = false;
static float g_vrMaxOffset    = 1.0f;       // m: limite de segurança do deslocamento da cabeça
static float g_worldScale     = 1.0f;       // vr_escala_mundo: >1 = mundo parece maior (olhos e cabeça encolhem)
static bool  g_turnSnap       = false;      // vr_giro_passos: 1 = gira em passos (conforto)
static float g_turnSpeed      = 120.0f;     // vr_giro_velocidade (graus/s, giro suave)
static float g_turnSnapDeg    = 30.0f;      // vr_giro_passo (graus)

static void QuatRotate(const float q[4], const float v[3], float o[3]) {
    // o = v + 2 * cross(q.xyz, cross(q.xyz, v) + w * v)
    float cx = q[1] * v[2] - q[2] * v[1] + q[3] * v[0];
    float cy = q[2] * v[0] - q[0] * v[2] + q[3] * v[1];
    float cz = q[0] * v[1] - q[1] * v[0] + q[3] * v[2];
    o[0] = v[0] + 2.0f * (q[1] * cz - q[2] * cy);
    o[1] = v[1] + 2.0f * (q[2] * cx - q[0] * cz);
    o[2] = v[2] + 2.0f * (q[0] * cy - q[1] * cx);
}
static float YawOf(float x, float z) { return std::atan2(-x, -z); }   // 0 = olhando para -Z
static void RotY(float th, const float v[3], float o[3]) {
    float c = std::cos(th), s = std::sin(th);
    float x = v[0] * c + v[2] * s, z = -v[0] * s + v[2] * c;
    o[0] = x; o[1] = v[1]; o[2] = z;
}
static float WrapPi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Chamado no update da FPC, antes do frame do jogo.
static void UpdateVrHead() {
    static bool askedStacks = false;
    if (!askedStacks && g_phase == RUNNING && g_vrEnabled) { askedStacks = true; g_stackWanted = 16; g_presentStacks = 2; Log("vr: registrando pilhas na primeira pessoa"); }
    g_vrHead = false;
    g_aerProjActive = false;
    if (!g_vrEnabled || !g_vrHeadTracking || !g_xrRunning || g_phase != RUNNING) { g_haveMouseYaw = false; return; }
    HmdPose p;
    AcquireSRWLockShared(&g_hmdLock); p = g_hmdPose; ReleaseSRWLockShared(&g_hmdLock);
    if (!p.valid) return;

    const float fwdXR[3] = { 0.0f, 0.0f, -1.0f }, upXR[3] = { 0.0f, 1.0f, 0.0f };
    float hf[3], hu[3];
    QuatRotate(p.quat, fwdXR, hf);
    QuatRotate(p.quat, upXR, hu);

    // Mouse na horizontal = girar o corpo (o pitch do mouse é ignorado; quem olha é a cabeça).
    float m[4];
    bool haveMouse = MouseLookDir(m);
    {   // analógico direito: girar o corpo (suave, ou em passos com vr_giro_passos=1)
        CtrlState rc; AcquireSRWLockShared(&g_hmdLock); rc = g_ctrl[1]; ReleaseSRWLockShared(&g_hmdLock);
        static bool snapHeld = false;
        float x = rc.sx;
        static bool turning = false;
        if ((std::fabs(x) > 0.2f) != turning) { turning = std::fabs(x) > 0.2f; Log("analogico direito: %s (%.2f)", turning ? "girando" : "parado", x); }
        if (!g_turnSnap) g_motionTurn = 0.0f;               // (em passos, a vinheta apaga sozinha)
        if (g_turnSnap) {
            if (std::fabs(x) > 0.7f && !snapHeld) { g_motionTurn = 1.0f; g_vrYaw = WrapPi(g_vrYaw - (x > 0 ? 1.0f : -1.0f) * g_turnSnapDeg * 0.0174533f); snapHeld = true; }
            if (std::fabs(x) < 0.3f) snapHeld = false;
        } else if (std::fabs(x) > 0.2f) {
            float k = (std::fabs(x) - 0.2f) / 0.8f * (x > 0 ? 1.0f : -1.0f);
            g_motionTurn = std::fabs(k);
            g_vrYaw = WrapPi(g_vrYaw - k * g_turnSpeed * 0.0174533f * g_dt);   // direita = yaw negativo
        }
    }
    if (g_laserOn) g_haveMouseYaw = false;                    // laser: o cursor se move, o corpo não gira
    if (haveMouse && !g_laserOn) {
        float my = YawOf(m[0], m[2]);
        if (g_haveMouseYaw) g_vrYaw = WrapPi(g_vrYaw + WrapPi(my - g_lastMouseYaw));
        g_lastMouseYaw = my; g_haveMouseYaw = true;
    }

    {   // modo painel: tecla ou botão de menu do controle esquerdo
        // toque curto no botão de menu = tela virtual; segurar = configurações (vr_config.inl)
        static bool menuDown = false; static ULONGLONG menuT = 0;
        bool menu; AcquireSRWLockShared(&g_hmdLock); menu = g_ctrl[0].menu; ReleaseSRWLockShared(&g_hmdLock);
        if (menu && !menuDown) menuT = GetTickCount64();
        bool menuTap = !menu && menuDown && GetTickCount64() - menuT < 600;
        menuDown = menu;
        if ((Pressed(g_panelKey) && GameHasFocus()) || menuTap) {
            g_uiPanel = !g_uiPanel;
            g_quadPlaced = false;                                  // a tela aparece na frente de onde você olha
            Log("vr: modo painel = %d (%s)", g_uiPanel, g_uiPanel ? "tela virtual com interface e cursor" : "estereo");
        }
    }
    bool recenterKey = Pressed(g_recenterKey) || g_cfgRecenter;
    g_cfgRecenter = false;
    if (recenterKey) Log("vr: tecla de recentrar vista (foco no jogo=%d)", (int)GameHasFocus());
    bool recenterBtn = false;
    {
        static bool rDown = false;
        bool r; AcquireSRWLockShared(&g_hmdLock); r = g_ctrl[1].stickClick; ReleaseSRWLockShared(&g_hmdLock);
        recenterBtn = r && !rDown; rDown = r;
    }
    if (g_vrNeedRecenter || recenterKey || recenterBtn) {
        // 1ª vez (entrar na primeira pessoa): a frente vem do mouse. Manual: mantém a direção atual da
        // câmera (sem pulo) e zera o deslocamento da cabeça; a tela virtual volta para a sua frente.
        float gameYaw = (g_vrNeedRecenter && haveMouse) ? YawOf(m[0], m[2]) : WrapPi(g_vrYaw + YawOf(hf[0], hf[2]));
        g_vrYaw = WrapPi(gameYaw - YawOf(hf[0], hf[2]));
        g_quadPlaced = false;
        g_hmdRefPos[0] = p.pos[0]; g_hmdRefPos[1] = p.pos[1]; g_hmdRefPos[2] = p.pos[2];
        g_vrNeedRecenter = false;
        Log("vr: recentrado (yaw %.1f graus)%s", g_vrYaw * 57.29578f, recenterBtn ? " pelo analogico direito" : recenterKey ? " pela tecla" : "");
    }

    // Estéreo (AER): a câmera usa a pose do olho da vez, e a projeção daquele olho.
    float camQuat[4] = { p.quat[0], p.quat[1], p.quat[2], p.quat[3] };
    float camPos[3]  = { p.pos[0], p.pos[1], p.pos[2] };
    g_aerProjActive = false;
    if (g_vrStereo && o_BuildProj) {
        EyeView ev[2]; bool vok;
        AcquireSRWLockShared(&g_hmdLock); vok = g_viewsValid; ev[0] = g_views[0]; ev[1] = g_views[1]; ReleaseSRWLockShared(&g_hmdLock);
        if (vok) {
            static int eye = 1;
            eye ^= 1;
            const EyeView& v = ev[eye];
            camQuat[0] = v.pose.orientation.x; camQuat[1] = v.pose.orientation.y;
            camQuat[2] = v.pose.orientation.z; camQuat[3] = v.pose.orientation.w;
            camPos[0] = v.pose.position.x; camPos[1] = v.pose.position.y; camPos[2] = v.pose.position.z;
            g_projTan[0] = std::tan(v.fov.angleLeft);  g_projTan[1] = std::tan(v.fov.angleRight);
            g_projTan[2] = std::tan(v.fov.angleUp);    g_projTan[3] = std::tan(v.fov.angleDown);
            g_aerProjActive = true;
            AerPush(eye, v);
        }
    }
    float cf[3], cu[3];
    QuatRotate(camQuat, fwdXR, cf);
    QuatRotate(camQuat, upXR, cu);
    RotY(g_vrYaw, cf, g_vrFwd); g_vrFwd[3] = 0.0f;
    RotY(g_vrYaw, cu, g_vrUp);  g_vrUp[3] = 0.0f;
    float ws = g_worldScale > 0.05f ? 1.0f / g_worldScale : 1.0f;
    g_centerHead[0] = p.pos[0]; g_centerHead[1] = p.pos[1]; g_centerHead[2] = p.pos[2];
    g_centerYaw = g_vrYaw; g_centerScale = ws;
    float d[3] = { (camPos[0] - g_hmdRefPos[0]) * ws, (camPos[1] - g_hmdRefPos[1]) * ws, (camPos[2] - g_hmdRefPos[2]) * ws };
    float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl > g_vrMaxOffset) { d[0] *= g_vrMaxOffset / dl; d[1] *= g_vrMaxOffset / dl; d[2] *= g_vrMaxOffset / dl; }
    RotY(g_vrYaw, d, g_vrOffset);
    g_vrHead = true;
}

// Direção do olhar para o resto do mod (andar, virar o corpo): headset se ativo, senão o mouse.
static bool GetViewDir(float out[4]) {
    if (g_vrHead) { memcpy(out, g_vrFwd, 16); return true; }
    return MouseLookDir(out);
}
