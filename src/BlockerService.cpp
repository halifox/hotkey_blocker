#include "BlockerService.h"

#include "Logger.h"
#include "InjectionOperationReaper.h"
#include "PathUtils.h"

#include <algorithm>
#include <chrono>
#include <windows.h>
#include <utility>

namespace {

constexpr std::chrono::milliseconds kPendingInjectionPollInterval(100);

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

BlockerServiceStartResult BlockerService::Start(const std::vector<AppRule>& rules) {
    Stop();

    {
        std::lock_guard lock(m_mutex);
        m_runtimeRules.clear();
        m_runtimeRules.reserve(rules.size());
        m_nextRuleRevision = 1;
        for (const AppRule& rule : rules) {
            m_runtimeRules.push_back({rule, {}, NextRuleRevisionLocked()});
        }
        m_processScan = {};
        m_lifecycle = {};
    }

    {
        std::lock_guard lock(m_injectionMutex);
        m_injectionQueue.clear();
        m_injectionStopRequested = false;
    }

    std::wstring policyError;
    if (!m_policyRegistry.Start(policyError)) {
        return FailStart(BlockerServiceFailureStage::PolicyRegistry,
                         policyError.empty() ? L"快捷键策略共享内存启动失败"
                                             : std::move(policyError));
    }

    try {
        m_injectionThread = std::thread(&BlockerService::RunInjectionWorker, this);
    } catch (...) {
        return FailStart(BlockerServiceFailureStage::InjectionWorker, L"注入线程启动失败");
    }

    {
        std::lock_guard lock(m_mutex);
        m_lifecycle.phase = LifecyclePhase::Running;
    }
    if (!m_monitor.Start(
            [this](const ProcessEvent& event) { OnProcessEvent(event); },
            [this](const ProcessScanResult& scan, bool conditionChanged) {
                OnScanChanged(scan, conditionChanged);
            })) {
        return FailStart(BlockerServiceFailureStage::ProcessMonitor, L"进程监控启动失败");
    }

    Log(L"进程监控已启动");
    NotifyStateChanged();
    return {};
}

BlockerServiceStartResult BlockerService::FailStart(BlockerServiceFailureStage stage,
                                                     std::wstring error) {
    m_monitor.Stop();
    StopInjectionWorker();
    m_policyRegistry.Stop();
    {
        std::lock_guard lock(m_mutex);
        m_lifecycle = {LifecyclePhase::Failed, error};
    }
    Log(error);
    NotifyStateChanged();
    return {stage, std::move(error)};
}

std::uint64_t BlockerService::NextRuleRevisionLocked() {
    const std::uint64_t revision = m_nextRuleRevision++;
    if (m_nextRuleRevision == 0) {
        m_nextRuleRevision = 1;
    }
    return revision;
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
        wasStarted = m_lifecycle.phase == LifecyclePhase::Running;
        m_runtimeRules.clear();
        m_processScan = {};
        m_lifecycle = {};
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
    {
        std::lock_guard lock(m_mutex);
        std::vector<RuntimeRule> updatedRules;
        updatedRules.reserve(rules.size());
        for (const AppRule& rule : rules) {
            RuntimeRule runtime{rule, {}, 0};
            const auto oldRule = std::find_if(
                m_runtimeRules.begin(), m_runtimeRules.end(), [&rule](const RuntimeRule& old) {
                    return PathUtils::SamePath(old.rule.path, rule.path);
                });
            if (oldRule != m_runtimeRules.end()) {
                runtime.processes = oldRule->processes;
                runtime.revision = oldRule->revision;
                const bool policyChanged =
                    !SameHotkeyPolicy(oldRule->rule.hotkeyPolicy, rule.hotkeyPolicy);
                const bool enabledChanged = oldRule->rule.enabled != rule.enabled;
                if (policyChanged || enabledChanged) {
                    runtime.revision = NextRuleRevisionLocked();
                }
                if (policyChanged) {
                    for (TrackedProcess& process : runtime.processes) {
                        if (process.protection != ProcessProtection::Injecting &&
                            process.protection != ProcessProtection::Pending) {
                            process.protection = ProcessProtection::Observed;
                            process.operationRevision = 0;
                            process.detail = L"快捷键策略已变更，请重启应用";
                        } else {
                            process.detail = L"规则策略已变更，当前注入仍在处理";
                        }
                    }
                } else if (enabledChanged) {
                    for (TrackedProcess& process : runtime.processes) {
                        if (process.protection == ProcessProtection::Queued) {
                            process.protection = ProcessProtection::Observed;
                            process.operationRevision = 0;
                            process.detail = L"规则状态已变更，请重启应用";
                        } else if (process.protection == ProcessProtection::Injecting ||
                                   process.protection == ProcessProtection::Pending) {
                            process.detail = L"规则状态已变更，当前注入仍在处理";
                        }
                    }
                }
            } else {
                runtime.revision = NextRuleRevisionLocked();
            }
            updatedRules.push_back(std::move(runtime));
        }

        ReconcileProcesses(m_processScan.processes, updatedRules);
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
        state.status = StateForRule(runtime.rule, runtime.processes);
        state.detail = DetailForRule(state.status, runtime.processes);
        const bool statusIndependentOfMonitoring = state.status == AppStatus::Disabled ||
                                                   state.status == AppStatus::PathMissing;
        if (!statusIndependentOfMonitoring && m_lifecycle.phase == LifecyclePhase::Failed) {
            state.status = AppStatus::MonitoringUnavailable;
            state.detail = m_lifecycle.error;
        } else if (!statusIndependentOfMonitoring &&
                   m_processScan.status == ProcessScanStatus::Failed) {
            state.status = AppStatus::MonitoringUnavailable;
            state.detail = L"进程快照失败，监控正在重试（错误码 " +
                           std::to_wstring(m_processScan.error) + L"）";
        } else if (!statusIndependentOfMonitoring &&
                   m_processScan.status == ProcessScanStatus::Partial) {
            if (!state.detail.empty()) {
                state.detail += L"；";
            }
            state.detail += L"有 " + std::to_wstring(m_processScan.queryFailures.size()) +
                            L" 个进程暂时无法查询，状态可能不完整";
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
            RecordProcessEventLocked(event);
            for (RuntimeRule& runtime : m_runtimeRules) {
                const auto oldSize = runtime.processes.size();
                runtime.processes.erase(
                    std::remove_if(runtime.processes.begin(), runtime.processes.end(),
                                   [&event](const TrackedProcess& process) {
                                       return SameProcessInstance(process.identity,
                                                                  event.process.identity);
                                   }),
                    runtime.processes.end());
                removed = removed || oldSize != runtime.processes.size();
            }
        }
        m_policyRegistry.Remove(event.process.identity);
        if (removed) {
            Log(L"目标进程退出 PID=" + std::to_wstring(event.process.identity.pid));
            NotifyStateChanged();
        }
        return;
    }

    InjectionRequest request;
    bool shouldInject = false;
    {
        std::lock_guard lock(m_mutex);
        RecordProcessEventLocked(event);
        const int ruleIndex = FindRuleIndexLocked(event.process.imagePath);
        if (ruleIndex < 0) {
            return;
        }

        for (RuntimeRule& runtime : m_runtimeRules) {
            runtime.processes.erase(
                std::remove_if(runtime.processes.begin(), runtime.processes.end(),
                               [&event](const TrackedProcess& process) {
                                   return process.identity.pid == event.process.identity.pid &&
                                          !SameProcessInstance(process.identity,
                                                               event.process.identity);
                               }),
                runtime.processes.end());
        }

        RuntimeRule& runtime = m_runtimeRules[static_cast<std::size_t>(ruleIndex)];
        const auto existing = std::find_if(
            runtime.processes.begin(), runtime.processes.end(), [&event](const TrackedProcess& process) {
                return SameProcessInstance(process.identity, event.process.identity);
            });
        if (existing != runtime.processes.end()) {
            return;
        }
        shouldInject = runtime.rule.enabled && !event.initialScan;
        runtime.processes.push_back({event.process.identity,
                                     shouldInject ? ProcessProtection::Queued
                                                  : ProcessProtection::Observed,
                                     0,
                                     event.initialScan ? L"目标进程已经运行，请重启应用"
                                                       : L"规则已停用"});
        request = {event.process, runtime.rule.path, runtime.revision};
    }

    Log(L"发现目标进程 PID=" + std::to_wstring(event.process.identity.pid) + L"：" +
        event.process.imagePath);
    NotifyStateChanged();
    if (shouldInject) {
        QueueInjection(std::move(request));
    }
}

void BlockerService::OnScanChanged(const ProcessScanResult& scan, bool conditionChanged) {
    bool processesChanged = false;
    {
        std::lock_guard lock(m_mutex);
        m_processScan = scan;
        processesChanged = ReconcileProcesses(scan.processes, m_runtimeRules);
    }
    if (conditionChanged || processesChanged) {
        NotifyStateChanged();
    }
}

void BlockerService::RecordProcessEventLocked(const ProcessEvent& event) {
    auto& processes = m_processScan.processes;
    const auto existing = std::find_if(
        processes.begin(), processes.end(), [&event](const ProcessInfo& process) {
            return SameProcessInstance(process.identity, event.process.identity);
        });
    if (event.type == ProcessEventType::Exited) {
        if (existing != processes.end()) {
            processes.erase(existing);
        }
    } else if (existing == processes.end()) {
        processes.push_back(event.process);
    } else {
        *existing = event.process;
    }
}

bool BlockerService::ReconcileProcesses(const std::vector<ProcessInfo>& processes,
                                        std::vector<RuntimeRule>& rules) {
    bool changed = false;
    for (const ProcessInfo& process : processes) {
        const auto rule = std::find_if(rules.begin(), rules.end(), [&process](const RuntimeRule& item) {
            return PathUtils::Matches(item.rule, process.imagePath);
        });
        if (rule == rules.end()) {
            continue;
        }
        const auto existing = std::find_if(
            rule->processes.begin(), rule->processes.end(), [&process](const TrackedProcess& item) {
                return SameProcessInstance(item.identity, process.identity);
            });
        if (existing == rule->processes.end()) {
            rule->processes.push_back({process.identity, ProcessProtection::Observed, 0,
                                       L"目标进程已经运行，请重启应用"});
            changed = true;
        }
    }
    return changed;
}

void BlockerService::QueueInjection(InjectionRequest request) {
    {
        std::lock_guard lock(m_injectionMutex);
        if (m_injectionStopRequested) {
            return;
        }
        m_injectionQueue.push_back(std::move(request));
    }
    m_injectionCondition.notify_one();
}

void BlockerService::RunInjectionWorker() {
    std::vector<PendingInjection> pendingInjections;
    for (;;) {
        InjectionRequest request;
        bool hasRequest = false;
        bool stopping = false;
        {
            std::unique_lock lock(m_injectionMutex);
            m_injectionCondition.wait_for(lock, kPendingInjectionPollInterval, [this] {
                return m_injectionStopRequested || !m_injectionQueue.empty();
            });
            stopping = m_injectionStopRequested;
            if (stopping) {
                m_injectionQueue.clear();
            } else if (!m_injectionQueue.empty()) {
                request = std::move(m_injectionQueue.front());
                m_injectionQueue.pop_front();
                hasRequest = true;
            }
        }

        for (auto operation = pendingInjections.begin(); operation != pendingInjections.end();) {
            InjectionCompletion completion;
            if (!operation->operation->TryComplete(completion)) {
                if (completion.status == InjectionStatus::Indeterminate) {
                    InjectionResult injection;
                    injection.status = InjectionStatus::Pending;
                    injection.architecture = operation->architecture;
                    injection.error = std::move(completion.error);
                    ApplyInjectionResult(operation->request, injection);
                }
                if (stopping) {
                    InjectionOperationReaper::Retain(operation->operation,
                                                      operation->policyMappingLifetime);
                    operation = pendingInjections.erase(operation);
                } else {
                    ++operation;
                }
                continue;
            }

            InjectionResult injection;
            injection.status = completion.status;
            injection.architecture = operation->architecture;
            injection.error = std::move(completion.error);
            if (injection.status == InjectionStatus::Failed &&
                ProcessMonitor::IsProcessAlive(operation->request.process)) {
                m_policyRegistry.Remove(operation->request.process.identity);
            }
            ApplyInjectionResult(operation->request, injection);
            operation = pendingInjections.erase(operation);
        }

        if (stopping) {
            return;
        }
        if (!hasRequest) {
            continue;
        }

        HotkeyPolicy policy;
        bool shouldInject = false;
        {
            std::lock_guard lock(m_mutex);
            const auto rule = std::find_if(
                m_runtimeRules.begin(), m_runtimeRules.end(), [&request](const RuntimeRule& item) {
                    return PathUtils::SamePath(item.rule.path, request.rulePath);
                });
            if (rule == m_runtimeRules.end() || rule->revision != request.ruleRevision ||
                rule->rule.enabled == false || m_lifecycle.phase != LifecyclePhase::Running) {
                continue;
            }
            const auto process = std::find_if(
                rule->processes.begin(), rule->processes.end(), [&request](const TrackedProcess& item) {
                    return SameProcessInstance(item.identity, request.process.identity);
                });
            if (process == rule->processes.end() ||
                process->protection != ProcessProtection::Queued) {
                continue;
            }
            process->protection = ProcessProtection::Injecting;
            process->operationRevision = request.ruleRevision;
            policy = rule->rule.hotkeyPolicy;
            shouldInject = true;
        }
        if (!shouldInject) {
            continue;
        }
        NotifyStateChanged();

        if (!ProcessMonitor::IsProcessAlive(request.process)) {
            InjectionResult injection;
            injection.error = L"目标进程已退出或进程标识已变化";
            ApplyInjectionResult(request, injection);
            continue;
        }

        std::wstring policyError;
        if (!m_policyRegistry.Publish(request.process.identity, policy, policyError)) {
            InjectionResult injection;
            injection.error = policyError.empty() ? L"发布快捷键策略失败" : policyError;
            ApplyInjectionResult(request, injection);
            continue;
        }

        std::shared_ptr<void> policyMappingLifetime =
            m_policyRegistry.RetainMappingLifetime(policyError);
        if (!policyMappingLifetime) {
            m_policyRegistry.Remove(request.process.identity);
            InjectionResult injection;
            injection.error = policyError.empty() ? L"保留快捷键策略共享内存失败"
                                                   : std::move(policyError);
            ApplyInjectionResult(request, injection);
            continue;
        }

        Log(L"开始注入 PID=" + std::to_wstring(request.process.identity.pid));
        InjectionResult injection = m_injector.Inject(request.process.identity.pid);
        if (injection.status == InjectionStatus::Pending &&
            injection.pendingOperation != nullptr) {
            ApplyInjectionResult(request, injection);
            pendingInjections.push_back(
                {std::move(request), injection.architecture,
                 std::move(injection.pendingOperation),
                 std::move(policyMappingLifetime)});
            continue;
        }
        if (injection.status == InjectionStatus::Failed &&
            ProcessMonitor::IsProcessAlive(request.process)) {
            m_policyRegistry.Remove(request.process.identity);
        }
        ApplyInjectionResult(request, injection);
    }
}

void BlockerService::ApplyInjectionResult(const InjectionRequest& request,
                                           const InjectionResult& injection) {
    std::wstring logMessage;
    {
        std::lock_guard lock(m_mutex);
        const auto rule = std::find_if(
            m_runtimeRules.begin(), m_runtimeRules.end(), [&request](const RuntimeRule& item) {
                return PathUtils::SamePath(item.rule.path, request.rulePath);
            });
        if (rule == m_runtimeRules.end()) {
            return;
        }

        const auto existing = std::find_if(
            rule->processes.begin(), rule->processes.end(), [&request](const TrackedProcess& process) {
                return SameProcessInstance(process.identity, request.process.identity);
            });
        const bool operationInFlight = existing != rule->processes.end() &&
            (existing->protection == ProcessProtection::Injecting ||
             existing->protection == ProcessProtection::Pending);
        if (!operationInFlight ||
            existing->operationRevision != request.ruleRevision) {
            return;
        }

        const bool requestBecameStale = rule->revision != request.ruleRevision;
        if (injection.status == InjectionStatus::Pending) {
            std::wstring detail = injection.error.empty() ? L"注入仍在目标进程中处理"
                                                          : injection.error;
            if (requestBecameStale) {
                detail += L"；规则已变更，完成后需要重启目标进程应用新状态";
            }
            if (existing->protection == ProcessProtection::Pending &&
                existing->detail == detail) {
                return;
            }
            existing->protection = ProcessProtection::Pending;
            existing->detail = std::move(detail);
            logMessage = L"注入仍在处理 PID=" +
                         std::to_wstring(request.process.identity.pid);
        } else if (requestBecameStale) {
            existing->operationRevision = 0;
            existing->protection = ProcessProtection::Observed;
            existing->detail = L"规则已变更，当前运行进程需重启以应用新状态";
            logMessage = L"规则已变更，注入结果不再代表当前策略 PID=" +
                         std::to_wstring(request.process.identity.pid);
        } else if (injection.status == InjectionStatus::Succeeded) {
            existing->operationRevision = 0;
            existing->protection = ProcessProtection::Blocked;
            existing->detail = L"PID=" + std::to_wstring(request.process.identity.pid) + L"（" +
                               ArchitectureName(injection.architecture) + L"）";
            logMessage = L"注入成功 PID=" + std::to_wstring(request.process.identity.pid);
        } else {
            existing->operationRevision = 0;
            existing->protection = ProcessProtection::Failed;
            existing->detail = injection.error.empty() ? L"未知注入错误" : injection.error;
            logMessage = L"注入失败 PID=" + std::to_wstring(request.process.identity.pid) + L"：" +
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
            return process.protection == ProcessProtection::Queued ||
                   process.protection == ProcessProtection::Injecting ||
                   process.protection == ProcessProtection::Pending;
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
    if (std::any_of(processes.begin(), processes.end(), [](const TrackedProcess& process) {
            return process.protection == ProcessProtection::Pending;
        })) {
        return AppStatus::InjectionPending;
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
    if (status == AppStatus::InjectionPending) {
        const auto pending = std::find_if(
            processes.begin(), processes.end(), [](const TrackedProcess& process) {
                return process.protection == ProcessProtection::Pending;
            });
        return pending == processes.end() ? L"目标进程仍在完成注入" : pending->detail;
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
        std::size_t pendingCount = 0;
        for (const TrackedProcess& process : processes) {
            blockedCount += process.protection == ProcessProtection::Blocked ? 1u : 0u;
            failedCount += process.protection == ProcessProtection::Failed ? 1u : 0u;
            pendingCount += process.protection == ProcessProtection::Queued ||
                                    process.protection == ProcessProtection::Injecting ||
                                    process.protection == ProcessProtection::Pending
                                ? 1u
                                : 0u;
        }
        std::wstring result = L"已拦截 " + std::to_wstring(blockedCount) + L"/" +
                              std::to_wstring(processes.size()) + L" 个进程";
        if (failedCount != 0) {
            result += L"，失败 " + std::to_wstring(failedCount) + L" 个";
        }
        if (pendingCount != 0) {
            result += L"，处理中 " + std::to_wstring(pendingCount) + L" 个";
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
        case AppStatus::InjectionPending:
            return L"等待注入完成";
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
