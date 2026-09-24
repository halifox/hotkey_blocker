#pragma once

#include <windows.h>

namespace InstallerSupport {

inline constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\HotkeyBlocker.SingleInstance";
inline constexpr wchar_t kPrepareCommandLineArgument[] = L"--prepare-installer-change";

int PrepareForInstallerChange(const wchar_t* installDirectory);

}  // namespace InstallerSupport
