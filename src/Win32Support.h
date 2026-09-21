#pragma once

#include <windows.h>

#include <filesystem>
#include <string>

namespace Win32Support {

std::wstring ErrorMessage(const wchar_t* operation, DWORD errorCode = GetLastError());
std::wstring ModulePath();
std::filesystem::path HotkeyBlockerDataDirectory();

}  // namespace Win32Support
