#include "ProcessMonitor.h"

#include "PathUtils.h"

#include <tlhelp32.h>

#include <chrono>
#include <utility>

namespace {

constexpr std::chrono::milliseconds kScanInterval(1000);

}  // namespace

ProcessMonitor::ProcessMonitor() = default;

ProcessMonitor::~ProcessMonitor() {
    Stop();
}

bool ProcessMonitor::Start(Callback callback) {
    Stop();

    {
        std::lock_guard lock(m_mutex);
        m_callback = std::move(callback);
        m_currentProcesses.clear();
        m_stopRequested = false;
        m_running = true;
    }

    try {
        m_thread = std::thread(&ProcessMonitor::Run, this);
    } catch (...) {
        std::lock_guard lock(m_mutex);
        m_callback = {};
        m_running = false;
        return false;
    }
    return true;
}

void ProcessMonitor::Stop() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_running && !m_thread.joinable()) {
            return;
        }
        m_stopRequested = true;
    }
    m_condition.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    std::lock_guard lock(m_mutex);
    m_callback = {};
    m_currentProcesses.clear();
    m_running = false;
    m_stopRequested = false;
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

            const auto oldProcess = previous.find(entry.th32ProcessID);
            if (oldProcess != previous.end()) {
                // A PID cannot be reused while it is present in the current
                // snapshot. Keep its known image path and avoid opening every
                // process on every scan.
                current.emplace(oldProcess->first, oldProcess->second);
                continue;
            }

            std::wstring imagePath;
            if (QueryImagePath(entry.th32ProcessID, imagePath)) {
                imagePath = PathUtils::NormalizePath(imagePath);
                if (!imagePath.empty()) {
                    current.emplace(entry.th32ProcessID,
                                    ProcessInfo{entry.th32ProcessID, std::move(imagePath)});
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return current;
}

bool ProcessMonitor::QueryImagePath(DWORD pid, std::wstring& imagePath) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return false;
    }

    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, buffer.data(), &length);
    CloseHandle(process);
    if (!success || length == 0) {
        return false;
    }
    imagePath.assign(buffer.data(), length);
    return true;
}

void ProcessMonitor::Run() {
    ProcessMap previous;
    bool firstScan = true;

    while (true) {
        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested) {
                break;
            }
        }

        ProcessMap current = Enumerate(previous);
        std::vector<ProcessEvent> events;
        events.reserve(current.size() + previous.size());

        for (const auto& [pid, process] : current) {
            const auto oldProcess = previous.find(pid);
            if (oldProcess == previous.end()) {
                events.push_back({ProcessEventType::Started, process, firstScan});
            } else if (!PathUtils::SamePath(oldProcess->second.imagePath,
                                             process.imagePath)) {
                // This also handles a PID that was reused between two scans.
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
            m_currentProcesses = current;
        }

        Callback callback;
        {
            std::lock_guard lock(m_mutex);
            callback = m_callback;
        }
        if (callback) {
            for (const ProcessEvent& event : events) {
                callback(event);
            }
        }

        previous = std::move(current);
        firstScan = false;

        std::unique_lock lock(m_mutex);
        if (m_condition.wait_for(lock, kScanInterval,
                                 [this] { return m_stopRequested; })) {
            break;
        }
    }
}
