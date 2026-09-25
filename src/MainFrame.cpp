#include "WindowsTarget.h"

#include <windows.h>
#include <shellapi.h>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlstr.h>
#include <atlframe.h>
#include <atlgdi.h>
#include <atlmisc.h>
#include <atluser.h>

#include <memory>
#include <new>
#include <string>
#include <utility>

#include "MainFrame.h"
#include "HotkeyPolicyDialog.h"
#include "Version.h"

MainFrame::MainFrame(bool startHidden) noexcept : m_startHidden(startHidden) {}

UINT MainFrame::TaskbarCreatedMessage() noexcept {
    static const UINT message = ::RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

bool MainFrame::Initialize() {
    const std::wstring title = L"Hotkey Blocker - " + std::wstring(hkb::version::kString);
    SetWindowText(title.c_str());

    if (!LoadWindowIcons()) {
        m_application.Log().Error(L"加载应用图标失败");
    }

    const ApplicationStartupResult startup = m_application.Initialize(
        [this] { QueueStateRefresh(); });
    if (!startup.rulesLoaded) {
        ShowError(L"加载配置失败", startup.ruleError);
        m_initializationFailed = true;
        return false;
    }

    if (!startup.startupSettingLoaded) {
        ShowError(L"读取登录时启动设置失败", startup.startupSettingError);
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
    if (m_trayIcon.IsAdded()) {
        StartUpdateCheck(false);
    }
    return true;
}

bool MainFrame::ShouldStartHidden() const noexcept {
    return m_startHidden && m_trayIcon.IsAdded();
}

BOOL MainFrame::PreTranslateMessage(MSG* message) {
    // Resource accelerators take precedence over child-dialog keyboard navigation.
    if (WTL::CFrameWindowImpl<MainFrame>::PreTranslateMessage(message)) {
        return TRUE;
    }
    return m_mainView.PreTranslateMessage(message);
}

LRESULT MainFrame::OnCreate(UINT, WPARAM, LPARAM, BOOL& handled) {
    const HWND viewWindow = m_mainView.Create(m_hWnd);
    if (viewWindow == nullptr || !m_mainView.IsReady()) {
        if (viewWindow != nullptr && ::IsWindow(viewWindow)) {
            m_mainView.DestroyWindow();
        }
        handled = TRUE;
        return -1;
    }

    m_hWndClient = viewWindow;
    m_mainView.SetActionHandler(
        [this](const std::wstring& path, ApplicationListAction action) {
            HandleListAction(path, action);
        });
    handled = TRUE;
    return 0;
}

LRESULT MainFrame::OnSetFocus(UINT, WPARAM, LPARAM, BOOL& handled) {
    if (m_mainView.IsReady()) {
        m_mainView.FocusList();
        handled = TRUE;
        return 0;
    }
    handled = FALSE;
    return 0;
}

LRESULT MainFrame::OnTaskbarCreated(UINT, WPARAM, LPARAM, BOOL& handled) {
    if (TaskbarCreatedMessage() == 0) {
        handled = FALSE;
        return 0;
    }
    handled = TRUE;
    RestoreTrayIcon();
    return 0;
}

LRESULT MainFrame::OnTrayMessage(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
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

LRESULT MainFrame::OnUpdateCheckCompleted(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
    handled = TRUE;
    std::unique_ptr<UpdateCheckResult> result(reinterpret_cast<UpdateCheckResult*>(lParam));
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

LRESULT MainFrame::OnStateChanged(UINT, WPARAM, LPARAM, BOOL& handled) {
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

LRESULT MainFrame::OnAddExecutable(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    AddExecutableApplication();
    return 0;
}

LRESULT MainFrame::OnAddFolder(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    AddFolderApplication();
    return 0;
}

LRESULT MainFrame::OnToggleAutoStart(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    UpdateAutoStart();
    return 0;
}

LRESULT MainFrame::OnCheckUpdates(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    StartUpdateCheck(true);
    return 0;
}

LRESULT MainFrame::OnShowFromTray(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    ShowFromTray();
    return 0;
}

LRESULT MainFrame::OnExit(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    ExitApplication();
    return 0;
}

LRESULT MainFrame::OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
    handled = TRUE;
    if (m_initializationFailed || !m_trayIcon.IsAdded()) {
        DestroyWindow();
        return 0;
    }
    ShowWindow(SW_HIDE);
    return 0;
}

LRESULT MainFrame::OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
    handled = TRUE;
    if (m_shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        return 0;
    }

    m_updateChecker.Stop();
    DrainUpdateCheckMessages();
    m_application.SetStateChangedCallback({});
    m_mainView.SetActionHandler({});
    m_trayIcon.Remove();
    m_application.Stop();
    DestroyWindowIcons();
    m_application.Log().Info(L"程序退出");
    ::PostQuitMessage(m_initializationFailed ? 1 : 0);
    return 0;
}

void MainFrame::RestoreTrayIcon() {
    if (m_hWnd == nullptr) {
        return;
    }
    if (!m_trayIcon.Restore()) {
        m_application.Log().Error(L"Explorer 重启后重新创建系统托盘图标失败");
        return;
    }
    NotifyActionableStates();
}

void MainFrame::ShowFromTray() {
    if (IsIconic()) {
        ShowWindow(SW_RESTORE);
    }
    ShowWindow(SW_SHOWNORMAL);
    ::SetForegroundWindow(m_hWnd);
    RefreshListView(true);
}

void MainFrame::QueueStateRefresh() {
    if (m_shuttingDown.load(std::memory_order_acquire) || m_hWnd == nullptr) {
        return;
    }
    if (!m_stateNotificationPosted.exchange(true, std::memory_order_acq_rel)) {
        if (!PostMessage(kStateChangedMessage, 0, 0)) {
            m_stateNotificationPosted.store(false, std::memory_order_release);
        }
    }
}

void MainFrame::ShowTrayMenu() {
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

void MainFrame::ExitApplication() {
    DestroyWindow();
}

void MainFrame::StartUpdateCheck(bool interactive) {
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
            if (!::PostMessageW(window, kUpdateCheckCompletedMessage, 0,
                                reinterpret_cast<LPARAM>(payload))) {
                delete payload;
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

void MainFrame::OpenPendingRelease() {
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
        ::ShellExecuteW(m_hWnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        m_application.Log().Error(L"无法打开 Release 页面：" + url);
    }
}

void MainFrame::DrainUpdateCheckMessages() {
    MSG message{};
    while (::PeekMessageW(&message, m_hWnd, kUpdateCheckCompletedMessage,
                          kUpdateCheckCompletedMessage, PM_REMOVE)) {
        delete reinterpret_cast<UpdateCheckResult*>(message.lParam);
    }
}

bool MainFrame::LoadWindowIcons() {
    m_largeIcon.LoadIcon(MAKEINTRESOURCEW(IDR_MAINFRAME), GetSystemMetrics(SM_CXICON),
                         GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    m_smallIcon.LoadIcon(MAKEINTRESOURCEW(IDR_MAINFRAME), GetSystemMetrics(SM_CXSMICON),
                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (m_largeIcon.IsNull() || m_smallIcon.IsNull()) {
        DestroyWindowIcons();
        return false;
    }

    SetIcon(m_largeIcon, TRUE);
    SetIcon(m_smallIcon, FALSE);
    return true;
}

void MainFrame::DestroyWindowIcons() noexcept {
    m_largeIcon = nullptr;
    m_smallIcon = nullptr;
}

int RunMainFrame(CMessageLoop& messageLoop, bool startHidden) {
    constexpr int kDefaultWindowWidth = 640;
    constexpr int kDefaultWindowHeight = 420;

    MainFrame frame(startHidden);
    RECT defaultWindowRect{0, 0, kDefaultWindowWidth, kDefaultWindowHeight};
    if (frame.CreateEx(nullptr, &defaultWindowRect) == nullptr) {
        return 1;
    }

    messageLoop.AddMessageFilter(&frame);
    frame.CenterWindow();
    if (frame.Initialize()) {
        frame.ShowWindow(frame.ShouldStartHidden() ? SW_HIDE : SW_SHOWNORMAL);
    } else if (frame.IsWindow()) {
        frame.DestroyWindow();
    }

    const int exitCode = messageLoop.Run();
    messageLoop.RemoveMessageFilter(&frame);
    return exitCode;
}
