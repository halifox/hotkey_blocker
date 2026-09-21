#include "RemoteInjector.h"

#include <windows.h>

#include <cwchar>
#include <iostream>
#include <utility>

namespace {

bool ParsePid(const wchar_t* text, DWORD& pid) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long value = wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || value == 0 || value > MAXDWORD) {
        return false;
    }
    pid = static_cast<DWORD>(value);
    return true;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::wcerr << L"用法：HotkeyBlockerInjector32.exe <PID> <HookDllPath>\n";
        return static_cast<int>(InjectorHelperExitCode::InvalidArguments);
    }

    DWORD pid = 0;
    if (!ParsePid(argv[1], pid)) {
        std::wcerr << L"无效 PID\n";
        return static_cast<int>(InjectorHelperExitCode::InvalidArguments);
    }

    RemoteInjectionResult result = InjectDllIntoProcess(pid, argv[2], true);
    while (result.status == InjectionStatus::Pending && result.pendingOperation != nullptr) {
        InjectionCompletion completion;
        if (result.pendingOperation->TryComplete(completion)) {
            result.status = completion.status;
            result.error = std::move(completion.error);
            result.pendingOperation.reset();
        } else {
            Sleep(50);
        }
    }
    if (result.status != InjectionStatus::Succeeded) {
        std::wcerr << (result.error.empty() ? L"注入失败" : result.error) << L'\n';
        return static_cast<int>(InjectorHelperExitCode::Failed);
    }
    return static_cast<int>(InjectorHelperExitCode::Succeeded);
}
