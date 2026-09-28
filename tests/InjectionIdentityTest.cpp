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
    if (missing.status != InjectionStatus::Failed || missing.pendingOperation ||
        missing.error != L"目标进程标识无效") return 3;

    // A suspended fresh process cannot finish DLL initialization. Starting its
    // injection must still return promptly and retain the outstanding operation.
    const auto probe = directory / L"hotkey_probe.exe";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(probe.c_str(), nullptr, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &child)) return 4;
    QueryProcessIdentity(child.hProcess, identity);
    const ULONGLONG started = GetTickCount64();
    auto pending = InjectDllIntoProcess(identity, hook.wstring());
    const bool prompt = GetTickCount64() - started < 2000 &&
                        pending.status == InjectionStatus::Pending && pending.pendingOperation;
    TerminateProcess(child.hProcess, 1);
    WaitForSingleObject(child.hProcess, 5000);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (!prompt) return 5;
    InjectionCompletion completion;
    return pending.pendingOperation->TryComplete(completion) &&
                   completion.status != InjectionStatus::Succeeded ? 0 : 6;
}
