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

#include "PathUtils.h"

namespace {

constexpr int kRuleStateColumn = 1;
constexpr int kRuntimeStatusColumn = 2;
constexpr int kConfigureActionColumn = 3;
constexpr int kDeleteActionColumn = 4;
constexpr int kEnabledColumnWidth = 60;
constexpr int kStatusColumnWidth = 190;
constexpr int kConfigureColumnWidth = 74;
constexpr int kDeleteColumnWidth = 74;

}  // namespace

void ApplicationListView::Initialize() {
    if (!IsWindow()) {
        return;
    }

    HFONT listFont = GetFont();
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
    SendMessage(CCM_SETVERSION, 5, 0);
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
    const int remainingWidth = listWidth - kEnabledColumnWidth - kStatusColumnWidth -
                               kConfigureColumnWidth - kDeleteColumnWidth;
    const int pathColumnWidth = remainingWidth > 0 ? remainingWidth : 1;
    InsertColumn(0, L"目标路径", LVCFMT_LEFT, pathColumnWidth, 0);
    InsertColumn(kRuleStateColumn, L"规则状态", LVCFMT_CENTER, kEnabledColumnWidth,
                 kRuleStateColumn);
    InsertColumn(kRuntimeStatusColumn, L"拦截状态", LVCFMT_CENTER, kStatusColumnWidth,
                 kRuntimeStatusColumn);
    InsertColumn(kConfigureActionColumn, L"配置", LVCFMT_CENTER, kConfigureColumnWidth,
                 kConfigureActionColumn);
    InsertColumn(kDeleteActionColumn, L"删除", LVCFMT_CENTER, kDeleteColumnWidth,
                 kDeleteActionColumn);

    CHeaderCtrl header = GetHeader();
    if (header.IsWindow()) {
        header.ModifyStyle(0, HDS_NOSIZING);
    }
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

    SetHoveredAction(-1, -1);
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
    UpdateHoveredActionFromCursor();
    UpdatePathColumnWidth();
}

LRESULT ApplicationListView::OnCustomDraw(int, LPNMHDR notification, BOOL& handled) {
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
            const bool focusedSelection = selected && ::GetFocus() == m_hWnd;
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

LRESULT ApplicationListView::OnClick(int, LPNMHDR, BOOL& handled) {
    int rowIndex = -1;
    int subItemIndex = -1;
    if (GetActionHitAtCursor(rowIndex, subItemIndex) && m_actionHandler) {
        const ApplicationListAction action = subItemIndex == kConfigureActionColumn
                                                 ? ApplicationListAction::Configure
                                                 : ApplicationListAction::Delete;
        const std::wstring path = m_rows[static_cast<std::size_t>(rowIndex)].path;
        m_actionHandler(path, action);
        handled = TRUE;
        return 0;
    }
    handled = FALSE;
    return 0;
}

LRESULT ApplicationListView::OnItemChanged(int, LPNMHDR, BOOL& handled) {
    UpdateHoveredActionFromCursor();
    handled = FALSE;
    return 0;
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

LRESULT ApplicationListView::OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
    TrackMouseMove(lParam);
    handled = FALSE;
    return 0;
}

LRESULT ApplicationListView::OnMouseLeave(UINT, WPARAM, LPARAM, BOOL& handled) {
    m_trackingMouseLeave = false;
    SetHoveredAction(-1, -1);
    handled = FALSE;
    return 0;
}

LRESULT ApplicationListView::OnSetCursor(UINT, WPARAM, LPARAM lParam, BOOL& handled) {
    if (LOWORD(lParam) == HTCLIENT) {
        CPoint point;
        int rowIndex = -1;
        int subItemIndex = -1;
        if (::GetCursorPos(&point) && ScreenToClient(&point) &&
            GetActionHitAtPoint(point, rowIndex, subItemIndex)) {
            ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
            handled = TRUE;
            return TRUE;
        }
    }
    handled = FALSE;
    return 0;
}

LRESULT ApplicationListView::OnUpdateHoverAfterDefault(UINT message, WPARAM wParam,
                                                        LPARAM lParam, BOOL& handled) {
    const LRESULT result = DefWindowProc(message, wParam, lParam);
    UpdateHoveredActionFromCursor();
    handled = TRUE;
    return result;
}

LRESULT ApplicationListView::OnNcDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
    m_trackingMouseLeave = false;
    m_hoveredActionRow = -1;
    m_hoveredActionColumn = -1;
    handled = FALSE;
    return 0;
}

bool ApplicationListView::GetActionHitRect(int rowIndex, int subItemIndex, CRect& rect) const {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(m_rows.size()) ||
        (subItemIndex != kConfigureActionColumn && subItemIndex != kDeleteActionColumn) ||
        !GetSubItemRect(rowIndex, subItemIndex, LVIR_BOUNDS, &rect)) {
        return false;
    }
    rect.InflateRect(-4, -2);
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool ApplicationListView::GetActionHitAtPoint(CPoint point, int& rowIndex,
                                               int& subItemIndex) const {
    if (!IsWindow()) {
        return false;
    }

    LVHITTESTINFO hit{};
    hit.pt = point;
    const int hitRow = SubItemHitTest(&hit);
    if (hitRow < 0) {
        return false;
    }

    CRect actionRect;
    if (!GetActionHitRect(hitRow, hit.iSubItem, actionRect) || !actionRect.PtInRect(point)) {
        return false;
    }
    rowIndex = hitRow;
    subItemIndex = hit.iSubItem;
    return true;
}

bool ApplicationListView::GetActionHitAtCursor(int& rowIndex, int& subItemIndex) const {
    if (!IsWindow()) {
        return false;
    }

    CPoint point;
    if (!::GetCursorPos(&point) || !ScreenToClient(&point)) {
        return false;
    }
    return GetActionHitAtPoint(point, rowIndex, subItemIndex);
}

void ApplicationListView::InvalidateActionCell(int rowIndex, int subItemIndex) {
    CRect rect;
    if (GetActionHitRect(rowIndex, subItemIndex, rect)) {
        InvalidateRect(&rect, FALSE);
    }
}

void ApplicationListView::SetHoveredAction(int rowIndex, int subItemIndex) {
    if (rowIndex == m_hoveredActionRow && subItemIndex == m_hoveredActionColumn) {
        return;
    }
    InvalidateActionCell(m_hoveredActionRow, m_hoveredActionColumn);
    m_hoveredActionRow = rowIndex;
    m_hoveredActionColumn = subItemIndex;
    InvalidateActionCell(m_hoveredActionRow, m_hoveredActionColumn);
}

void ApplicationListView::UpdateHoveredActionFromCursor() {
    CPoint point;
    int rowIndex = -1;
    int subItemIndex = -1;
    if (::GetCursorPos(&point) && ScreenToClient(&point) &&
        GetActionHitAtPoint(point, rowIndex, subItemIndex)) {
        SetHoveredAction(rowIndex, subItemIndex);
    } else {
        SetHoveredAction(-1, -1);
    }
}

void ApplicationListView::TrackMouseMove(LPARAM lParam) {
    if (!m_trackingMouseLeave) {
        TRACKMOUSEEVENT trackMouse{};
        trackMouse.cbSize = sizeof(trackMouse);
        trackMouse.dwFlags = TME_LEAVE;
        trackMouse.hwndTrack = m_hWnd;
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

void ApplicationListView::UpdatePathColumnWidth() {
    if (!IsWindow()) {
        return;
    }
    CRect listClientRect;
    GetClientRect(&listClientRect);
    const int fixedWidth = kEnabledColumnWidth + kStatusColumnWidth + kConfigureColumnWidth +
                           kDeleteColumnWidth;
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
    SetItemText(itemIndex, kConfigureActionColumn, L"配置");
    SetItemText(itemIndex, kDeleteActionColumn, L"删除");
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
