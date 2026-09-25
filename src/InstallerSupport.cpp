#include "InstallerSupport.h"

#include "Win32Support.h"

#include <restartmanager.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace InstallerSupport {
namespace {

constexpr wchar_t kMainWindowClassName[] = L"HotkeyBlocker.MainFrame";

struct OccupyingProcess final {
    std::wstring name;
    DWORD processId = 0;
};

void ShowMessage(const std::wstring& message, UINT flags = MB_OK | MB_ICONWARNING) {
    MessageBoxW(nullptr, message.c_str(), L"Hotkey Blocker 安装器",
                flags | MB_SETFOREGROUND | MB_TOPMOST);
}

void NotifyOccupiedProcesses(const std::wstring& processList, ChangeOperation operation) {
    const bool installing = operation == ChangeOperation::Install;
    std::wstring message = processList;
    message += L"\r\n\r\n请关闭以上应用后重新运行";
    message += installing ? L"安装程序。安装程序不会结束这些进程，本次安装已取消。"
                          : L"卸载程序。卸载程序不会结束这些进程，本次卸载已取消。";

    MessageBoxW(nullptr, message.c_str(),
                installing ? L"Hotkey Blocker 安装器" : L"Hotkey Blocker 卸载程序",
                MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
}

bool OpenApplicationMutex(HANDLE& mutex, bool& ownsMutex) {
    mutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (mutex == nullptr) {
        ShowMessage(L"无法检查 Hotkey Blocker 是否正在运行。\r\n\r\n" +
                    Win32Support::ErrorMessage(L"创建 Hotkey Blocker 单实例锁失败"));
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(mutex, 0);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        ownsMutex = true;
        return true;
    }
    if (waitResult == WAIT_TIMEOUT) {
        ownsMutex = false;
        return true;
    }

    CloseHandle(mutex);
    mutex = nullptr;
    ShowMessage(L"无法检查 Hotkey Blocker 是否正在运行。\r\n\r\n" +
                Win32Support::ErrorMessage(L"等待 Hotkey Blocker 单实例锁失败"));
    return false;
}

bool RefreshApplicationMutex(HANDLE mutex, bool& ownsMutex) {
    if (ownsMutex) {
        return true;
    }

    const DWORD waitResult = WaitForSingleObject(mutex, 0);
    if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED) {
        ownsMutex = true;
        return true;
    }
    if (waitResult == WAIT_TIMEOUT) {
        return true;
    }

    ShowMessage(L"无法重新检查 Hotkey Blocker 是否正在运行。\r\n\r\n" +
                Win32Support::ErrorMessage(L"等待 Hotkey Blocker 单实例锁失败"));
    return false;
}
std::vector<std::filesystem::path> GetInstalledResources(const wchar_t* installDirectory) {
    std::vector<std::filesystem::path> paths;
    const std::filesystem::path directory(installDirectory);
    paths.push_back(directory / L"HotkeyBlocker.exe");

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

bool WaitForInstalledResourcesToBeReleased(const wchar_t* installDirectory, HANDLE mutex,
                                           bool& ownsMutex, ChangeOperation operation) {
    if (installDirectory == nullptr || installDirectory[0] == L'\0') {
        ShowMessage(L"未提供有效的安装目录，本次操作已取消。");
        return false;
    }

    const std::vector<std::filesystem::path> resources = GetInstalledResources(installDirectory);
    std::array<wchar_t, CCH_RM_SESSION_KEY + 1> sessionKey{};
    swprintf_s(sessionKey.data(), sessionKey.size(), L"HKB-%lu-%lu", GetCurrentProcessId(),
               GetTickCount());

    RestartManagerSession session;
    if (!resources.empty()) {
        DWORD status = RmStartSession(&session.handle, 0, sessionKey.data());
        if (status != ERROR_SUCCESS) {
            ShowMessage(L"无法检查安装文件的占用情况，本次操作已取消。\r\n\r\n" +
                        Win32Support::ErrorMessage(L"启动 Windows Restart Manager 失败", status));
            return false;
        }
        session.started = true;

        std::vector<LPCWSTR> resourcePaths;
        resourcePaths.reserve(resources.size());
        for (const std::filesystem::path& path : resources) {
            resourcePaths.push_back(path.c_str());
        }

        status = RmRegisterResources(session.handle, static_cast<UINT>(resourcePaths.size()),
                                     resourcePaths.data(), 0, nullptr, 0, nullptr);
        if (status != ERROR_SUCCESS) {
            ShowMessage(L"无法登记安装文件以检查占用情况，本次操作已取消。\r\n\r\n" +
                        Win32Support::ErrorMessage(L"登记安装文件失败", status));
            return false;
        }
    }

    const auto getAffectedProcesses = [&](std::vector<RM_PROCESS_INFO>& affectedProcesses,
                                          DWORD& rebootReasons) {
        affectedProcesses.clear();
        rebootReasons = RmRebootReasonNone;
        if (!session.started) {
            return true;
        }

        constexpr int kMaximumListRetries = 3;
        for (int attempt = 0; attempt < kMaximumListRetries; ++attempt) {
            UINT processesNeeded = 0;
            UINT processCount = 0;
            DWORD listStatus = RmGetList(session.handle, &processesNeeded, &processCount,
                                         nullptr, &rebootReasons);
            if (listStatus != ERROR_SUCCESS && listStatus != ERROR_MORE_DATA) {
                ShowMessage(L"无法读取安装文件的占用进程，本次操作已取消。\r\n\r\n" +
                            Win32Support::ErrorMessage(L"读取占用进程失败", listStatus));
                return false;
            }
            if (processesNeeded == 0) {
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
                ShowMessage(L"无法完整读取安装文件的占用进程，本次操作已取消。\r\n\r\n" +
                            Win32Support::ErrorMessage(L"读取占用进程失败", listStatus));
                return false;
            }

            affectedProcesses.resize(processCount);
            affectedProcesses.erase(
                    std::remove_if(affectedProcesses.begin(), affectedProcesses.end(), [](const auto& process) {
                        return process.Process.dwProcessId == GetCurrentProcessId();
                    }),
                    affectedProcesses.end());
            return true;
        }

        ShowMessage(L"安装文件的占用进程列表持续变化，本次操作已取消。请稍后重新运行安装器或卸载程序。");
        return false;
    };

    const auto buildProcessList = [&](const std::vector<RM_PROCESS_INFO>& affectedProcesses) {
        std::vector<OccupyingProcess> processes;
        processes.reserve(affectedProcesses.size() + 1);
        for (const RM_PROCESS_INFO& process : affectedProcesses) {
            const DWORD processId = process.Process.dwProcessId;
            const auto duplicate = std::find_if(processes.begin(), processes.end(), [processId](const auto& item) {
                return item.processId == processId;
            });
            if (duplicate == processes.end()) {
                processes.push_back({process.strAppName[0] == L'\0' ? L"未知应用" : process.strAppName,
                                     processId});
            }
        }

        if (!ownsMutex) {
            const HWND window = FindWindowW(kMainWindowClassName, nullptr);
            DWORD processId = 0;
            if (window != nullptr) {
                GetWindowThreadProcessId(window, &processId);
            }
            const auto duplicate = std::find_if(processes.begin(), processes.end(), [processId](const auto& item) {
                return processId != 0 && item.processId == processId;
            });
            if (processId != 0 && duplicate == processes.end()) {
                processes.push_back({L"Hotkey Blocker", processId});
            } else if (processId == 0 &&
                       std::none_of(processes.begin(), processes.end(), [](const auto& item) {
                           return _wcsicmp(item.name.c_str(), L"Hotkey Blocker") == 0 ||
                                  _wcsicmp(item.name.c_str(), L"HotkeyBlocker.exe") == 0;
                       })) {
                processes.push_back({L"Hotkey Blocker（正在运行，PID 暂不可用）", 0});
            }
        }

        std::wstring message = L"请先关闭以下应用后再继续：\r\n";
        for (const OccupyingProcess& process : processes) {
            message += L"\r\n• ";
            message += process.name;
            if (process.processId == 0) {
                message += L"（PID 未知）";
            } else {
                message += L"（PID ";
                message += std::to_wstring(process.processId);
                message += L"）";
            }
        }
        return std::pair<std::vector<OccupyingProcess>, std::wstring>{std::move(processes), std::move(message)};
    };

    if (!RefreshApplicationMutex(mutex, ownsMutex)) {
        return false;
    }

    std::vector<RM_PROCESS_INFO> affectedProcesses;
    DWORD rebootReasons = RmRebootReasonNone;
    if (!getAffectedProcesses(affectedProcesses, rebootReasons)) {
        return false;
    }

    auto [processes, processList] = buildProcessList(affectedProcesses);
    if (processes.empty()) {
        if (rebootReasons == RmRebootReasonNone) {
            return true;
        }
        ShowMessage(L"需要重启电脑才能释放安装文件。请重启后再运行安装器或卸载程序。");
        return false;
    }

    NotifyOccupiedProcesses(processList, operation);
    return false;
}

}  // namespace

int PrepareForChange(const wchar_t* installDirectory, ChangeOperation operation) {
    HANDLE mutex = nullptr;
    bool ownsMutex = false;
    if (!OpenApplicationMutex(mutex, ownsMutex)) {
        return 1;
    }
    const int result = WaitForInstalledResourcesToBeReleased(installDirectory, mutex, ownsMutex,
                                                              operation)
                               ? 0
                               : 2;
    // This short-lived helper exits immediately after this function returns. Keep the mutex
    // handle open until process teardown so the app cannot restart in the final handoff window.
    return result;
}

}  // namespace InstallerSupport
