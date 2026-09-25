#pragma once

#include <windows.h>

namespace InstallerSupport {

inline constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\HotkeyBlocker.SingleInstance";
inline constexpr wchar_t kPrepareInstallerCommandLineArgument[] = L"--prepare-installer-change";
inline constexpr wchar_t kPrepareUninstallerCommandLineArgument[] = L"--prepare-uninstaller-change";

enum class ChangeOperation {
    Install,
    Uninstall,
};

int PrepareForChange(const wchar_t* installDirectory, ChangeOperation operation);

}  // namespace InstallerSupport
