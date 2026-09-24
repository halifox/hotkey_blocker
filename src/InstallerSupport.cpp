#include "InstallerSupport.h"

#include "Win32Support.h"
#include "resource.h"

#include <commctrl.h>
#include <restartmanager.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace InstallerSupport {
namespace {

constexpr wchar_t kMainWindowClassName[] = L"HotkeyBlocker.MainFrame";
constexpr DWORD kApplicationShutdownTimeoutMs = 30000;
constexpr DWORD kWindowDiscoveryTimeoutMs = 10000;
constexpr DWORD kWindowDiscoveryIntervalMs = 100;
constexpr DWORD kMutexReleaseTimeoutMs = 5000;

void ShowMessage(const std::wstring& message, UINT flags = MB_OK | MB_ICONWARNING) {
    MessageBoxW(nullptr, message.c_str(), L"Hotkey Blocker 安装器",
                flags | MB_SETFOREGROUND | MB_TOPMOST);
}

bool ConfirmForceCloseProcesses(const std::wstring& processList) {
    constexpr int kForceCloseButtonId = 1001;
    constexpr int kCancelButtonId = 1002;
    const TASKDIALOG_BUTTON buttons[] = {
            {kForceCloseButtonId, L"强制关闭并继续"},
            {kCancelButtonId, L"取消"},
    };

    std::wstring content = processList;
    content += L"\r\n\r\n强制关闭可能导致未保存的数据丢失。Windows 会先请求程序退出，"
               L"未能及时退出的进程将在等待超时后被强制关闭。";

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"Hotkey Blocker 安装器";
    config.pszMainInstruction = L"这些进程正在占用 Hook DLL";
    config.pszContent = content.c_str();
    config.pszMainIcon = TD_WARNING_ICON;
    config.cButtons = static_cast<UINT>(sizeof(buttons) / sizeof(buttons[0]));
    config.pButtons = buttons;
    config.nDefaultButton = kCancelButtonId;

    int selectedButtonId = kCancelButtonId;
    const HRESULT result = TaskDialogIndirect(&config, &selectedButtonId, nullptr, nullptr);
    if (SUCCEEDED(result)) {
        return selectedButtonId == kForceCloseButtonId;
    }

    content += L"\r\n\r\n点击“是”强制关闭占用进程并继续安装或卸载。";
    return MessageBoxW(nullptr, content.c_str(), L"Hotkey Blocker 安装器",
                       MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST) == IDYES;
}

bool RequestRunningApplicationToExit(HANDLE& mutex) {
    mutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (mutex == nullptr) {
        ShowMessage(L"无法检查 Hotkey Blocker 是否正在运行。请从系统托盘退出程序后重试。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"创建 Hotkey Blocker 单实例锁失败"));
        return false;
    }

    DWORD waitResult = WaitForSingleObject(mutex, 0);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        // Keep ownership until the DLL check completes so the application cannot restart mid-update.
        return true;
    }
    if (waitResult != WAIT_TIMEOUT) {
        CloseHandle(mutex);
        mutex = nullptr;
        ShowMessage(L"无法检查 Hotkey Blocker 是否正在运行。请从系统托盘退出程序后重试。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"等待 Hotkey Blocker 单实例锁失败"));
        return false;
    }

    HWND window = nullptr;
    const DWORD discoveryAttempts = kWindowDiscoveryTimeoutMs / kWindowDiscoveryIntervalMs;
    for (DWORD attempt = 0; attempt < discoveryAttempts; ++attempt) {
        window = FindWindowW(kMainWindowClassName, nullptr);
        if (window != nullptr) {
            break;
        }

        waitResult = WaitForSingleObject(mutex, kWindowDiscoveryIntervalMs);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
            // The application exited while starting; this helper now owns the lock.
            return true;
        }
        if (waitResult != WAIT_TIMEOUT) {
            CloseHandle(mutex);
            mutex = nullptr;
            ShowMessage(L"无法检查 Hotkey Blocker 是否正在运行。请从系统托盘退出程序后重试。\r\n\r\n" +
                        Win32Support::ErrorMessage(L"等待 Hotkey Blocker 单实例锁失败"));
            return false;
        }
    }

    if (window == nullptr) {
        CloseHandle(mutex);
        mutex = nullptr;
        ShowMessage(L"Hotkey Blocker 正在启动，但无法找到主窗口。请等待程序启动完成后重试。");
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    HANDLE process = processId == 0 ? nullptr : OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        waitResult = WaitForSingleObject(mutex, 0);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
            return true;
        }
        CloseHandle(mutex);
        mutex = nullptr;
        ShowMessage(L"无法连接到正在运行的 Hotkey Blocker。请从系统托盘退出程序后重试。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"打开 Hotkey Blocker 进程失败"));
        return false;
    }

    const BOOL posted = PostMessageW(window, WM_COMMAND, MAKEWPARAM(ID_TRAY_EXIT, 0), 0);
    waitResult = WaitForSingleObject(process, kApplicationShutdownTimeoutMs);
    CloseHandle(process);

    if (waitResult == WAIT_OBJECT_0) {
        waitResult = WaitForSingleObject(mutex, kMutexReleaseTimeoutMs);
        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
            // Keep ownership until the DLL check completes so the application cannot restart mid-update.
            return true;
        }
    }

    const std::wstring reason = posted
                                    ? L"Hotkey Blocker 未能在规定时间内正常退出。请从系统托盘退出程序后重试。"
                                    : L"无法向 Hotkey Blocker 发送正常退出请求。请从系统托盘退出程序后重试。";
    CloseHandle(mutex);
    mutex = nullptr;
    ShowMessage(reason);
    return false;
}

std::vector<std::filesystem::path> GetInstalledHookDlls(const wchar_t* installDirectory) {
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path directory(installDirectory);

#ifdef _WIN64
    paths.push_back(directory / L"HotkeyHook64.dll");
    paths.push_back(directory / L"win32" / L"HotkeyHook32.dll");
#else
    paths.push_back(directory / L"HotkeyHook32.dll");
    paths.push_back(directory / L"win32" / L"HotkeyHook32.dll");
#endif

    paths.erase(std::remove_if(paths.begin(), paths.end(), [](const auto& path) {
                    const DWORD attributes = GetFileAttributesW(path.c_str());
                    return attributes == INVALID_FILE_ATTRIBUTES ||
                           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                }),
                paths.end());
    return paths;
}

struct RestartManagerSession final {
    DWORD handle = 0;
    bool started = false;

    ~RestartManagerSession() {
        if (started) {
            RmEndSession(handle);
        }
    }
};

bool FindProcessesUsingHookDlls(const wchar_t* installDirectory) {
    if (installDirectory == nullptr || installDirectory[0] == L'\0') {
        ShowMessage(L"未提供有效的安装目录，本次操作已取消。");
        return false;
    }

    const std::vector<std::filesystem::path> hookPaths = GetInstalledHookDlls(installDirectory);
    if (hookPaths.empty()) {
        return true;
    }

    std::array<wchar_t, CCH_RM_SESSION_KEY + 1> sessionKey{};
    swprintf_s(sessionKey.data(), sessionKey.size(), L"HKB-%lu-%lu", GetCurrentProcessId(),
               GetTickCount());

    RestartManagerSession session;
    DWORD status = RmStartSession(&session.handle, 0, sessionKey.data());
    if (status != ERROR_SUCCESS) {
        ShowMessage(L"无法检查 Hook DLL 的占用情况，本次操作已取消。请关闭目标程序后重试。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"启动 Windows Restart Manager 失败", status));
        return false;
    }
    session.started = true;

    std::vector<LPCWSTR> resourcePaths;
    resourcePaths.reserve(hookPaths.size());
    for (const std::filesystem::path& path : hookPaths) {
        resourcePaths.push_back(path.c_str());
    }

    status = RmRegisterResources(session.handle, static_cast<UINT>(resourcePaths.size()),
                                 resourcePaths.data(), 0, nullptr, 0, nullptr);
    if (status != ERROR_SUCCESS) {
        ShowMessage(L"无法登记 Hook DLL 以检查占用情况，本次操作已取消。请关闭目标程序后重试。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"登记 Hook DLL 失败", status));
        return false;
    }

    const auto getAffectedProcesses = [&](std::vector<RM_PROCESS_INFO>& affectedProcesses,
                                          DWORD& rebootReasons) {
        constexpr int kMaximumListRetries = 3;
        for (int attempt = 0; attempt < kMaximumListRetries; ++attempt) {
            UINT processesNeeded = 0;
            UINT processCount = 0;
            rebootReasons = RmRebootReasonNone;
            DWORD listStatus = RmGetList(session.handle, &processesNeeded, &processCount,
                                         nullptr, &rebootReasons);
            if (listStatus != ERROR_SUCCESS && listStatus != ERROR_MORE_DATA) {
                ShowMessage(L"无法读取 Hook DLL 的占用进程，本次操作已取消。\r\n\r\n" +
                            Win32Support::ErrorMessage(L"读取 DLL 占用进程失败", listStatus));
                return false;
            }
            if (processesNeeded == 0) {
                affectedProcesses.clear();
                return true;
            }

            affectedProcesses.resize(processesNeeded);
            processCount = processesNeeded;
            listStatus = RmGetList(session.handle, &processesNeeded, &processCount,
                                   affectedProcesses.data(), &rebootReasons);
            if (listStatus == ERROR_MORE_DATA) {
                continue;
            }
            if (listStatus != ERROR_SUCCESS) {
                ShowMessage(L"无法完整读取 Hook DLL 的占用进程，本次操作已取消。\r\n\r\n" +
                            Win32Support::ErrorMessage(L"读取 DLL 占用进程失败", listStatus));
                return false;
            }

            affectedProcesses.resize(processCount);
            return true;
        }

        ShowMessage(L"Hook DLL 的占用进程列表持续变化，本次操作已取消。请稍后重试。");
        return false;
    };

    const auto describeAffectedProcesses = [](const std::vector<RM_PROCESS_INFO>& processes) {
        std::wstring message = L"以下程序仍在使用 Hotkey Blocker 的 Hook DLL：\r\n";
        for (const RM_PROCESS_INFO& process : processes) {
            message += L"\r\n• ";
            message += process.strAppName[0] == L'\0' ? L"未知程序" : process.strAppName;
            message += L"（PID ";
            message += std::to_wstring(process.Process.dwProcessId);
            message += L"）";
        }
        return message;
    };

    std::vector<RM_PROCESS_INFO> affectedProcesses;
    DWORD rebootReasons = RmRebootReasonNone;
    if (!getAffectedProcesses(affectedProcesses, rebootReasons)) {
        return false;
    }
    if (affectedProcesses.empty()) {
        if (rebootReasons == RmRebootReasonNone) {
            return true;
        }
        ShowMessage(
            L"Windows 指示需要重启才能释放 Hook DLL。请重启电脑后、重新打开目标程序前再次运行安装器或卸载程序。");
        return false;
    }

    if (!ConfirmForceCloseProcesses(describeAffectedProcesses(affectedProcesses))) {
        return false;
    }

    const DWORD shutdownStatus = RmShutdown(session.handle, RmForceShutdown, nullptr);
    if (!getAffectedProcesses(affectedProcesses, rebootReasons)) {
        return false;
    }
    if (affectedProcesses.empty() && shutdownStatus != ERROR_FAIL_NOACTION_REBOOT &&
        rebootReasons == RmRebootReasonNone) {
        return true;
    }

    std::wstring remainingMessage;
    if (shutdownStatus == ERROR_FAIL_NOACTION_REBOOT || rebootReasons != RmRebootReasonNone) {
        remainingMessage =
            L"Windows 指示需要重启才能释放这些文件。请重启电脑后、重新打开目标程序前再次运行安装器或卸载程序。\r\n\r\n";
    } else if (shutdownStatus != ERROR_SUCCESS) {
        remainingMessage = L"Windows 未能正常关闭所有占用程序。请保存工作，手动退出剩余程序后重试。\r\n\r\n";
    } else {
        remainingMessage = L"部分程序仍在使用 Hook DLL。请保存工作，手动退出剩余程序后重试。\r\n\r\n";
    }
    if (!affectedProcesses.empty()) {
        remainingMessage += describeAffectedProcesses(affectedProcesses);
    } else {
        remainingMessage += L"Hook DLL 仍需要重启系统后才能释放。";
    }
    ShowMessage(remainingMessage);
    return false;
}

}  // namespace

int PrepareForInstallerChange(const wchar_t* installDirectory) {
    HANDLE mutex = nullptr;
    if (!RequestRunningApplicationToExit(mutex)) {
        return 1;
    }
    const int result = FindProcessesUsingHookDlls(installDirectory) ? 0 : 2;
    // This short-lived helper exits immediately after this function returns. Keep the mutex
    // handle open until process teardown so the app cannot restart in the final handoff window.
    return result;
}

}  // namespace InstallerSupport
