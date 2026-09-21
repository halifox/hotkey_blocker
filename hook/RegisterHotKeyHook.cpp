#include "RegisterHotKeyHook.h"

#include "HotkeyPolicyTransport.h"

#include <detours/detours.h>

#include <mutex>
#include <utility>

namespace {

RegisterHotKeyFunction g_realRegisterHotKey = nullptr;
bool g_hookInstalled = false;
HotkeyPolicy g_hotkeyPolicy;
std::once_flag g_hotkeyPolicyOnce;

}  // namespace

void LoadHotkeyPolicy() {
    HotkeyPolicy loaded;
    if (LoadHotkeyPolicyForCurrentProcess(loaded)) {
        g_hotkeyPolicy = std::move(loaded);
    } else {
        g_hotkeyPolicy.mode = HotkeyMode::BlockAll;
        g_hotkeyPolicy.hotkeys.clear();
    }
}

extern "C" BOOL WINAPI HookRegisterHotKey(HWND window, int identifier, UINT modifiers,
                                           UINT virtualKey) {
    std::call_once(g_hotkeyPolicyOnce, LoadHotkeyPolicy);
    if (ShouldBlockHotkey(g_hotkeyPolicy, {modifiers, virtualKey})) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return g_realRegisterHotKey(window, identifier, modifiers, virtualKey);
}

bool InstallRegisterHotKeyHook() {
    if (g_hookInstalled) {
        return true;
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        user32 = LoadLibraryW(L"user32.dll");
    }
    if (user32 == nullptr) {
        return false;
    }

    g_realRegisterHotKey = reinterpret_cast<RegisterHotKeyFunction>(
        GetProcAddress(user32, "RegisterHotKey"));
    if (g_realRegisterHotKey == nullptr) {
        return false;
    }

    LONG status = DetourTransactionBegin();
    if (status != NO_ERROR) {
        return false;
    }
    status = DetourUpdateThread(GetCurrentThread());
    if (status == NO_ERROR) {
        status = DetourAttach(reinterpret_cast<PVOID*>(&g_realRegisterHotKey),
                              reinterpret_cast<PVOID>(HookRegisterHotKey));
    }
    if (status == NO_ERROR) {
        status = DetourTransactionCommit();
    } else {
        DetourTransactionAbort();
    }

    if (status != NO_ERROR) {
        g_realRegisterHotKey = nullptr;
        return false;
    }
    g_hookInstalled = true;
    return true;
}

void RemoveRegisterHotKeyHook() {
    if (!g_hookInstalled || g_realRegisterHotKey == nullptr) {
        return;
    }

    LONG status = DetourTransactionBegin();
    if (status == NO_ERROR) {
        status = DetourUpdateThread(GetCurrentThread());
    }
    if (status == NO_ERROR) {
        status = DetourDetach(reinterpret_cast<PVOID*>(&g_realRegisterHotKey),
                              reinterpret_cast<PVOID>(HookRegisterHotKey));
    }
    if (status == NO_ERROR) {
        status = DetourTransactionCommit();
    } else {
        DetourTransactionAbort();
    }

    if (status == NO_ERROR) {
        g_hookInstalled = false;
        g_realRegisterHotKey = nullptr;
    }
}
