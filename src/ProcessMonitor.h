#pragma once

#include "ProcessIdentity.h"

#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

bool SameProcessInstance(const ProcessIdentity& left, const ProcessIdentity& right) noexcept;

struct ProcessInfo {
    ProcessIdentity identity;
    std::wstring imagePath;
};

struct ProcessQueryFailure {
    DWORD pid = 0;
    DWORD error = ERROR_SUCCESS;
};

enum class ProcessScanStatus {
    NotStarted,
    Complete,
    Partial,
    Failed,
};

struct ProcessScanResult {
    // `error` is set when the process table could not be read as a whole.
    // Per-process query failures are reported separately below.
    ProcessScanStatus status = ProcessScanStatus::NotStarted;
    DWORD error = ERROR_SUCCESS;
    // Only process identities queried successfully during this scan.
    std::vector<ProcessInfo> processes;
    std::vector<ProcessQueryFailure> queryFailures;
};

enum class ProcessEventType {
    Started,
    Exited,
};

struct ProcessEvent {
    ProcessEventType type = ProcessEventType::Started;
    ProcessInfo process;
    bool initialScan = false;
};

class ProcessMonitor final {
public:
    using Callback = std::function<void(const ProcessEvent&)>;
    // Runs after the scan's process events; the flag reports changes to scan health.
    using ScanCallback = std::function<void(const ProcessScanResult&, bool)>;

    ProcessMonitor();
    ~ProcessMonitor();

    ProcessMonitor(const ProcessMonitor&) = delete;
    ProcessMonitor& operator=(const ProcessMonitor&) = delete;

    bool Start(Callback callback, ScanCallback scanCallback = {});
    void Stop();
    bool WaitUntilReady(DWORD timeoutMs) const;

    // Used immediately before injection and when processing delayed exit
    // notifications. A creation timestamp prevents a reused PID from being
    // mistaken for the original process.
    static bool IsProcessAlive(const ProcessInfo& process);

private:
    using ProcessMap = std::unordered_map<DWORD, ProcessInfo>;

    static ProcessScanResult Enumerate();
    static bool QueryProcessInfo(DWORD pid, ProcessInfo& process, DWORD& error);
    static bool SameScanCondition(const ProcessScanResult& left,
                                  const ProcessScanResult& right);

    void Run();
    void Publish(const ProcessEvent& event);
    void PublishScan(const ProcessScanResult& scan, bool conditionChanged);
    void SignalReady();

    mutable std::mutex m_mutex;
    Callback m_callback;
    ScanCallback m_scanCallback;
    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_readyEvent = nullptr;
    bool m_stopRequested = false;
    bool m_running = false;
    bool m_hasSuccessfulScan = false;
    ULONGLONG m_startedAt = 0;
};
