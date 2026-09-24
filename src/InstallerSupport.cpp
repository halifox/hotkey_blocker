#include "InstallerSupport.h"

#include "Win32Support.h"

#include <restartmanager.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

namespace InstallerSupport {
namespace {

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\HotkeyBlocker.SingleInstance";
constexpr wchar_t kMainWindowClassName[] = L"HotkeyBlocker.MainFrame";
constexpr DWORD kApplicationShutdownTimeoutMs = 30000;
constexpr DWORD kWindowDiscoveryTimeoutMs = 10000;
constexpr DWORD kWindowDiscoveryIntervalMs = 100;
constexpr DWORD kMutexReleaseTimeoutMs = 5000;

void ShowMessage(const std::wstring& message, UINT flags = MB_OK | MB_ICONWARNING) {
    MessageBoxW(nullptr, message.c_str(), L"Hotkey Blocker 安装器",
                flags | MB_SETFOREGROUND | MB_TOPMOST);
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

    const BOOL posted = PostMessageW(window, kShutdownMessage, 0, 0);
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

std::vector<std::filesystem::path> GetInstalledHookDlls() {
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path executablePath(Win32Support::ModulePath());
    if (executablePath.empty()) {
        return paths;
    }

    const std::filesystem::path installDirectory = executablePath.parent_path();
#ifdef _WIN64
    paths.push_back(installDirectory / L"HotkeyHook64.dll");
    paths.push_back(installDirectory / L"win32" / L"HotkeyHook32.dll");
#else
    paths.push_back(installDirectory / L"HotkeyHook32.dll");
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

bool FindProcessesUsingHookDlls() {
    const std::vector<std::filesystem::path> hookPaths = GetInstalledHookDlls();
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

    UINT processesNeeded = 0;
    UINT processCount = 0;
    DWORD rebootReasons = 0;
    status = RmGetList(session.handle, &processesNeeded, &processCount, nullptr, &rebootReasons);
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) {
        ShowMessage(L"无法读取 Hook DLL 的占用进程，本次操作已取消。请关闭目标程序后重试。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"读取 DLL 占用进程失败", status));
        return false;
    }
    if (processesNeeded == 0) {
        return true;
    }

    std::vector<RM_PROCESS_INFO> affectedProcesses(processesNeeded);
    processCount = processesNeeded;
    status = RmGetList(session.handle, &processesNeeded, &processCount,
                       affectedProcesses.data(), &rebootReasons);
    if (status != ERROR_SUCCESS) {
        ShowMessage(L"无法完整读取 Hook DLL 的占用进程，本次操作已取消。请关闭目标程序后重试。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"读取 DLL 占用进程失败", status));
        return false;
    }

    std::wstring message = L"以下程序仍在使用 Hotkey Blocker 的 Hook DLL：\r\n";
    for (UINT index = 0; index < processCount; ++index) {
        const RM_PROCESS_INFO& process = affectedProcesses[index];
        message += L"\r\n• ";
        message += process.strAppName[0] == L'\0' ? L"未知程序" : process.strAppName;
        message += L"（PID ";
        message += std::to_wstring(process.Process.dwProcessId);
        message += L"）";
    }
    message += L"\r\n\r\n请保存工作并退出这些程序，然后重新运行安装器或卸载程序。";
    ShowMessage(message);
    return false;
}

}  // namespace

int PrepareForInstallerChange() {
    HANDLE mutex = nullptr;
    if (!RequestRunningApplicationToExit(mutex)) {
        return 1;
    }
    const int result = FindProcessesUsingHookDlls() ? 0 : 2;
    CloseHandle(mutex);
    return result;
}

}  // namespace InstallerSupport
