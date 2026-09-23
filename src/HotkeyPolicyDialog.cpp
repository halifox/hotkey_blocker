#include "WindowsTarget.h"

#include "HotkeyPolicyDialog.h"

#include <iterator>
#include <string>
#include <utility>

namespace {

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

}  // namespace

HotkeyPolicyDialog::HotkeyPolicyDialog(HotkeyPolicy policy, bool enabled)
    : m_policy(std::move(policy)),
      m_enabled(enabled),
      m_captureEdit(this, kCaptureMessageMap) {
    NormalizeHotkeyPolicy(m_policy);
    if (!IsValidHotkeyMode(m_policy.mode)) {
        m_policy.mode = HotkeyMode::BlockAll;
    }
}

const HotkeyPolicy& HotkeyPolicyDialog::Policy() const noexcept {
    return m_policy;
}

bool HotkeyPolicyDialog::Enabled() const noexcept {
    return m_enabled;
}

LRESULT HotkeyPolicyDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
    handled = TRUE;
    m_modeControl = GetDlgItem(IDC_HOTKEY_MODE);
    m_hotkeyList = GetDlgItem(IDC_HOTKEY_LIST);
    m_ruleEnabledControl = GetDlgItem(IDC_RULE_ENABLED);
    m_addButton = GetDlgItem(IDC_HOTKEY_ADD);
    m_removeButton = GetDlgItem(IDC_HOTKEY_REMOVE);
    m_captureEdit.SubclassWindow(GetDlgItem(IDC_HOTKEY_CAPTURE));
    m_ruleEnabledControl.SetCheck(m_enabled ? BST_CHECKED : BST_UNCHECKED);

    for (const HotkeyMode modeValue :
         {HotkeyMode::BlockAll, HotkeyMode::Blacklist, HotkeyMode::Whitelist}) {
        m_modeControl.AddString(HotkeyModeText(modeValue));
    }
    m_modeControl.SetCurSel(static_cast<int>(m_policy.mode));
    RefreshList();
    UpdateControls();
    CenterWindow(GetParent());
    return TRUE;
}

LRESULT HotkeyPolicyDialog::OnModeChanged(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    const int selection = m_modeControl.GetCurSel();
    if (selection >= 0 && selection <= static_cast<int>(HotkeyMode::Whitelist)) {
        m_policy.mode = static_cast<HotkeyMode>(selection);
    }
    UpdateControls();
    return 0;
}

LRESULT HotkeyPolicyDialog::OnListSelectionChanged(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    UpdateControls();
    return 0;
}

LRESULT HotkeyPolicyDialog::OnAdd(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    if (m_policy.mode == HotkeyMode::BlockAll) {
        return 0;
    }
    if (!IsValidHotkey(m_capturedHotkey)) {
        MessageBox(L"请先在输入框中按下要配置的快捷键。", L"添加快捷键",
                   MB_OK | MB_ICONINFORMATION);
        return 0;
    }
    const HotkeySpec hotkey = m_capturedHotkey;
    if (!ContainsHotkey(m_policy, hotkey)) {
        m_policy.hotkeys.push_back(hotkey);
        NormalizeHotkeyPolicy(m_policy);
        RefreshList();
    }
    m_capturedHotkey = {};
    m_captureEdit.SetWindowText(L"");
    m_captureEdit.SetFocus();
    return 0;
}

LRESULT HotkeyPolicyDialog::OnRemove(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    const int selection = m_hotkeyList.GetCurSel();
    if (selection >= 0 && selection < static_cast<int>(m_policy.hotkeys.size())) {
        m_policy.hotkeys.erase(m_policy.hotkeys.begin() + selection);
        RefreshList();
    }
    return 0;
}

LRESULT HotkeyPolicyDialog::OnOk(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    NormalizeHotkeyPolicy(m_policy);
    if (!ValidateHotkeyPolicy(m_policy)) {
        MessageBox(L"快捷键策略无效，请检查配置。", L"保存快捷键策略",
                   MB_OK | MB_ICONERROR);
        return 0;
    }
    m_enabled = m_ruleEnabledControl.GetCheck() == BST_CHECKED;
    EndDialog(IDOK);
    return 0;
}

LRESULT HotkeyPolicyDialog::OnCancel(WORD, WORD, HWND, BOOL& handled) {
    handled = TRUE;
    EndDialog(IDCANCEL);
    return 0;
}

LRESULT HotkeyPolicyDialog::OnCaptureGetDlgCode(UINT, WPARAM, LPARAM, BOOL& handled) {
    handled = TRUE;
    return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
}

LRESULT HotkeyPolicyDialog::OnCaptureKeyDown(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
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
    m_capturedHotkey = NormalizeHotkey(captured);
    const std::wstring text = FormatHotkey(m_capturedHotkey);
    m_captureEdit.SetWindowText(text.c_str());
    return 0;
}

LRESULT HotkeyPolicyDialog::OnCaptureIgnoredCharacter(UINT, WPARAM, LPARAM, BOOL& handled) {
    handled = TRUE;
    return 0;
}

void HotkeyPolicyDialog::RefreshList() {
    m_hotkeyList.ResetContent();
    for (const HotkeySpec& hotkey : m_policy.hotkeys) {
        const std::wstring text = FormatHotkey(hotkey);
        m_hotkeyList.AddString(text.c_str());
    }
    UpdateControls();
}

void HotkeyPolicyDialog::UpdateControls() {
    const bool editable = m_policy.mode != HotkeyMode::BlockAll;
    m_hotkeyList.EnableWindow(editable);
    m_captureEdit.EnableWindow(editable);
    m_addButton.EnableWindow(editable);
    const int selection = m_hotkeyList.GetCurSel();
    m_removeButton.EnableWindow(editable && selection >= 0 &&
                                 selection < static_cast<int>(m_policy.hotkeys.size()));
}
