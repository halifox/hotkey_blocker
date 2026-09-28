#include "WindowsTarget.h"

#include "ApplicationListView.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <atluser.h>

#include "PathUtils.h"
#include "resource.h"

namespace {

constexpr int kRuleStateColumn = 1;
constexpr int kRuntimeStatusColumn = 2;
constexpr int kEnabledColumnWidth = 60;
constexpr int kStatusColumnWidth = 190;

}  // namespace

void ApplicationListView::Initialize() {
    if (!IsWindow()) {
        return;
    }

    SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
                             LVS_EX_LABELTIP | LVS_EX_INFOTIP);

    SHFILEINFOW shellFileInfo{};
    const DWORD_PTR systemImageList = SHGetFileInfoW(
        L"C:\\Windows", FILE_ATTRIBUTE_DIRECTORY, &shellFileInfo, sizeof(shellFileInfo),
        SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
    if (systemImageList != 0) {
        m_systemImageList = reinterpret_cast<HIMAGELIST>(systemImageList);
        SetImageList(m_systemImageList, LVSIL_SMALL);
    }

    CRect listClientRect;
    GetClientRect(&listClientRect);
    const int listWidth = listClientRect.Width();
    const int remainingWidth = listWidth - kEnabledColumnWidth - kStatusColumnWidth;
    const int pathColumnWidth = remainingWidth > 0 ? remainingWidth : 1;
    InsertColumn(0, L"目标路径", LVCFMT_LEFT, pathColumnWidth, 0);
    InsertColumn(kRuleStateColumn, L"规则状态", LVCFMT_CENTER, kEnabledColumnWidth,
                 kRuleStateColumn);
    InsertColumn(kRuntimeStatusColumn, L"拦截状态", LVCFMT_CENTER, kStatusColumnWidth,
                 kRuntimeStatusColumn);
}

void ApplicationListView::SetActionHandler(ActionHandler handler) {
    m_actionHandler = std::move(handler);
}

void ApplicationListView::SetRows(const std::vector<ApplicationListRow>& rows, bool force) {
    if (!IsWindow() || (!force && SameRows(rows, m_rows))) {
        return;
    }

    std::wstring selectedPath;
    const int selectedIndex = GetNextItem(-1, LVNI_SELECTED);
    if (selectedIndex >= 0 && selectedIndex < static_cast<int>(m_rows.size())) {
        selectedPath = m_rows[static_cast<std::size_t>(selectedIndex)].path;
    }

    SetRedraw(FALSE);
    DeleteAllItems();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        InsertRow(static_cast<int>(index), rows[index]);
    }
    SetRedraw(TRUE);
    InvalidateRect(nullptr, TRUE);

    if (!selectedPath.empty()) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
            if (!PathUtils::SamePath(rows[index].path, selectedPath)) {
                continue;
            }
            SetItemState(static_cast<int>(index), LVIS_SELECTED | LVIS_FOCUSED,
                         LVIS_SELECTED | LVIS_FOCUSED);
            break;
        }
    }

    m_rows = rows;
    UpdatePathColumnWidth();
}

std::wstring ApplicationListView::SelectedPath() const {
    const int selectedIndex = GetNextItem(-1, LVNI_SELECTED);
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(m_rows.size())) {
        return {};
    }
    return m_rows[static_cast<std::size_t>(selectedIndex)].path;
}

LRESULT ApplicationListView::OnGetInfoTip(int, LPNMHDR notification, BOOL& handled) {
    const auto* infoTip = reinterpret_cast<const NMLVGETINFOTIPW*>(notification);
    if (infoTip->iItem >= 0 && infoTip->iItem < static_cast<int>(m_rows.size()) &&
        infoTip->pszText != nullptr && infoTip->cchTextMax > 0) {
        const ApplicationListRow& row = m_rows[static_cast<std::size_t>(infoTip->iItem)];
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

LRESULT ApplicationListView::OnSize(UINT message, WPARAM wParam, LPARAM lParam,
                                    BOOL& handled) {
    const LRESULT result = DefWindowProc(message, wParam, lParam);
    UpdatePathColumnWidth();
    handled = TRUE;
    return result;
}

LRESULT ApplicationListView::OnContextMenu(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
    CPoint screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    if (lParam == static_cast<LPARAM>(-1)) {
        const int focusedIndex = GetNextItem(-1, LVNI_FOCUSED);
        if (focusedIndex >= 0) {
            CRect itemRect;
            if (GetItemRect(focusedIndex, &itemRect, LVIR_BOUNDS)) {
                screenPoint = CPoint(itemRect.left + 16,
                                     itemRect.top + itemRect.Height() / 2);
                ClientToScreen(&screenPoint);
            }
        } else {
            CPoint cursor;
            if (::GetCursorPos(&cursor)) {
                screenPoint = cursor;
            }
        }
    } else {
        CPoint clientPoint = screenPoint;
        ScreenToClient(&clientPoint);
        LVHITTESTINFO hit{};
        hit.pt = clientPoint;
        const int hitIndex = SubItemHitTest(&hit);
        if (hitIndex >= 0) {
            SetItemState(-1, 0, LVIS_SELECTED);
            SetItemState(hitIndex, LVIS_SELECTED | LVIS_FOCUSED,
                         LVIS_SELECTED | LVIS_FOCUSED);
            SetFocus();
        }
    }

    const std::wstring selectedPath = SelectedPath();
    if (selectedPath.empty()) {
        handled = TRUE;
        return 0;
    }

    CMenu menu;
    if (!menu.LoadMenu(MAKEINTRESOURCEW(IDR_APP_LIST_CONTEXT_MENU))) {
        handled = FALSE;
        return 0;
    }
    CMenuHandle popup = menu.GetSubMenu(0);
    if (popup.IsNull()) {
        handled = TRUE;
        return 0;
    }

    const UINT command = ::TrackPopupMenu(popup.m_hMenu,
                                          TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                          screenPoint.x, screenPoint.y, 0,
                                          m_hWnd, nullptr);
    if (!m_actionHandler) {
        handled = TRUE;
        return 0;
    }
    if (command == ID_APP_LIST_CONFIGURE) {
        m_actionHandler(selectedPath, ApplicationListAction::Configure);
    } else if (command == ID_APP_LIST_DELETE) {
        m_actionHandler(selectedPath, ApplicationListAction::Delete);
    }
    handled = TRUE;
    return 0;
}

LRESULT ApplicationListView::OnNcDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
    handled = FALSE;
    return 0;
}

void ApplicationListView::UpdatePathColumnWidth() {
    if (!IsWindow()) {
        return;
    }
    CRect listClientRect;
    GetClientRect(&listClientRect);
    const int fixedWidth = kEnabledColumnWidth + kStatusColumnWidth;
    const int remainingWidth = listClientRect.Width() - fixedWidth;
    SetColumnWidth(0, remainingWidth > 0 ? remainingWidth : 1);
}

int ApplicationListView::FileIconIndex(const std::wstring& path) {
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
    if (SHGetFileInfoW(path.c_str(), attributes, &shellFileInfo, sizeof(shellFileInfo), flags) == 0) {
        m_iconIndices.emplace(path, -1);
        return -1;
    }
    m_iconIndices.emplace(path, shellFileInfo.iIcon);
    return shellFileInfo.iIcon;
}

void ApplicationListView::InsertRow(int itemIndex, const ApplicationListRow& row) {
    InsertItem(itemIndex, row.path.c_str(), FileIconIndex(row.path));
    SetItemText(itemIndex, kRuleStateColumn, row.enabled ? L"启用" : L"停用");
    SetItemText(itemIndex, kRuntimeStatusColumn, row.status.c_str());
}

bool ApplicationListView::SameRows(const std::vector<ApplicationListRow>& left,
                                   const std::vector<ApplicationListRow>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].path != right[index].path || left[index].enabled != right[index].enabled ||
            left[index].status != right[index].status || left[index].detail != right[index].detail) {
            return false;
        }
    }
    return true;
}
