#pragma once

#include <windows.h>

#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring imagePath;
    ULONGLONG creationTime = 0;
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

    ProcessMonitor();
    ~ProcessMonitor();

    ProcessMonitor(const ProcessMonitor&) = delete;
    ProcessMonitor& operator=(const ProcessMonitor&) = delete;

    bool Start(Callback callback);
    void Stop();
    std::vector<ProcessInfo> Snapshot() const;
    bool WaitUntilReady(DWORD timeoutMs) const;

    // Used immediately before injection and when processing delayed exit
    // notifications. A creation timestamp prevents a reused PID from being
    // mistaken for the original process.
    static bool IsProcessAlive(const ProcessInfo& process);

private:
    using ProcessMap = std::unordered_map<DWORD, ProcessInfo>;

    static ProcessMap Enumerate(const ProcessMap& previous);
    static bool QueryProcessInfo(DWORD pid, ProcessInfo& process);
    static bool SameProcess(const ProcessInfo& left, const ProcessInfo& right);

    void Run();
    void Publish(const ProcessEvent& event);
    void SignalReady();

    mutable std::mutex m_mutex;
    Callback m_callback;
    ProcessMap m_currentProcesses;
    std::thread m_thread;
    HANDLE m_stopEvent = nullptr;
    HANDLE m_readyEvent = nullptr;
    bool m_stopRequested = false;
    bool m_running = false;
};
