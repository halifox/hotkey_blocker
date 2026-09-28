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
    AddExecutable,
    AddFolder,
};

class ApplicationListView final : public ATL::CWindowImpl<ApplicationListView, CListViewCtrl> {
public:
    using ActionHandler = std::function<void(const std::wstring&, ApplicationListAction)>;

    DECLARE_WND_SUPERCLASS(nullptr, WC_LISTVIEW)

    BEGIN_MSG_MAP(ApplicationListView)
        MESSAGE_HANDLER(WM_SIZE, OnSize)
        MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
        MESSAGE_HANDLER(WM_NCDESTROY, OnNcDestroy)
        REFLECTED_NOTIFY_CODE_HANDLER(LVN_GETINFOTIPW, OnGetInfoTip)
    END_MSG_MAP()

    void Initialize();
    void SetActionHandler(ActionHandler handler);
    void SetRows(const std::vector<ApplicationListRow>& rows, bool force);
    std::wstring SelectedPath() const;

private:
    LRESULT OnGetInfoTip(int, LPNMHDR notification, BOOL& handled);
    LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnNcDestroy(UINT, WPARAM, LPARAM, BOOL& handled);

    void UpdatePathColumnWidth();
    int FileIconIndex(const std::wstring& path);
    void InsertRow(int itemIndex, const ApplicationListRow& row);

    static bool SameRows(const std::vector<ApplicationListRow>& left,
                         const std::vector<ApplicationListRow>& right);

    ActionHandler m_actionHandler;
    std::vector<ApplicationListRow> m_rows;
    std::unordered_map<std::wstring, int> m_iconIndices;
    CImageList m_systemImageList;  // Non-owning wrapper for the shared shell image list.
};
