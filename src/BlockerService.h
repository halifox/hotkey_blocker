#pragma once

#include "ConfigStore.h"
#include "Injector.h"
#include "ProcessMonitor.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class AppStatus {
    Waiting,
    RestartRequired,
    Blocked,
    InjectionFailed,
    Disabled,
};

struct RuntimeRuleState {
    AppRule rule;
    AppStatus status = AppStatus::Waiting;
    std::wstring detail;
};

class Logger;

class BlockerService final {
public:
    using StateChangedCallback = std::function<void()>;

    explicit BlockerService(Logger* logger = nullptr);
    ~BlockerService();

    BlockerService(const BlockerService&) = delete;
    BlockerService& operator=(const BlockerService&) = delete;

    bool Start(const std::vector<AppRule>& rules);
    void Stop();
    void UpdateRules(const std::vector<AppRule>& rules);
    std::vector<RuntimeRuleState> Snapshot() const;
    bool WaitUntilReady(DWORD timeoutMs) const;
    void SetStateChangedCallback(StateChangedCallback callback);

private:
    enum class ProcessProtection {
        Observed,
        Blocked,
        Failed,
    };

    struct TrackedProcess {
        DWORD pid = 0;
        ULONGLONG creationTime = 0;
        ProcessProtection protection = ProcessProtection::Observed;
        std::wstring detail;
    };

    struct PathLess {
        bool operator()(const std::wstring& left, const std::wstring& right) const {
            return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
                   CSTR_LESS_THAN;
        }
    };

    void OnProcessEvent(const ProcessEvent& event);
    void RunInjectionWorker();
    void QueueInjection(const ProcessEvent& event);
    void ApplyInjectionResult(const ProcessEvent& event, const InjectionResult& injection);
    void RebuildStatesLocked();
    void RebuildRuleIndexLocked();
    int FindRuleIndexLocked(const std::wstring& path) const;
    void NotifyStateChanged() const;
    static bool SameProcess(const TrackedProcess& tracked, const ProcessInfo& process);
    static AppStatus StateForRule(const AppRule& rule,
                                  const std::vector<TrackedProcess>& processes);
    static std::wstring DetailForRule(AppStatus status,
                                      const std::vector<TrackedProcess>& processes);
    void Log(const std::wstring& message) const;

    mutable std::mutex m_mutex;
    ProcessMonitor m_monitor;
    Injector m_injector;
    Logger* m_logger = nullptr;
    std::vector<AppRule> m_rules;
    std::map<std::wstring, std::size_t, PathLess> m_ruleIndices;
    std::vector<std::vector<TrackedProcess>> m_processes;
    std::vector<RuntimeRuleState> m_states;
    StateChangedCallback m_stateChangedCallback;
    bool m_started = false;

    mutable std::mutex m_injectionMutex;
    std::condition_variable m_injectionCondition;
    std::deque<ProcessEvent> m_injectionQueue;
    std::thread m_injectionThread;
    bool m_injectionStopRequested = false;
};

const wchar_t* AppStatusText(AppStatus status);
