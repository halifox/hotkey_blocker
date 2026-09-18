#include "ProcessMonitor.h"

#include "PathUtils.h"

#include <windows.h>

#include <tlhelp32.h>

#include <utility>
#include <vector>

namespace {

constexpr DWORD kScanIntervalMs = 1000;

bool IsStopRequested(HANDLE stopEvent) {
    return stopEvent != nullptr && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
}

}  // namespace

ProcessMonitor::ProcessMonitor() = default;

ProcessMonitor::~ProcessMonitor() {
    Stop();
}

bool ProcessMonitor::Start(Callback callback) {
    Stop();

    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr || readyEvent == nullptr) {
        if (stopEvent != nullptr) {
            CloseHandle(stopEvent);
        }
        if (readyEvent != nullptr) {
            CloseHandle(readyEvent);
        }
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_callback = std::move(callback);
        m_currentProcesses.clear();
        m_stopEvent = stopEvent;
        m_readyEvent = readyEvent;
        m_stopRequested = false;
        m_running = true;
    }

    try {
        m_thread = std::thread(&ProcessMonitor::Run, this);
    } catch (...) {
        std::lock_guard lock(m_mutex);
        m_callback = {};
        m_currentProcesses.clear();
        m_running = false;
        m_stopRequested = false;
        m_stopEvent = nullptr;
        m_readyEvent = nullptr;
        CloseHandle(stopEvent);
        CloseHandle(readyEvent);
        return false;
    }
    return true;
}

void ProcessMonitor::Stop() {
    HANDLE stopEvent = nullptr;
    HANDLE readyEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (!m_running && !m_thread.joinable()) {
            return;
        }
        m_stopRequested = true;
        stopEvent = m_stopEvent;
        readyEvent = m_readyEvent;
    }

    if (stopEvent != nullptr) {
        SetEvent(stopEvent);
    }
    if (readyEvent != nullptr) {
        SetEvent(readyEvent);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    {
        std::lock_guard lock(m_mutex);
        m_callback = {};
        m_currentProcesses.clear();
        m_running = false;
        m_stopRequested = false;
        m_stopEvent = nullptr;
        m_readyEvent = nullptr;
    }

    if (stopEvent != nullptr) {
        CloseHandle(stopEvent);
    }
    if (readyEvent != nullptr) {
        CloseHandle(readyEvent);
    }
}

std::vector<ProcessInfo> ProcessMonitor::Snapshot() const {
    std::lock_guard lock(m_mutex);
    std::vector<ProcessInfo> result;
    result.reserve(m_currentProcesses.size());
    for (const auto& [pid, process] : m_currentProcesses) {
        (void)pid;
        result.push_back(process);
    }
    return result;
}

bool ProcessMonitor::WaitUntilReady(DWORD timeoutMs) const {
    HANDLE readyEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        readyEvent = m_readyEvent;
    }
    return readyEvent != nullptr && WaitForSingleObject(readyEvent, timeoutMs) == WAIT_OBJECT_0;
}

bool ProcessMonitor::IsProcessAlive(const ProcessInfo& process) {
    if (process.pid == 0) {
        return false;
    }

    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                process.pid);
    if (handle == nullptr) {
        return false;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    bool sameProcess = true;
    if (process.creationTime != 0) {
        if (!GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
            CloseHandle(handle);
            return false;
        }
        ULARGE_INTEGER value{};
        value.LowPart = creation.dwLowDateTime;
        value.HighPart = creation.dwHighDateTime;
        sameProcess = value.QuadPart == process.creationTime;
    }

    const DWORD waitResult = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return sameProcess && waitResult == WAIT_TIMEOUT;
}

bool ProcessMonitor::QueryProcessInfo(DWORD pid, ProcessInfo& process) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (handle == nullptr) {
        return false;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
        CloseHandle(handle);
        return false;
    }

    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const BOOL success = QueryFullProcessImageNameW(handle, 0, buffer.data(), &length);
    CloseHandle(handle);
    if (!success || length == 0) {
        return false;
    }

    std::wstring imagePath = PathUtils::NormalizePath(std::wstring(buffer.data(), length));
    if (imagePath.empty()) {
        return false;
    }

    ULARGE_INTEGER creationValue{};
    creationValue.LowPart = creation.dwLowDateTime;
    creationValue.HighPart = creation.dwHighDateTime;
    process = {pid, std::move(imagePath), creationValue.QuadPart};
    return true;
}

bool ProcessMonitor::SameProcess(const ProcessInfo& left, const ProcessInfo& right) {
    if (left.pid != right.pid) {
        return false;
    }
    return left.creationTime == 0 || right.creationTime == 0 ||
           left.creationTime == right.creationTime;
}

ProcessMonitor::ProcessMap ProcessMonitor::Enumerate(const ProcessMap& previous) {
    ProcessMap current;
    const DWORD ownPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return previous;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == ownPid || entry.th32ProcessID == 0) {
                continue;
            }

            ProcessInfo process;
            if (!QueryProcessInfo(entry.th32ProcessID, process)) {
                const auto oldProcess = previous.find(entry.th32ProcessID);
                if (oldProcess != previous.end()) {
                    current.emplace(oldProcess->first, oldProcess->second);
                }
                continue;
            }
            current.emplace(process.pid, std::move(process));
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return current;
}

void ProcessMonitor::Publish(const ProcessEvent& event) {
    Callback callback;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopRequested) {
            return;
        }
        callback = m_callback;
    }
    if (callback) {
        callback(event);
    }
}

void ProcessMonitor::SignalReady() {
    HANDLE readyEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        readyEvent = m_readyEvent;
    }
    if (readyEvent != nullptr) {
        SetEvent(readyEvent);
    }
}

void ProcessMonitor::Run() {
    ProcessMap previous;
    bool firstScan = true;

    while (!IsStopRequested(m_stopEvent)) {
        ProcessMap current = Enumerate(previous);
        std::vector<ProcessEvent> events;
        events.reserve(current.size() + previous.size());

        for (const auto& [pid, process] : current) {
            const auto oldProcess = previous.find(pid);
            if (oldProcess == previous.end()) {
                events.push_back({ProcessEventType::Started, process, firstScan});
            } else if (!SameProcess(oldProcess->second, process) ||
                       !PathUtils::SamePath(oldProcess->second.imagePath, process.imagePath)) {
                events.push_back({ProcessEventType::Exited, oldProcess->second, false});
                events.push_back({ProcessEventType::Started, process, false});
            }
        }
        for (const auto& [pid, process] : previous) {
            if (current.find(pid) == current.end()) {
                events.push_back({ProcessEventType::Exited, process, false});
            }
        }

        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested) {
                break;
            }
            m_currentProcesses = current;
        }
        for (const ProcessEvent& event : events) {
            Publish(event);
        }

        if (firstScan) {
            SignalReady();
        }

        previous = std::move(current);
        firstScan = false;
        if (WaitForSingleObject(m_stopEvent, kScanIntervalMs) == WAIT_OBJECT_0) {
            break;
        }
    }
}
