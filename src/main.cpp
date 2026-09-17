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

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ApplicationDiscovery.h"
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
constexpr UINT kStateChangedMessage = WM_APP + 2;
constexpr UINT kDiscoveryCompletedMessage = WM_APP + 3;

}  // namespace

class MainWindow final : public ATL::CDialogImpl<MainWindow> {
public:
    enum { IDD = IDD_MAIN_WINDOW };

    explicit MainWindow(bool startHidden)
        : m_startHidden(startHidden), m_blockerService(&m_logger) {}

    ~MainWindow() {
        StopDiscoveryScan();
    }

    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(kStateChangedMessage, OnStateChanged)
        MESSAGE_HANDLER(kDiscoveryCompletedMessage, OnDiscoveryCompleted)
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

private:
    struct DisplayRow {
        std::wstring key;
        std::wstring name;
        std::wstring path;
        std::wstring source;
        std::wstring enabled;
        std::wstring status;
        std::wstring detail;
        int imageIndex = -1;
        bool isRule = false;
    };

    struct DiscoveryResult {
        std::vector<DiscoveredApplication> applications;
        std::wstring warning;
        DiscoveryStats stats;
    };

    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        m_listView = GetDlgItem(IDC_APP_LIST);
        InitializeListView();

        m_logger.Info(L"程序启动");
        if (!m_ruleManager.Load()) {
            m_logger.Error(L"加载规则失败：" + m_ruleManager.LastError());
            ShowError(L"加载配置失败", m_ruleManager.LastError());
            m_initializationFailed = true;
            ::PostMessageW(m_hWnd, WM_CLOSE, 0, 0);
            return TRUE;
        }
        m_logger.Info(L"加载规则：" + std::to_wstring(m_ruleManager.Rules().size()) + L" 条");

        ::CheckDlgButton(m_hWnd, IDC_AUTOSTART,
                         m_ruleManager.AutoStart() ? BST_CHECKED : BST_UNCHECKED);
        std::wstring startupError;
        if (!m_startupManager.SetEnabled(m_ruleManager.AutoStart(), startupError)) {
            m_logger.Error(startupError);
            if (m_ruleManager.AutoStart()) {
                ShowError(L"设置开机启动失败", startupError);
            }
        }

        m_blockerService.SetStateChangedCallback([this] { QueueStateRefresh(); });
        if (!m_blockerService.Start(m_ruleManager.Rules())) {
            m_logger.Error(L"进程监控启动失败");
            ShowError(L"启动进程监控失败", L"无法创建进程监控线程");
        }
        RefreshListView(true);
        StartDiscoveryScan();

        m_trayIconAdded = AddTrayIcon();
        NotifyActionableStates();
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

    LRESULT OnDiscoveryCompleted(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        if (m_discoveryThread.joinable()) {
            m_discoveryThread.join();
        }

        std::optional<DiscoveryResult> result;
        {
            std::lock_guard lock(m_discoveryMutex);
            result = std::move(m_pendingDiscovery);
            m_pendingDiscovery.reset();
        }
        m_discoveryRunning.store(false, std::memory_order_release);
        if (HWND refreshButton = GetDlgItem(IDC_REFRESH_APPS); refreshButton != nullptr) {
            ::EnableWindow(refreshButton, TRUE);
        }
        if (!result.has_value()) {
            return 0;
        }
        if (!result->warning.empty()) {
            m_logger.Error(L"应用扫描提示：" + result->warning);
        }
        m_discoveredApps = std::move(result->applications);
        m_discoveryStats = result->stats;
        SetScanStatusText(DiscoverySummary(m_discoveryStats));
        m_logger.Info(L"应用扫描完成：" + DiscoverySummary(m_discoveryStats));
        RefreshListView(true);
        return 0;
    }

    LRESULT OnCommand(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
        handled = TRUE;
        switch (LOWORD(wParam)) {
            case IDC_ADD_APP:
                AddSelectedApplication();
                break;
            case IDC_ADD_PORTABLE:
                AddPortableApplication();
                break;
            case IDC_REFRESH_APPS:
                StartDiscoveryScan();
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
        if (header != nullptr && header->idFrom == IDC_APP_LIST && header->code == NM_DBLCLK) {
            handled = TRUE;
            const int index = SelectedIndex();
            if (index >= 0 && index < static_cast<int>(m_renderedRows.size()) &&
                !m_renderedRows[static_cast<std::size_t>(index)].isRule) {
                AddSelectedApplication();
            } else {
                OpenSelectedLocation();
            }
            return 0;
        }
        handled = FALSE;
        return 0;
    }

    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        if (m_initializationFailed) {
            DestroyWindow();
            return 0;
        }
        ::ShowWindow(m_hWnd, SW_HIDE);
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        m_blockerService.SetStateChangedCallback({});
        StopDiscoveryScan();
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

    void InitializeListView() {
        if (m_listView == nullptr) {
            return;
        }
        ListView_SetExtendedListViewStyle(
            m_listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
                            LVS_EX_LABELTIP);
        SHFILEINFOW shellFileInfo{};
        const DWORD_PTR systemImageList = SHGetFileInfoW(
            L"C:\\Windows", FILE_ATTRIBUTE_DIRECTORY, &shellFileInfo, sizeof(shellFileInfo),
            SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (systemImageList != 0) {
            m_systemImageList = reinterpret_cast<HIMAGELIST>(systemImageList);
            ListView_SetImageList(m_listView, m_systemImageList, LVSIL_SMALL);
        }
        InsertColumn(0, L"应用", 170);
        InsertColumn(1, L"完整路径", 360);
        InsertColumn(2, L"来源", 70);
        InsertColumn(3, L"启用", 55);
        InsertColumn(4, L"状态", 89);
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

        const std::vector<DisplayRow> rows = BuildDisplayRows();
        if (!force && SameRows(rows, m_renderedRows)) {
            return;
        }

        std::wstring selectedKey;
        const int selectedIndex = ListView_GetNextItem(m_listView, -1, LVNI_SELECTED);
        if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_renderedRows.size())) {
            selectedKey = m_renderedRows[static_cast<std::size_t>(selectedIndex)].key;
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
                if (rows[index].key != selectedKey) {
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
    }

    std::vector<DisplayRow> BuildDisplayRows() const {
        const std::vector<RuntimeRuleState> states = m_blockerService.Snapshot();
        std::vector<DisplayRow> rows;
        rows.reserve(states.size() + m_discoveredApps.size());

        const auto displayNameForRule = [](const AppRule& rule) {
            if (!rule.displayName.empty()) {
                return rule.displayName;
            }
            return std::filesystem::path(rule.path).stem().wstring();
        };

        const auto hasRuleTarget = [&states](const std::wstring& path) {
            for (const RuntimeRuleState& state : states) {
                if (PathUtils::SamePath(state.rule.path, path)) {
                    return true;
                }
                for (const std::wstring& target : state.rule.targets) {
                    if (PathUtils::SamePath(target, path)) {
                        return true;
                    }
                }
            }
            return false;
        };

        for (const RuntimeRuleState& state : states) {
            DisplayRow row;
            row.key = state.rule.path;
            row.name = displayNameForRule(state.rule);
            row.path = state.rule.path;
            row.source = AppSourceText(state.rule.source);
            row.enabled = state.rule.enabled ? L"是" : L"否";
            row.status = IsActionableStatus(state.status) ? AppStatusText(state.status) : L"";
            row.detail = state.detail;
            row.imageIndex = FileIconIndex(state.rule.path);
            if (state.rule.targets.size() > 1) {
                if (!row.detail.empty()) {
                    row.detail += L"；";
                }
                row.detail += L"目标进程 " + std::to_wstring(state.rule.targets.size()) + L" 个";
            }
            row.isRule = true;
            rows.push_back(std::move(row));
        }

        for (const DiscoveredApplication& application : m_discoveredApps) {
            if (hasRuleTarget(application.path)) {
                continue;
            }
            DisplayRow row;
            row.key = application.path;
            row.name = application.displayName.empty()
                           ? std::filesystem::path(application.path).stem().wstring()
                           : application.displayName;
            row.path = application.path;
            row.source = AppSourceText(application.source);
            row.enabled = L"—";
            row.status.clear();
            row.imageIndex = FileIconIndex(application.path);
            row.detail = application.publisher;
            if (!application.version.empty()) {
                if (!row.detail.empty()) {
                    row.detail += L"；";
                }
                row.detail += L"版本 " + application.version;
            }
            if (row.detail.empty()) {
                row.detail = L"选择后加入拦截规则";
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
            if (left[index].key != right[index].key || left[index].name != right[index].name ||
                left[index].path != right[index].path ||
                left[index].source != right[index].source ||
                left[index].enabled != right[index].enabled ||
                left[index].status != right[index].status ||
                left[index].detail != right[index].detail ||
                left[index].imageIndex != right[index].imageIndex ||
                left[index].isRule != right[index].isRule) {
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
        item.pszText = const_cast<LPWSTR>(row.name.c_str());
        SendMessageW(m_listView, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        SetListItemText(itemIndex, 1, const_cast<LPWSTR>(row.path.c_str()));
        SetListItemText(itemIndex, 2, const_cast<LPWSTR>(row.source.c_str()));
        SetListItemText(itemIndex, 3, const_cast<LPWSTR>(row.enabled.c_str()));
        SetListItemText(itemIndex, 4, const_cast<LPWSTR>(row.status.c_str()));
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

    std::vector<int> SelectedIndices() const {
        std::vector<int> indices;
        if (m_listView == nullptr) {
            return indices;
        }
        for (int index = -1;;) {
            index = ListView_GetNextItem(m_listView, index, LVNI_SELECTED);
            if (index < 0) {
                break;
            }
            indices.push_back(index);
        }
        return indices;
    }

    void AddSelectedApplication() {
        const std::vector<int> selected = SelectedIndices();
        std::vector<const DisplayRow*> candidates;
        for (const int index : selected) {
            if (index >= 0 && index < static_cast<int>(m_renderedRows.size()) &&
                !m_renderedRows[static_cast<std::size_t>(index)].isRule) {
                candidates.push_back(&m_renderedRows[static_cast<std::size_t>(index)]);
            }
        }
        if (candidates.empty()) {
            ShowError(L"添加应用失败", L"请先选择一个未添加的已安装程序；便携程序请使用“添加便携程序”");
            return;
        }

        AppRule rule;
        rule.path = candidates.front()->path;
        rule.displayName = candidates.front()->name;
        rule.source = AppSource::Installed;
        rule.enabled = true;
        for (const DisplayRow* candidate : candidates) {
            rule.targets.push_back(candidate->path);
        }
        if (!m_ruleManager.AddRule(std::move(rule))) {
            ShowError(L"添加应用失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void AddPortableApplication() {
        CComPtr<IFileOpenDialog> dialog;
        HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dialog));
        if (FAILED(result)) {
            ShowErrorCode(L"创建文件选择器失败", result);
            return;
        }

        const COMDLG_FILTERSPEC filters[] = {{L"应用程序 (*.exe)", L"*.exe"}};
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(L"选择便携式应用程序");
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
        if (index < 0 || index >= static_cast<int>(m_renderedRows.size()) ||
            !m_renderedRows[static_cast<std::size_t>(index)].isRule) {
            ShowError(L"删除应用失败", L"请先选择一个应用");
            return;
        }

        const DisplayRow& row = m_renderedRows[static_cast<std::size_t>(index)];
        const std::wstring message = L"确定删除规则？\n\n" + row.path;
        if (::MessageBoxW(m_hWnd, message.c_str(), L"删除应用",
                          MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }

        if (!m_ruleManager.Remove(row.path)) {
            ShowError(L"删除应用失败", m_ruleManager.LastError());
            return;
        }
        m_blockerService.UpdateRules(m_ruleManager.Rules());
        RefreshListView(true);
    }

    void ToggleSelectedApplication() {
        const int index = SelectedIndex();
        if (index < 0 || index >= static_cast<int>(m_renderedRows.size()) ||
            !m_renderedRows[static_cast<std::size_t>(index)].isRule) {
            ShowError(L"修改应用状态失败", L"请先选择一个应用");
            return;
        }

        const DisplayRow& row = m_renderedRows[static_cast<std::size_t>(index)];
        if (!m_ruleManager.SetEnabled(row.path, row.enabled != L"是")) {
            ShowError(L"修改应用状态失败", m_ruleManager.LastError());
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

    void StartDiscoveryScan() {
        if (m_discoveryRunning.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        m_discoveryCancel.store(false, std::memory_order_release);
        SetScanStatusText(L"正在扫描已安装程序...");
        if (HWND refreshButton = GetDlgItem(IDC_REFRESH_APPS); refreshButton != nullptr) {
            ::EnableWindow(refreshButton, FALSE);
        }

        const HWND window = m_hWnd;
        try {
            m_discoveryThread = std::thread([this, window] {
                DiscoveryResult result;
                result.applications = ApplicationDiscovery::Scan(
                    result.warning, &m_discoveryCancel, &result.stats);
                {
                    std::lock_guard lock(m_discoveryMutex);
                    m_pendingDiscovery = std::move(result);
                }
                if (window != nullptr) {
                    ::PostMessageW(window, kDiscoveryCompletedMessage, 0, 0);
                }
            });
        } catch (...) {
            m_discoveryRunning.store(false, std::memory_order_release);
            if (HWND refreshButton = GetDlgItem(IDC_REFRESH_APPS); refreshButton != nullptr) {
                ::EnableWindow(refreshButton, TRUE);
            }
            SetScanStatusText(L"应用扫描启动失败");
            ShowError(L"刷新程序失败", L"无法创建应用扫描线程");
        }
    }

    void StopDiscoveryScan() {
        m_discoveryCancel.store(true, std::memory_order_release);
        if (m_discoveryThread.joinable()) {
            m_discoveryThread.join();
        }
        m_discoveryRunning.store(false, std::memory_order_release);
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

    static bool IsActionableStatus(AppStatus status) {
        switch (status) {
            case AppStatus::RestartRequired:
            case AppStatus::PartiallyBlocked:
            case AppStatus::InjectionFailed:
            case AppStatus::PathMissing:
                return true;
            case AppStatus::Waiting:
            case AppStatus::Injecting:
            case AppStatus::Blocked:
            case AppStatus::Disabled:
            default:
                return false;
        }
    }

    static std::wstring DiscoverySummary(const DiscoveryStats& stats) {
        std::wstring result = L"发现 " + std::to_wstring(stats.applications) + L" 个可用程序";
        if (stats.fixedExecutableEntries != 0) {
            result += L"，检查 " + std::to_wstring(stats.fixedExecutableEntries) +
                      L" 个固定目录程序文件";
        }
        if (stats.unresolvedEntries != 0) {
            result += L"，跳过 " + std::to_wstring(stats.unresolvedEntries) +
                      L" 个无法定位启动文件的条目";
        }
        if (stats.filteredEntries != 0) {
            result += L"，过滤 " + std::to_wstring(stats.filteredEntries) +
                      L" 个系统组件、更新项、辅助程序或低信息文件";
        }
        return result;
    }

    void SetScanStatusText(const std::wstring& text) const {
        if (HWND status = GetDlgItem(IDC_SCAN_STATUS); status != nullptr) {
            ::SetWindowTextW(status, text.c_str());
        }
    }

    void NotifyActionableStates() {
        const std::vector<RuntimeRuleState> states = m_blockerService.Snapshot();
        std::unordered_map<std::wstring, AppStatus> current;
        for (const RuntimeRuleState& state : states) {
            if (!IsActionableStatus(state.status)) {
                continue;
            }
            current[state.rule.path] = state.status;
            if (!m_trayIconAdded) {
                continue;
            }

            const auto previous = m_notifiedActionableStates.find(state.rule.path);
            if (previous != m_notifiedActionableStates.end() &&
                previous->second == state.status) {
                continue;
            }

            std::wstring message = AppStatusText(state.status);
            if (!state.detail.empty()) {
                message += L"：" + state.detail;
            }
            ShowTrayNotification(state.rule.displayName.empty() ? L"应用状态提醒"
                                                                 : state.rule.displayName,
                                 message);
            m_notifiedActionableStates[state.rule.path] = state.status;
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
    Logger m_logger;
    RuleManager m_ruleManager;
    StartupManager m_startupManager;
    BlockerService m_blockerService;
    std::vector<DisplayRow> m_renderedRows;
    std::vector<DiscoveredApplication> m_discoveredApps;
    DiscoveryStats m_discoveryStats;
    mutable std::unordered_map<std::wstring, int> m_iconIndices;
    std::unordered_map<std::wstring, AppStatus> m_notifiedActionableStates;
    std::atomic_bool m_stateNotificationPosted = false;
    std::atomic_bool m_discoveryRunning = false;
    std::atomic_bool m_discoveryCancel = false;
    std::mutex m_discoveryMutex;
    std::optional<DiscoveryResult> m_pendingDiscovery;
    std::thread m_discoveryThread;
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
    window.ShowWindow(startHidden ? SW_HIDE : SW_SHOWNORMAL);

    const int exitCode = messageLoop.Run();
    CloseHandle(instanceMutex);
    _Module.RemoveMessageLoop();
    _Module.Term();
    CoUninitialize();
    return exitCode;
}
