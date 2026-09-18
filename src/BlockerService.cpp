#include "BlockerService.h"

#include "Logger.h"
#include "PathUtils.h"

#include <algorithm>
#include <windows.h>
#include <utility>

namespace {

bool RuleTargetExists(const AppRule& rule) {
    const DWORD attributes = GetFileAttributesW(rule.path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if (rule.kind == RuleKind::Directory) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace

BlockerService::BlockerService(Logger* logger) : m_logger(logger) {}

BlockerService::~BlockerService() {
    Stop();
}

bool BlockerService::Start(const std::vector<AppRule>& rules) {
    Stop();

    {
        std::lock_guard lock(m_mutex);
        m_runtimeRules.clear();
        m_runtimeRules.reserve(rules.size());
        for (const AppRule& rule : rules) {
            m_runtimeRules.push_back({rule, {}});
        }
        m_serviceError.clear();
        m_started = true;
    }

    {
        std::lock_guard lock(m_injectionMutex);
        m_injectionQueue.clear();
        m_injectionStopRequested = false;
    }

    std::wstring policyError;
    if (!m_policyRegistry.Start(policyError)) {
        std::lock_guard lock(m_mutex);
        m_started = false;
        m_runtimeRules.clear();
        m_serviceError = policyError.empty() ? L"快捷键策略共享内存启动失败" : policyError;
        Log(m_serviceError);
        return false;
    }

    try {
        m_injectionThread = std::thread(&BlockerService::RunInjectionWorker, this);
    } catch (...) {
        m_policyRegistry.Stop();
        std::lock_guard lock(m_mutex);
        m_started = false;
        m_runtimeRules.clear();
        m_serviceError = L"注入线程启动失败";
        Log(m_serviceError);
        return false;
    }

    if (!m_monitor.Start([this](const ProcessEvent& event) { OnProcessEvent(event); })) {
        StopInjectionWorker();
        m_policyRegistry.Stop();
        {
            std::lock_guard lock(m_mutex);
            m_started = false;
            m_serviceError = L"进程监控启动失败";
        }
        Log(L"进程监控启动失败");
        return false;
    }

    Log(L"进程监控已启动");
    return true;
}

void BlockerService::StopInjectionWorker() {
    {
        std::lock_guard lock(m_injectionMutex);
        m_injectionStopRequested = true;
        m_injectionQueue.clear();
    }
    m_injectionCondition.notify_all();
    if (m_injectionThread.joinable()) {
        m_injectionThread.join();
    }
}

void BlockerService::Stop() {
    m_monitor.Stop();
    StopInjectionWorker();
    m_policyRegistry.Stop();

    bool wasStarted = false;
    {
        std::lock_guard lock(m_mutex);
        wasStarted = m_started;
        m_started = false;
        m_runtimeRules.clear();
        m_serviceError.clear();
    }
    if (wasStarted) {
        Log(L"进程监控已停止");
    }
}

void BlockerService::SetStateChangedCallback(StateChangedCallback callback) {
    std::lock_guard lock(m_mutex);
    m_stateChangedCallback = std::move(callback);
}

void BlockerService::UpdateRules(const std::vector<AppRule>& rules) {
    const std::vector<ProcessInfo> runningProcesses = m_monitor.Snapshot();

    {
        std::lock_guard lock(m_mutex);
        std::vector<RuntimeRule> updatedRules;
        updatedRules.reserve(rules.size());
        for (const AppRule& rule : rules) {
            RuntimeRule runtime{rule, {}};
            const auto oldRule = std::find_if(
                m_runtimeRules.begin(), m_runtimeRules.end(), [&rule](const RuntimeRule& old) {
                    return PathUtils::SamePath(old.rule.path, rule.path);
                });
            if (oldRule != m_runtimeRules.end()) {
                runtime.processes = oldRule->processes;
                if (!SameHotkeyPolicy(oldRule->rule.hotkeyPolicy, rule.hotkeyPolicy)) {
                    for (TrackedProcess& process : runtime.processes) {
                        process.protection = ProcessProtection::Observed;
                        process.detail = L"快捷键策略已变更，请重启应用";
                    }
                }
            }
            updatedRules.push_back(std::move(runtime));
        }

        for (const ProcessInfo& process : runningProcesses) {
            const auto rule = std::find_if(
                updatedRules.begin(), updatedRules.end(), [&process](const RuntimeRule& item) {
                    return PathUtils::Matches(item.rule, process.imagePath);
                });
            if (rule == updatedRules.end()) {
                continue;
            }
            const auto existing = std::find_if(
                rule->processes.begin(), rule->processes.end(), [&process](const TrackedProcess& item) {
                    return SameProcess(item, process);
                });
            if (existing == rule->processes.end()) {
                rule->processes.push_back({process.pid, process.creationTime,
                                           ProcessProtection::Observed,
                                           L"目标进程已经运行，请重启应用"});
            }
        }
        m_runtimeRules = std::move(updatedRules);
    }
    NotifyStateChanged();
}

std::vector<RuntimeRuleState> BlockerService::Snapshot() const {
    std::lock_guard lock(m_mutex);
    std::vector<RuntimeRuleState> result;
    result.reserve(m_runtimeRules.size());
    for (const RuntimeRule& runtime : m_runtimeRules) {
        RuntimeRuleState state;
        state.path = runtime.rule.path;
        if (!m_serviceError.empty() && !m_started) {
            state.status = AppStatus::MonitoringUnavailable;
            state.detail = m_serviceError;
        } else {
            state.status = StateForRule(runtime.rule, runtime.processes);
            state.detail = DetailForRule(state.status, runtime.processes);
        }
        result.push_back(std::move(state));
    }
    return result;
}

bool BlockerService::WaitUntilReady(DWORD timeoutMs) const {
    return m_monitor.WaitUntilReady(timeoutMs);
}

void BlockerService::OnProcessEvent(const ProcessEvent& event) {
    if (event.type == ProcessEventType::Exited) {
        bool removed = false;
        {
            std::lock_guard lock(m_mutex);
            for (RuntimeRule& runtime : m_runtimeRules) {
                const auto oldSize = runtime.processes.size();
                runtime.processes.erase(
                    std::remove_if(runtime.processes.begin(), runtime.processes.end(),
                                   [&event](const TrackedProcess& process) {
                                       return SameProcess(process, event.process);
                                   }),
                    runtime.processes.end());
                removed = removed || oldSize != runtime.processes.size();
            }
        }
        m_policyRegistry.Remove(event.process.pid);
        if (removed) {
            Log(L"目标进程退出 PID=" + std::to_wstring(event.process.pid));
            NotifyStateChanged();
        }
        return;
    }

    AppRule rule;
    bool shouldInject = false;
    {
        std::lock_guard lock(m_mutex);
        const int ruleIndex = FindRuleIndexLocked(event.process.imagePath);
        if (ruleIndex < 0) {
            return;
        }

        for (RuntimeRule& runtime : m_runtimeRules) {
            runtime.processes.erase(
                std::remove_if(runtime.processes.begin(), runtime.processes.end(),
                               [&event](const TrackedProcess& process) {
                                   return process.pid == event.process.pid &&
                                          !SameProcess(process, event.process);
                               }),
                runtime.processes.end());
        }

        RuntimeRule& runtime = m_runtimeRules[static_cast<std::size_t>(ruleIndex)];
        rule = runtime.rule;
        const auto existing = std::find_if(
            runtime.processes.begin(), runtime.processes.end(), [&event](const TrackedProcess& process) {
                return SameProcess(process, event.process);
            });
        if (existing != runtime.processes.end()) {
            return;
        }
        runtime.processes.push_back({event.process.pid, event.process.creationTime,
                                     event.initialScan ? ProcessProtection::Observed
                                                       : ProcessProtection::Pending,
                                     event.initialScan ? L"目标进程已经运行，请重启应用" : L""});
        shouldInject = rule.enabled && !event.initialScan;
    }

    Log(L"发现目标进程 PID=" + std::to_wstring(event.process.pid) + L"：" +
        event.process.imagePath);
    NotifyStateChanged();
    if (shouldInject) {
        QueueInjection(event);
    }
}

void BlockerService::QueueInjection(const ProcessEvent& event) {
    {
        std::lock_guard lock(m_injectionMutex);
        if (m_injectionStopRequested) {
            return;
        }
        m_injectionQueue.push_back(event);
    }
    m_injectionCondition.notify_one();
}

void BlockerService::RunInjectionWorker() {
    for (;;) {
        ProcessEvent event;
        {
            std::unique_lock lock(m_injectionMutex);
            m_injectionCondition.wait(lock, [this] {
                return m_injectionStopRequested || !m_injectionQueue.empty();
            });
            if (m_injectionStopRequested) {
                return;
            }
            event = std::move(m_injectionQueue.front());
            m_injectionQueue.pop_front();
        }

        bool enabled = false;
        HotkeyPolicy policy;
        {
            std::lock_guard lock(m_mutex);
            const int ruleIndex = FindRuleIndexLocked(event.process.imagePath);
            if (ruleIndex >= 0) {
                const RuntimeRule& runtime =
                    m_runtimeRules[static_cast<std::size_t>(ruleIndex)];
                enabled = runtime.rule.enabled && m_started && m_serviceError.empty();
                policy = runtime.rule.hotkeyPolicy;
            }
        }
        if (!enabled || !ProcessMonitor::IsProcessAlive(event.process)) {
            continue;
        }

        std::wstring policyError;
        if (!m_policyRegistry.Publish(event.process.pid, policy, policyError)) {
            InjectionResult injection;
            injection.error = policyError.empty() ? L"发布快捷键策略失败" : policyError;
            ApplyInjectionResult(event, injection);
            continue;
        }

        Log(L"开始注入 PID=" + std::to_wstring(event.process.pid));
        const InjectionResult injection = m_injector.Inject(event.process.pid);
        if (!injection.success) {
            m_policyRegistry.Remove(event.process.pid);
        }
        ApplyInjectionResult(event, injection);
    }
}

void BlockerService::ApplyInjectionResult(const ProcessEvent& event,
                                           const InjectionResult& injection) {
    std::wstring logMessage;
    {
        std::lock_guard lock(m_mutex);
        const int ruleIndex = FindRuleIndexLocked(event.process.imagePath);
        if (ruleIndex < 0) {
            return;
        }

        auto& processes = m_runtimeRules[static_cast<std::size_t>(ruleIndex)].processes;
        const auto existing = std::find_if(
            processes.begin(), processes.end(), [&event](const TrackedProcess& process) {
                return SameProcess(process, event.process);
            });
        if (existing == processes.end()) {
            return;
        }

        if (injection.success) {
            existing->protection = ProcessProtection::Blocked;
            existing->detail = L"PID=" + std::to_wstring(event.process.pid) + L"（" +
                               ArchitectureName(injection.architecture) + L"）";
            logMessage = L"注入成功 PID=" + std::to_wstring(event.process.pid);
        } else {
            existing->protection = ProcessProtection::Failed;
            existing->detail = injection.error.empty() ? L"未知注入错误" : injection.error;
            logMessage = L"注入失败 PID=" + std::to_wstring(event.process.pid) + L"：" +
                         existing->detail;
        }
    }
    Log(logMessage);
    NotifyStateChanged();
}

int BlockerService::FindRuleIndexLocked(const std::wstring& path) const {
    for (std::size_t index = 0; index < m_runtimeRules.size(); ++index) {
        if (PathUtils::Matches(m_runtimeRules[index].rule, path)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void BlockerService::NotifyStateChanged() const {
    StateChangedCallback callback;
    {
        std::lock_guard lock(m_mutex);
        callback = m_stateChangedCallback;
    }
    if (callback) {
        callback();
    }
}

bool BlockerService::SameProcess(const TrackedProcess& tracked, const ProcessInfo& process) {
    if (tracked.pid != process.pid) {
        return false;
    }
    return tracked.creationTime == 0 || process.creationTime == 0 ||
           tracked.creationTime == process.creationTime;
}

AppStatus BlockerService::StateForRule(
    const AppRule& rule, const std::vector<TrackedProcess>& processes) {
    if (!rule.enabled) {
        return AppStatus::Disabled;
    }

    if (processes.empty()) {
        return RuleTargetExists(rule) ? AppStatus::Waiting : AppStatus::PathMissing;
    }

    const std::size_t blockedCount = static_cast<std::size_t>(std::count_if(
        processes.begin(), processes.end(), [](const TrackedProcess& process) {
            return process.protection == ProcessProtection::Blocked;
        }));
    const bool hasPending = std::any_of(
        processes.begin(), processes.end(), [](const TrackedProcess& process) {
            return process.protection == ProcessProtection::Pending;
        });
    const bool hasObserved = std::any_of(
        processes.begin(), processes.end(), [](const TrackedProcess& process) {
            return process.protection == ProcessProtection::Observed;
        });
    const bool hasFailed = std::any_of(
        processes.begin(), processes.end(), [](const TrackedProcess& process) {
            return process.protection == ProcessProtection::Failed;
        });

    if (blockedCount == processes.size()) {
        return AppStatus::Blocked;
    }
    if (blockedCount > 0) {
        return AppStatus::PartiallyBlocked;
    }
    if (hasFailed) {
        return AppStatus::InjectionFailed;
    }
    if (hasPending) {
        return AppStatus::Injecting;
    }
    if (hasObserved) {
        return AppStatus::RestartRequired;
    }
    return AppStatus::Waiting;
}

std::wstring BlockerService::DetailForRule(
    AppStatus status, const std::vector<TrackedProcess>& processes) {
    if (status == AppStatus::Disabled) {
        return processes.empty() ? std::wstring{} : L"已停用；已运行进程需重启后解除拦截";
    }
    if (status == AppStatus::PathMissing) {
        return L"目标程序路径不存在，请重新定位或删除规则";
    }
    if (status == AppStatus::Injecting) {
        return L"正在处理 " + std::to_wstring(processes.size()) + L" 个运行进程";
    }
    if (status == AppStatus::RestartRequired) {
        return L"有 " + std::to_wstring(processes.size()) + L" 个进程已在运行，请重启应用";
    }
    if (status == AppStatus::InjectionFailed) {
        const auto failed = std::find_if(
            processes.begin(), processes.end(), [](const TrackedProcess& process) {
                return process.protection == ProcessProtection::Failed;
            });
        return failed == processes.end() ? L"注入失败" : failed->detail;
    }
    if (status == AppStatus::Blocked) {
        std::wstring result;
        for (const TrackedProcess& process : processes) {
            if (process.protection != ProcessProtection::Blocked) {
                continue;
            }
            if (!result.empty()) {
                result += L"；";
            }
            result += process.detail;
        }
        return result;
    }
    if (status == AppStatus::PartiallyBlocked) {
        std::size_t blockedCount = 0;
        std::size_t failedCount = 0;
        for (const TrackedProcess& process : processes) {
            blockedCount += process.protection == ProcessProtection::Blocked ? 1u : 0u;
            failedCount += process.protection == ProcessProtection::Failed ? 1u : 0u;
        }
        std::wstring result = L"已拦截 " + std::to_wstring(blockedCount) + L"/" +
                              std::to_wstring(processes.size()) + L" 个进程";
        if (failedCount != 0) {
            result += L"，失败 " + std::to_wstring(failedCount) + L" 个";
        }
        return result;
    }
    return {};
}

void BlockerService::Log(const std::wstring& message) const {
    if (m_logger != nullptr && !message.empty()) {
        m_logger->Info(message);
    }
}

const wchar_t* AppStatusText(AppStatus status) {
    switch (status) {
        case AppStatus::Waiting:
            return L"等待程序启动";
        case AppStatus::Injecting:
            return L"正在启用拦截";
        case AppStatus::RestartRequired:
            return L"重启程序后生效";
        case AppStatus::Blocked:
            return L"拦截已生效";
        case AppStatus::PartiallyBlocked:
            return L"部分拦截生效";
        case AppStatus::InjectionFailed:
            return L"启用拦截失败";
        case AppStatus::PathMissing:
            return L"程序路径不存在";
        case AppStatus::Disabled:
            return L"已停用";
        case AppStatus::MonitoringUnavailable:
            return L"进程监控不可用";
        default:
            return L"未知状态";
    }
}
