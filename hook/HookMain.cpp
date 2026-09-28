#include "RegisterHotKeyHook.h"
#include "HotkeyPolicyTransport.h"

// Stable export name for both x86 stdcall and x64 builds.
#ifdef _M_IX86
#pragma comment(linker, "/EXPORT:HkbInitializeHook=_HkbInitializeHook@4")
#else
#pragma comment(linker, "/EXPORT:HkbInitializeHook")
#endif

extern "C" DWORD WINAPI HkbInitializeHook(LPVOID) {
    try {
        return InstallRegisterHotKeyHook() ? ERROR_SUCCESS : ERROR_DLL_INIT_FAILED;
    } catch (...) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        ReleaseHotkeyPolicyMappingForCurrentProcess();
    }
    return TRUE;
}
