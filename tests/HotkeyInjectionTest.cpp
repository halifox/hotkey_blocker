#include "Injector.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
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
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring UniqueName(const wchar_t* prefix) {
    return std::wstring(L"Local\\") + prefix + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(GetTickCount64());
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

int wmain(int argc, wchar_t* argv[]) {
    const bool use32BitTarget = argc == 2 && _wcsicmp(argv[1], L"--win32") == 0;
    if (argc > 2 || (argc == 2 && !use32BitTarget)) {
        return 9;
    }

    const std::wstring modulePath = ModulePath();
    if (modulePath.empty()) {
        return 10;
    }
    const std::filesystem::path directory = std::filesystem::path(modulePath).parent_path();
    const std::filesystem::path probeDirectory =
        use32BitTarget ? directory / L"win32" : directory;
    const std::filesystem::path probe = probeDirectory / L"hotkey_probe.exe";
    const std::filesystem::path hook =
        use32BitTarget ? probeDirectory / L"HotkeyHook32.dll" : directory / L"HotkeyHook64.dll";
    if (GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(hook.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return 11;
    }

    const std::wstring readyName = UniqueName(L"hkb_ready_");
    const std::wstring releaseName = UniqueName(L"hkb_release_");
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, releaseName.c_str());
    if (ready == nullptr || release == nullptr) {
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        if (release != nullptr) {
            CloseHandle(release);
        }
        return 12;
    }

    wchar_t temporaryDirectory[MAX_PATH]{};
    wchar_t temporaryFile[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporaryDirectory) == 0 ||
        GetTempFileNameW(temporaryDirectory, L"hkb", 0, temporaryFile) == 0) {
        CloseHandle(ready);
        CloseHandle(release);
        return 13;
    }
    const std::filesystem::path outputPath(temporaryFile);

    std::wstring commandLine = Quote(probe.wstring()) + L" " + Quote(readyName) + L" " +
                               Quote(releaseName) + L" " + Quote(outputPath.wstring());
    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(probe.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, probeDirectory.c_str(), &startupInfo,
                                        &processInfo);
    if (!created) {
        DeleteFileW(outputPath.c_str());
        CloseHandle(ready);
        CloseHandle(release);
        return 14;
    }

    int exitCode = 1;
    if (WaitForSingleObject(ready, 5000) == WAIT_OBJECT_0) {
        const Injector injector(directory);
        const InjectionResult injection = injector.Inject(processInfo.dwProcessId);
        if (injection.IsSuccess()) {
            SetEvent(release);
            if (WaitForSingleObject(processInfo.hProcess, 10000) == WAIT_OBJECT_0) {
                DWORD childExitCode = 1;
                if (GetExitCodeProcess(processInfo.hProcess, &childExitCode)) {
                    exitCode = static_cast<int>(childExitCode);
                }
            }
        } else {
            std::wcerr << (injection.error.empty() ? L"注入失败" : injection.error) << L'\n';
            SetEvent(release);
            WaitForSingleObject(processInfo.hProcess, 10000);
        }
    } else {
        SetEvent(release);
        WaitForSingleObject(processInfo.hProcess, 10000);
    }

    if (WaitForSingleObject(processInfo.hProcess, 0) != WAIT_OBJECT_0) {
        TerminateProcess(processInfo.hProcess, 1);
        WaitForSingleObject(processInfo.hProcess, 2000);
    }
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    CloseHandle(ready);
    CloseHandle(release);

    std::string output;
    const bool read = ReadText(outputPath, output);
    DeleteFileW(outputPath.c_str());
    return read && output.find("baseline=1") != std::string::npos &&
                   output.find("blocked=1") != std::string::npos && exitCode == 0
               ? 0
               : 15;
}
