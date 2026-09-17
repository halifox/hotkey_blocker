#include "RegisterHotKeyHook.h"

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            if (!InstallRegisterHotKeyHook()) {
                // Returning FALSE makes LoadLibraryW report a failed load to
                // the injector instead of silently running without a hook.
                return FALSE;
            }
            break;
        case DLL_PROCESS_DETACH:
            RemoveRegisterHotKeyHook();
            break;
        default:
            break;
    }
    return TRUE;
}
