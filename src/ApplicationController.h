#pragma once

#include "BlockerService.h"
#include "Logger.h"
#include "RuleManager.h"
#include "StartupManager.h"

#include <functional>
#include <string>
#include <vector>

struct ApplicationStartupResult {
    bool rulesLoaded = false;
    std::wstring ruleError;
    bool startupSettingLoaded = false;
    bool autoStartEnabled = false;
    std::wstring startupSettingError;
    BlockerServiceStartResult blockerStart;
};

class ApplicationController final {
public:
    ApplicationController();

    ApplicationStartupResult Initialize(BlockerService::StateChangedCallback stateChanged);
    void SetStateChangedCallback(BlockerService::StateChangedCallback stateChanged);
    void Stop();

    Logger& Log() noexcept;
    const std::vector<AppRule>& Rules() const noexcept;
    std::vector<RuntimeRuleState> RuntimeStates() const;
    const std::wstring& LastRuleError() const noexcept;
    bool AutoStartEnabled() const noexcept;

    bool AddRule(AppRule rule);
    bool RemoveRule(const std::wstring& path);
    bool SetRuleSettings(const std::wstring& path, bool enabled, HotkeyPolicy policy);
    bool SetAutoStartEnabled(bool enabled, std::wstring& error);

private:
    void ApplyRulesToRuntime();

    Logger m_logger;
    RuleManager m_ruleManager;
    StartupManager m_startupManager;
    BlockerService m_blockerService;
    bool m_autoStartEnabled = false;
};
