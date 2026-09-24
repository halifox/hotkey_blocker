#include "WindowsTarget.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlstr.h>
#include <atlframe.h>
#include <atlgdi.h>
#include <atlmisc.h>
#include <atldlgs.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "ApplicationController.h"
#include "ApplicationListView.h"
#include "HotkeyPolicy.h"
#include "HotkeyPolicyDialog.h"
#include "PathUtils.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "Version.h"
#include "resource.h"

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStateChangedMessage = WM_APP + 2;
constexpr UINT kUpdateCheckCompletedMessage = WM_APP + 3;
UINT kTaskbarCreatedMessage = 0;

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

class MainWindow final : public ATL::CDialogImpl<MainWindow>,
                         public WTL::CUpdateUI<MainWindow> {
public:
    enum { IDD = IDD_MAIN_WINDOW };

    explicit MainWindow(bool startHidden) : m_startHidden(startHidden) {}

    ~MainWindow() = default;

    bool ShouldStartHidden() const noexcept {
        return m_startHidden && m_trayIcon.IsAdded();
    }

    BEGIN_UPDATE_UI_MAP(MainWindow)
        UPDATE_ELEMENT(ID_MAIN_AUTOSTART, UPDUI_MENUBAR)
    END_UPDATE_UI_MAP()

    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(kStateChangedMessage, OnStateChanged)
        MESSAGE_HANDLER(kUpdateCheckCompletedMessage, OnUpdateCheckCompleted)
        MESSAGE_HANDLER(kTaskbarCreatedMessage, OnTaskbarCreated)
        COMMAND_ID_HANDLER(ID_MAIN_ADD_EXECUTABLE, OnAddExecutable)
        COMMAND_ID_HANDLER(ID_MAIN_ADD_FOLDER, OnAddFolder)
        COMMAND_ID_HANDLER(ID_MAIN_AUTOSTART, OnToggleAutoStart)
        COMMAND_ID_HANDLER(ID_TRAY_CHECK_UPDATES, OnCheckUpdates)
        COMMAND_ID_HANDLER(ID_TRAY_SHOW, OnShowFromTray)
        COMMAND_ID_HANDLER(ID_TRAY_EXIT, OnExit)
        REFLECT_NOTIFICATIONS()
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        CHAIN_MSG_MAP(WTL::CUpdateUI<MainWindow>)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        const std::wstring title = L"Hotkey Blocker - " + std::wstring(hkb::version::kString);
        SetWindowText(title.c_str());
        m_applicationList.SubclassWindow(GetDlgItem(IDC_APP_LIST));
        m_applicationList.SetActionHandler(
            [this](const std::wstring& path, ApplicationListAction action) {
                if (action == ApplicationListAction::Configure) {
                    ConfigureApplication(path);
                } else {
                    DeleteApplication(path);
                }
            });
        m_applicationList.Initialize();

        if (!LoadWindowIcons()) {
            m_application.Log().Error(L"加载应用图标失败");
        }

        const ApplicationStartupResult startup = m_application.Initialize(
            [this] { QueueStateRefresh(); });
        if (!startup.rulesLoaded) {
            ShowError(L"加载配置失败", startup.ruleError);
            m_initializationFailed = true;
            PostMessage(WM_CLOSE, 0, 0);
            return TRUE;
        }

        if (!startup.startupSettingLoaded) {
            ShowError(L"读取开机启动设置失败", startup.startupSettingError);
        }
        UIAddMenuBar(m_hWnd);
        UISetCheck(ID_MAIN_AUTOSTART, startup.autoStartEnabled, TRUE);
        UIUpdateMenuBar(TRUE, TRUE);

        if (!startup.blockerStart) {
            ShowError(L"启动运行服务失败", startup.blockerStart.error);
        }
        RefreshListView(true);

        if (!m_trayIcon.Add(m_hWnd, kTrayMessage, m_smallIcon, L"Hotkey Blocker")) {
            m_application.Log().Error(L"创建系统托盘图标失败，窗口将保持可见");
            ShowError(L"托盘初始化失败",
                      L"无法创建系统托盘图标，程序将保持窗口可见；关闭窗口将退出程序。");
        }
        NotifyActionableStates();
        if (ShouldStartHidden()) {
            ShowWindow(SW_HIDE);
        }
        if (m_trayIcon.IsAdded()) {
            StartUpdateCheck(false);
        }
        return TRUE;
    }

    LRESULT OnTaskbarCreated(UINT, WPARAM, LPARAM, BOOL& handled) {
        if (kTaskbarCreatedMessage == 0) {
            handled = FALSE;
            return 0;
        }
        handled = TRUE;
        RestoreTrayIcon();
        return 0;
    }

    LRESULT OnTrayMessage(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
        handled = TRUE;
        switch (LOWORD(lParam)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                ShowFromTray();
                break;
            case WM_RBUTTONUP:
                ShowTrayMenu();
                break;
            case NIN_BALLOONUSERCLICK:
                OpenPendingRelease();
                break;
            default:
                break;
        }
        return 0;
    }

    LRESULT OnUpdateCheckCompleted(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
        handled = TRUE;
        std::unique_ptr<UpdateCheckResult> result(
            reinterpret_cast<UpdateCheckResult*>(lParam));
        if (!result) {
            return 0;
        }

        m_updateCheckRunning = false;

        if (!result->error.empty()) {
            m_application.Log().Error(L"检查更新失败：" + result->error);
            if (m_updateCheckInteractive) {
                ShowError(L"检查更新失败", result->error);
            }
            m_updateCheckInteractive = false;
            return 0;
        }

        if (result->updateAvailable) {
            m_pendingReleaseUrl = result->releaseUrl;
            if (m_updateCheckInteractive) {
                const std::wstring message =
                    L"发现新版本 " + result->latestVersion + L"。\n当前版本：" +
                    result->currentVersion + L"\n\n是否打开下载页面？";
                if (MessageBox(message.c_str(), L"发现新版本",
                               MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                    OpenPendingRelease();
                }
            } else {
                ShowTrayNotification(L"发现新版本",
                                     L"Hotkey Blocker " + result->latestVersion +
                                         L" 已发布，点击通知打开下载页面。");
            }
        } else if (m_updateCheckInteractive) {
            const std::wstring message = L"当前已经是最新版本（" + result->currentVersion + L"）。";
            MessageBox(message.c_str(), L"检查更新", MB_OK | MB_ICONINFORMATION);
        }

        m_updateCheckInteractive = false;
        return 0;
    }

    LRESULT OnStateChanged(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        for (;;) {
            m_stateNotificationPosted.store(false, std::memory_order_release);
            RefreshListView(false);
            NotifyActionableStates();
            if (!m_stateNotificationPosted.load(std::memory_order_acquire)) {
                break;
            }
        }
        return 0;
    }

    LRESULT OnAddExecutable(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        AddExecutableApplication();
        return 0;
    }

    LRESULT OnAddFolder(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        AddFolderApplication();
        return 0;
    }

    LRESULT OnToggleAutoStart(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        UpdateAutoStart();
        return 0;
    }

    LRESULT OnCheckUpdates(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        StartUpdateCheck(true);
        return 0;
    }

    LRESULT OnShowFromTray(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        ShowFromTray();
        return 0;
    }

    LRESULT OnExit(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        ExitApplication();
        return 0;
    }

    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        if (m_initializationFailed) {
            DestroyWindow();
            return 0;
        }
        if (!m_trayIcon.IsAdded()) {
            DestroyWindow();
            return 0;
        }
        ShowWindow(SW_HIDE);
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        m_updateChecker.Stop();
        DrainUpdateCheckMessages();
        m_application.SetStateChangedCallback({});
        m_trayIcon.Remove();
        m_application.Stop();
        DestroyWindowIcons();
        m_application.Log().Info(L"程序退出");
        PostQuitMessage(0);
        return 0;
    }

    void RestoreTrayIcon() {
        if (m_hWnd == nullptr) {
            return;
        }
        if (!m_trayIcon.Restore()) {
            m_application.Log().Error(L"Explorer 重启后重新创建系统托盘图标失败");
            return;
        }
        NotifyActionableStates();
    }

    void ShowFromTray() {
        if (IsIconic()) {
            ShowWindow(SW_RESTORE);
        }
        ShowWindow(SW_SHOWNORMAL);
        ::SetForegroundWindow(m_hWnd);
        RefreshListView(true);
    }

    void QueueStateRefresh() {
        if (m_hWnd == nullptr) {
            return;
        }
        if (!m_stateNotificationPosted.exchange(true, std::memory_order_acq_rel)) {
            if (!PostMessage(kStateChangedMessage, 0, 0)) {
                m_stateNotificationPosted.store(false, std::memory_order_release);
            }
        }
    }

    void ShowTrayMenu() {
        CMenu menu;
        if (!menu.LoadMenu(MAKEINTRESOURCEW(IDR_TRAY_MENU))) {
            return;
        }
        CMenuHandle popup = menu.GetSubMenu(0);
        if (!popup.IsNull()) {
            CPoint cursor;
            ::GetCursorPos(&cursor);
            ::SetForegroundWindow(m_hWnd);
            popup.TrackPopupMenu(TPM_RIGHTBUTTON, cursor.x, cursor.y, m_hWnd, nullptr);
            PostMessage(WM_NULL, 0, 0);
        }
    }

    void ExitApplication() {
        DestroyWindow();
    }

    void StartUpdateCheck(bool interactive) {
        if (m_updateCheckRunning) {
            if (interactive) {
                MessageBox(L"版本检查正在进行，请稍候。", L"检查更新",
                           MB_OK | MB_ICONINFORMATION);
            }
            return;
        }

        m_updateCheckInteractive = interactive;
        m_updateCheckRunning = true;

        const HWND window = m_hWnd;
        if (m_updateChecker.Start([window](UpdateCheckResult result) {
                auto* payload = new (std::nothrow) UpdateCheckResult(std::move(result));
                if (payload == nullptr) {
                    return;
                }
                CWindow target(window);
                if (!target.PostMessage(kUpdateCheckCompletedMessage, 0,
                                        reinterpret_cast<LPARAM>(payload))) {
                    delete payload;
                    return;
                }
            })) {
            return;
        }

        m_updateCheckRunning = false;
        if (interactive) {
            ShowError(L"检查更新失败", L"无法启动版本检查线程。");
        }
        m_updateCheckInteractive = false;
    }

    void OpenPendingRelease() {
        if (m_pendingReleaseUrl.empty()) {
            return;
        }

        const std::wstring url = m_pendingReleaseUrl;
        m_pendingReleaseUrl.clear();
        constexpr wchar_t kTrustedPrefix[] =
            L"https://github.com/halifox/hotkey_blocker/releases/";
        if (url.rfind(kTrustedPrefix, 0) != 0) {
            m_application.Log().Error(L"拒绝打开非预期的 Release 地址：" + url);
            return;
        }

        const HINSTANCE result =
            ShellExecuteW(m_hWnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            m_application.Log().Error(L"无法打开 Release 页面：" + url);
        }
    }

    void DrainUpdateCheckMessages() {
        MSG message{};
        while (::PeekMessageW(&message, m_hWnd, kUpdateCheckCompletedMessage,
                              kUpdateCheckCompletedMessage, PM_REMOVE)) {
            delete reinterpret_cast<UpdateCheckResult*>(message.lParam);
        }
    }

    bool LoadWindowIcons() {
        m_largeIcon.LoadIcon(MAKEINTRESOURCEW(IDI_APP_ICON), GetSystemMetrics(SM_CXICON),
                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
        m_smallIcon.LoadIcon(MAKEINTRESOURCEW(IDI_APP_ICON), GetSystemMetrics(SM_CXSMICON),
                             GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        if (m_largeIcon.IsNull() || m_smallIcon.IsNull()) {
            DestroyWindowIcons();
            return false;
        }

        SetIcon(m_largeIcon, TRUE);
        SetIcon(m_smallIcon, FALSE);
        return true;
    }

    void DestroyWindowIcons() {
        m_largeIcon = nullptr;
        m_smallIcon = nullptr;
    }

    void RefreshListView(bool force) {
        if (!m_applicationList.IsWindow()) {
            return;
        }
        m_applicationList.SetRows(BuildDisplayRows(), force);
    }

    std::vector<ApplicationListRow> BuildDisplayRows() const {
        const std::vector<AppRule>& rules = m_application.Rules();
        const std::vector<RuntimeRuleState> states = m_application.RuntimeStates();
        std::vector<ApplicationListRow> rows;
        rows.reserve(rules.size());

        for (const AppRule& rule : rules) {
            ApplicationListRow row;
            row.path = rule.path;
            row.enabled = rule.enabled;
            row.status = AppStatusText(AppStatus::Waiting);
            row.detail = HotkeyPolicySummary(rule.hotkeyPolicy);
            if (rule.kind == RuleKind::Directory) {
                row.detail += L"；拦截文件夹内所有 EXE";
            }

            const auto state = std::find_if(
                states.begin(), states.end(), [&rule](const RuntimeRuleState& candidate) {
                    return PathUtils::SamePath(candidate.path, rule.path);
                });
            if (state != states.end()) {
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
    bool PickApplicationPath(bool folder, std::wstring& path) {
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

    void AddExecutableApplication() {
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

    void AddFolderApplication() {
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

    void DeleteApplication(const std::wstring& path) {
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

    void ConfigureApplication(const std::wstring& path) {
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

    void UpdateAutoStart() {
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

    static bool IsActionableStatus(AppStatus status) {
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

    void NotifyActionableStates() {
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
            if (previous != m_notifiedActionableStates.end() &&
                previous->second == state.status) {
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

    void ShowTrayNotification(const std::wstring& title, const std::wstring& message) const {
        m_trayIcon.ShowNotification(title, message);
    }

    void ShowError(const wchar_t* title, const std::wstring& message) {
        MessageBox(message.empty() ? L"未知错误" : message.c_str(), title, MB_OK | MB_ICONERROR);
    }

    void ShowErrorCode(const wchar_t* title, HRESULT result) {
        std::wstring message = L"错误码：0x" + std::to_wstring(static_cast<unsigned long>(result));
        ShowError(title, message);
    }

    bool m_startHidden = false;
    bool m_initializationFailed = false;
    ApplicationListView m_applicationList;
    CIcon m_largeIcon;
    CIcon m_smallIcon;
    ApplicationController m_application;
    TrayIcon m_trayIcon;
    UpdateChecker m_updateChecker;
    std::wstring m_pendingReleaseUrl;
    bool m_updateCheckRunning = false;
    bool m_updateCheckInteractive = false;
    std::unordered_map<std::wstring, AppStatus> m_notifiedActionableStates;
    std::atomic_bool m_stateNotificationPosted = false;
};

int RunMainWindow(CMessageLoop& messageLoop, bool startHidden) {
    kTaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    MainWindow window(startHidden);
    if (window.Create(nullptr) == nullptr) {
        return 1;
    }

    window.CenterWindow();
    window.ShowWindow(window.ShouldStartHidden() ? SW_HIDE : SW_SHOWNORMAL);

    return messageLoop.Run();
}
