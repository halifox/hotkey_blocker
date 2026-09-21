#include "RegisterHotKeyHook.h"
#include "HotkeyPolicyTransport.h"

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            if (!RetainHotkeyPolicyMappingForCurrentProcess()) {
                return FALSE;
            }
            if (!InstallRegisterHotKeyHook()) {
                // Returning FALSE makes LoadLibraryW report a failed load to
                // the injector instead of silently running without a hook.
                ReleaseHotkeyPolicyMappingForCurrentProcess();
                return FALSE;
            }
            break;
        case DLL_PROCESS_DETACH:
            if (reserved == nullptr) {
                RemoveRegisterHotKeyHook();
            }
            ReleaseHotkeyPolicyMappingForCurrentProcess();
            break;
        default:
            break;
    }
    return TRUE;
}
