#include "WindowsTarget.h"

#include "ApplicationListPresenter.h"

#include <algorithm>
#include <string>
#include <utility>

#include "PathUtils.h"

namespace {

std::wstring HotkeyPolicySummary(const HotkeyPolicy& policy) {
    switch (policy.mode) {
        case HotkeyMode::Blacklist:
            return L"黑名单：" + std::to_wstring(policy.hotkeys.size()) + L" 个快捷键";
        case HotkeyMode::Whitelist:
            return L"白名单：" + std::to_wstring(policy.hotkeys.size()) + L" 个快捷键";
        case HotkeyMode::BlockAll:
        default:
            return L"拦截全部快捷键";
    }
}

}  // namespace

std::vector<ApplicationListRow> ApplicationListPresenter::BuildRows(
    const std::vector<AppRule>& rules,
    const std::vector<RuntimeRuleState>& runtimeStates) const {
    std::vector<ApplicationListRow> rows;
    rows.reserve(rules.size());

    for (const AppRule& rule : rules) {
        ApplicationListRow row;
        row.path = rule.path;
        row.enabled = rule.enabled;
        row.status = AppStatusText(AppStatus::Waiting);
        row.detail = HotkeyPolicySummary(rule.hotkeyPolicy);
        if (rule.kind == RuleKind::Directory) {
            row.detail += L"；递归匹配文件夹中的 EXE";
        }

        const auto state = std::find_if(
            runtimeStates.begin(), runtimeStates.end(), [&rule](const RuntimeRuleState& candidate) {
                return PathUtils::SamePath(candidate.path, rule.path);
            });
        if (state != runtimeStates.end()) {
            row.status = AppStatusText(state->status);
            if (!state->detail.empty()) {
                if (!row.detail.empty()) {
                    row.detail += L"；";
                }
                row.detail += state->detail;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}
