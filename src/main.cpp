#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0601
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <atlbase.h>
#include <atlapp.h>
#include <atlctrls.h>
#include <atlwin.h>
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
#include "Logger.h"
#include "PathUtils.h"
#include "RuleManager.h"
#include "StartupManager.h"
#include "UpdateChecker.h"
#include "Version.h"
#include "resource.h"

CAppModule _Module;

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

const wchar_t* HotkeyModeText(HotkeyMode mode) {
    switch (mode) {
        case HotkeyMode::Blacklist:
            return L"黑名单：拦截列表中的快捷键";
        case HotkeyMode::Whitelist:
            return L"白名单：仅允许列表中的快捷键";
        case HotkeyMode::BlockAll:
        default:
            return L"拦截全部快捷键";
    }
}

std::wstring FormatVirtualKey(uint32_t virtualKey) {
    if (virtualKey >= L'A' && virtualKey <= L'Z') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= L'0' && virtualKey <= L'9') {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1);
    }

    switch (virtualKey) {
        case VK_BACK:
            return L"Backspace";
        case VK_TAB:
            return L"Tab";
        case VK_RETURN:
            return L"Enter";
        case VK_ESCAPE:
            return L"Esc";
        case VK_SPACE:
            return L"Space";
        case VK_PRIOR:
            return L"PageUp";
        case VK_NEXT:
            return L"PageDown";
        case VK_END:
            return L"End";
        case VK_HOME:
            return L"Home";
        case VK_LEFT:
            return L"Left";
        case VK_UP:
            return L"Up";
        case VK_RIGHT:
            return L"Right";
        case VK_DOWN:
            return L"Down";
        case VK_INSERT:
            return L"Insert";
        case VK_DELETE:
            return L"Delete";
        case VK_NUMPAD0:
        case VK_NUMPAD1:
        case VK_NUMPAD2:
        case VK_NUMPAD3:
        case VK_NUMPAD4:
        case VK_NUMPAD5:
        case VK_NUMPAD6:
        case VK_NUMPAD7:
        case VK_NUMPAD8:
        case VK_NUMPAD9:
            return L"Num" + std::to_wstring(virtualKey - VK_NUMPAD0);
        default:
            break;
    }

    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    if (scanCode != 0) {
        wchar_t keyName[64]{};
        const LONG keyNameResult = GetKeyNameTextW(static_cast<LONG>(scanCode << 16), keyName,
                                                   static_cast<int>(std::size(keyName)));
        if (keyNameResult > 0) {
            return keyName;
        }
    }
    return L"VK_" + std::to_wstring(virtualKey);
}

std::wstring FormatHotkey(const HotkeySpec& hotkey) {
    const uint32_t modifiers = NormalizeHotkey(hotkey).modifiers;
    std::wstring text;
    if ((modifiers & HotkeyPolicyConstants::kModifierControl) != 0) {
        text += L"Ctrl+";
    }
    if ((modifiers & HotkeyPolicyConstants::kModifierAlt) != 0) {
        text += L"Alt+";
    }
    if ((modifiers & HotkeyPolicyConstants::kModifierShift) != 0) {
        text += L"Shift+";
    }
    if ((modifiers & HotkeyPolicyConstants::kModifierWin) != 0) {
        text += L"Win+";
    }
    text += FormatVirtualKey(hotkey.virtualKey);
    return text;
}

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

bool IsModifierVirtualKey(UINT virtualKey) {
    switch (virtualKey) {
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_LWIN:
        case VK_RWIN:
            return true;
        default:
            return false;
    }
}

uint32_t CaptureModifiers() {
    uint32_t modifiers = 0;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= HotkeyPolicyConstants::kModifierControl;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= HotkeyPolicyConstants::kModifierAlt;
    }
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= HotkeyPolicyConstants::kModifierShift;
    }
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        modifiers |= HotkeyPolicyConstants::kModifierWin;
    }
    return modifiers;
}

class HotkeyCapture final : public ATL::CWindowImpl<HotkeyCapture, CEdit> {
public:
    HotkeySpec Hotkey() const noexcept {
        return m_hotkey;
    }

    bool HasHotkey() const noexcept {
        return IsValidHotkey(m_hotkey);
    }

    void Clear() {
        m_hotkey = {};
        SetWindowTextW(L"");
    }

    BEGIN_MSG_MAP(HotkeyCapture)
        MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_SYSKEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_CHAR, OnIgnoredCharacter)
        MESSAGE_HANDLER(WM_SYSCHAR, OnIgnoredCharacter)
    END_MSG_MAP()

private:
    LRESULT OnGetDlgCode(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    }

    LRESULT OnKeyDown(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
        handled = TRUE;
        const UINT virtualKey = static_cast<UINT>(wParam);
        if (IsModifierVirtualKey(virtualKey)) {
            return 0;
        }

        HotkeySpec captured;
        captured.modifiers = CaptureModifiers();
        captured.virtualKey = virtualKey;
        if (!IsValidHotkey(captured)) {
            return 0;
        }
        m_hotkey = NormalizeHotkey(captured);
        const std::wstring text = FormatHotkey(m_hotkey);
        SetWindowTextW(text.c_str());
        return 0;
    }

    LRESULT OnIgnoredCharacter(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        return 0;
    }

    HotkeySpec m_hotkey;
};

class HotkeyPolicyDialog final : public ATL::CDialogImpl<HotkeyPolicyDialog> {
public:
    enum { IDD = IDD_HOTKEY_POLICY_DIALOG };

    HotkeyPolicyDialog(HotkeyPolicy policy, bool enabled)
        : m_policy(std::move(policy)), m_enabled(enabled) {
        NormalizeHotkeyPolicy(m_policy);
        if (!IsValidHotkeyMode(m_policy.mode)) {
            m_policy.mode = HotkeyMode::BlockAll;
        }
    }

    const HotkeyPolicy& Policy() const noexcept {
        return m_policy;
    }

    bool Enabled() const noexcept {
        return m_enabled;
    }

    BEGIN_MSG_MAP(HotkeyPolicyDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDC_HOTKEY_MODE, CBN_SELCHANGE, OnModeChanged)
        COMMAND_HANDLER(IDC_HOTKEY_LIST, LBN_SELCHANGE, OnListSelectionChanged)
        COMMAND_ID_HANDLER(IDC_HOTKEY_ADD, OnAdd)
        COMMAND_ID_HANDLER(IDC_HOTKEY_REMOVE, OnRemove)
        COMMAND_ID_HANDLER(IDOK, OnOk)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        m_capture.SubclassWindow(GetDlgItem(IDC_HOTKEY_CAPTURE));
        ::CheckDlgButton(m_hWnd, IDC_RULE_ENABLED, m_enabled ? BST_CHECKED : BST_UNCHECKED);

        const HWND mode = GetDlgItem(IDC_HOTKEY_MODE);
        for (const HotkeyMode modeValue :
             {HotkeyMode::BlockAll, HotkeyMode::Blacklist, HotkeyMode::Whitelist}) {
            SendMessageW(mode, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(HotkeyModeText(modeValue)));
        }
        SendMessageW(mode, CB_SETCURSEL, static_cast<WPARAM>(m_policy.mode), 0);
        RefreshList();
        UpdateControls();
        CenterWindow(GetParent());
        return TRUE;
    }

    LRESULT OnModeChanged(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        const LRESULT selection = SendMessageW(GetDlgItem(IDC_HOTKEY_MODE), CB_GETCURSEL, 0, 0);
        if (selection >= 0 && selection <= static_cast<LRESULT>(HotkeyMode::Whitelist)) {
            m_policy.mode = static_cast<HotkeyMode>(selection);
        }
        UpdateControls();
        return 0;
    }

    LRESULT OnAdd(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        if (m_policy.mode == HotkeyMode::BlockAll) {
            return 0;
        }
        if (!m_capture.HasHotkey()) {
            MessageBoxW(L"请先在输入框中按下要配置的快捷键。", L"添加快捷键",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        const HotkeySpec hotkey = m_capture.Hotkey();
        if (!ContainsHotkey(m_policy, hotkey)) {
            m_policy.hotkeys.push_back(hotkey);
            NormalizeHotkeyPolicy(m_policy);
            RefreshList();
        }
        m_capture.Clear();
        m_capture.SetFocus();
        return 0;
    }

    LRESULT OnListSelectionChanged(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        UpdateControls();
        return 0;
    }

    LRESULT OnRemove(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        const LRESULT selection = SendMessageW(GetDlgItem(IDC_HOTKEY_LIST), LB_GETCURSEL, 0, 0);
        if (selection >= 0 && selection < static_cast<LRESULT>(m_policy.hotkeys.size())) {
            m_policy.hotkeys.erase(m_policy.hotkeys.begin() + selection);
            RefreshList();
        }
        return 0;
    }

    LRESULT OnOk(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        NormalizeHotkeyPolicy(m_policy);
        if (!ValidateHotkeyPolicy(m_policy)) {
            MessageBoxW(L"快捷键策略无效，请检查配置。", L"保存快捷键策略",
                        MB_OK | MB_ICONERROR);
            return 0;
        }
        m_enabled = ::IsDlgButtonChecked(m_hWnd, IDC_RULE_ENABLED) == BST_CHECKED;
        EndDialog(IDOK);
        return 0;
    }

    LRESULT OnCancel(WORD, WORD, HWND, BOOL& handled) {
        handled = TRUE;
        EndDialog(IDCANCEL);
        return 0;
    }

    void RefreshList() {
        const HWND list = GetDlgItem(IDC_HOTKEY_LIST);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const HotkeySpec& hotkey : m_policy.hotkeys) {
            const std::wstring text = FormatHotkey(hotkey);
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        UpdateControls();
    }

    void UpdateControls() {
        const bool editable = m_policy.mode != HotkeyMode::BlockAll;
        ::EnableWindow(GetDlgItem(IDC_HOTKEY_LIST), editable);
        ::EnableWindow(GetDlgItem(IDC_HOTKEY_CAPTURE), editable);
        ::EnableWindow(GetDlgItem(IDC_HOTKEY_ADD), editable);
        const LRESULT selection = SendMessageW(GetDlgItem(IDC_HOTKEY_LIST), LB_GETCURSEL, 0, 0);
        ::EnableWindow(GetDlgItem(IDC_HOTKEY_REMOVE),
                       editable && selection >= 0 &&
                           selection < static_cast<LRESULT>(m_policy.hotkeys.size()));
    }

    HotkeyPolicy m_policy;
    bool m_enabled = true;
    HotkeyCapture m_capture;
};

}  // namespace

class MainWindow final : public ATL::CDialogImpl<MainWindow>, public CMessageFilter {
public:
    enum { IDD = IDD_MAIN_WINDOW };

    explicit MainWindow(bool startHidden)
        : m_startHidden(startHidden), m_blockerService(&m_logger) {}

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
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
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
        ::SetWindowTextW(m_hWnd, title.c_str());
        m_listView = GetDlgItem(IDC_APP_LIST);
        InitializeListView();

        if (!LoadWindowIcons()) {
            m_logger.Error(L"加载应用图标失败");
        }

        m_logger.Info(L"程序启动");
        if (!m_ruleManager.Load()) {
            m_logger.Error(L"加载规则失败：" + m_ruleManager.LastError());
            ShowError(L"加载配置失败", m_ruleManager.LastError());
            m_initializationFailed = true;
            ::PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
            return TRUE;
        }
        m_logger.Info(L"加载规则：" + std::to_wstring(m_ruleManager.Rules().size()) + L" 条");

        bool autoStartEnabled = false;
        std::wstring startupError;
        if (!m_startupManager.GetEnabled(autoStartEnabled, startupError)) {
            m_logger.Error(startupError);
            ShowError(L"读取开机启动设置失败", startupError);
        }
        if (HMENU menu = ::GetMenu(m_hWnd); menu != nullptr) {
            ::CheckMenuItem(menu, ID_MAIN_AUTOSTART,
                            MF_BYCOMMAND | (autoStartEnabled ? MF_CHECKED : MF_UNCHECKED));
        }

        m_blockerService.SetStateChangedCallback([this] { QueueStateRefresh(); });
        if (!m_blockerService.Start(m_ruleManager.Rules())) {
            m_logger.Error(L"进程监控启动失败");
            ShowError(L"启动进程监控失败", L"无法创建进程监控线程");
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
            ::ShowWindow(m_hWnd, SW_HIDE);
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
                if (::MessageBoxW(m_hWnd, message.c_str(), L"发现新版本",
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
            ::MessageBoxW(m_hWnd, message.c_str(), L"检查更新", MB_OK | MB_ICONINFORMATION);
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

    LRESULT OnCommand(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
        handled = TRUE;
        switch (LOWORD(wParam)) {
            case ID_MAIN_ADD_EXECUTABLE:
                AddExecutableApplication();
                break;
            case ID_MAIN_ADD_FOLDER:
                AddFolderApplication();
                break;
            case ID_MAIN_AUTOSTART:
                UpdateAutoStart();
                break;
            case ID_TRAY_CHECK_UPDATES:
                StartUpdateCheck(true);
                break;
            case ID_TRAY_SHOW:
                ShowFromTray();
                break;
            case ID_TRAY_EXIT:
                ExitApplication();
                break;
            default:
                handled = FALSE;
                break;
        }
        return 0;
    }

    LRESULT OnNotify(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (header != nullptr && header->idFrom == IDC_APP_LIST &&
            header->code == NM_CUSTOMDRAW) {
            auto* customDraw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
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
                RECT buttonRect{};
                const int rowIndex = static_cast<int>(customDraw->nmcd.dwItemSpec);
                if (GetActionButtonRect(rowIndex, customDraw->iSubItem, buttonRect)) {
                    const wchar_t* text = customDraw->iSubItem == kConfigureActionColumn
                                              ? L"配置"
                                              : L"删除";
                    const int savedDc = ::SaveDC(customDraw->nmcd.hdc);
                    ::DrawFrameControl(customDraw->nmcd.hdc, &buttonRect, DFC_BUTTON,
                                       DFCS_BUTTONPUSH);
                    ::SetBkMode(customDraw->nmcd.hdc, TRANSPARENT);
                    ::DrawTextW(customDraw->nmcd.hdc, text, -1, &buttonRect,
                                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    if (savedDc != 0) {
                        ::RestoreDC(customDraw->nmcd.hdc, savedDc);
                    }
                }
                handled = TRUE;
                return CDRF_SKIPDEFAULT;
            }
            handled = TRUE;
            return CDRF_DODEFAULT;
        }
        if (header != nullptr && header->idFrom == IDC_APP_LIST && header->code == NM_CLICK) {
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
        }
        if (header != nullptr && header->idFrom == IDC_APP_LIST &&
            header->code == LVN_GETINFOTIPW) {
            const auto* infoTip = reinterpret_cast<const NMLVGETINFOTIPW*>(lParam);
            if (infoTip->iItem >= 0 &&
                infoTip->iItem < static_cast<int>(m_renderedRows.size()) &&
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
        if (header != nullptr && header->idFrom == IDC_APP_LIST &&
            header->code == LVN_KEYDOWN) {
            const auto* keyDown = reinterpret_cast<const NMLVKEYDOWN*>(lParam);
            const int selectedIndex = SelectedIndex();
            if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_renderedRows.size())) {
                const std::wstring path =
                    m_renderedRows[static_cast<std::size_t>(selectedIndex)].path;
                if (keyDown->wVKey == VK_RETURN) {
                    ConfigureApplication(path);
                    handled = TRUE;
                    return 0;
                }
                if (keyDown->wVKey == VK_DELETE) {
                    DeleteApplication(path);
                    handled = TRUE;
                    return 0;
                }
            }
        }
        if (header != nullptr && header->idFrom == IDC_APP_LIST && header->code == NM_DBLCLK) {
            int rowIndex = -1;
            int subItemIndex = -1;
            if (GetActionHitAtCursor(rowIndex, subItemIndex)) {
                handled = TRUE;
                return 0;
            }
            handled = TRUE;
            OpenSelectedLocation();
            return 0;
        }
        handled = FALSE;
        return 0;
    }

    bool GetActionButtonRect(int rowIndex, int subItemIndex, RECT& rect) const {
        if (rowIndex < 0 || rowIndex >= static_cast<int>(m_renderedRows.size()) ||
            (subItemIndex != kConfigureActionColumn && subItemIndex != kDeleteActionColumn) ||
            !ListView_GetSubItemRect(m_listView, rowIndex, subItemIndex, LVIR_BOUNDS, &rect)) {
            return false;
        }
        ::InflateRect(&rect, -4, -2);
        return rect.right > rect.left && rect.bottom > rect.top;
    }

    bool GetActionHitAtCursor(int& rowIndex, int& subItemIndex) const {
        if (m_listView == nullptr) {
            return false;
        }

        POINT point{};
        if (!::GetCursorPos(&point) || !::ScreenToClient(m_listView, &point)) {
            return false;
        }
        LVHITTESTINFO hit{};
        hit.pt = point;
        const int hitRow = ListView_SubItemHitTest(m_listView, &hit);
        if (hitRow < 0) {
            return false;
        }

        RECT buttonRect{};
        if (!GetActionButtonRect(hitRow, hit.iSubItem, buttonRect) ||
            !::PtInRect(&buttonRect, point)) {
            return false;
        }
        rowIndex = hitRow;
        subItemIndex = hit.iSubItem;
        return true;
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
        ::ShowWindow(m_hWnd, SW_HIDE);
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
        if (m_smallIcon == nullptr) {
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
        if (::IsIconic(m_hWnd)) {
            ::ShowWindow(m_hWnd, SW_RESTORE);
        }
        ::ShowWindow(m_hWnd, SW_SHOWNORMAL);
        ::SetForegroundWindow(m_hWnd);
        RefreshListView(true);
    }

    void QueueStateRefresh() {
        if (m_hWnd == nullptr) {
            return;
        }
        if (!m_stateNotificationPosted.exchange(true, std::memory_order_acq_rel)) {
            if (!::PostMessageW(m_hWnd, kStateChangedMessage, 0, 0)) {
                m_stateNotificationPosted.store(false, std::memory_order_release);
            }
        }
    }

    void ShowTrayMenu() {
        HMENU menu = LoadMenuW(_Module.GetResourceInstance(), MAKEINTRESOURCEW(IDR_TRAY_MENU));
        if (menu == nullptr) {
            return;
        }
        HMENU popup = GetSubMenu(menu, 0);
        if (popup != nullptr) {
            POINT cursor{};
            GetCursorPos(&cursor);
            ::SetForegroundWindow(m_hWnd);
            TrackPopupMenu(popup, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, m_hWnd, nullptr);
            PostMessage(WM_NULL, 0, 0);
        }
        DestroyMenu(menu);
    }

    void ExitApplication() {
        DestroyWindow();
    }

    void StartUpdateCheck(bool interactive) {
        if (m_updateCheckRunning) {
            if (interactive) {
                ::MessageBoxW(m_hWnd, L"版本检查正在进行，请稍候。", L"检查更新",
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
        if (m_listView == nullptr) {
            return;
        }
        ListView_SetExtendedListViewStyle(
            m_listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
                            LVS_EX_LABELTIP | LVS_EX_INFOTIP);
        SHFILEINFOW shellFileInfo{};
        const DWORD_PTR systemImageList = SHGetFileInfoW(
            L"C:\\Windows", FILE_ATTRIBUTE_DIRECTORY, &shellFileInfo, sizeof(shellFileInfo),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (systemImageList != 0) {
            m_systemImageList = reinterpret_cast<HIMAGELIST>(systemImageList);
            ListView_SetImageList(m_listView, m_systemImageList, LVSIL_SMALL);
        }
        RECT listClientRect{};
        ::GetClientRect(m_listView, &listClientRect);
        const int listWidth = listClientRect.right - listClientRect.left;
        const int remainingWidth = listWidth - kEnabledColumnWidth - kStatusColumnWidth -
                                   kConfigureColumnWidth - kDeleteColumnWidth;
        const int pathColumnWidth = remainingWidth > 0 ? remainingWidth : 1;
        InsertColumn(0, L"目标路径", pathColumnWidth, LVCFMT_LEFT);
        InsertColumn(kRuleStateColumn, L"规则状态", kEnabledColumnWidth, LVCFMT_CENTER);
        InsertColumn(kRuntimeStatusColumn, L"拦截状态", kStatusColumnWidth, LVCFMT_CENTER);
        InsertColumn(kConfigureActionColumn, L"配置", kConfigureColumnWidth, LVCFMT_CENTER);
        InsertColumn(kDeleteActionColumn, L"删除", kDeleteColumnWidth, LVCFMT_CENTER);

        const HWND header = ListView_GetHeader(m_listView);
        if (header != nullptr) {
            const LONG_PTR headerStyle = ::GetWindowLongPtrW(header, GWL_STYLE);
            ::SetWindowLongPtrW(header, GWL_STYLE, headerStyle | HDS_NOSIZING);
        }
    }

    void UpdatePathColumnWidth() const {
        if (m_listView == nullptr) {
            return;
        }
        RECT listClientRect{};
        ::GetClientRect(m_listView, &listClientRect);
        const int listWidth = listClientRect.right - listClientRect.left;
        const int fixedWidth = kEnabledColumnWidth + kStatusColumnWidth +
                               kConfigureColumnWidth + kDeleteColumnWidth;
        const int remainingWidth = listWidth - fixedWidth;
        const int pathColumnWidth = remainingWidth > 0 ? remainingWidth : 1;
        ListView_SetColumnWidth(m_listView, 0, pathColumnWidth);
    }

    bool LoadWindowIcons() {
        const HINSTANCE resourceInstance = _Module.GetResourceInstance();
        m_largeIcon = reinterpret_cast<HICON>(LoadImageW(
            resourceInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        m_smallIcon = reinterpret_cast<HICON>(LoadImageW(
            resourceInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (m_largeIcon == nullptr || m_smallIcon == nullptr) {
            DestroyWindowIcons();
            return false;
        }

        SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(m_largeIcon));
        SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(m_smallIcon));
        return true;
    }

    void DestroyWindowIcons() {
        if (m_largeIcon != nullptr) {
            DestroyIcon(m_largeIcon);
            m_largeIcon = nullptr;
        }
        if (m_smallIcon != nullptr) {
            DestroyIcon(m_smallIcon);
            m_smallIcon = nullptr;
        }
    }

    void InsertColumn(int index, const wchar_t* title, int width, int format) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        column.cx = width;
        column.fmt = format;
        column.iSubItem = index;
        column.pszText = const_cast<LPWSTR>(title);
        SendMessageW(m_listView, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&column));
    }

    void RefreshListView(bool force) {
        if (m_listView == nullptr) {
            return;
        }

        const std::vector<DisplayRow> rows = BuildDisplayRows();
        if (!force && SameRows(rows, m_renderedRows)) {
            return;
        }

        std::wstring selectedKey;
        const int selectedIndex = ListView_GetNextItem(m_listView, -1, LVNI_SELECTED);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_renderedRows.size())) {
            selectedKey = m_renderedRows[static_cast<std::size_t>(selectedIndex)].path;
        }

        SendMessageW(m_listView, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(m_listView);
        for (std::size_t index = 0; index < rows.size(); ++index) {
            InsertListItem(static_cast<int>(index), rows[index]);
        }
        SendMessageW(m_listView, WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(m_listView, nullptr, TRUE);

        if (!selectedKey.empty()) {
            for (std::size_t index = 0; index < rows.size(); ++index) {
                if (!PathUtils::SamePath(rows[index].path, selectedKey)) {
                    continue;
                }
                LVITEMW item{};
                item.state = LVIS_SELECTED | LVIS_FOCUSED;
                item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
                SendMessageW(m_listView, LVM_SETITEMSTATE, static_cast<WPARAM>(index),
                             reinterpret_cast<LPARAM>(&item));
                break;
            }
        }
        m_renderedRows = rows;
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

    void InsertListItem(int itemIndex, const DisplayRow& row) const {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = itemIndex;
        item.iImage = row.imageIndex;
        item.pszText = const_cast<LPWSTR>(row.path.c_str());
        SendMessageW(m_listView, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        const wchar_t* enabled = row.enabled ? L"启用" : L"停用";
        SetListItemText(itemIndex, kRuleStateColumn, const_cast<LPWSTR>(enabled));
        SetListItemText(itemIndex, kRuntimeStatusColumn, const_cast<LPWSTR>(row.status.c_str()));
        SetListItemText(itemIndex, kConfigureActionColumn, const_cast<LPWSTR>(L"配置"));
        SetListItemText(itemIndex, kDeleteActionColumn, const_cast<LPWSTR>(L"删除"));
    }

    void SetListItemText(int itemIndex, int subItemIndex, LPWSTR text) const {
        LVITEMW item{};
        item.iSubItem = subItemIndex;
        item.pszText = text;
        SendMessageW(m_listView, LVM_SETITEMTEXTW, static_cast<WPARAM>(itemIndex),
                     reinterpret_cast<LPARAM>(&item));
    }

    int FileIconIndex(const std::wstring& path) const {
        const auto cached = m_iconIndices.find(path);
        if (cached != m_iconIndices.end()) {
            return cached->second;
        }
        if (m_systemImageList == nullptr || path.empty()) {
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

    int SelectedIndex() const {
        if (m_listView == nullptr) {
            return -1;
        }
        return ListView_GetNextItem(m_listView, -1, LVNI_SELECTED);
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
        if (::MessageBoxW(m_hWnd, message.c_str(), L"删除应用",
                          MB_YESNO | MB_ICONQUESTION) != IDYES) {
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

    void OpenSelectedLocation() {
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(m_renderedRows.size())) {
            return;
        }

        const std::wstring parameters = L"/select,\"" +
                                        m_renderedRows[static_cast<std::size_t>(index)].path + L"\"";
        const HINSTANCE result = ShellExecuteW(m_hWnd, L"open", L"explorer.exe",
                                               parameters.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            ShowError(L"打开文件位置失败", L"无法打开资源管理器");
        }
    }

    void UpdateAutoStart() {
        HMENU menu = ::GetMenu(m_hWnd);
        if (menu == nullptr) {
            return;
        }
        const UINT state = ::GetMenuState(menu, ID_MAIN_AUTOSTART, MF_BYCOMMAND);
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
        ::CheckMenuItem(menu, ID_MAIN_AUTOSTART,
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

    void ShowError(const wchar_t* title, const std::wstring& message) const {
        ::MessageBoxW(m_hWnd, message.empty() ? L"未知错误" : message.c_str(), title,
                      MB_OK | MB_ICONERROR);
    }

    void ShowErrorCode(const wchar_t* title, HRESULT result) const {
        std::wstring message = L"错误码：0x" + std::to_wstring(static_cast<unsigned long>(result));
        ShowError(title, message);
    }

    bool m_startHidden = false;
    bool m_initializationFailed = false;
    bool m_trayIconAdded = false;
    HWND m_listView = nullptr;
    HIMAGELIST m_systemImageList = nullptr;
    HICON m_largeIcon = nullptr;
    HICON m_smallIcon = nullptr;
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        return 1;
    }

    HRESULT result = _Module.Init(nullptr, instance);
    if (FAILED(result)) {
        CoUninitialize();
        return static_cast<int>(result);
    }

    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\HotkeyBlocker.SingleInstance");
    if (instanceMutex == nullptr) {
        _Module.Term();
        CoUninitialize();
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(instanceMutex);
        _Module.Term();
        CoUninitialize();
        return 0;
    }

    AtlInitCommonControls(ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES);
    CMessageLoop messageLoop;
    _Module.AddMessageLoop(&messageLoop);
    kTaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    const bool startHidden = commandLine != nullptr &&
                             wcsstr(commandLine, L"--background") != nullptr;
    MainWindow window(startHidden);
    if (window.Create(nullptr) == nullptr) {
        CloseHandle(instanceMutex);
        _Module.RemoveMessageLoop();
        _Module.Term();
        CoUninitialize();
        return 1;
    }
    messageLoop.AddMessageFilter(&window);
    window.CenterWindow();
    window.ShowWindow(window.ShouldStartHidden() ? SW_HIDE : SW_SHOWNORMAL);

    const int exitCode = messageLoop.Run();
    messageLoop.RemoveMessageFilter(&window);
    CloseHandle(instanceMutex);
    _Module.RemoveMessageLoop();
    _Module.Term();
    CoUninitialize();
    return exitCode;
}
