#include "RuleManager.h"

#include "PathUtils.h"

#include <algorithm>
#include <utility>

namespace {

const auto FindRuleByPath = [](std::vector<AppRule>& rules, const std::wstring& path) {
    return std::find_if(rules.begin(), rules.end(), [&path](const AppRule& rule) {
        return PathUtils::SamePath(rule.path, path);
    });
};

}  // namespace

RuleManager::RuleManager() = default;

RuleManager::RuleManager(ConfigStore store) : m_store(std::move(store)) {}

bool RuleManager::Load() {
    m_config = AppConfig{};
    std::wstring error;
    AppConfig loadedConfig;
    if (!m_store.Load(loadedConfig, error)) {
        m_lastError = std::move(error);
        return false;
    }

    std::vector<AppRule> normalizedRules;
    normalizedRules.reserve(loadedConfig.apps.size());
    for (AppRule& rule : loadedConfig.apps) {
        if (!PathUtils::NormalizeRule(rule, error)) {
            m_lastError = std::move(error);
            return false;
        }
        const auto duplicate = std::find_if(
            normalizedRules.begin(), normalizedRules.end(), [&rule](const AppRule& existing) {
                return PathUtils::Overlaps(existing, rule);
            });
        if (duplicate != normalizedRules.end()) {
            m_lastError = L"配置包含重叠规则：" + rule.path;
            return false;
        }
        normalizedRules.push_back(std::move(rule));
    }

    loadedConfig.apps = std::move(normalizedRules);
    m_config = std::move(loadedConfig);
    m_lastError.clear();
    return true;
}

bool RuleManager::Save() {
    std::wstring error;
    if (!m_store.Save(m_config, error)) {
        m_lastError = std::move(error);
        return false;
    }
    m_lastError.clear();
    return true;
}

bool RuleManager::AddRule(AppRule rule) {
    std::wstring error;
    if (!PathUtils::NormalizeRule(rule, error)) {
        m_lastError = std::move(error);
        return false;
    }

    const auto duplicate = std::find_if(
        m_config.apps.begin(), m_config.apps.end(), [&rule](const AppRule& existing) {
            return PathUtils::Overlaps(existing, rule);
        });
    if (duplicate != m_config.apps.end()) {
        m_lastError = L"该应用或文件夹已经添加";
        return false;
    }

    const AppConfig previousConfig = m_config;
    m_config.apps.push_back(std::move(rule));
    return SaveAfterChange(previousConfig);
}

bool RuleManager::Remove(const std::wstring& path) {
    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    if (normalizedPath.empty()) {
        m_lastError = L"规则路径无效";
        return false;
    }

    const auto iterator = FindRuleByPath(m_config.apps, normalizedPath);
    if (iterator == m_config.apps.end()) {
        m_lastError = L"未找到要删除的规则";
        return false;
    }

    const AppConfig previousConfig = m_config;
    m_config.apps.erase(iterator);
    return SaveAfterChange(previousConfig);
}

bool RuleManager::SetEnabled(const std::wstring& path, bool enabled) {
    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    if (normalizedPath.empty()) {
        m_lastError = L"规则路径无效";
        return false;
    }

    const auto iterator = FindRuleByPath(m_config.apps, normalizedPath);
    if (iterator == m_config.apps.end()) {
        m_lastError = L"未找到要修改的规则";
        return false;
    }
    if (iterator->enabled == enabled) {
        return true;
    }

    const AppConfig previousConfig = m_config;
    iterator->enabled = enabled;
    return SaveAfterChange(previousConfig);
}

bool RuleManager::SetHotkeyPolicy(const std::wstring& path, HotkeyPolicy policy) {
    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    if (normalizedPath.empty()) {
        m_lastError = L"规则路径无效";
        return false;
    }
    NormalizeHotkeyPolicy(policy);
    if (!ValidateHotkeyPolicy(policy)) {
        m_lastError = L"快捷键策略无效或快捷键数量超过限制";
        return false;
    }

    const auto iterator = FindRuleByPath(m_config.apps, normalizedPath);
    if (iterator == m_config.apps.end()) {
        m_lastError = L"未找到要修改的规则";
        return false;
    }
    if (SameHotkeyPolicy(iterator->hotkeyPolicy, policy)) {
        return true;
    }

    const AppConfig previousConfig = m_config;
    iterator->hotkeyPolicy = std::move(policy);
    return SaveAfterChange(previousConfig);
}

bool RuleManager::SetRuleSettings(const std::wstring& path, bool enabled, HotkeyPolicy policy) {
    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    if (normalizedPath.empty()) {
        m_lastError = L"规则路径无效";
        return false;
    }
    NormalizeHotkeyPolicy(policy);
    if (!ValidateHotkeyPolicy(policy)) {
        m_lastError = L"快捷键策略无效或快捷键数量超过限制";
        return false;
    }

    const auto iterator = FindRuleByPath(m_config.apps, normalizedPath);
    if (iterator == m_config.apps.end()) {
        m_lastError = L"未找到要修改的规则";
        return false;
    }
    if (iterator->enabled == enabled && SameHotkeyPolicy(iterator->hotkeyPolicy, policy)) {
        m_lastError.clear();
        return true;
    }

    const AppConfig previousConfig = m_config;
    iterator->enabled = enabled;
    iterator->hotkeyPolicy = std::move(policy);
    return SaveAfterChange(previousConfig);
}

const std::vector<AppRule>& RuleManager::Rules() const noexcept {
    return m_config.apps;
}

const std::wstring& RuleManager::LastError() const noexcept {
    return m_lastError;
}

bool RuleManager::SaveAfterChange(const AppConfig& previousConfig) {
    if (Save()) {
        return true;
    }
    m_config = previousConfig;
    return false;
}
