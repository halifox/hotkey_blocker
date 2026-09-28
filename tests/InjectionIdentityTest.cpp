#include "RemoteInjector.h"
#include "Win32Support.h"

#include <filesystem>
#include <iostream>

int wmain() {
    ProcessIdentity identity;
    if (!QueryProcessIdentity(GetCurrentProcess(), identity)) return 1;
    const auto directory = std::filesystem::path(Win32Support::ModulePath()).parent_path();
    const auto hook = directory / (sizeof(void*) == 8 ? L"HotkeyHook64.dll" : L"HotkeyHook32.dll");
    // Use the real PID with the wrong creation time: rejection must precede
    // remote allocation/thread creation, even though OpenProcess succeeds.
    ++identity.creationTime;
    const auto mismatch = InjectDllIntoProcess(identity, hook.wstring());
    if (mismatch.status != InjectionStatus::Failed || mismatch.pendingOperation ||
        mismatch.error != L"目标进程已退出或进程标识已变化") {
        std::wcerr << mismatch.error << L'\n';
        return 2;
    }
    identity.creationTime = 0;
    const auto missing = InjectDllIntoProcess(identity, hook.wstring());
    return missing.status == InjectionStatus::Failed && !missing.pendingOperation &&
                   missing.error == L"目标进程标识无效" ? 0 : 3;
}
