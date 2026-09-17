#include "RuleManager.h"

#include "PathUtils.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace {

bool IsExePath(const std::wstring& path) {
    const std::wstring extension = std::filesystem::path(path).extension().wstring();
    if (extension.size() != 4) {
        return false;
    }
    return extension[0] == L'.' && ((extension[1] == L'e' || extension[1] == L'E') &&
            (extension[2] == L'x' || extension[2] == L'X') &&
            (extension[3] == L'e' || extension[3] == L'E'));
}

}  // namespace

RuleManager::RuleManager() = default;

RuleManager::RuleManager(ConfigStore store) : m_store(std::move(store)) {}

bool RuleManager::Load() {
    AppConfig loadedConfig;
    std::wstring error;
    if (!m_store.Load(loadedConfig, error)) {
        m_lastError = std::move(error);
        return false;
    }

    std::vector<AppRule> normalizedRules;
    normalizedRules.reserve(loadedConfig.apps.size());
    for (AppRule& rule : loadedConfig.apps) {
        rule.path = PathUtils::NormalizePath(rule.path);
        if (rule.path.empty() || !IsExePath(rule.path)) {
            continue;
        }

        const auto duplicate = std::find_if(
            normalizedRules.begin(), normalizedRules.end(), [&rule](const AppRule& existing) {
                return PathUtils::SamePath(existing.path, rule.path);
            });
        if (duplicate == normalizedRules.end()) {
            normalizedRules.push_back(std::move(rule));
        }
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

bool RuleManager::Add(const std::wstring& path) {
    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    if (normalizedPath.empty() || !IsExePath(normalizedPath)) {
        m_lastError = L"只能添加 .exe 应用程序";
        return false;
    }

    const auto duplicate = std::find_if(
        m_config.apps.begin(), m_config.apps.end(), [&normalizedPath](const AppRule& existing) {
            return PathUtils::SamePath(existing.path, normalizedPath);
        });
    if (duplicate != m_config.apps.end()) {
        m_lastError = L"该应用已经添加";
        return false;
    }

    const AppConfig previousConfig = m_config;
    m_config.apps.push_back({normalizedPath, true});
    return SaveAfterChange(previousConfig);
}

bool RuleManager::Remove(const std::wstring& path) {
    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    const auto iterator = std::find_if(
        m_config.apps.begin(), m_config.apps.end(), [&normalizedPath](const AppRule& existing) {
            return PathUtils::SamePath(existing.path, normalizedPath);
        });
    if (iterator == m_config.apps.end()) {
        m_lastError = L"未找到要删除的应用";
        return false;
    }

    const AppConfig previousConfig = m_config;
    m_config.apps.erase(iterator);
    return SaveAfterChange(previousConfig);
}

bool RuleManager::SetEnabled(const std::wstring& path, bool enabled) {
    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    const auto iterator = std::find_if(
        m_config.apps.begin(), m_config.apps.end(), [&normalizedPath](const AppRule& existing) {
            return PathUtils::SamePath(existing.path, normalizedPath);
        });
    if (iterator == m_config.apps.end()) {
        m_lastError = L"未找到要修改的应用";
        return false;
    }
    if (iterator->enabled == enabled) {
        return true;
    }

    const AppConfig previousConfig = m_config;
    iterator->enabled = enabled;
    return SaveAfterChange(previousConfig);
}

const std::vector<AppRule>& RuleManager::Rules() const noexcept {
    return m_config.apps;
}

const std::filesystem::path& RuleManager::ConfigPath() const noexcept {
    return m_store.Path();
}

const std::wstring& RuleManager::LastError() const noexcept {
    return m_lastError;
}

bool RuleManager::AutoStart() const noexcept {
    return m_config.autoStart;
}

bool RuleManager::SetAutoStart(bool enabled) {
    if (m_config.autoStart == enabled) {
        return true;
    }

    const AppConfig previousConfig = m_config;
    m_config.autoStart = enabled;
    return SaveAfterChange(previousConfig);
}

bool RuleManager::SaveAfterChange(const AppConfig& previousConfig) {
    if (Save()) {
        return true;
    }
    m_config = previousConfig;
    return false;
}
