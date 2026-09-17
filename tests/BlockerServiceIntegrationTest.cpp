#include "BlockerService.h"

#include "Logger.h"
#include "PathUtils.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::wstring ModulePath() {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}

std::wstring Quote(const std::wstring& argument) {
    std::wstring result;
    result.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring UniqueName(const wchar_t* prefix) {
    return std::wstring(L"Local\\") + prefix + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(GetTickCount64());
}

struct StateWaiter {
    std::mutex mutex;
    std::condition_variable condition;

    void Notify() {
        std::lock_guard lock(mutex);
        condition.notify_all();
    }
};

bool WaitForStatus(BlockerService& service, StateWaiter& waiter, AppStatus expected,
                   DWORD timeoutMs, std::wstring& detail) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    const auto matches = [&] {
        const std::vector<RuntimeRuleState> states = service.Snapshot();
        if (!states.empty()) {
            detail = states.front().detail;
            if (states.front().status == expected) {
                return true;
            }
            if (states.front().status == AppStatus::InjectionFailed) {
                return false;
            }
        }
        return false;
    };

    if (matches()) {
        return true;
    }
    std::unique_lock lock(waiter.mutex);
    return waiter.condition.wait_until(lock, deadline, matches);
}

bool ReadText(const std::filesystem::path& path, std::string& text) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

}  // namespace

int wmain() {
    const std::filesystem::path directory = std::filesystem::path(ModulePath()).parent_path();
    const std::filesystem::path probe = directory / L"hotkey_probe.exe";
    if (directory.empty() || GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wcerr << L"找不到同目录下的 hotkey_probe.exe\n";
        return 1;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryFile[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0 ||
        GetTempFileNameW(temporaryDirectory, L"hbs", 0, temporaryFile) == 0) {
        return 2;
    }
    const std::filesystem::path outputPath(temporaryFile);
    DeleteFileW(outputPath.c_str());

    const std::wstring readyName = UniqueName(L"hkb_service_ready_");
    const std::wstring releaseName = UniqueName(L"hkb_service_release_");
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
    if (ready == nullptr || release == nullptr) {
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        if (release != nullptr) {
            CloseHandle(release);
        }
        DeleteFileW(outputPath.c_str());
        return 3;
    }

    const std::wstring rulePath = PathUtils::NormalizePath(probe.wstring());
    const std::filesystem::path logPath = std::filesystem::path(temporaryFile).wstring() +
                                          L".log";
    Logger logger(logPath);
    BlockerService service(&logger);
    StateWaiter waiter;
    service.SetStateChangedCallback([&waiter] { waiter.Notify(); });
    const std::vector<AppRule> rules = {{rulePath, true}};
    if (!service.Start(rules)) {
        CloseHandle(ready);
        CloseHandle(release);
        DeleteFileW(outputPath.c_str());
        DeleteFileW(logPath.c_str());
        return 4;
    }

    if (!service.WaitUntilReady(5000)) {
        service.Stop();
        CloseHandle(ready);
        CloseHandle(release);
        DeleteFileW(outputPath.c_str());
        DeleteFileW(logPath.c_str());
        return 5;
    }

    std::wstring commandLine = Quote(probe.wstring()) + L" " + Quote(readyName) + L" " +
                               Quote(releaseName) + L" " + Quote(outputPath.wstring());
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(probe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, directory.c_str(), &startupInfo,
                                        &processInfo);
    if (!created) {
        service.Stop();
        CloseHandle(ready);
        CloseHandle(release);
        DeleteFileW(outputPath.c_str());
        DeleteFileW(logPath.c_str());
        return 5;
    }

    int result = 6;
    std::wstring detail;
    if (WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0 &&
        WaitForStatus(service, waiter, AppStatus::Blocked, 10000, detail)) {
        SetEvent(release);
        if (WaitForSingleObject(processInfo.hProcess, 10000) == WAIT_OBJECT_0) {
            DWORD childExitCode = 1;
            std::string output;
            if (GetExitCodeProcess(processInfo.hProcess, &childExitCode) &&
                ReadText(outputPath, output) && childExitCode == 0 &&
                output.find("baseline=1") != std::string::npos &&
                output.find("blocked=1") != std::string::npos) {
                result = 0;
            }
        }
    } else {
        std::wcerr << L"服务未进入已拦截状态：" << detail << L'\n';
        SetEvent(release);
    }

    if (WaitForSingleObject(processInfo.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, 2000);
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    if (result == 0) {
        std::wstring exitDetail;
        if (!WaitForStatus(service, waiter, AppStatus::Waiting, 5000, exitDetail)) {
            std::wcerr << L"目标进程退出后未恢复等待状态：" << exitDetail << L'\n';
            result = 7;
        }
    }

    service.Stop();
    CloseHandle(ready);
    CloseHandle(release);
    DeleteFileW(outputPath.c_str());
    if (result == 0) {
        DeleteFileW(logPath.c_str());
        DeleteFileW((logPath.wstring() + L".1").c_str());
        DeleteFileW((logPath.wstring() + L".2").c_str());
    } else {
        std::wcerr << L"保留失败日志：" << logPath.wstring() << L'\n';
    }
    return result;
}
