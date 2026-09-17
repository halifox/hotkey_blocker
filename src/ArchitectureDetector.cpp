#include "ArchitectureDetector.h"

#include <windows.h>

#include <iterator>
#include <utility>

namespace {

using IsWow64Process2Function = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
using IsWow64ProcessFunction = BOOL(WINAPI*)(HANDLE, PBOOL);

std::wstring Win32Error(const wchar_t* operation) {
    const DWORD errorCode = GetLastError();
    wchar_t buffer[256]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, errorCode, 0, buffer,
                                       static_cast<DWORD>(std::size(buffer)), nullptr);
    if (length == 0) {
        return std::wstring(operation) + L"（错误码 " + std::to_wstring(errorCode) + L"）";
    }
    std::wstring result(operation);
    result += L"：";
    result.append(buffer, length);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}

}  // namespace

ArchitectureResult DetectProcessArchitecture(DWORD pid) {
    ArchitectureResult result;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        result.error = Win32Error(L"打开目标进程失败");
        return result;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto isWow64Process2 = kernel32 == nullptr
                                ? nullptr
                                : reinterpret_cast<IsWow64Process2Function>(
                                      GetProcAddress(kernel32, "IsWow64Process2"));
    if (isWow64Process2 != nullptr) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (!isWow64Process2(process, &processMachine, &nativeMachine)) {
            result.error = Win32Error(L"检测目标进程架构失败");
            CloseHandle(process);
            return result;
        }

        if (processMachine == IMAGE_FILE_MACHINE_I386) {
            result.architecture = ProcessArchitecture::X86;
        } else if (processMachine == IMAGE_FILE_MACHINE_AMD64) {
            result.architecture = ProcessArchitecture::X64;
        } else if (nativeMachine == IMAGE_FILE_MACHINE_I386) {
            result.architecture = ProcessArchitecture::X86;
        } else if (nativeMachine == IMAGE_FILE_MACHINE_AMD64) {
            result.architecture = ProcessArchitecture::X64;
        }

        if (result.architecture == ProcessArchitecture::Unknown) {
            result.error = L"目标进程使用了不支持的处理器架构";
        }
        CloseHandle(process);
        return result;
    }

    auto isWow64Process = kernel32 == nullptr
                              ? nullptr
                              : reinterpret_cast<IsWow64ProcessFunction>(
                                    GetProcAddress(kernel32, "IsWow64Process"));
    if (isWow64Process == nullptr) {
        result.error = L"系统不支持进程架构检测";
        CloseHandle(process);
        return result;
    }

    BOOL wow64 = FALSE;
    if (!isWow64Process(process, &wow64)) {
        result.error = Win32Error(L"检测目标进程架构失败");
        CloseHandle(process);
        return result;
    }

    if (wow64 || sizeof(void*) == 4) {
        result.architecture = ProcessArchitecture::X86;
    } else {
        result.architecture = ProcessArchitecture::X64;
    }
    CloseHandle(process);
    return result;
}

const wchar_t* ArchitectureName(ProcessArchitecture architecture) {
    switch (architecture) {
        case ProcessArchitecture::X86:
            return L"32 位";
        case ProcessArchitecture::X64:
            return L"64 位";
        default:
            return L"未知架构";
    }
}
