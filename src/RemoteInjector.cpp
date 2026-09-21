#include "RemoteInjector.h"
#include "Win32Support.h"

#include <windows.h>

#include <filesystem>
#include <memory>
#include <utility>

namespace {

constexpr DWORD kInjectionTimeoutMs = 10000;

class RemoteThreadOperation final : public InjectionOperation {
public:
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

        DWORD moduleHandle = 0;
        if (!GetExitCodeThread(m_thread, &moduleHandle)) {
            completion.status = InjectionStatus::Indeterminate;
            completion.error = Win32Support::ErrorMessage(L"获取远程线程结果失败");
        } else if (moduleHandle == 0) {
            completion.status = InjectionStatus::Failed;
            completion.error = L"目标进程拒绝加载 Hook DLL";
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

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
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
        operation = std::make_shared<RemoteThreadOperation>();
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

    InjectionCompletion completion;
    if (operation->TryComplete(completion)) {
        result.status = completion.status;
        result.error = std::move(completion.error);
    }
    return result;
}
