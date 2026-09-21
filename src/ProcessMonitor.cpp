#include "ProcessMonitor.h"

#include "PathUtils.h"

#include <windows.h>

#include <tlhelp32.h>

#include <algorithm>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kScanIntervalMs = 1000;

bool IsStopRequested(HANDLE stopEvent) {
    return stopEvent != nullptr && WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0;
}

ULONGLONG CurrentFileTime() {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return value.QuadPart;
}

bool StartedAfter(const ProcessInfo& process, ULONGLONG timestamp) {
    return timestamp != 0 && process.identity.creationTime >= timestamp;
}

}  // namespace

bool SameProcessInstance(const ProcessIdentity& left, const ProcessIdentity& right) noexcept {
    return left.pid == right.pid &&
           (left.creationTime == 0 || right.creationTime == 0 ||
            left.creationTime == right.creationTime);
}

ProcessMonitor::ProcessMonitor() = default;

ProcessMonitor::~ProcessMonitor() {
    Stop();
}

bool ProcessMonitor::Start(Callback callback, ScanCallback scanCallback) {
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
        m_scanCallback = std::move(scanCallback);
        m_stopEvent = stopEvent;
        m_readyEvent = readyEvent;
        m_stopRequested = false;
        m_running = true;
        m_hasSuccessfulScan = false;
        m_startedAt = CurrentFileTime();
    }

    try {
        m_thread = std::thread(&ProcessMonitor::Run, this);
    } catch (...) {
        std::lock_guard lock(m_mutex);
        m_callback = {};
        m_scanCallback = {};
        m_running = false;
        m_stopRequested = false;
        m_hasSuccessfulScan = false;
        m_startedAt = 0;
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
        m_scanCallback = {};
        m_running = false;
        m_stopRequested = false;
        m_hasSuccessfulScan = false;
        m_startedAt = 0;
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

bool ProcessMonitor::WaitUntilReady(DWORD timeoutMs) const {
    HANDLE readyEvent = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (m_readyEvent == nullptr ||
            !DuplicateHandle(GetCurrentProcess(), m_readyEvent, GetCurrentProcess(),
                             &readyEvent, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return false;
        }
    }
    const DWORD waitResult = WaitForSingleObject(readyEvent, timeoutMs);
    CloseHandle(readyEvent);
    if (waitResult != WAIT_OBJECT_0) {
        return false;
    }
    std::lock_guard lock(m_mutex);
    return m_hasSuccessfulScan && !m_stopRequested;
}

bool ProcessMonitor::IsProcessAlive(const ProcessInfo& process) {
    if (process.identity.pid == 0) {
        return false;
    }

    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                process.identity.pid);
    if (handle == nullptr) {
        return false;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    ProcessIdentity currentIdentity{process.identity.pid, 0};
    if (process.identity.creationTime != 0) {
        if (!GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
            CloseHandle(handle);
            return false;
        }
        ULARGE_INTEGER value{};
        value.LowPart = creation.dwLowDateTime;
        value.HighPart = creation.dwHighDateTime;
        currentIdentity.creationTime = value.QuadPart;
    }

    const DWORD waitResult = WaitForSingleObject(handle, 0);
    CloseHandle(handle);
    return SameProcessInstance(process.identity, currentIdentity) && waitResult == WAIT_TIMEOUT;
}

bool ProcessMonitor::QueryProcessInfo(DWORD pid, ProcessInfo& process, DWORD& error) {
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (handle == nullptr) {
        error = GetLastError();
        return false;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(handle, &creation, &exit, &kernel, &user)) {
        error = GetLastError();
        CloseHandle(handle);
        return false;
    }

    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    const BOOL success = QueryFullProcessImageNameW(handle, 0, buffer.data(), &length);
    const DWORD queryError = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!success || length == 0) {
        error = success ? ERROR_INVALID_DATA : queryError;
        return false;
    }

    std::wstring imagePath = PathUtils::NormalizePath(std::wstring(buffer.data(), length));
    if (imagePath.empty()) {
        error = ERROR_INVALID_DATA;
        return false;
    }

    ULARGE_INTEGER creationValue{};
    creationValue.LowPart = creation.dwLowDateTime;
    creationValue.HighPart = creation.dwHighDateTime;
    process = {{pid, creationValue.QuadPart}, std::move(imagePath)};
    error = ERROR_SUCCESS;
    return true;
}

bool ProcessMonitor::SameScanCondition(const ProcessScanResult& left,
                                       const ProcessScanResult& right) {
    if (left.status != right.status || left.error != right.error ||
        left.queryFailures.size() != right.queryFailures.size()) {
        return false;
    }

    std::vector<ProcessQueryFailure> leftFailures = left.queryFailures;
    std::vector<ProcessQueryFailure> rightFailures = right.queryFailures;
    const auto byPid = [](const ProcessQueryFailure& first, const ProcessQueryFailure& second) {
        return first.pid < second.pid ||
               (first.pid == second.pid && first.error < second.error);
    };
    std::sort(leftFailures.begin(), leftFailures.end(), byPid);
    std::sort(rightFailures.begin(), rightFailures.end(), byPid);
    for (std::size_t i = 0; i < leftFailures.size(); ++i) {
        if (leftFailures[i].pid != rightFailures[i].pid ||
            leftFailures[i].error != rightFailures[i].error) {
            return false;
        }
    }
    return true;
}

ProcessScanResult ProcessMonitor::Enumerate() {
    ProcessScanResult result;
    const DWORD ownPid = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        result.status = ProcessScanStatus::Failed;
        result.error = GetLastError();
        return result;
    }

    std::vector<DWORD> pids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    SetLastError(ERROR_SUCCESS);
    BOOL hasEntry = Process32FirstW(snapshot, &entry);
    DWORD enumerationError = hasEntry ? ERROR_SUCCESS : GetLastError();
    while (hasEntry) {
        if (entry.th32ProcessID != ownPid && entry.th32ProcessID != 0) {
            pids.push_back(entry.th32ProcessID);
        }
        SetLastError(ERROR_SUCCESS);
        hasEntry = Process32NextW(snapshot, &entry);
        if (!hasEntry) {
            enumerationError = GetLastError();
        }
    }

    if (enumerationError != ERROR_NO_MORE_FILES) {
        CloseHandle(snapshot);
        result.status = ProcessScanStatus::Failed;
        result.error = enumerationError == ERROR_SUCCESS ? ERROR_GEN_FAILURE
                                                          : enumerationError;
        return result;
    }
    CloseHandle(snapshot);

    result.processes.reserve(pids.size());
    result.queryFailures.reserve(pids.size());
    for (const DWORD pid : pids) {
        ProcessInfo process;
        DWORD error = ERROR_SUCCESS;
        if (QueryProcessInfo(pid, process, error)) {
            result.processes.push_back(std::move(process));
        } else {
            result.queryFailures.push_back({pid, error});
        }
    }

    result.status = result.queryFailures.empty() ? ProcessScanStatus::Complete
                                                 : ProcessScanStatus::Partial;
    return result;
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

void ProcessMonitor::PublishScan(const ProcessScanResult& scan, bool conditionChanged) {
    ScanCallback callback;
    {
        std::lock_guard lock(m_mutex);
        if (m_stopRequested) {
            return;
        }
        callback = m_scanCallback;
    }
    if (callback) {
        callback(scan, conditionChanged);
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
    ULONGLONG startedAt = 0;
    {
        std::lock_guard lock(m_mutex);
        startedAt = m_startedAt;
    }
    ProcessMap previous;
    bool hasInitialSnapshot = false;
    std::unordered_set<DWORD> unresolvedPids;
    ProcessScanResult previousScan;

    while (!IsStopRequested(m_stopEvent)) {
        ProcessScanResult scan = Enumerate();
        const bool scanConditionChanged = !SameScanCondition(previousScan, scan);
        std::vector<ProcessEvent> events;

        if (scan.status != ProcessScanStatus::Failed) {
            ProcessMap current;
            std::unordered_set<DWORD> enumeratedPids;
            current.reserve(scan.processes.size() + scan.queryFailures.size());
            enumeratedPids.reserve(scan.processes.size() + scan.queryFailures.size());

            for (const ProcessInfo& process : scan.processes) {
                enumeratedPids.insert(process.identity.pid);
                const bool wasUnresolved = unresolvedPids.erase(process.identity.pid) != 0;
                const auto oldProcess = previous.find(process.identity.pid);
                if (oldProcess == previous.end()) {
                    const bool mayBeInitial = !hasInitialSnapshot || wasUnresolved;
                    const bool initialProcess = mayBeInitial && !StartedAfter(process, startedAt);
                    events.push_back({ProcessEventType::Started, process, initialProcess});
                } else if (!SameProcessInstance(oldProcess->second.identity, process.identity) ||
                           !PathUtils::SamePath(oldProcess->second.imagePath,
                                                process.imagePath)) {
                    events.push_back({ProcessEventType::Exited, oldProcess->second, false});
                    events.push_back({ProcessEventType::Started, process,
                                      wasUnresolved && !StartedAfter(process, startedAt)});
                }
                current.emplace(process.identity.pid, process);
            }

            for (const ProcessQueryFailure& failure : scan.queryFailures) {
                enumeratedPids.insert(failure.pid);
                const auto oldProcess = previous.find(failure.pid);
                if (oldProcess != previous.end()) {
                    // Keep the last verified identity as the comparison baseline,
                    // but do not publish it as a current confirmation.
                    current.emplace(failure.pid, oldProcess->second);
                }
                unresolvedPids.insert(failure.pid);
            }

            for (auto pid = unresolvedPids.begin(); pid != unresolvedPids.end();) {
                if (enumeratedPids.find(*pid) == enumeratedPids.end()) {
                    pid = unresolvedPids.erase(pid);
                } else {
                    ++pid;
                }
            }

            for (const auto& [pid, process] : previous) {
                if (enumeratedPids.find(pid) == enumeratedPids.end()) {
                    events.push_back({ProcessEventType::Exited, process, false});
                }
            }

            previous = current;
            hasInitialSnapshot = true;
        }

        {
            std::lock_guard lock(m_mutex);
            if (m_stopRequested) {
                break;
            }
            if (scan.status != ProcessScanStatus::Failed) {
                m_hasSuccessfulScan = true;
            }
        }
        previousScan = scan;
        for (const ProcessEvent& event : events) {
            Publish(event);
        }
        PublishScan(scan, scanConditionChanged);

        if (hasInitialSnapshot) {
            SignalReady();
        }

        if (WaitForSingleObject(m_stopEvent, kScanIntervalMs) == WAIT_OBJECT_0) {
            break;
        }
    }
}
