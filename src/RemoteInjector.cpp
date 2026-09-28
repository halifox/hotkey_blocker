#include "RemoteInjector.h"
#include "Win32Support.h"

#include <windows.h>
#include <tlhelp32.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace {

constexpr DWORD kInjectionTimeoutMs = 10000;

class RemoteThreadOperation final : public InjectionOperation {
public:
    explicit RemoteThreadOperation(std::wstring dllPath, ULONG_PTR initializerOffset)
        : m_dllPath(std::move(dllPath)), m_initializerOffset(initializerOffset) {}

    void Attach(HANDLE process, HANDLE thread, LPVOID remotePath) noexcept {
        m_process = process;
        m_thread = thread;
        m_remotePath = remotePath;
    }

    ~RemoteThreadOperation() override {
        // LoadLibraryW may still be reading this allocation until its remote thread exits.
        if (m_thread != nullptr && WaitForSingleObject(m_thread, 0) == WAIT_OBJECT_0 &&
            m_process != nullptr && m_remotePath != nullptr) {
            VirtualFreeEx(m_process, m_remotePath, 0, MEM_RELEASE);
            m_remotePath = nullptr;
        }
        CloseHandles();
    }

    bool TryComplete(InjectionCompletion& completion) override {
        if (m_thread == nullptr) {
            completion.status = InjectionStatus::Failed;
            completion.error = L"远程注入线程句柄无效";
            return true;
        }

        const DWORD waitResult = WaitForSingleObject(m_thread, 0);
        if (waitResult == WAIT_TIMEOUT) {
            return false;
        }
        if (waitResult == WAIT_FAILED) {
            const std::wstring waitError = Win32Support::ErrorMessage(L"等待远程线程失败");
            const DWORD processWait = m_process == nullptr ? WAIT_FAILED
                                                           : WaitForSingleObject(m_process, 0);
            if (processWait == WAIT_OBJECT_0) {
                completion.status = InjectionStatus::Failed;
                completion.error = L"目标进程已退出，无法完成 DLL 加载";
                m_remotePath = nullptr;
                CloseHandles();
                return true;
            }
            completion.status = InjectionStatus::Indeterminate;
            completion.error = waitError;
            return false;
        }
        if (waitResult != WAIT_OBJECT_0) {
            completion.status = InjectionStatus::Indeterminate;
            completion.error = L"等待远程线程返回未知状态";
            return false;
        }

        DWORD threadResult = 0;
        if (!GetExitCodeThread(m_thread, &threadResult)) {
            completion.status = InjectionStatus::Indeterminate;
            completion.error = Win32Support::ErrorMessage(L"获取远程线程结果失败");
        } else if (!m_initializing) {
            // A thread exit code truncates HMODULE on x64. Resolve the full base
            // address from the target's module list instead.
            ULONG_PTR base = 0;
            HANDLE snapshot = INVALID_HANDLE_VALUE;
            for (int attempt = 0; attempt < 8; ++attempt) {
                snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetProcessId(m_process));
                if (snapshot != INVALID_HANDLE_VALUE || GetLastError() != ERROR_BAD_LENGTH) break;
            }
            if (snapshot != INVALID_HANDLE_VALUE) {
                MODULEENTRY32W entry{sizeof(entry)};
                for (BOOL next = Module32FirstW(snapshot, &entry); next;
                     next = Module32NextW(snapshot, &entry)) {
                    if (_wcsicmp(entry.szExePath, m_dllPath.c_str()) == 0) {
                        base = reinterpret_cast<ULONG_PTR>(entry.modBaseAddr);
                        break;
                    }
                }
                CloseHandle(snapshot);
            }
            if (base != 0) {
                CloseHandle(m_thread);
                m_thread = CreateRemoteThread(m_process, nullptr, 0,
                    reinterpret_cast<LPTHREAD_START_ROUTINE>(base + m_initializerOffset),
                    nullptr, 0, nullptr);
                m_initializing = true;
                if (m_thread != nullptr) return false;
                completion.error = Win32Support::ErrorMessage(L"创建 Hook 初始化线程失败");
            } else {
                completion.error = L"目标进程未加载预期的 Hook DLL";
            }
            completion.status = InjectionStatus::Failed;
        } else if (threadResult != ERROR_SUCCESS) {
            completion.status = InjectionStatus::Failed;
            completion.error = Win32Support::ErrorMessage(L"Hook 初始化失败", threadResult);
        } else {
            completion.status = InjectionStatus::Succeeded;
        }

        if (m_process != nullptr && m_remotePath != nullptr) {
            VirtualFreeEx(m_process, m_remotePath, 0, MEM_RELEASE);
            m_remotePath = nullptr;
        }
        CloseHandles();
        return true;
    }

private:
    void CloseHandles() {
        if (m_thread != nullptr) {
            CloseHandle(m_thread);
            m_thread = nullptr;
        }
        if (m_process != nullptr) {
            CloseHandle(m_process);
            m_process = nullptr;
        }
    }

    HANDLE m_process = nullptr;
    HANDLE m_thread = nullptr;
    LPVOID m_remotePath = nullptr;
    std::wstring m_dllPath;
    ULONG_PTR m_initializerOffset = 0;
    bool m_initializing = false;
};

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

}  // namespace

RemoteInjectionResult InjectDllIntoProcess(DWORD pid, const std::wstring& dllPath,
                                           bool waitForCompletion) {
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

    // Map only to resolve the export RVA; do not execute the hook in the injector.
    HMODULE image = LoadLibraryExW(absoluteDllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    FARPROC initializer = image == nullptr ? nullptr : GetProcAddress(image, "HkbInitializeHook");
    if (initializer == nullptr) {
        result.error = L"Hook DLL 缺少初始化入口";
        if (image != nullptr) FreeLibrary(image);
        return result;
    }
    const ULONG_PTR initializerOffset = reinterpret_cast<ULONG_PTR>(initializer) -
                                        reinterpret_cast<ULONG_PTR>(image);
    FreeLibrary(image);

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                                 FALSE, pid);
    if (process == nullptr) {
        result.error = Win32Support::ErrorMessage(L"打开目标进程失败");
        return result;
    }

    const SIZE_T pathBytes = (absoluteDllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
    if (remotePath == nullptr) {
        result.error = Win32Support::ErrorMessage(L"分配目标进程内存失败");
        CloseHandle(process);
        return result;
    }

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(process, remotePath, absoluteDllPath.c_str(), pathBytes,
                            &bytesWritten) ||
        bytesWritten != pathBytes) {
        result.error = Win32Support::ErrorMessage(L"写入目标进程内存失败");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC loadLibrary = kernel32 == nullptr
                              ? nullptr
                              : GetProcAddress(kernel32, "LoadLibraryW");
    if (loadLibrary == nullptr) {
        result.error = Win32Support::ErrorMessage(L"获取 LoadLibraryW 地址失败");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }

    std::shared_ptr<RemoteThreadOperation> operation;
    try {
        operation = std::make_shared<RemoteThreadOperation>(absoluteDllPath, initializerOffset);
    } catch (...) {
        result.error = L"无法保留远程注入操作状态";
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }

    HANDLE remoteThread = CreateRemoteThread(
        process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(loadLibrary), remotePath, 0, nullptr);
    if (remoteThread == nullptr) {
        result.error = Win32Support::ErrorMessage(L"创建远程线程失败");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }
    operation->Attach(process, remoteThread, remotePath);

    const DWORD waitResult = WaitForSingleObject(
        remoteThread, waitForCompletion ? INFINITE : kInjectionTimeoutMs);
    if (waitResult != WAIT_OBJECT_0) {
        result.status = InjectionStatus::Pending;
        result.error = waitResult == WAIT_TIMEOUT
                           ? L"等待远程 DLL 加载超时"
                           : Win32Support::ErrorMessage(L"等待远程线程失败");
        result.pendingOperation = std::move(operation);
        return result;
    }

    const ULONGLONG started = GetTickCount64();
    for (;;) {
        InjectionCompletion completion;
        if (operation->TryComplete(completion)) {
            result.status = completion.status;
            result.error = std::move(completion.error);
            break;
        }
        if (!waitForCompletion && GetTickCount64() - started >= kInjectionTimeoutMs) {
            result.status = InjectionStatus::Pending;
            result.pendingOperation = std::move(operation);
            break;
        }
        Sleep(10);
    }
    return result;
}
