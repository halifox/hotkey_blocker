#include "WindowsTarget.h"

#include "MainView.h"

#include <windows.h>

bool MainView::IsReady() const noexcept {
    return m_ready && m_applicationList.IsWindow();
}

void MainView::SetActionHandler(ActionHandler handler) {
    m_actionHandler = std::move(handler);
    m_applicationList.SetActionHandler(m_actionHandler);
}

void MainView::SetRows(const std::vector<ApplicationListRow>& rows, bool force) {
    m_applicationList.SetRows(rows, force);
}

std::wstring MainView::SelectedPath() const {
    return m_applicationList.SelectedPath();
}

void MainView::FocusList() {
    if (m_applicationList.IsWindow()) {
        m_applicationList.SetFocus();
    }
}

BOOL MainView::PreTranslateMessage(MSG* message) {
    if (message == nullptr || !IsReady()) {
        return FALSE;
    }

    if (message->hwnd == m_applicationList.m_hWnd && message->message == WM_KEYDOWN) {
        const bool isKeyRepeat = (message->lParam & (1L << 30)) != 0;
        if (message->wParam == VK_RETURN) {
            if (!isKeyRepeat) {
                DispatchKeyboardAction(ApplicationListAction::Configure);
            }
            return TRUE;
        }
        if (message->wParam == VK_DELETE) {
            if (!isKeyRepeat) {
                DispatchKeyboardAction(ApplicationListAction::Delete);
            }
            return TRUE;
        }
    }

    // Let the resource-backed child dialog handle Tab and Shift+Tab navigation.
    if (message->hwnd == m_hWnd || ::IsChild(m_hWnd, message->hwnd)) {
        return ::IsDialogMessageW(m_hWnd, message);
    }
    return FALSE;
}

LRESULT MainView::OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
    handled = TRUE;
    if (!m_applicationList.SubclassWindow(GetDlgItem(IDC_APP_LIST))) {
        return FALSE;
    }

    m_applicationList.Initialize();
    DlgResize_Init(false, false, WS_CLIPCHILDREN);
    m_ready = true;
    return TRUE;
}

void MainView::DispatchKeyboardAction(ApplicationListAction action) {
    if (!m_actionHandler) {
        return;
    }
    const std::wstring path = SelectedPath();
    if (!path.empty()) {
        m_actionHandler(path, action);
    }
}
