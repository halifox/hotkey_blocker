#include "Injector.h"

#include "RemoteInjector.h"
#include "Win32Support.h"

#include <windows.h>

#include <filesystem>
#include <utility>

namespace {

constexpr DWORD kHelperTimeoutMs = 15000;

std::filesystem::path Find32BitHelper(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / L"win32" / L"HotkeyBlockerInjector32.exe";
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
               ? path
               : std::filesystem::path{};
}

std::filesystem::path FindHookDll(const std::filesystem::path& directory,
                                   ProcessArchitecture architecture) {
    const wchar_t* fileName = architecture == ProcessArchitecture::X86
                                   ? L"HotkeyHook32.dll"
                                   : L"HotkeyHook64.dll";
    const bool crossArchitecture = sizeof(void*) == 8 && architecture == ProcessArchitecture::X86;
    const std::filesystem::path path = crossArchitecture ? directory / L"win32" / fileName
                                                         : directory / fileName;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0
               ? path
               : std::filesystem::path{};
}

}  // namespace

Injector::Injector() : m_executableDirectory(ExecutableDirectory()) {}

Injector::Injector(std::filesystem::path executableDirectory)
    : m_executableDirectory(std::move(executableDirectory)) {}

InjectionResult Injector::Inject(DWORD pid) const {
    InjectionResult result;
    const ArchitectureResult architecture = DetectProcessArchitecture(pid);
    result.architecture = architecture.architecture;
    if (architecture.architecture == ProcessArchitecture::Unknown) {
        result.error = architecture.error.empty() ? L"无法识别目标进程架构" : architecture.error;
        return result;
    }

    const bool targetIs32Bit = architecture.architecture == ProcessArchitecture::X86;
    if (!targetIs32Bit && sizeof(void*) == 4) {
        result.error = L"32 位主程序不能向 64 位目标进程注入";
        return result;
    }

    const std::filesystem::path dllPath = FindHookDll(m_executableDirectory,
                                                       architecture.architecture);
    if (dllPath.empty()) {
        result.error = std::wstring(L"未找到 ") + ArchitectureName(architecture.architecture) +
                       L" Hook DLL";
        return result;
    }

    if (targetIs32Bit && sizeof(void*) == 8) {
        return InjectWith32BitHelper(pid, dllPath);
    }

    const RemoteInjectionResult remoteResult =
        InjectDllIntoProcess(pid, dllPath.wstring());
    result.success = remoteResult.success;
    result.error = remoteResult.error;
    return result;
}

InjectionResult Injector::InjectWith32BitHelper(
    DWORD pid, const std::filesystem::path& dllPath) const {
    InjectionResult result;
    result.architecture = ProcessArchitecture::X86;

    const std::filesystem::path helper = Find32BitHelper(m_executableDirectory);
    if (helper.empty()) {
        result.error = L"未找到 32 位注入辅助程序 HotkeyBlockerInjector32.exe";
        return result;
    }
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        result.error = L"32 位 Hook DLL 不存在：" + dllPath.wstring();
        return result;
    }

    std::wstring commandLine = QuoteCommandLineArgument(helper.wstring());
    commandLine += L" " + std::to_wstring(pid);
    commandLine += L" " + QuoteCommandLineArgument(dllPath.wstring());

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(helper.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, m_executableDirectory.c_str(), &startupInfo,
                        &processInfo)) {
        result.error = Win32Support::ErrorMessage(L"启动 32 位注入辅助程序失败");
        return result;
    }

    const DWORD waitResult = WaitForSingleObject(processInfo.hProcess, kHelperTimeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        result.error = L"等待 32 位注入辅助程序超时";
    } else if (waitResult != WAIT_OBJECT_0) {
        result.error = Win32Support::ErrorMessage(L"等待 32 位注入辅助程序失败");
    } else {
        DWORD exitCode = 1;
        if (!GetExitCodeProcess(processInfo.hProcess, &exitCode)) {
            result.error = Win32Support::ErrorMessage(L"获取 32 位注入结果失败");
        } else if (exitCode == 0) {
            result.success = true;
        } else {
            result.error = L"32 位注入辅助程序失败（退出码 " + std::to_wstring(exitCode) + L"）";
        }
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return result;
}

std::wstring Injector::QuoteCommandLineArgument(const std::wstring& argument) {
    std::wstring result;
    result.reserve(argument.size() + 2);
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

std::filesystem::path Injector::ExecutableDirectory() {
    const std::wstring modulePath = Win32Support::ModulePath();
    if (modulePath.empty()) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(modulePath).parent_path();
}
