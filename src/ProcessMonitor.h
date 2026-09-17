#pragma once

#include <windows.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct ProcessInfo {
    DWORD pid = 0;
    std::wstring imagePath;
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

private:
    using ProcessMap = std::unordered_map<DWORD, ProcessInfo>;

    static ProcessMap Enumerate(const ProcessMap& previous);
    static bool QueryImagePath(DWORD pid, std::wstring& imagePath);
    void Run();

    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    Callback m_callback;
    ProcessMap m_currentProcesses;
    std::thread m_thread;
    bool m_stopRequested = false;
    bool m_running = false;
};
