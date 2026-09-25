#pragma once

#include "WindowsTarget.h"

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>

#include "HotkeyPolicy.h"
#include "resource.h"

class HotkeyPolicyDialog final : public ATL::CDialogImpl<HotkeyPolicyDialog> {
public:
    enum { IDD = IDD_HOTKEY_POLICY_DIALOG };
    enum { kCaptureMessageMap = 1 };

    HotkeyPolicyDialog(HotkeyPolicy policy, bool enabled);

    const HotkeyPolicy& Policy() const noexcept;
    bool Enabled() const noexcept;

    BEGIN_MSG_MAP(HotkeyPolicyDialog)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        COMMAND_HANDLER(IDC_HOTKEY_MODE, CBN_SELCHANGE, OnModeChanged)
        COMMAND_HANDLER(IDC_HOTKEY_LIST, LBN_SELCHANGE, OnListSelectionChanged)
        COMMAND_ID_HANDLER(IDC_HOTKEY_ADD, OnAdd)
        COMMAND_ID_HANDLER(IDC_HOTKEY_REMOVE, OnRemove)
        COMMAND_ID_HANDLER(IDOK, OnOk)
        COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
        ALT_MSG_MAP(kCaptureMessageMap)
        MESSAGE_HANDLER(WM_GETDLGCODE, OnCaptureGetDlgCode)
        MESSAGE_HANDLER(WM_KEYDOWN, OnCaptureKeyDown)
        MESSAGE_HANDLER(WM_SYSKEYDOWN, OnCaptureKeyDown)
        MESSAGE_HANDLER(WM_CHAR, OnCaptureIgnoredCharacter)
        MESSAGE_HANDLER(WM_SYSCHAR, OnCaptureIgnoredCharacter)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnModeChanged(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnListSelectionChanged(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnAdd(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnRemove(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnOk(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnCancel(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnCaptureGetDlgCode(UINT, WPARAM, LPARAM lParam, BOOL& handled);
    LRESULT OnCaptureKeyDown(UINT, WPARAM wParam, LPARAM, BOOL& handled);
    LRESULT OnCaptureIgnoredCharacter(UINT, WPARAM, LPARAM, BOOL& handled);

    void RefreshList();
    void UpdateControls();

    HotkeyPolicy m_policy;
    bool m_enabled = true;
    CComboBox m_modeControl;
    CListBox m_hotkeyList;
    CButton m_ruleEnabledControl;
    CButton m_addButton;
    CButton m_removeButton;
    ATL::CContainedWindowT<CEdit> m_captureEdit;
    HotkeySpec m_capturedHotkey;
};
