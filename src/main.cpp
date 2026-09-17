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
#include <shellapi.h>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atldlgs.h>

#include "resource.h"

CAppModule _Module;

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;

}  // namespace

class MainWindow final : public ATL::CDialogImpl<MainWindow> {
public:
    enum { IDD = IDD_MAIN_WINDOW };

    BEGIN_MSG_MAP(MainWindow)
        MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(WM_COMMAND, OnCommand)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    END_MSG_MAP()

private:
    LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        m_trayIconAdded = AddTrayIcon();
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

    LRESULT OnCommand(UINT, WPARAM wParam, LPARAM, BOOL& handled) {
        handled = TRUE;

        switch (LOWORD(wParam)) {
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

    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        ShowWindow(SW_HIDE);
        return 0;
    }

    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
        handled = TRUE;
        RemoveTrayIcon();
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
        wcscpy_s(data.szTip, L"Hello WTL");

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
        if (IsIconic()) {
            ShowWindow(SW_RESTORE);
        }
        ShowWindow(SW_SHOWNORMAL);
        SetForegroundWindow(m_hWnd);
    }

    void ShowTrayMenu() {
        HMENU menu = LoadMenuW(_Module.GetResourceInstance(),
                              MAKEINTRESOURCEW(IDR_TRAY_MENU));
        if (menu == nullptr) {
            return;
        }

        HMENU popup = GetSubMenu(menu, 0);
        if (popup != nullptr) {
            POINT cursor{};
            GetCursorPos(&cursor);
            SetForegroundWindow(m_hWnd);
            TrackPopupMenu(popup, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0,
                           m_hWnd, nullptr);
            PostMessage(WM_NULL, 0, 0);
        }

        DestroyMenu(menu);
    }

    void ExitApplication() {
        DestroyWindow();
    }

    bool m_trayIconAdded = false;
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HRESULT result = _Module.Init(nullptr, instance);
    if (FAILED(result)) {
        return static_cast<int>(result);
    }

    AtlInitCommonControls(ICC_WIN95_CLASSES);

    CMessageLoop messageLoop;
    _Module.AddMessageLoop(&messageLoop);

    MainWindow window;
    if (window.Create(nullptr) == nullptr) {
        _Module.RemoveMessageLoop();
        _Module.Term();
        return 1;
    }

    const int exitCode = messageLoop.Run();
    _Module.RemoveMessageLoop();
    _Module.Term();
    return exitCode;
}
