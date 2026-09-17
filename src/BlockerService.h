#pragma once

#include "ConfigStore.h"
#include "Injector.h"
#include "ProcessMonitor.h"

#include <mutex>
#include <string>
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
    explicit BlockerService(Logger* logger = nullptr);
    ~BlockerService();

    BlockerService(const BlockerService&) = delete;
    BlockerService& operator=(const BlockerService&) = delete;

    bool Start(const std::vector<AppRule>& rules);
    void Stop();
    void UpdateRules(const std::vector<AppRule>& rules);
    std::vector<RuntimeRuleState> Snapshot() const;

private:
    enum class ProcessProtection {
        Observed,
        Blocked,
        Failed,
    };

    struct TrackedProcess {
        DWORD pid = 0;
        ProcessProtection protection = ProcessProtection::Observed;
        std::wstring detail;
    };

    void OnProcessEvent(const ProcessEvent& event);
    void RebuildStatesLocked();
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
    std::vector<std::vector<TrackedProcess>> m_processes;
    std::vector<RuntimeRuleState> m_states;
    bool m_started = false;
};

const wchar_t* AppStatusText(AppStatus status);
