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
#include <atlwin.h>
#include <atldlgs.h>

#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "BlockerService.h"
#include "Logger.h"
#include "PathUtils.h"
#include "RuleManager.h"
#include "StartupManager.h"
#include "resource.h"

CAppModule _Module;

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kRuntimeTimerId = 1;
constexpr UINT kRuntimeRefreshIntervalMs = 250;

}  // namespace

class MainWindow final : public ATL::CDialogImpl<MainWindow> {
public:
    enum { IDD = IDD_MAIN_WINDOW };

    explicit MainWindow(bool startHidden)
        : m_startHidden(startHidden), m_blockerService(&m_logger) {}

    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(WM_TIMER, OnTimer)
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        m_listView = GetDlgItem(IDC_APP_LIST);
        InitializeListView();

        m_logger.Info(L"程序启动");
        if (!m_ruleManager.Load()) {
            m_logger.Error(L"加载规则失败：" + m_ruleManager.LastError());
            ShowError(L"加载配置失败", m_ruleManager.LastError());
        } else {
            m_logger.Info(L"加载规则：" + std::to_wstring(m_ruleManager.Rules().size()) + L" 条");
        }

        ::CheckDlgButton(m_hWnd, IDC_AUTOSTART,
                         m_ruleManager.AutoStart() ? BST_CHECKED : BST_UNCHECKED);
        std::wstring startupError;
        if (!m_startupManager.SetEnabled(m_ruleManager.AutoStart(), startupError)) {
            m_logger.Error(startupError);
            if (m_ruleManager.AutoStart()) {
                ShowError(L"设置开机启动失败", startupError);
            }
        }

        if (!m_blockerService.Start(m_ruleManager.Rules())) {
            m_logger.Error(L"进程监控启动失败");
            ShowError(L"启动进程监控失败", L"无法创建进程监控线程");
        }
        SetTimer(kRuntimeTimerId, kRuntimeRefreshIntervalMs, nullptr);
        RefreshListView(true);

        m_trayIconAdded = AddTrayIcon();
        if (m_startHidden) {
            ::ShowWindow(m_hWnd, SW_HIDE);
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
            default:
                break;
        }
        return 0;
    }

    LRESULT OnTimer(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
        handled = TRUE;
        if (wParam == kRuntimeTimerId) {
            RefreshListView(false);
        }
        return 0;
    }

    LRESULT OnCommand(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
        handled = TRUE;
        switch (LOWORD(wParam)) {
            case IDC_ADD_APP:
                AddApplication();
                break;
            case IDC_DELETE_APP:
                DeleteSelectedApplication();
                break;
            case IDC_TOGGLE_APP:
                ToggleSelectedApplication();
                break;
            case IDC_AUTOSTART:
                UpdateAutoStart();
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
        if (header != nullptr && header->idFrom == IDC_APP_LIST && header->code == NM_DBLCLK) {
            handled = TRUE;
            OpenSelectedLocation();
            return 0;
        }
        handled = FALSE;
        return 0;
    }

    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        ::ShowWindow(m_hWnd, SW_HIDE);
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        KillTimer(kRuntimeTimerId);
        m_blockerService.Stop();
        RemoveTrayIcon();
        m_logger.Info(L"程序退出");
        PostQuitMessage(0);
        return 0;
    }

    bool AddTrayIcon() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_hWnd;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(data.szTip, L"Hotkey Blocker");
        return Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
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

    void InitializeListView() {
        if (m_listView == nullptr) {
            return;
        }
        ListView_SetExtendedListViewStyle(
            m_listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        InsertColumn(0, L"应用", 150);
        InsertColumn(1, L"完整路径", 300);
        InsertColumn(2, L"启用", 50);
        InsertColumn(3, L"状态", 90);
        InsertColumn(4, L"详情", 250);
    }

    void InsertColumn(int index, const wchar_t* title, int width) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.cx = width;
        column.iSubItem = index;
        column.pszText = const_cast<LPWSTR>(title);
        SendMessageW(m_listView, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index),
                     reinterpret_cast<LPARAM>(&column));
    }

    void RefreshListView(bool force) {
        if (m_listView == nullptr) {
            return;
        }

        const std::vector<RuntimeRuleState> states = m_blockerService.Snapshot();
        if (!force && SameStates(states, m_renderedStates)) {
            return;
        }

        std::wstring selectedPath;
        const int selectedIndex = ListView_GetNextItem(m_listView, -1, LVNI_SELECTED);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_renderedStates.size())) {
            selectedPath = m_renderedStates[static_cast<std::size_t>(selectedIndex)].rule.path;
        }

        ListView_DeleteAllItems(m_listView);
        for (std::size_t index = 0; index < states.size(); ++index) {
            const RuntimeRuleState& state = states[index];
            const std::wstring name = std::filesystem::path(state.rule.path).filename().wstring();
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(index);
            item.pszText = const_cast<LPWSTR>(name.c_str());
            SendMessageW(m_listView, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
            SetListItemText(static_cast<int>(index), 1,
                            const_cast<LPWSTR>(state.rule.path.c_str()));
            SetListItemText(static_cast<int>(index), 2,
                            const_cast<LPWSTR>(state.rule.enabled ? L"是" : L"否"));
            SetListItemText(static_cast<int>(index), 3,
                            const_cast<LPWSTR>(AppStatusText(state.status)));
            SetListItemText(static_cast<int>(index), 4,
                            const_cast<LPWSTR>(state.detail.c_str()));
        }

        if (!selectedPath.empty()) {
            for (std::size_t index = 0; index < states.size(); ++index) {
                if (!PathUtils::SamePath(selectedPath, states[index].rule.path)) {
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
        m_renderedStates = states;
    }

    static bool SameStates(const std::vector<RuntimeRuleState>& left,
                           const std::vector<RuntimeRuleState>& right) {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (!PathUtils::SamePath(left[index].rule.path, right[index].rule.path) ||
                left[index].rule.enabled != right[index].rule.enabled ||
                left[index].status != right[index].status ||
                left[index].detail != right[index].detail) {
                return false;
            }
        }
        return true;
    }

    void SetListItemText(int itemIndex, int subItemIndex, LPWSTR text) const {
        LVITEMW item{};
        item.iSubItem = subItemIndex;
        item.pszText = text;
        SendMessageW(m_listView, LVM_SETITEMTEXTW, static_cast<WPARAM>(itemIndex),
                     reinterpret_cast<LPARAM>(&item));
    }

    int SelectedIndex() const {
        if (m_listView == nullptr) {
            return -1;
        }
        return ListView_GetNextItem(m_listView, -1, LVNI_SELECTED);
    }

    void AddApplication() {
        CComPtr<IFileOpenDialog> dialog;
        HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dialog));
        if (FAILED(result)) {
            ShowErrorCode(L"创建文件选择器失败", result);
            return;
        }

        const COMDLG_FILTERSPEC filters[] = {{L"应用程序 (*.exe)", L"*.exe"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"选择要禁止全局快捷键的应用");
        result = dialog->Show(m_hWnd);
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            return;
        }
        if (FAILED(result)) {
            ShowErrorCode(L"打开文件选择器失败", result);
            return;
        }

        CComPtr<IShellItem> item;
        result = dialog->GetResult(&item);
        if (FAILED(result)) {
            ShowErrorCode(L"获取所选文件失败", result);
            return;
        }

        PWSTR path = nullptr;
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (FAILED(result) || path == nullptr) {
            ShowErrorCode(L"获取所选文件路径失败", result);
            return;
        }

        const bool added = m_ruleManager.Add(path);
        CoTaskMemFree(path);
        if (!added) {
            ShowError(L"添加应用失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void DeleteSelectedApplication() {
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(m_ruleManager.Rules().size())) {
            ShowError(L"删除应用失败", L"请先选择一个应用");
            return;
        }

        const AppRule& rule = m_ruleManager.Rules()[static_cast<std::size_t>(index)];
        const std::wstring message = L"确定删除规则？\n\n" + rule.path;
        if (::MessageBoxW(m_hWnd, message.c_str(), L"删除应用",
                          MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }

        if (!m_ruleManager.Remove(rule.path)) {
            ShowError(L"删除应用失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void ToggleSelectedApplication() {
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(m_ruleManager.Rules().size())) {
            ShowError(L"修改应用状态失败", L"请先选择一个应用");
            return;
        }

        const AppRule& rule = m_ruleManager.Rules()[static_cast<std::size_t>(index)];
        if (!m_ruleManager.SetEnabled(rule.path, !rule.enabled)) {
            ShowError(L"修改应用状态失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void OpenSelectedLocation() {
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(m_ruleManager.Rules().size())) {
            return;
        }

        const std::wstring parameters = L"/select,\"" +
                                        m_ruleManager.Rules()[static_cast<std::size_t>(index)].path +
                                        L"\"";
        const HINSTANCE result = ShellExecuteW(m_hWnd, L"open", L"explorer.exe",
                                               parameters.c_str(), nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) {
            ShowError(L"打开文件位置失败", L"无法打开资源管理器");
        }
    }

    void UpdateAutoStart() {
        const bool enabled = ::IsDlgButtonChecked(m_hWnd, IDC_AUTOSTART) == BST_CHECKED;
        const bool previous = m_ruleManager.AutoStart();
        if (!m_ruleManager.SetAutoStart(enabled)) {
            ::CheckDlgButton(m_hWnd, IDC_AUTOSTART, previous ? BST_CHECKED : BST_UNCHECKED);
            ShowError(L"保存开机启动设置失败", m_ruleManager.LastError());
            return;
        }

        std::wstring error;
        if (!m_startupManager.SetEnabled(enabled, error)) {
            m_ruleManager.SetAutoStart(previous);
            ::CheckDlgButton(m_hWnd, IDC_AUTOSTART, previous ? BST_CHECKED : BST_UNCHECKED);
            m_logger.Error(error);
            ShowError(L"设置开机启动失败", error);
        }
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
    bool m_trayIconAdded = false;
    HWND m_listView = nullptr;
    Logger m_logger;
    RuleManager m_ruleManager;
    StartupManager m_startupManager;
    BlockerService m_blockerService;
    std::vector<RuntimeRuleState> m_renderedStates;
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

    AtlInitCommonControls(ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES);
    CMessageLoop messageLoop;
    _Module.AddMessageLoop(&messageLoop);

    const bool startHidden = commandLine != nullptr &&
                             wcsstr(commandLine, L"--background") != nullptr;
    MainWindow window(startHidden);
    if (window.Create(nullptr) == nullptr) {
        _Module.RemoveMessageLoop();
        _Module.Term();
        CoUninitialize();
        return 1;
    }
    window.ShowWindow(startHidden ? SW_HIDE : SW_SHOWNORMAL);

    const int exitCode = messageLoop.Run();
    _Module.RemoveMessageLoop();
    _Module.Term();
    CoUninitialize();
    return exitCode;
}
