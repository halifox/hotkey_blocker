#include "RemoteInjector.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>
#include <vector>
#include <thread>

namespace {

constexpr DWORD kInjectionTimeoutMs = 10000;

std::wstring Win32Error(const wchar_t* operation, DWORD errorCode = GetLastError()) {
    wchar_t buffer[512]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, errorCode, 0, buffer,
                                       static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring result(operation);
    if (length != 0) {
        result += L"：";
        result.append(buffer, length);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
    } else {
        result += L"（错误码 " + std::to_wstring(errorCode) + L"）";
    }
    return result;
}

std::wstring FullPath(const std::wstring& path) {
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0 || required >= MAXDWORD - 1) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()),
                                           result.data(), nullptr);
    if (written == 0 || written >= result.size()) {
        return {};
    }
    result.resize(written);
    return result;
}

void ReapTimedOutRemoteThread(HANDLE process, HANDLE remoteThread, LPVOID remotePath) {
    try {
        std::thread([process, remoteThread, remotePath] {
            // The remote thread may still be using the path buffer. Keep both
            // handles alive until it exits, then release the target allocation.
            WaitForSingleObject(remoteThread, INFINITE);
            VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
            CloseHandle(remoteThread);
            CloseHandle(process);
        }).detach();
    } catch (...) {
        // There is no safe way to free remotePath while the remote thread may
        // still be reading it. Keep the original handles/remote allocation
        // alive only as a last-resort leak until the target process exits.
    }
}

}  // namespace

RemoteInjectionResult InjectDllIntoProcess(DWORD pid, const std::wstring& dllPath) {
    RemoteInjectionResult result;
    const std::wstring absoluteDllPath = FullPath(dllPath);
    if (absoluteDllPath.empty()) {
        result.error = L"无法解析 Hook DLL 路径";
        return result;
    }

    const DWORD attributes = GetFileAttributesW(absoluteDllPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        result.error = L"Hook DLL 不存在：" + absoluteDllPath;
        return result;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (process == nullptr) {
        result.error = Win32Error(L"打开目标进程失败");
        return result;
    }

    const SIZE_T pathBytes = (absoluteDllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (remotePath == nullptr) {
        result.error = Win32Error(L"分配目标进程内存失败");
        CloseHandle(process);
        return result;
    }

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(process, remotePath, absoluteDllPath.c_str(), pathBytes,
                            &bytesWritten) ||
        bytesWritten != pathBytes) {
        result.error = Win32Error(L"写入目标进程内存失败");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = kernel32 == nullptr
                              ? nullptr
                              : GetProcAddress(kernel32, "LoadLibraryW");
    if (loadLibrary == nullptr) {
        result.error = Win32Error(L"获取 LoadLibraryW 地址失败");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }

    HANDLE remoteThread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), remotePath, 0, nullptr);
    if (remoteThread == nullptr) {
        result.error = Win32Error(L"创建远程线程失败");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }

    const DWORD waitResult = WaitForSingleObject(remoteThread, kInjectionTimeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        result.error = L"等待远程 DLL 加载超时";
        ReapTimedOutRemoteThread(process, remoteThread, remotePath);
        return result;
    }
    if (waitResult != WAIT_OBJECT_0) {
        result.error = Win32Error(L"等待远程线程失败");
        ReapTimedOutRemoteThread(process, remoteThread, remotePath);
        return result;
    }

    DWORD moduleHandle = 0;
    if (!GetExitCodeThread(remoteThread, &moduleHandle)) {
        result.error = Win32Error(L"获取远程线程结果失败");
    } else if (moduleHandle == 0) {
        result.error = L"目标进程拒绝加载 Hook DLL";
    } else {
        result.success = true;
    }

    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(remoteThread);
    CloseHandle(process);
    return result;
}
