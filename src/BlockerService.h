#pragma once

#include "ConfigStore.h"
#include "Injector.h"
#include "HotkeyPolicyTransport.h"
#include "ProcessMonitor.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class AppStatus {
    Waiting,
    Injecting,
    InjectionPending,
    RestartRequired,
    Blocked,
    PartiallyBlocked,
    InjectionFailed,
    PathMissing,
    Disabled,
    MonitoringUnavailable,
};

struct RuntimeRuleState {
    std::wstring path;
    AppStatus status = AppStatus::Waiting;
    std::wstring detail;
};

enum class BlockerServiceFailureStage {
    None,
    PolicyRegistry,
    InjectionWorker,
    ProcessMonitor,
};

struct BlockerServiceStartResult {
    BlockerServiceFailureStage failureStage = BlockerServiceFailureStage::None;
    std::wstring error;

    explicit operator bool() const noexcept {
        return failureStage == BlockerServiceFailureStage::None;
    }
};

class Logger;

class BlockerService final {
public:
    using StateChangedCallback = std::function<void()>;

    explicit BlockerService(Logger* logger = nullptr);
    ~BlockerService();

    BlockerService(const BlockerService&) = delete;
    BlockerService& operator=(const BlockerService&) = delete;

    BlockerServiceStartResult Start(const std::vector<AppRule>& rules);
    void Stop();
    void UpdateRules(const std::vector<AppRule>& rules);
    std::vector<RuntimeRuleState> Snapshot() const;
    bool WaitUntilReady(DWORD timeoutMs) const;
    void SetStateChangedCallback(StateChangedCallback callback);

private:
    enum class ProcessProtection {
        Observed,
        Queued,
        Injecting,
        Pending,
        Blocked,
        Failed,
    };

    struct TrackedProcess {
        ProcessIdentity identity;
        ProcessProtection protection = ProcessProtection::Observed;
        std::uint64_t operationRevision = 0;
        std::wstring detail;
    };

    struct RuntimeRule {
        AppRule rule;
        std::vector<TrackedProcess> processes;
        std::uint64_t revision = 1;
    };

    struct InjectionRequest {
        ProcessInfo process;
        std::wstring rulePath;
        std::uint64_t ruleRevision = 0;
    };

    struct PendingInjection {
        InjectionRequest request;
        ProcessArchitecture architecture = ProcessArchitecture::Unknown;
        std::shared_ptr<InjectionOperation> operation;
        std::shared_ptr<void> policyMappingLifetime;
    };

    enum class LifecyclePhase {
        Stopped,
        Running,
        Failed,
    };

    struct LifecycleState {
        LifecyclePhase phase = LifecyclePhase::Stopped;
        std::wstring error;
    };

    void OnProcessEvent(const ProcessEvent& event);
    void OnScanChanged(const ProcessScanResult& scan, bool conditionChanged);
    void RecordProcessEventLocked(const ProcessEvent& event);
    void RunInjectionWorker();
    void StopInjectionWorker();
    void QueueInjection(InjectionRequest request);
    void ApplyInjectionResult(const InjectionRequest& request,
                              const InjectionResult& injection);
    BlockerServiceStartResult FailStart(BlockerServiceFailureStage stage,
                                        std::wstring error);
    std::uint64_t NextRuleRevisionLocked();
    static bool ReconcileProcesses(const std::vector<ProcessInfo>& processes,
                                   std::vector<RuntimeRule>& rules);
    int FindRuleIndexLocked(const std::wstring& path) const;
    void NotifyStateChanged() const;
    static AppStatus StateForRule(const AppRule& rule,
                                  const std::vector<TrackedProcess>& processes);
    static std::wstring DetailForRule(AppStatus status,
                                      const std::vector<TrackedProcess>& processes);
    void Log(const std::wstring& message) const;

    mutable std::mutex m_mutex;
    ProcessMonitor m_monitor;
    Injector m_injector;
    Logger* m_logger = nullptr;
    HotkeyPolicyRegistry m_policyRegistry;
    std::vector<RuntimeRule> m_runtimeRules;
    ProcessScanResult m_processScan;
    std::uint64_t m_nextRuleRevision = 1;
    StateChangedCallback m_stateChangedCallback;
    LifecycleState m_lifecycle;

    mutable std::mutex m_injectionMutex;
    std::condition_variable m_injectionCondition;
    std::deque<InjectionRequest> m_injectionQueue;
    std::thread m_injectionThread;
    bool m_injectionStopRequested = false;
};

const wchar_t* AppStatusText(AppStatus status);
