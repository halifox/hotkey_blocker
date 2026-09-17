#pragma once

#include "ConfigStore.h"

#include <filesystem>
#include <string>
#include <vector>

class RuleManager final {
public:
    RuleManager();
    explicit RuleManager(ConfigStore store);

    bool Load();
    bool Save();

    bool Add(const std::wstring& path);
    bool AddRule(AppRule rule);
    bool Remove(const std::wstring& path);
    bool SetEnabled(const std::wstring& path, bool enabled);
    bool AutoStart() const noexcept;
    bool SetAutoStart(bool enabled);

    const std::vector<AppRule>& Rules() const noexcept;
    const std::filesystem::path& ConfigPath() const noexcept;
    const std::wstring& LastError() const noexcept;

private:
    bool SaveAfterChange(const AppConfig& previousConfig);

    ConfigStore m_store;
    AppConfig m_config;
    std::wstring m_lastError;
};
