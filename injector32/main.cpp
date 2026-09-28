#include "RemoteInjector.h"

#include <windows.h>

#include <cwchar>
#include <cerrno>
#include <iostream>
#include <utility>

namespace {

bool ParseNumber(const wchar_t* text, ULONGLONG& number) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    for (const wchar_t* digit = text; *digit; ++digit) {
        if (*digit < L'0' || *digit > L'9') return false;
    }
    errno = 0;
    const unsigned long long value = wcstoull(text, &end, 10);
    if (end == text || *end != L'\0' || value == 0 || errno == ERANGE) {
        return false;
    }
    number = value;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
        std::wcerr << L"用法：HotkeyBlockerInjector32.exe <PID> <CreationTime> <HookDllPath>\n";
        return static_cast<int>(InjectorHelperExitCode::InvalidArguments);
    }

    ULONGLONG pid = 0;
    ULONGLONG creationTime = 0;
    if (!ParseNumber(argv[1], pid) || pid > MAXDWORD ||
        !ParseNumber(argv[2], creationTime)) {
        std::wcerr << L"无效进程标识\n";
        return static_cast<int>(InjectorHelperExitCode::InvalidArguments);
    }

    RemoteInjectionResult result = InjectDllIntoProcess(
        {static_cast<DWORD>(pid), creationTime}, argv[3], true);
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
