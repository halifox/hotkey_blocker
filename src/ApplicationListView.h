#pragma once

#include "WindowsTarget.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlgdi.h>
#include <atlmisc.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct ApplicationListRow {
    std::wstring path;
    bool enabled = false;
    std::wstring status;
    std::wstring detail;
};

enum class ApplicationListAction {
    Configure,
    Delete,
};

class ApplicationListView final : public ATL::CWindowImpl<ApplicationListView, CListViewCtrl> {
public:
    using ActionHandler = std::function<void(const std::wstring&, ApplicationListAction)>;

    DECLARE_WND_SUPERCLASS(nullptr, WC_LISTVIEW)

    BEGIN_MSG_MAP(ApplicationListView)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
        MESSAGE_HANDLER(WM_SETCURSOR, OnSetCursor)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
        MESSAGE_HANDLER(WM_VSCROLL, OnUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_HSCROLL, OnUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_KEYDOWN, OnUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_KEYUP, OnUpdateHoverAfterDefault)
        MESSAGE_HANDLER(WM_NCDESTROY, OnNcDestroy)
        REFLECTED_NOTIFY_CODE_HANDLER(NM_CUSTOMDRAW, OnCustomDraw)
        REFLECTED_NOTIFY_CODE_HANDLER(NM_CLICK, OnClick)
        REFLECTED_NOTIFY_CODE_HANDLER(LVN_ITEMCHANGED, OnItemChanged)
        REFLECTED_NOTIFY_CODE_HANDLER(LVN_GETINFOTIPW, OnGetInfoTip)
    END_MSG_MAP()

    void Initialize();
    void SetActionHandler(ActionHandler handler);
    void SetRows(const std::vector<ApplicationListRow>& rows, bool force);
    std::wstring SelectedPath() const;

private:
    LRESULT OnCustomDraw(int, LPNMHDR notification, BOOL& handled);
    LRESULT OnClick(int, LPNMHDR, BOOL& handled);
    LRESULT OnItemChanged(int, LPNMHDR, BOOL& handled);
    LRESULT OnGetInfoTip(int, LPNMHDR notification, BOOL& handled);
    LRESULT OnMouseMove(UINT, WPARAM, LPARAM lParam, BOOL& handled);
    LRESULT OnMouseLeave(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnSetCursor(UINT, WPARAM, LPARAM lParam, BOOL& handled);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnUpdateHoverAfterDefault(UINT message, WPARAM wParam, LPARAM lParam,
                                      BOOL& handled);
    LRESULT OnNcDestroy(UINT, WPARAM, LPARAM, BOOL& handled);

    bool GetActionHitRect(int rowIndex, int subItemIndex, CRect& rect) const;
    bool GetActionHitAtPoint(CPoint point, int& rowIndex, int& subItemIndex) const;
    bool GetActionHitAtCursor(int& rowIndex, int& subItemIndex) const;
    void InvalidateActionCell(int rowIndex, int subItemIndex);
    void SetHoveredAction(int rowIndex, int subItemIndex);
    void UpdateHoveredActionFromCursor();
    void TrackMouseMove(LPARAM lParam);
    void UpdatePathColumnWidth();
    int FileIconIndex(const std::wstring& path);
    void InsertRow(int itemIndex, const ApplicationListRow& row);

    static bool SameRows(const std::vector<ApplicationListRow>& left,
                         const std::vector<ApplicationListRow>& right);

    ActionHandler m_actionHandler;
    std::vector<ApplicationListRow> m_rows;
    std::unordered_map<std::wstring, int> m_iconIndices;
    CImageList m_systemImageList;  // Non-owning wrapper for the shared shell image list.
    CFont m_actionHoverFont;
    bool m_trackingMouseLeave = false;
    int m_hoveredActionRow = -1;
    int m_hoveredActionColumn = -1;
};
