#pragma once

#include "WindowsTarget.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlframe.h>

#include <functional>
#include <string>

#include "ApplicationListView.h"
#include "resource.h"

class MainView final : public ATL::CDialogImpl<MainView>,
                       public WTL::CDialogResize<MainView> {
public:
    using ActionHandler = ApplicationListView::ActionHandler;

    enum { IDD = IDD_MAIN_VIEW };

    BEGIN_DLGRESIZE_MAP(MainView)
        DLGRESIZE_CONTROL(IDC_APP_LIST, DLSZ_SIZE_X | DLSZ_SIZE_Y)
        DLGRESIZE_CONTROL(IDC_APP_ADD_EXECUTABLE, DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(IDC_APP_ADD_FOLDER, DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(IDC_APP_CONFIGURE, DLSZ_MOVE_X | DLSZ_MOVE_Y)
        DLGRESIZE_CONTROL(IDC_APP_DELETE, DLSZ_MOVE_X | DLSZ_MOVE_Y)
    END_DLGRESIZE_MAP()

    BEGIN_MSG_MAP(MainView)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_ID_HANDLER(IDC_APP_ADD_EXECUTABLE, OnAddExecutable)
        COMMAND_ID_HANDLER(IDC_APP_ADD_FOLDER, OnAddFolder)
        COMMAND_ID_HANDLER(IDC_APP_CONFIGURE, OnConfigure)
        COMMAND_ID_HANDLER(IDC_APP_DELETE, OnDelete)
        NOTIFY_HANDLER(IDC_APP_LIST, LVN_ITEMCHANGED, OnListItemChanged)
        REFLECT_NOTIFICATIONS()
        CHAIN_MSG_MAP(WTL::CDialogResize<MainView>)
    END_MSG_MAP()

    bool IsReady() const noexcept;
    void SetActionHandler(ActionHandler handler);
    void SetRows(const std::vector<ApplicationListRow>& rows, bool force);
    std::wstring SelectedPath() const;
    void FocusList();
    BOOL PreTranslateMessage(MSG* message);

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnAddExecutable(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnAddFolder(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnConfigure(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnDelete(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnListItemChanged(int, LPNMHDR, BOOL& handled);
    void DispatchKeyboardAction(ApplicationListAction action);
    void UpdateSelectionButtons();

    ApplicationListView m_applicationList;
    ActionHandler m_actionHandler;
    bool m_ready = false;
};
