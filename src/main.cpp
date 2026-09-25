#include "WindowsTarget.h"

#include <windows.h>

#include <cwchar>
#include <cwctype>

#include <atlbase.h>
#include <atlapp.h>
#include <shellapi.h>

#include "InstallerSupport.h"
#include "MainFrame.h"

CAppModule _Module;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    if (commandLine != nullptr) {
        const auto hasHelperArgument = [commandLine](const wchar_t* option) {
            const std::size_t optionLength = std::wcslen(option);
            return std::wcsncmp(commandLine, option, optionLength) == 0 &&
                   (commandLine[optionLength] == L'\0' ||
                    std::iswspace(commandLine[optionLength]));
        };
        const bool prepareInstaller =
                hasHelperArgument(InstallerSupport::kPrepareInstallerCommandLineArgument);
        const bool prepareUninstaller =
                hasHelperArgument(InstallerSupport::kPrepareUninstallerCommandLineArgument);
        if (prepareInstaller || prepareUninstaller) {
            const wchar_t* expectedArgument =
                    prepareInstaller ? InstallerSupport::kPrepareInstallerCommandLineArgument
                                     : InstallerSupport::kPrepareUninstallerCommandLineArgument;
            int argumentCount = 0;
            LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
            if (arguments == nullptr || argumentCount < 3 || arguments[2][0] == L'\0' ||
                std::wcscmp(arguments[1], expectedArgument) != 0) {
                MessageBoxW(nullptr, L"安装器检查参数无效，本次操作已取消。", L"Hotkey Blocker 安装器",
                            MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
                if (arguments != nullptr) {
                    LocalFree(arguments);
                }
                return 1;
            }

            const wchar_t* installDirectory = argumentCount >= 3 ? arguments[2] : nullptr;
            const InstallerSupport::ChangeOperation operation =
                    prepareUninstaller ? InstallerSupport::ChangeOperation::Uninstall
                                       : InstallerSupport::ChangeOperation::Install;
            const int exitCode = InstallerSupport::PrepareForChange(installDirectory, operation);
            LocalFree(arguments);
            return exitCode;
        }
    }

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        return 1;
    }

    HRESULT result = _Module.Init(nullptr, instance);
    if (FAILED(result)) {
        CoUninitialize();
        return static_cast<int>(result);
    }

    HANDLE rawInstanceMutex =
        CreateMutexW(nullptr, TRUE, InstallerSupport::kSingleInstanceMutexName);
    if (rawInstanceMutex == nullptr) {
        _Module.Term();
        CoUninitialize();
        return 1;
    }
    const DWORD mutexStatus = GetLastError();
    CHandle instanceMutex(rawInstanceMutex);
    if (mutexStatus == ERROR_ALREADY_EXISTS) {
        _Module.Term();
        CoUninitialize();
        return 0;
    }

    AtlInitCommonControls(ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES);
    CMessageLoop messageLoop;
    _Module.AddMessageLoop(&messageLoop);

    const bool startHidden = commandLine != nullptr &&
                             wcsstr(commandLine, L"--background") != nullptr;
    const int exitCode = RunMainFrame(messageLoop, startHidden);

    _Module.RemoveMessageLoop();
    _Module.Term();
    CoUninitialize();
    return exitCode;
}
