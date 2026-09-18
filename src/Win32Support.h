#pragma once

#include <windows.h>

#include <string>

namespace Win32Support {

std::wstring ErrorMessage(const wchar_t* operation, DWORD errorCode = GetLastError());
std::wstring ModulePath();

}  // namespace Win32Support
