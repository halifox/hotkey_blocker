#include "RegisterHotKeyHook.h"
#include "HotkeyPolicyTransport.h"

#include <detours/detours.h>
#include <tlhelp32.h>
#include <mutex>
#include <vector>

namespace {
RegisterHotKeyFunction g_realRegisterHotKey = nullptr;
bool g_hookInstalled = false;
HotkeyPolicy g_hotkeyPolicy;
std::mutex g_installMutex;

struct ProcessThreads {
    std::vector<HANDLE> handles;
    ~ProcessThreads() {
        for (HANDLE handle : handles) CloseHandle(handle);
    }

    bool Collect() {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        THREADENTRY32 entry{sizeof(entry)};
        BOOL next = Thread32First(snapshot, &entry);
        bool success = next != FALSE;
        while (next) {
            if (entry.th32OwnerProcessID == GetCurrentProcessId() &&
                entry.th32ThreadID != GetCurrentThreadId()) {
                HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                               THREAD_SET_CONTEXT | SYNCHRONIZE,
                                           FALSE, entry.th32ThreadID);
                if (thread != nullptr) {
                    handles.push_back(thread);
                } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
                    success = false;
                    break;
                }
            }
            next = Thread32Next(snapshot, &entry);
        }
        if (success && GetLastError() != ERROR_NO_MORE_FILES) success = false;
        CloseHandle(snapshot);
        return success;
    }
};
}  // namespace

extern "C" BOOL WINAPI HookRegisterHotKey(HWND window, int identifier, UINT modifiers,
                                           UINT virtualKey) {
    if (ShouldBlockHotkey(g_hotkeyPolicy, {modifiers, virtualKey})) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return g_realRegisterHotKey(window, identifier, modifiers, virtualKey);
}

bool InstallRegisterHotKeyHook() {
    // Called by an explicit remote entry point, never under the loader lock.
    std::lock_guard lock(g_installMutex);
    if (g_hookInstalled) return true;
    if (!LoadHotkeyPolicyForCurrentProcess(g_hotkeyPolicy)) return false;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) return false;
    g_realRegisterHotKey = reinterpret_cast<RegisterHotKeyFunction>(
        GetProcAddress(user32, "RegisterHotKey"));
    if (g_realRegisterHotKey == nullptr) return false;

    // Keep code and policy alive until process exit; live unloading would race callers.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&InstallRegisterHotKeyHook), &pinned)) {
        return false;
    }
    ProcessThreads threads;
    if (!threads.Collect()) return false;
    LONG status = DetourTransactionBegin();
    if (status != NO_ERROR) return false;
    status = DetourAttach(reinterpret_cast<PVOID*>(&g_realRegisterHotKey),
                          reinterpret_cast<PVOID>(HookRegisterHotKey));
    for (HANDLE thread : threads.handles) {
        if (status != NO_ERROR) break;
        if (WaitForSingleObject(thread, 0) == WAIT_OBJECT_0) continue;
        status = DetourUpdateThread(thread);
    }
    // Commit/abort resumes every enlisted thread before its handle is closed.
    if (status == NO_ERROR) status = DetourTransactionCommit();
    else DetourTransactionAbort();
    if (status != NO_ERROR) return false;
    g_hookInstalled = true;
    return true;
}
