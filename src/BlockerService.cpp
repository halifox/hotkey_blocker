#include "BlockerService.h"

#include "Logger.h"
#include "PathUtils.h"

#include <algorithm>
#include <windows.h>
#include <utility>

namespace {

template <typename Callback>
void ForEachRuleTarget(const AppRule& rule, Callback callback) {
    bool hasPrimary = false;
    if (!rule.path.empty()) {
        callback(rule.path);
        hasPrimary = true;
    }
    for (const std::wstring& target : rule.targets) {
        if (!target.empty() && (!hasPrimary || !PathUtils::SamePath(target, rule.path))) {
            callback(target);
        }
    }
}

bool AnyRuleTargetExists(const AppRule& rule) {
    bool exists = false;
    ForEachRuleTarget(rule, [&exists](const std::wstring& target) {
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            exists = true;
        }
    });
    return exists;
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
        m_rules = rules;
        RebuildRuleIndexLocked();
        m_processes.assign(m_rules.size(), {});
        m_states.resize(m_rules.size());
        RebuildStatesLocked();
        m_started = true;
    }

    {
        std::lock_guard lock(m_injectionMutex);
        m_injectionQueue.clear();
        m_injectionStopRequested = false;
    }

    try {
        m_injectionThread = std::thread(&BlockerService::RunInjectionWorker, this);
    } catch (...) {
        {
            std::lock_guard lock(m_mutex);
            m_started = false;
            m_rules.clear();
            m_ruleIndices.clear();
            m_processes.clear();
            m_states.clear();
        }
        Log(L"注入线程启动失败");
        return false;
    }

    if (!m_monitor.Start([this](const ProcessEvent& event) { OnProcessEvent(event); })) {
        Stop();
        Log(L"进程监控启动失败");
        return false;
    }

    Log(L"进程监控已启动");
    return true;
}

void BlockerService::Stop() {
    m_monitor.Stop();

    {
        std::lock_guard lock(m_injectionMutex);
        m_injectionStopRequested = true;
        m_injectionQueue.clear();
    }
    m_injectionCondition.notify_all();
    if (m_injectionThread.joinable()) {
        m_injectionThread.join();
    }

    bool wasStarted = false;
    {
        std::lock_guard lock(m_mutex);
        wasStarted = m_started;
        m_started = false;
        m_rules.clear();
        m_ruleIndices.clear();
        m_processes.clear();
        m_states.clear();
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
        std::vector<std::vector<TrackedProcess>> newProcesses(rules.size());
        for (std::size_t newIndex = 0; newIndex < rules.size(); ++newIndex) {
            const int oldIndex = FindRuleIndexLocked(rules[newIndex].path);
            if (oldIndex >= 0 && static_cast<std::size_t>(oldIndex) < m_processes.size()) {
                newProcesses[newIndex] = m_processes[static_cast<std::size_t>(oldIndex)];
            }
        }

        for (const ProcessInfo& process : runningProcesses) {
            const int ruleIndex = FindRuleIndex(rules, process.imagePath);
            if (ruleIndex < 0) {
                continue;
            }
            auto& tracked = newProcesses[static_cast<std::size_t>(ruleIndex)];
            const auto existing = std::find_if(
                tracked.begin(), tracked.end(), [&process](const TrackedProcess& item) {
                    return SameProcess(item, process);
                });
            if (existing == tracked.end()) {
                tracked.push_back({process.pid, process.creationTime, ProcessProtection::Observed,
                                   L"目标进程已经运行，请重启应用"});
            }
        }

        m_rules = rules;
        RebuildRuleIndexLocked();
        m_processes = std::move(newProcesses);
        m_states.resize(m_rules.size());
        RebuildStatesLocked();
    }
    NotifyStateChanged();
}

std::vector<RuntimeRuleState> BlockerService::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_states;
}

std::vector<ProcessInfo> BlockerService::RunningProcesses() const {
    return m_monitor.Snapshot();
}

bool BlockerService::WaitUntilReady(DWORD timeoutMs) const {
    return m_monitor.WaitUntilReady(timeoutMs);
}

void BlockerService::OnProcessEvent(const ProcessEvent& event) {
    if (event.type == ProcessEventType::Exited) {
        bool removed = false;
        {
            std::lock_guard lock(m_mutex);
            for (auto& tracked : m_processes) {
                const auto oldSize = tracked.size();
                tracked.erase(std::remove_if(tracked.begin(), tracked.end(),
                                             [&event](const TrackedProcess& process) {
                                                 return SameProcess(process, event.process);
                                             }),
                              tracked.end());
                removed = removed || oldSize != tracked.size();
            }
            if (removed) {
                RebuildStatesLocked();
            }
        }
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

        // A start event for a reused PID may arrive before a delayed stop
        // event. Remove the stale identity before adding the new one.
        for (auto& tracked : m_processes) {
            tracked.erase(std::remove_if(tracked.begin(), tracked.end(),
                                         [&event](const TrackedProcess& process) {
                                             return process.pid == event.process.pid &&
                                                    !SameProcess(process, event.process);
                                         }),
                          tracked.end());
        }

        rule = m_rules[static_cast<std::size_t>(ruleIndex)];
        auto& tracked = m_processes[static_cast<std::size_t>(ruleIndex)];
        const auto existing = std::find_if(
            tracked.begin(), tracked.end(), [&event](const TrackedProcess& process) {
                return SameProcess(process, event.process);
            });
        if (existing != tracked.end()) {
            return;
        }
        tracked.push_back({event.process.pid, event.process.creationTime,
                           event.initialScan ? ProcessProtection::Observed
                                              : ProcessProtection::Pending,
                           event.initialScan ? L"目标进程已经运行，请重启应用" : L""});
        RebuildStatesLocked();
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
        {
            std::lock_guard lock(m_mutex);
            const int ruleIndex = FindRuleIndexLocked(event.process.imagePath);
            enabled = ruleIndex >= 0 && m_rules[static_cast<std::size_t>(ruleIndex)].enabled &&
                      m_started;
        }
        if (!enabled || !ProcessMonitor::IsProcessAlive(event.process)) {
            continue;
        }

        Log(L"开始注入 PID=" + std::to_wstring(event.process.pid));
        const InjectionResult injection = m_injector.Inject(event.process.pid);
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

        auto& tracked = m_processes[static_cast<std::size_t>(ruleIndex)];
        const auto existing = std::find_if(
            tracked.begin(), tracked.end(), [&event](const TrackedProcess& process) {
                return SameProcess(process, event.process);
            });
        if (existing == tracked.end()) {
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
        RebuildStatesLocked();
    }
    Log(logMessage);
    NotifyStateChanged();
}

void BlockerService::RebuildStatesLocked() {
    m_states.resize(m_rules.size());
    for (std::size_t index = 0; index < m_rules.size(); ++index) {
        m_states[index].rule = m_rules[index];
        m_states[index].status = StateForRule(m_rules[index], m_processes[index]);
        m_states[index].detail = DetailForRule(m_states[index].status, m_processes[index]);
    }
}

void BlockerService::RebuildRuleIndexLocked() {
    m_ruleIndices.clear();
    for (std::size_t index = 0; index < m_rules.size(); ++index) {
        ForEachRuleTarget(m_rules[index], [this, index](const std::wstring& target) {
            m_ruleIndices.emplace(target, index);
        });
    }
}

int BlockerService::FindRuleIndexLocked(const std::wstring& path) const {
    const auto iterator = m_ruleIndices.find(path);
    return iterator == m_ruleIndices.end() ? -1 : static_cast<int>(iterator->second);
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
        return AnyRuleTargetExists(rule) ? AppStatus::Waiting : AppStatus::PathMissing;
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

int BlockerService::FindRuleIndex(const std::vector<AppRule>& rules,
                                  const std::wstring& path) {
    for (std::size_t index = 0; index < rules.size(); ++index) {
        bool matched = false;
        ForEachRuleTarget(rules[index], [&matched, &path](const std::wstring& target) {
            matched = matched || PathUtils::SamePath(target, path);
        });
        if (matched) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void BlockerService::Log(const std::wstring& message) const {
    if (m_logger != nullptr && !message.empty()) {
        m_logger->Info(message);
    }
}

const wchar_t* AppStatusText(AppStatus status) {
    switch (status) {
        case AppStatus::Waiting:
            return L"等待启动";
        case AppStatus::Injecting:
            return L"正在处理";
        case AppStatus::RestartRequired:
            return L"需要重启";
        case AppStatus::Blocked:
            return L"已拦截";
        case AppStatus::PartiallyBlocked:
            return L"部分拦截";
        case AppStatus::InjectionFailed:
            return L"注入失败";
        case AppStatus::PathMissing:
            return L"路径不存在";
        case AppStatus::Disabled:
            return L"已停用";
        default:
            return L"未知状态";
    }
}
