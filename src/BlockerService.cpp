#include "BlockerService.h"

#include "Logger.h"
#include "PathUtils.h"

#include <algorithm>
#include <utility>

BlockerService::BlockerService(Logger* logger) : m_logger(logger) {}

BlockerService::~BlockerService() {
    Stop();
}

bool BlockerService::Start(const std::vector<AppRule>& rules) {
    Stop();

    {
        std::lock_guard lock(m_mutex);
        m_rules = rules;
        m_processes.assign(m_rules.size(), {});
        m_states.resize(m_rules.size());
        RebuildStatesLocked();
        m_started = true;
    }

    if (!m_monitor.Start([this](const ProcessEvent& event) { OnProcessEvent(event); })) {
        std::lock_guard lock(m_mutex);
        m_started = false;
        m_rules.clear();
        m_processes.clear();
        m_states.clear();
        Log(L"进程监控启动失败");
        return false;
    }

    Log(L"进程监控已启动");
    return true;
}

void BlockerService::Stop() {
    m_monitor.Stop();
    std::lock_guard lock(m_mutex);
    if (m_started) {
        Log(L"进程监控已停止");
    }
    m_started = false;
    m_rules.clear();
    m_processes.clear();
    m_states.clear();
}

void BlockerService::UpdateRules(const std::vector<AppRule>& rules) {
    const std::vector<ProcessInfo> runningProcesses = m_monitor.Snapshot();

    std::lock_guard lock(m_mutex);
    std::vector<std::vector<TrackedProcess>> newProcesses(rules.size());
    for (std::size_t newIndex = 0; newIndex < rules.size(); ++newIndex) {
        const int oldIndex = PathUtils::FindRuleIndex(m_rules, rules[newIndex].path);
        if (oldIndex >= 0 && static_cast<std::size_t>(oldIndex) < m_processes.size()) {
            newProcesses[newIndex] = m_processes[static_cast<std::size_t>(oldIndex)];
        }
    }

    for (const ProcessInfo& process : runningProcesses) {
        const int ruleIndex = PathUtils::FindRuleIndex(rules, process.imagePath);
        if (ruleIndex < 0) {
            continue;
        }
        auto& tracked = newProcesses[static_cast<std::size_t>(ruleIndex)];
        const auto existing = std::find_if(
            tracked.begin(), tracked.end(), [&process](const TrackedProcess& item) {
                return item.pid == process.pid;
            });
        if (existing == tracked.end()) {
            tracked.push_back({process.pid, ProcessProtection::Observed,
                               L"目标进程已经运行，请重启应用"});
        }
    }

    m_rules = rules;
    m_processes = std::move(newProcesses);
    m_states.resize(m_rules.size());
    RebuildStatesLocked();
}

std::vector<RuntimeRuleState> BlockerService::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_states;
}

void BlockerService::OnProcessEvent(const ProcessEvent& event) {
    if (event.type == ProcessEventType::Exited) {
        std::lock_guard lock(m_mutex);
        for (auto& tracked : m_processes) {
            tracked.erase(std::remove_if(tracked.begin(), tracked.end(),
                                         [&event](const TrackedProcess& process) {
                                             return process.pid == event.process.pid;
                                         }),
                          tracked.end());
        }
        RebuildStatesLocked();
        Log(L"目标进程退出 PID=" + std::to_wstring(event.process.pid));
        return;
    }

    int ruleIndex = -1;
    AppRule rule;
    {
        std::lock_guard lock(m_mutex);
        ruleIndex = PathUtils::FindRuleIndex(m_rules, event.process.imagePath);
        if (ruleIndex < 0) {
            return;
        }

        rule = m_rules[static_cast<std::size_t>(ruleIndex)];
        auto& tracked = m_processes[static_cast<std::size_t>(ruleIndex)];
        const auto existing = std::find_if(
            tracked.begin(), tracked.end(), [&event](const TrackedProcess& process) {
                return process.pid == event.process.pid;
            });
        if (existing != tracked.end()) {
            return;
        }
        tracked.push_back({event.process.pid, ProcessProtection::Observed,
                           event.initialScan ? L"目标进程已经运行，请重启应用" : L""});
        RebuildStatesLocked();
    }

    Log(L"发现目标进程 PID=" + std::to_wstring(event.process.pid) + L"：" +
        event.process.imagePath);

    if (!rule.enabled || event.initialScan) {
        return;
    }

    Log(L"开始注入 PID=" + std::to_wstring(event.process.pid));
    const InjectionResult injection = m_injector.Inject(event.process.pid);

    std::lock_guard lock(m_mutex);
    ruleIndex = PathUtils::FindRuleIndex(m_rules, event.process.imagePath);
    if (ruleIndex < 0) {
        return;
    }

    auto& tracked = m_processes[static_cast<std::size_t>(ruleIndex)];
    const auto existing = std::find_if(
        tracked.begin(), tracked.end(), [&event](const TrackedProcess& process) {
            return process.pid == event.process.pid;
        });
    if (existing == tracked.end()) {
        return;
    }

    if (injection.success) {
        existing->protection = ProcessProtection::Blocked;
        existing->detail = L"PID=" + std::to_wstring(event.process.pid) + L"（" +
                           ArchitectureName(injection.architecture) + L"）";
        Log(L"注入成功 PID=" + std::to_wstring(event.process.pid));
    } else {
        existing->protection = ProcessProtection::Failed;
        existing->detail = injection.error.empty() ? L"未知注入错误" : injection.error;
        Log(L"注入失败 PID=" + std::to_wstring(event.process.pid) + L"：" +
            existing->detail);
    }
    RebuildStatesLocked();
}

void BlockerService::RebuildStatesLocked() {
    m_states.resize(m_rules.size());
    for (std::size_t index = 0; index < m_rules.size(); ++index) {
        m_states[index].rule = m_rules[index];
        m_states[index].status = StateForRule(m_rules[index], m_processes[index]);
        m_states[index].detail = DetailForRule(m_states[index].status, m_processes[index]);
    }
}

AppStatus BlockerService::StateForRule(
    const AppRule& rule, const std::vector<TrackedProcess>& processes) {
    if (!rule.enabled) {
        return AppStatus::Disabled;
    }

    if (std::any_of(processes.begin(), processes.end(), [](const TrackedProcess& process) {
            return process.protection == ProcessProtection::Blocked;
        })) {
        return AppStatus::Blocked;
    }
    if (std::any_of(processes.begin(), processes.end(), [](const TrackedProcess& process) {
            return process.protection == ProcessProtection::Failed;
        })) {
        return AppStatus::InjectionFailed;
    }
    if (!processes.empty()) {
        return AppStatus::RestartRequired;
    }
    return AppStatus::Waiting;
}

std::wstring BlockerService::DetailForRule(
    AppStatus status, const std::vector<TrackedProcess>& processes) {
    if (status == AppStatus::RestartRequired) {
        return L"目标进程已在运行，请重启应用";
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
    return {};
}

void BlockerService::Log(const std::wstring& message) const {
    if (m_logger != nullptr) {
        m_logger->Info(message);
    }
}

const wchar_t* AppStatusText(AppStatus status) {
    switch (status) {
        case AppStatus::Waiting:
            return L"等待启动";
        case AppStatus::RestartRequired:
            return L"需要重启";
        case AppStatus::Blocked:
            return L"已拦截";
        case AppStatus::InjectionFailed:
            return L"注入失败";
        case AppStatus::Disabled:
            return L"已停用";
        default:
            return L"未知状态";
    }
}
