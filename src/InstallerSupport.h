#pragma once

#include <windows.h>

namespace InstallerSupport {

inline constexpr wchar_t kPrepareCommandLineArgument[] = L"--prepare-installer-change";
inline constexpr UINT kShutdownMessage = WM_APP + 4;

int PrepareForInstallerChange();

}  // namespace InstallerSupport
