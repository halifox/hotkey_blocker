#include "WindowsTarget.h"

#include <windows.h>
#include <shellapi.h>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlstr.h>
#include <atldlgs.h>
#include <atlmisc.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "MainFrame.h"
#include "HotkeyPolicy.h"
#include "HotkeyPolicyDialog.h"
#include "PathUtils.h"

void MainFrame::HandleListAction(const std::wstring& path, ApplicationListAction action) {
    if (action == ApplicationListAction::Configure) {
        ConfigureApplication(path);
    } else {
        DeleteApplication(path);
    }
}

void MainFrame::RefreshListView(bool force) {
    if (!m_mainView.IsReady()) {
        return;
    }
    const std::vector<RuntimeRuleState> states = m_application.RuntimeStates();
    m_mainView.SetRows(m_listPresenter.BuildRows(m_application.Rules(), states), force);
}

bool MainFrame::PickApplicationPath(bool folder, std::wstring& path) {
    CShellFileOpenDialog dialog;
    if (dialog.IsNull()) {
        ShowError(L"创建文件选择器失败", L"无法创建 Windows 文件选择器。");
        return false;
    }

    FILEOPENDIALOGOPTIONS options = 0;
    HRESULT result = dialog.GetPtr()->GetOptions(&options);
    if (FAILED(result)) {
        ShowErrorCode(L"配置文件选择器失败", result);
        return false;
    }
    options |= FOS_FORCEFILESYSTEM;
    if (folder) {
        options |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
    } else {
        options |= FOS_FILEMUSTEXIST;
        const COMDLG_FILTERSPEC filters[] = {{L"应用程序 (*.exe)", L"*.exe"}};
        result = dialog.GetPtr()->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        if (FAILED(result)) {
            ShowErrorCode(L"配置文件选择器失败", result);
            return false;
        }
    }
    result = dialog.GetPtr()->SetOptions(options);
    if (FAILED(result)) {
        ShowErrorCode(L"配置文件选择器失败", result);
        return false;
    }
    result = dialog.GetPtr()->SetTitle(folder ? L"选择要拦截的程序文件夹"
                                               : L"选择要拦截的 EXE 文件");
    if (FAILED(result)) {
        ShowErrorCode(L"配置文件选择器失败", result);
        return false;
    }

    const INT_PTR dialogResult = dialog.DoModal(m_hWnd);
    if (dialogResult == IDCANCEL) {
        return false;
    }
    if (dialogResult != IDOK) {
        ShowError(L"打开文件选择器失败", L"Windows 文件选择器无法打开。");
        return false;
    }

    ATL::CString selectedPath;
    result = dialog.GetFilePath(selectedPath);
    if (FAILED(result)) {
        ShowErrorCode(L"获取所选文件失败", result);
        return false;
    }
    path.assign(selectedPath.GetString());
    return true;
}

void MainFrame::AddExecutableApplication() {
    std::wstring path;
    if (!PickApplicationPath(false, path)) {
        return;
    }

    AppRule rule;
    rule.path = std::move(path);
    rule.enabled = true;
    if (!m_application.AddRule(std::move(rule))) {
        ShowError(L"添加应用失败", m_application.LastRuleError());
        return;
    }
    RefreshListView(true);
}

void MainFrame::AddFolderApplication() {
    std::wstring path;
    if (!PickApplicationPath(true, path)) {
        return;
    }

    AppRule rule;
    rule.path = std::move(path);
    rule.enabled = true;
    rule.kind = RuleKind::Directory;
    if (!m_application.AddRule(std::move(rule))) {
        ShowError(L"添加文件夹失败", m_application.LastRuleError());
        return;
    }
    RefreshListView(true);
}

void MainFrame::DeleteApplication(const std::wstring& path) {
    const std::wstring message = L"确定删除规则？\n\n" + path;
    if (MessageBox(message.c_str(), L"删除应用", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return;
    }

    if (!m_application.RemoveRule(path)) {
        ShowError(L"删除应用失败", m_application.LastRuleError());
        return;
    }
    RefreshListView(true);
}

void MainFrame::ConfigureApplication(const std::wstring& path) {
    const auto rule = std::find_if(
        m_application.Rules().begin(), m_application.Rules().end(),
        [&path](const AppRule& candidate) { return PathUtils::SamePath(candidate.path, path); });
    if (rule == m_application.Rules().end()) {
        ShowError(L"配置规则失败", L"找不到所选应用规则");
        return;
    }

    HotkeyPolicyDialog dialog(rule->hotkeyPolicy, rule->enabled);
    if (dialog.DoModal(m_hWnd) != IDOK) {
        return;
    }
    if (!m_application.SetRuleSettings(path, dialog.Enabled(), dialog.Policy())) {
        ShowError(L"保存规则配置失败", m_application.LastRuleError());
        return;
    }
    RefreshListView(true);
}

void MainFrame::UpdateAutoStart() {
    const bool enabled = !m_application.AutoStartEnabled();
    std::wstring error;
    if (!m_application.SetAutoStartEnabled(enabled, error)) {
        m_application.Log().Error(error);
        ShowError(L"设置开机启动失败", error);
        return;
    }
    UISetCheck(ID_MAIN_AUTOSTART, enabled);
    UIUpdateMenuBar(FALSE, TRUE);
}

bool MainFrame::IsActionableStatus(AppStatus status) noexcept {
    switch (status) {
        case AppStatus::RestartRequired:
        case AppStatus::PartiallyBlocked:
        case AppStatus::InjectionFailed:
        case AppStatus::PathMissing:
        case AppStatus::MonitoringUnavailable:
            return true;
        case AppStatus::Waiting:
        case AppStatus::Injecting:
        case AppStatus::InjectionPending:
        case AppStatus::Blocked:
        case AppStatus::Disabled:
        default:
            return false;
    }
}

void MainFrame::NotifyActionableStates() {
    const std::vector<RuntimeRuleState> states = m_application.RuntimeStates();
    std::unordered_map<std::wstring, AppStatus> current;
    for (const RuntimeRuleState& state : states) {
        if (!IsActionableStatus(state.status)) {
            continue;
        }
        current[state.path] = state.status;
        if (!m_trayIcon.IsAdded()) {
            continue;
        }

        const auto previous = m_notifiedActionableStates.find(state.path);
        if (previous != m_notifiedActionableStates.end() && previous->second == state.status) {
            continue;
        }

        std::wstring message = AppStatusText(state.status);
        if (!state.detail.empty()) {
            message += L"：" + state.detail;
        }
        std::wstring title = L"应用状态提醒";
        const auto rule = std::find_if(
            m_application.Rules().begin(), m_application.Rules().end(),
            [&state](const AppRule& candidate) {
                return PathUtils::SamePath(candidate.path, state.path);
            });
        if (rule != m_application.Rules().end() && !rule->displayName.empty()) {
            title = rule->displayName;
        }
        ShowTrayNotification(title, message);
        m_notifiedActionableStates[state.path] = state.status;
    }

    for (auto iterator = m_notifiedActionableStates.begin();
         iterator != m_notifiedActionableStates.end();) {
        if (current.find(iterator->first) == current.end()) {
            iterator = m_notifiedActionableStates.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void MainFrame::ShowTrayNotification(const std::wstring& title,
                                     const std::wstring& message) const {
    m_trayIcon.ShowNotification(title, message);
}

void MainFrame::ShowError(const wchar_t* title, const std::wstring& message) {
    MessageBox(message.empty() ? L"未知错误" : message.c_str(), title, MB_OK | MB_ICONERROR);
}

void MainFrame::ShowErrorCode(const wchar_t* title, HRESULT result) {
    wchar_t errorCode[32]{};
    swprintf_s(errorCode, L"错误码：0x%08X", static_cast<unsigned int>(result));
    ShowError(title, errorCode);
}
