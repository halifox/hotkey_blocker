#include "WindowsTarget.h"

#include "ApplicationController.h"

#include <utility>

ApplicationController::ApplicationController() : m_blockerService(&m_logger) {}

ApplicationStartupResult ApplicationController::Initialize(
    BlockerService::StateChangedCallback stateChanged) {
    ApplicationStartupResult result;
    m_logger.Info(L"程序启动");

    if (!m_ruleManager.Load()) {
        result.ruleError = m_ruleManager.LastError();
        m_logger.Error(L"加载规则失败：" + result.ruleError);
        return result;
    }
    result.rulesLoaded = true;
    m_logger.Info(L"加载规则：" + std::to_wstring(m_ruleManager.Rules().size()) + L" 条");

    result.startupSettingLoaded =
        m_startupManager.GetEnabled(result.autoStartEnabled, result.startupSettingError);
    if (result.startupSettingLoaded) {
        m_autoStartEnabled = result.autoStartEnabled;
    } else {
        m_logger.Error(result.startupSettingError);
    }

    m_blockerService.SetStateChangedCallback(std::move(stateChanged));
    result.blockerStart = m_blockerService.Start(m_ruleManager.Rules());
    if (!result.blockerStart) {
        m_logger.Error(L"运行服务启动失败：" + result.blockerStart.error);
    }
    return result;
}

void ApplicationController::SetStateChangedCallback(
    BlockerService::StateChangedCallback stateChanged) {
    m_blockerService.SetStateChangedCallback(std::move(stateChanged));
}

void ApplicationController::Stop() {
    m_blockerService.Stop();
}

Logger& ApplicationController::Log() noexcept {
    return m_logger;
}

const std::vector<AppRule>& ApplicationController::Rules() const noexcept {
    return m_ruleManager.Rules();
}

std::vector<RuntimeRuleState> ApplicationController::RuntimeStates() const {
    return m_blockerService.Snapshot();
}

const std::wstring& ApplicationController::LastRuleError() const noexcept {
    return m_ruleManager.LastError();
}

bool ApplicationController::AutoStartEnabled() const noexcept {
    return m_autoStartEnabled;
}

bool ApplicationController::AddRule(AppRule rule) {
    if (!m_ruleManager.AddRule(std::move(rule))) {
        return false;
    }
    ApplyRulesToRuntime();
    return true;
}

bool ApplicationController::RemoveRule(const std::wstring& path) {
    if (!m_ruleManager.Remove(path)) {
        return false;
    }
    ApplyRulesToRuntime();
    return true;
}

bool ApplicationController::SetRuleSettings(const std::wstring& path, bool enabled,
                                            HotkeyPolicy policy) {
    if (!m_ruleManager.SetRuleSettings(path, enabled, std::move(policy))) {
        return false;
    }
    ApplyRulesToRuntime();
    return true;
}

bool ApplicationController::SetAutoStartEnabled(bool enabled, std::wstring& error) {
    if (!m_startupManager.SetEnabled(enabled, error)) {
        return false;
    }
    m_autoStartEnabled = enabled;
    return true;
}

void ApplicationController::ApplyRulesToRuntime() {
    m_blockerService.UpdateRules(m_ruleManager.Rules());
}
