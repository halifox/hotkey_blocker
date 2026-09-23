#include "WindowsTarget.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
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

#include "BlockerService.h"
#include "HotkeyPolicy.h"
#include "HotkeyPolicyDialog.h"
#include "Logger.h"
#include "PathUtils.h"
#include "RuleManager.h"
#include "StartupManager.h"
#include "UpdateChecker.h"
#include "Version.h"
#include "resource.h"

extern CAppModule _Module;

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStateChangedMessage = WM_APP + 2;
constexpr UINT kUpdateCheckCompletedMessage = WM_APP + 3;
constexpr int kRuleStateColumn = 1;
constexpr int kRuntimeStatusColumn = 2;
constexpr int kConfigureActionColumn = 3;
constexpr int kDeleteActionColumn = 4;
constexpr int kEnabledColumnWidth = 60;
constexpr int kStatusColumnWidth = 190;
constexpr int kConfigureColumnWidth = 74;
constexpr int kDeleteColumnWidth = 74;
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

class MainWindow final : public ATL::CDialogImpl<MainWindow>, public CMessageFilter {
public:
    enum { IDD = IDD_MAIN_WINDOW };
    enum { kListViewMessageMap = 1 };

    explicit MainWindow(bool startHidden)
        : m_startHidden(startHidden),
          m_listView(this, kListViewMessageMap),
          m_blockerService(&m_logger) {}

    ~MainWindow() = default;

    bool ShouldStartHidden() const noexcept {
        return m_startHidden && m_trayIconAdded;
    }

    BOOL PreTranslateMessage(MSG* message) override {
        if (message != nullptr && kTaskbarCreatedMessage != 0 &&
            message->message == kTaskbarCreatedMessage) {
            RestoreTrayIcon();
            return TRUE;
        }
        return FALSE;
    }

    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(kStateChangedMessage, OnStateChanged)
        MESSAGE_HANDLER(kUpdateCheckCompletedMessage, OnUpdateCheckCompleted)
        COMMAND_ID_HANDLER(ID_MAIN_ADD_EXECUTABLE, OnAddExecutable)
        COMMAND_ID_HANDLER(ID_MAIN_ADD_FOLDER, OnAddFolder)
        COMMAND_ID_HANDLER(ID_MAIN_AUTOSTART, OnToggleAutoStart)
        COMMAND_ID_HANDLER(ID_TRAY_CHECK_UPDATES, OnCheckUpdates)
        COMMAND_ID_HANDLER(ID_TRAY_SHOW, OnShowFromTray)
        COMMAND_ID_HANDLER(ID_TRAY_EXIT, OnExit)
        NOTIFY_HANDLER(IDC_APP_LIST, NM_CUSTOMDRAW, OnListCustomDraw)
        NOTIFY_HANDLER(IDC_APP_LIST, NM_CLICK, OnListClick)
        NOTIFY_HANDLER(IDC_APP_LIST, LVN_ITEMCHANGED, OnListItemChanged)
        NOTIFY_HANDLER(IDC_APP_LIST, LVN_GETINFOTIPW, OnListGetInfoTip)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        ALT_MSG_MAP(kListViewMessageMap)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnListViewMouseMoveMessage)
        MESSAGE_HANDLER(WM_MOUSELEAVE, OnListViewMouseLeaveMessage)
        MESSAGE_HANDLER(WM_SETCURSOR, OnListViewSetCursor)
        MESSAGE_HANDLER(WM_VSCROLL, OnListViewUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_HSCROLL, OnListViewUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnListViewUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_KEYDOWN, OnListViewUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_KEYUP, OnListViewUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_NCDESTROY, OnListViewNcDestroy)
    END_MSG_MAP()

private:
    struct DisplayRow {
        std::wstring path;
        bool enabled = false;
        std::wstring status;
        std::wstring detail;
        int imageIndex = -1;
    };

    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        const std::wstring title = L"Hotkey Blocker - " + std::wstring(hkb::version::kString);
        SetWindowText(title.c_str());
        m_listView.SubclassWindow(GetDlgItem(IDC_APP_LIST));
        InitializeListView();

        if (!LoadWindowIcons()) {
            m_logger.Error(L"加载应用图标失败");
        }

        m_logger.Info(L"程序启动");
        if (!m_ruleManager.Load()) {
            m_logger.Error(L"加载规则失败：" + m_ruleManager.LastError());
            ShowError(L"加载配置失败", m_ruleManager.LastError());
            m_initializationFailed = true;
            PostMessage(WM_CLOSE, 0, 0);
            return TRUE;
        }
        m_logger.Info(L"加载规则：" + std::to_wstring(m_ruleManager.Rules().size()) + L" 条");

        bool autoStartEnabled = false;
        std::wstring startupError;
        if (!m_startupManager.GetEnabled(autoStartEnabled, startupError)) {
            m_logger.Error(startupError);
            ShowError(L"读取开机启动设置失败", startupError);
        }
        if (CMenuHandle menu = GetMenu(); !menu.IsNull()) {
            menu.CheckMenuItem(ID_MAIN_AUTOSTART,
                               MF_BYCOMMAND | (autoStartEnabled ? MF_CHECKED : MF_UNCHECKED));
        }

        m_blockerService.SetStateChangedCallback([this] { QueueStateRefresh(); });
        const BlockerServiceStartResult serviceStart =
            m_blockerService.Start(m_ruleManager.Rules());
        if (!serviceStart) {
            m_logger.Error(L"运行服务启动失败：" + serviceStart.error);
            ShowError(L"启动运行服务失败", serviceStart.error);
        }
        RefreshListView(true);

        m_trayIconAdded = AddTrayIcon();
        if (!m_trayIconAdded) {
            m_logger.Error(L"创建系统托盘图标失败，窗口将保持可见");
            ShowError(L"托盘初始化失败",
                      L"无法创建系统托盘图标，程序将保持窗口可见；关闭窗口将退出程序。");
        }
        NotifyActionableStates();
        if (ShouldStartHidden()) {
            ShowWindow(SW_HIDE);
        }
        if (m_trayIconAdded) {
            StartUpdateCheck(false);
        }
        return TRUE;
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
            m_logger.Error(L"检查更新失败：" + result->error);
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

    LRESULT OnListCustomDraw(int, LPNMHDR notification, BOOL& handled) {
        auto* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(notification);
        const DWORD drawStage = customDraw->nmcd.dwDrawStage;
        if (drawStage == CDDS_PREPAINT) {
            handled = TRUE;
            return CDRF_NOTIFYITEMDRAW;
        }
        if (drawStage == CDDS_ITEMPREPAINT) {
            handled = TRUE;
            return CDRF_NOTIFYSUBITEMDRAW;
        }
        if (drawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) &&
            (customDraw->iSubItem == kConfigureActionColumn ||
             customDraw->iSubItem == kDeleteActionColumn)) {
            CRect actionRect;
            const int rowIndex = static_cast<int>(customDraw->nmcd.dwItemSpec);
            if (GetActionHitRect(rowIndex, customDraw->iSubItem, actionRect)) {
                const bool selected = (customDraw->nmcd.uItemState & CDIS_SELECTED) != 0;
                const bool focusedSelection = selected && ::GetFocus() == m_listView;
                const bool hovered = rowIndex == m_hoveredActionRow &&
                                     customDraw->iSubItem == m_hoveredActionColumn;
                if (focusedSelection) {
                    customDraw->clrText = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
                } else if (customDraw->iSubItem == kConfigureActionColumn) {
                    customDraw->clrText = ::GetSysColor(COLOR_HOTLIGHT);
                } else {
                    HIGHCONTRASTW highContrast{};
                    highContrast.cbSize = sizeof(highContrast);
                    const bool highContrastEnabled =
                        !::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(highContrast),
                                                 &highContrast, 0) ||
                        (highContrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
                    customDraw->clrText = highContrastEnabled
                                              ? ::GetSysColor(COLOR_WINDOWTEXT)
                                              : RGB(180, 35, 24);
                }
                if (hovered && !m_actionHoverFont.IsNull()) {
                    ::SelectObject(customDraw->nmcd.hdc, m_actionHoverFont);
                }
            }
            handled = TRUE;
            return CDRF_NEWFONT;
        }
        handled = TRUE;
        return CDRF_DODEFAULT;
    }

    LRESULT OnListClick(int, LPNMHDR, BOOL& handled) {
        int rowIndex = -1;
        int subItemIndex = -1;
        if (GetActionHitAtCursor(rowIndex, subItemIndex)) {
            const std::wstring path = m_renderedRows[static_cast<std::size_t>(rowIndex)].path;
            if (subItemIndex == kConfigureActionColumn) {
                ConfigureApplication(path);
            } else {
                DeleteApplication(path);
            }
            handled = TRUE;
            return 0;
        }
        handled = FALSE;
        return 0;
    }

    LRESULT OnListItemChanged(int, LPNMHDR, BOOL& handled) {
        UpdateHoveredActionFromCursor();
        handled = FALSE;
        return 0;
    }

    LRESULT OnListGetInfoTip(int, LPNMHDR notification, BOOL& handled) {
        const auto* infoTip = reinterpret_cast<const NMLVGETINFOTIPW*>(notification);
        if (infoTip->iItem >= 0 && infoTip->iItem < static_cast<int>(m_renderedRows.size()) &&
            infoTip->pszText != nullptr && infoTip->cchTextMax > 0) {
            const DisplayRow& row = m_renderedRows[static_cast<std::size_t>(infoTip->iItem)];
            std::wstring tip = row.detail;
            if (!tip.empty() && !row.path.empty()) {
                tip += L"\n";
            }
            if (!row.path.empty()) {
                tip += row.path;
            }
            wcsncpy_s(infoTip->pszText, infoTip->cchTextMax, tip.c_str(), _TRUNCATE);
        }
        handled = TRUE;
        return 0;
    }

    bool GetActionHitRect(int rowIndex, int subItemIndex, CRect& rect) const {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(m_renderedRows.size()) ||
            (subItemIndex != kConfigureActionColumn && subItemIndex != kDeleteActionColumn) ||
            !m_listView.GetSubItemRect(rowIndex, subItemIndex, LVIR_BOUNDS, &rect)) {
            return false;
        }
        rect.InflateRect(-4, -2);
        return rect.right > rect.left && rect.bottom > rect.top;
    }

    bool GetActionHitAtPoint(CPoint point, int& rowIndex, int& subItemIndex) const {
        if (!m_listView.IsWindow()) {
            return false;
        }

        LVHITTESTINFO hit{};
        hit.pt = point;
        const int hitRow = m_listView.SubItemHitTest(&hit);
        if (hitRow < 0) {
            return false;
        }

        CRect actionRect;
        if (!GetActionHitRect(hitRow, hit.iSubItem, actionRect) ||
            !actionRect.PtInRect(point)) {
            return false;
        }
        rowIndex = hitRow;
        subItemIndex = hit.iSubItem;
        return true;
    }

    bool GetActionHitAtCursor(int& rowIndex, int& subItemIndex) const {
        if (!m_listView.IsWindow()) {
            return false;
        }

        CPoint point;
        if (!::GetCursorPos(&point) || !m_listView.ScreenToClient(&point)) {
            return false;
        }
        return GetActionHitAtPoint(point, rowIndex, subItemIndex);
    }

    void InvalidateActionCell(int rowIndex, int subItemIndex) {
        CRect rect;
        if (GetActionHitRect(rowIndex, subItemIndex, rect)) {
            m_listView.InvalidateRect(&rect, FALSE);
        }
    }

    void SetHoveredAction(int rowIndex, int subItemIndex) {
        if (rowIndex == m_hoveredActionRow && subItemIndex == m_hoveredActionColumn) {
            return;
        }
        InvalidateActionCell(m_hoveredActionRow, m_hoveredActionColumn);
        m_hoveredActionRow = rowIndex;
        m_hoveredActionColumn = subItemIndex;
        InvalidateActionCell(m_hoveredActionRow, m_hoveredActionColumn);
    }

    void UpdateHoveredActionFromCursor() {
        CPoint point;
        int rowIndex = -1;
        int subItemIndex = -1;
        if (::GetCursorPos(&point) && m_listView.ScreenToClient(&point) &&
            GetActionHitAtPoint(point, rowIndex, subItemIndex)) {
            SetHoveredAction(rowIndex, subItemIndex);
        } else {
            SetHoveredAction(-1, -1);
        }
    }

    void TrackListViewMouseMove(LPARAM lParam) {
        if (!m_trackingMouseLeave) {
            TRACKMOUSEEVENT trackMouse{};
            trackMouse.cbSize = sizeof(trackMouse);
            trackMouse.dwFlags = TME_LEAVE;
            trackMouse.hwndTrack = m_listView;
            m_trackingMouseLeave = ::TrackMouseEvent(&trackMouse) != FALSE;
        }

        const CPoint point{static_cast<SHORT>(LOWORD(lParam)),
                           static_cast<SHORT>(HIWORD(lParam))};
        int rowIndex = -1;
        int subItemIndex = -1;
        if (GetActionHitAtPoint(point, rowIndex, subItemIndex)) {
            SetHoveredAction(rowIndex, subItemIndex);
        } else {
            SetHoveredAction(-1, -1);
        }
    }

    LRESULT OnListViewMouseMoveMessage(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
        TrackListViewMouseMove(lParam);
        handled = FALSE;
        return 0;
    }

    LRESULT OnListViewMouseLeaveMessage(UINT, WPARAM, LPARAM, BOOL& handled) {
        m_trackingMouseLeave = false;
        SetHoveredAction(-1, -1);
        handled = FALSE;
        return 0;
    }

    LRESULT OnListViewSetCursor(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
        if (LOWORD(lParam) == HTCLIENT) {
            CPoint point;
            int rowIndex = -1;
            int subItemIndex = -1;
            if (::GetCursorPos(&point) && m_listView.ScreenToClient(&point) &&
                GetActionHitAtPoint(point, rowIndex, subItemIndex)) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                handled = TRUE;
                return TRUE;
            }
        }
        handled = FALSE;
        return 0;
    }

    LRESULT OnListViewUpdateHoverAfterDefault(UINT message, WPARAM wParam, LPARAM lParam,
                                               BOOL& handled) {
        const LRESULT result = m_listView.DefWindowProc(message, wParam, lParam);
        UpdateHoveredActionFromCursor();
        handled = TRUE;
        return result;
    }

    LRESULT OnListViewNcDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
        m_trackingMouseLeave = false;
        SetHoveredAction(-1, -1);
        handled = FALSE;
        return 0;
    }

    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        if (m_initializationFailed) {
            DestroyWindow();
            return 0;
        }
        if (!m_trayIconAdded) {
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
        m_blockerService.SetStateChangedCallback({});
        RemoveTrayIcon();
        m_blockerService.Stop();
        DestroyWindowIcons();
        m_logger.Info(L"程序退出");
        PostQuitMessage(0);
        return 0;
    }

    bool AddTrayIcon() {
        if (m_smallIcon.IsNull()) {
            return false;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_hWnd;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = m_smallIcon;
        wcscpy_s(data.szTip, L"Hotkey Blocker");
        return Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    }

    void RestoreTrayIcon() {
        if (m_hWnd == nullptr) {
            return;
        }
        m_trayIconAdded = false;
        m_trayIconAdded = AddTrayIcon();
        if (!m_trayIconAdded) {
            m_logger.Error(L"Explorer 重启后重新创建系统托盘图标失败");
            return;
        }
        NotifyActionableStates();
    }

    void RemoveTrayIcon() {
        if (!m_trayIconAdded) {
            return;
        }
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_hWnd;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        m_trayIconAdded = false;
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
            m_logger.Error(L"拒绝打开非预期的 Release 地址：" + url);
            return;
        }

        const HINSTANCE result =
            ShellExecuteW(m_hWnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            m_logger.Error(L"无法打开 Release 页面：" + url);
        }
    }

    void DrainUpdateCheckMessages() {
        MSG message{};
        while (::PeekMessageW(&message, m_hWnd, kUpdateCheckCompletedMessage,
                              kUpdateCheckCompletedMessage, PM_REMOVE)) {
            delete reinterpret_cast<UpdateCheckResult*>(message.lParam);
        }
    }

    void InitializeListView() {
        if (!m_listView.IsWindow()) {
            return;
        }
        HFONT listFont = m_listView.GetFont();
        if (listFont == nullptr) {
            listFont = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        }
        LOGFONTW hoverFont{};
        if (listFont != nullptr &&
            ::GetObjectW(listFont, static_cast<int>(sizeof(hoverFont)), &hoverFont) ==
                static_cast<int>(sizeof(hoverFont))) {
            hoverFont.lfUnderline = TRUE;
            m_actionHoverFont.CreateFontIndirect(&hoverFont);
        }
        // Version 5 avoids clipping when the hover state selects an alternate font.
        m_listView.SendMessage(CCM_SETVERSION, 5, 0);
        m_listView.SetExtendedListViewStyle(
            LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP |
            LVS_EX_INFOTIP);
        SHFILEINFOW shellFileInfo{};
        const DWORD_PTR systemImageList = SHGetFileInfoW(
            L"C:\\Windows", FILE_ATTRIBUTE_DIRECTORY, &shellFileInfo, sizeof(shellFileInfo),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (systemImageList != 0) {
            m_systemImageList = reinterpret_cast<HIMAGELIST>(systemImageList);
            m_listView.SetImageList(m_systemImageList, LVSIL_SMALL);
        }
        CRect listClientRect;
        m_listView.GetClientRect(&listClientRect);
        const int listWidth = listClientRect.Width();
        const int remainingWidth = listWidth - kEnabledColumnWidth - kStatusColumnWidth -
                                   kConfigureColumnWidth - kDeleteColumnWidth;
        const int pathColumnWidth = remainingWidth > 0 ? remainingWidth : 1;
        InsertColumn(0, L"目标路径", pathColumnWidth, LVCFMT_LEFT);
        InsertColumn(kRuleStateColumn, L"规则状态", kEnabledColumnWidth, LVCFMT_CENTER);
        InsertColumn(kRuntimeStatusColumn, L"拦截状态", kStatusColumnWidth, LVCFMT_CENTER);
        InsertColumn(kConfigureActionColumn, L"配置", kConfigureColumnWidth, LVCFMT_CENTER);
        InsertColumn(kDeleteActionColumn, L"删除", kDeleteColumnWidth, LVCFMT_CENTER);

        CHeaderCtrl header = m_listView.GetHeader();
        if (header.IsWindow()) {
            header.ModifyStyle(0, HDS_NOSIZING);
        }
    }

    void UpdatePathColumnWidth() {
        if (!m_listView.IsWindow()) {
            return;
        }
        CRect listClientRect;
        m_listView.GetClientRect(&listClientRect);
        const int listWidth = listClientRect.Width();
        const int fixedWidth = kEnabledColumnWidth + kStatusColumnWidth +
                               kConfigureColumnWidth + kDeleteColumnWidth;
        const int remainingWidth = listWidth - fixedWidth;
        const int pathColumnWidth = remainingWidth > 0 ? remainingWidth : 1;
        m_listView.SetColumnWidth(0, pathColumnWidth);
    }

    bool LoadWindowIcons() {
        const HINSTANCE resourceInstance = _Module.GetResourceInstance();
        m_largeIcon = reinterpret_cast<HICON>(LoadImageW(
            resourceInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
            GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        m_smallIcon = reinterpret_cast<HICON>(LoadImageW(
            resourceInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
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

    void InsertColumn(int index, const wchar_t* title, int width, int format) {
        m_listView.InsertColumn(index, title, format, width, index);
    }

    void RefreshListView(bool force) {
        if (!m_listView.IsWindow()) {
            return;
        }

        const std::vector<DisplayRow> rows = BuildDisplayRows();
        if (!force && SameRows(rows, m_renderedRows)) {
            return;
        }

        std::wstring selectedKey;
        const int selectedIndex = m_listView.GetNextItem(-1, LVNI_SELECTED);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_renderedRows.size())) {
            selectedKey = m_renderedRows[static_cast<std::size_t>(selectedIndex)].path;
        }

        SetHoveredAction(-1, -1);
        m_listView.SetRedraw(FALSE);
        m_listView.DeleteAllItems();
        for (std::size_t index = 0; index < rows.size(); ++index) {
            InsertListItem(static_cast<int>(index), rows[index]);
        }
        m_listView.SetRedraw(TRUE);
        m_listView.InvalidateRect(nullptr, TRUE);

        if (!selectedKey.empty()) {
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (!PathUtils::SamePath(rows[index].path, selectedKey)) {
                    continue;
                }
                m_listView.SetItemState(static_cast<int>(index), LVIS_SELECTED | LVIS_FOCUSED,
                                        LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
        m_renderedRows = rows;
        UpdateHoveredActionFromCursor();
        UpdatePathColumnWidth();
    }

    std::vector<DisplayRow> BuildDisplayRows() const {
        const std::vector<AppRule>& rules = m_ruleManager.Rules();
        const std::vector<RuntimeRuleState> states = m_blockerService.Snapshot();
        std::vector<DisplayRow> rows;
        rows.reserve(rules.size());

        for (const AppRule& rule : rules) {
            DisplayRow row;
            row.path = rule.path;
            row.enabled = rule.enabled;
            row.status = AppStatusText(AppStatus::Waiting);
            row.imageIndex = FileIconIndex(rule.path);
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

    static bool SameRows(const std::vector<DisplayRow>& left,
                         const std::vector<DisplayRow>& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (left[index].path != right[index].path ||
                left[index].enabled != right[index].enabled ||
                left[index].status != right[index].status ||
                left[index].detail != right[index].detail ||
                left[index].imageIndex != right[index].imageIndex) {
                return false;
            }
        }
        return true;
    }

    void InsertListItem(int itemIndex, const DisplayRow& row) {
        m_listView.InsertItem(itemIndex, row.path.c_str(), row.imageIndex);
        const wchar_t* enabled = row.enabled ? L"启用" : L"停用";
        SetListItemText(itemIndex, kRuleStateColumn, enabled);
        SetListItemText(itemIndex, kRuntimeStatusColumn, row.status.c_str());
        SetListItemText(itemIndex, kConfigureActionColumn, L"配置");
        SetListItemText(itemIndex, kDeleteActionColumn, L"删除");
    }

    void SetListItemText(int itemIndex, int subItemIndex, LPCTSTR text) {
        m_listView.SetItemText(itemIndex, subItemIndex, text);
    }

    int FileIconIndex(const std::wstring& path) const {
        const auto cached = m_iconIndices.find(path);
        if (cached != m_iconIndices.end()) {
            return cached->second;
        }
        if (m_systemImageList.IsNull() || path.empty()) {
            return -1;
        }

        SHFILEINFOW shellFileInfo{};
        DWORD attributes = GetFileAttributesW(path.c_str());
        UINT flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON;
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            attributes = FILE_ATTRIBUTE_NORMAL;
            flags |= SHGFI_USEFILEATTRIBUTES;
        }
        if (SHGetFileInfoW(path.c_str(), attributes, &shellFileInfo, sizeof(shellFileInfo), flags) ==
            0) {
            m_iconIndices.emplace(path, -1);
            return -1;
        }
        m_iconIndices.emplace(path, shellFileInfo.iIcon);
        return shellFileInfo.iIcon;
    }

    bool PickApplicationPath(bool folder, std::wstring& path) {
        CComPtr<IFileOpenDialog> dialog;
        HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dialog));
        if (FAILED(result)) {
            ShowErrorCode(L"创建文件选择器失败", result);
            return false;
        }

        FILEOPENDIALOGOPTIONS options = 0;
        result = dialog->GetOptions(&options);
        if (FAILED(result)) {
            ShowErrorCode(L"配置文件选择器失败", result);
            return false;
        }
        options |= FOS_FORCEFILESYSTEM;
        if (folder) {
            options |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
            dialog->SetTitle(L"选择要拦截的程序文件夹");
        } else {
            options |= FOS_FILEMUSTEXIST;
            const COMDLG_FILTERSPEC filters[] = {{L"应用程序 (*.exe)", L"*.exe"}};
            result = dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
            if (FAILED(result)) {
                ShowErrorCode(L"配置文件选择器失败", result);
                return false;
            }
            dialog->SetTitle(L"选择要拦截的 EXE 文件");
        }
        result = dialog->SetOptions(options);
        if (FAILED(result)) {
            ShowErrorCode(L"配置文件选择器失败", result);
            return false;
        }
        result = dialog->Show(m_hWnd);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            return false;
        }
        if (FAILED(result)) {
            ShowErrorCode(L"打开文件选择器失败", result);
            return false;
        }

        CComPtr<IShellItem> item;
        result = dialog->GetResult(&item);
        if (FAILED(result)) {
            ShowErrorCode(L"获取所选文件失败", result);
            return false;
        }

        PWSTR rawPath = nullptr;
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
        if (FAILED(result) || rawPath == nullptr) {
            ShowErrorCode(L"获取所选文件路径失败", result);
            return false;
        }
        path.assign(rawPath);
        CoTaskMemFree(rawPath);
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
        if (!m_ruleManager.AddRule(std::move(rule))) {
            ShowError(L"添加应用失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
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
        if (!m_ruleManager.AddRule(std::move(rule))) {
            ShowError(L"添加文件夹失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void DeleteApplication(const std::wstring& path) {
        const std::wstring message = L"确定删除规则？\n\n" + path;
        if (MessageBox(message.c_str(), L"删除应用", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }

        if (!m_ruleManager.Remove(path)) {
            ShowError(L"删除应用失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void ConfigureApplication(const std::wstring& path) {
        const auto rule = std::find_if(
            m_ruleManager.Rules().begin(), m_ruleManager.Rules().end(),
            [&path](const AppRule& candidate) { return PathUtils::SamePath(candidate.path, path); });
        if (rule == m_ruleManager.Rules().end()) {
            ShowError(L"配置规则失败", L"找不到所选应用规则");
            return;
        }

        HotkeyPolicyDialog dialog(rule->hotkeyPolicy, rule->enabled);
        if (dialog.DoModal(m_hWnd) != IDOK) {
            return;
        }
        if (!m_ruleManager.SetRuleSettings(path, dialog.Enabled(), dialog.Policy())) {
            ShowError(L"保存规则配置失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void UpdateAutoStart() {
        CMenuHandle menu = GetMenu();
        if (menu.IsNull()) {
            return;
        }
        const UINT state = menu.GetMenuState(ID_MAIN_AUTOSTART, MF_BYCOMMAND);
        if (state == static_cast<UINT>(-1)) {
            return;
        }
        const bool enabled = (state & MF_CHECKED) == 0;
        std::wstring error;
        if (!m_startupManager.SetEnabled(enabled, error)) {
            m_logger.Error(error);
            ShowError(L"设置开机启动失败", error);
            return;
        }
        menu.CheckMenuItem(ID_MAIN_AUTOSTART,
                           MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
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
        const std::vector<RuntimeRuleState> states = m_blockerService.Snapshot();
        std::unordered_map<std::wstring, AppStatus> current;
        for (const RuntimeRuleState& state : states) {
            if (!IsActionableStatus(state.status)) {
                continue;
            }
            current[state.path] = state.status;
            if (!m_trayIconAdded) {
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
                m_ruleManager.Rules().begin(), m_ruleManager.Rules().end(),
                [&state](const AppRule& candidate) {
                    return PathUtils::SamePath(candidate.path, state.path);
                });
            if (rule != m_ruleManager.Rules().end() && !rule->displayName.empty()) {
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
        if (!m_trayIconAdded) {
            return;
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_hWnd;
        data.uID = kTrayIconId;
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_WARNING;
        data.uTimeout = 5000;
        wcsncpy_s(data.szInfoTitle, std::size(data.szInfoTitle), title.c_str(), _TRUNCATE);
        wcsncpy_s(data.szInfo, std::size(data.szInfo), message.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
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
    bool m_trayIconAdded = false;
    bool m_trackingMouseLeave = false;
    ATL::CContainedWindowT<CListViewCtrl> m_listView;
    CImageList m_systemImageList;  // Non-owning wrapper for the shared shell image list.
    CFont m_actionHoverFont;
    CIcon m_largeIcon;
    CIcon m_smallIcon;
    int m_hoveredActionRow = -1;
    int m_hoveredActionColumn = -1;
    UpdateChecker m_updateChecker;
    std::wstring m_pendingReleaseUrl;
    bool m_updateCheckRunning = false;
    bool m_updateCheckInteractive = false;
    Logger m_logger;
    RuleManager m_ruleManager;
    StartupManager m_startupManager;
    BlockerService m_blockerService;
    std::vector<DisplayRow> m_renderedRows;
    mutable std::unordered_map<std::wstring, int> m_iconIndices;
    std::unordered_map<std::wstring, AppStatus> m_notifiedActionableStates;
    std::atomic_bool m_stateNotificationPosted = false;
};

int RunMainWindow(CMessageLoop& messageLoop, bool startHidden) {
    kTaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    MainWindow window(startHidden);
    if (window.Create(nullptr) == nullptr) {
        return 1;
    }

    messageLoop.AddMessageFilter(&window);
    window.CenterWindow();
    window.ShowWindow(window.ShouldStartHidden() ? SW_HIDE : SW_SHOWNORMAL);

    const int exitCode = messageLoop.Run();
    messageLoop.RemoveMessageFilter(&window);
    return exitCode;
}
