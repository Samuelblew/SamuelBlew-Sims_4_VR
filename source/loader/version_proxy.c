// version_proxy.c: tiny loader for the TS4 VR mod.
//
// The Sims 4 (TS4_x64.exe) imports version.dll. Windows looks for it in the game folder first, so this
// proxy is loaded instead of the system one. It:
//   1. loads the real version.dll from the Windows system folder and forwards every export to it;
//   2. loads TS4VR.dll (the mod) from the same folder as this proxy.
// No Microsoft file is copied or redistributed: the real DLL is always the one installed with Windows.

#include <windows.h>
#include <stdint.h>

static HMODULE g_real = NULL;
static FARPROC g_fn[17];
static const char* const g_names[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW", "VerQueryValueA", "VerQueryValueW" };

static void LoadReal(void) {
    if (g_real) return;
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n > MAX_PATH - 16) return;
    lstrcatW(path, L"\\version.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) return;
    for (int i = 0; i < 17; ++i) g_fn[i] = GetProcAddress(g_real, g_names[i]);
}

// Generic forwarders. On x64 every argument of these functions is an integer or pointer, so taking 8
// pointer-sized arguments and passing them on is safe for all of them (the extra ones are ignored).
typedef uintptr_t (WINAPI *Fwd8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define FORWARD(idx, name) \
    uintptr_t WINAPI Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) { \
        LoadReal(); \
        if (!g_fn[idx]) { SetLastError(ERROR_PROC_NOT_FOUND); return 0; } \
        return ((Fwd8)g_fn[idx])(a, b, c, d, e, f, g, h); }

FORWARD(0, GetFileVersionInfoA)
FORWARD(1, GetFileVersionInfoByHandle)
FORWARD(2, GetFileVersionInfoExA)
FORWARD(3, GetFileVersionInfoExW)
FORWARD(4, GetFileVersionInfoSizeA)
FORWARD(5, GetFileVersionInfoSizeExA)
FORWARD(6, GetFileVersionInfoSizeExW)
FORWARD(7, GetFileVersionInfoSizeW)
FORWARD(8, GetFileVersionInfoW)
FORWARD(9, VerFindFileA)
FORWARD(10, VerFindFileW)
FORWARD(11, VerInstallFileA)
FORWARD(12, VerInstallFileW)
FORWARD(13, VerLanguageNameA)
FORWARD(14, VerLanguageNameW)
FORWARD(15, VerQueryValueA)
FORWARD(16, VerQueryValueW)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadReal();
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(inst, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            wchar_t* slash = path + n;
            while (slash > path && slash[-1] != L'\\' && slash[-1] != L'/') --slash;
            if ((size_t)(slash - path) + 10 < MAX_PATH) { lstrcpyW(slash, L"TS4VR.dll"); LoadLibraryW(path); }
        }
    }
    return TRUE;
}
