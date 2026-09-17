#pragma once

#include <windows.h>

#include <string>

struct RemoteInjectionResult {
    bool success = false;
    std::wstring error;
};

RemoteInjectionResult InjectDllIntoProcess(DWORD pid, const std::wstring& dllPath);
