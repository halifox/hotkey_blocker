#include "RemoteInjector.h"

#include <windows.h>

#include <cwchar>
#include <iostream>

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
        return 2;
    }

    DWORD pid = 0;
    if (!ParsePid(argv[1], pid)) {
        std::wcerr << L"无效 PID\n";
        return 2;
    }

    const RemoteInjectionResult result = InjectDllIntoProcess(pid, argv[2]);
    if (!result.success) {
        std::wcerr << (result.error.empty() ? L"注入失败" : result.error) << L'\n';
        return 1;
    }
    return 0;
}
