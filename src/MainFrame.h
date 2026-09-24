#pragma once

#include "WindowsTarget.h"

#include <windows.h>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlframe.h>
#include <atlgdi.h>

#include <atomic>
#include <string>
#include <unordered_map>

#include "ApplicationController.h"
#include "ApplicationListPresenter.h"
#include "MainView.h"
#include "TrayIcon.h"
#include "UpdateChecker.h"
#include "resource.h"

class MainFrame final : public WTL::CFrameWindowImpl<MainFrame>,
                        public WTL::CUpdateUI<MainFrame>,
                        public WTL::CMessageFilter {
public:
    DECLARE_FRAME_WND_CLASS(L"HotkeyBlocker.MainFrame", IDR_MAINFRAME)

    explicit MainFrame(bool startHidden) noexcept;

    bool Initialize();
    bool ShouldStartHidden() const noexcept;
    BOOL PreTranslateMessage(MSG* message) override;

    BEGIN_UPDATE_UI_MAP(MainFrame)
        UPDATE_ELEMENT(ID_MAIN_AUTOSTART, WTL::CUpdateUI<MainFrame>::UPDUI_MENUBAR)
    END_UPDATE_UI_MAP()

    BEGIN_MSG_MAP(MainFrame)
        MESSAGE_HANDLER(WM_CREATE, OnCreate)
        MESSAGE_HANDLER(WM_SETFOCUS, OnSetFocus)
        MESSAGE_HANDLER(kTrayMessage, OnTrayMessage)
        MESSAGE_HANDLER(kStateChangedMessage, OnStateChanged)
        MESSAGE_HANDLER(kUpdateCheckCompletedMessage, OnUpdateCheckCompleted)
        MESSAGE_HANDLER(TaskbarCreatedMessage(), OnTaskbarCreated)
        COMMAND_ID_HANDLER(ID_MAIN_ADD_EXECUTABLE, OnAddExecutable)
        COMMAND_ID_HANDLER(ID_MAIN_ADD_FOLDER, OnAddFolder)
        COMMAND_ID_HANDLER(ID_MAIN_AUTOSTART, OnToggleAutoStart)
        COMMAND_ID_HANDLER(ID_TRAY_CHECK_UPDATES, OnCheckUpdates)
        COMMAND_ID_HANDLER(ID_TRAY_SHOW, OnShowFromTray)
        COMMAND_ID_HANDLER(ID_TRAY_EXIT, OnExit)
        MESSAGE_HANDLER(WM_CLOSE, OnClose)
        MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
        CHAIN_MSG_MAP(WTL::CUpdateUI<MainFrame>)
        CHAIN_MSG_MAP(WTL::CFrameWindowImpl<MainFrame>)
    END_MSG_MAP()

private:
    static constexpr UINT kTrayMessage = WM_APP + 1;
    static constexpr UINT kStateChangedMessage = WM_APP + 2;
    static constexpr UINT kUpdateCheckCompletedMessage = WM_APP + 3;

    static UINT TaskbarCreatedMessage() noexcept;
    LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnSetFocus(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnTrayMessage(UINT, WPARAM, LPARAM lParam, BOOL& handled);
    LRESULT OnStateChanged(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnUpdateCheckCompleted(UINT, WPARAM, LPARAM lParam, BOOL& handled);
    LRESULT OnTaskbarCreated(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnAddExecutable(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnAddFolder(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnToggleAutoStart(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnCheckUpdates(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnShowFromTray(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnExit(WORD, WORD, HWND, BOOL& handled);
    LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& handled);
    LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled);

    void HandleListAction(const std::wstring& path, ApplicationListAction action);
    void RestoreTrayIcon();
    void ShowFromTray();
    void QueueStateRefresh();
    void ShowTrayMenu();
    void ExitApplication();
    void StartUpdateCheck(bool interactive);
    void OpenPendingRelease();
    void DrainUpdateCheckMessages();
    bool LoadWindowIcons();
    void DestroyWindowIcons() noexcept;
    void RefreshListView(bool force);
    bool PickApplicationPath(bool folder, std::wstring& path);
    void AddExecutableApplication();
    void AddFolderApplication();
    void DeleteApplication(const std::wstring& path);
    void ConfigureApplication(const std::wstring& path);
    void UpdateAutoStart();
    static bool IsActionableStatus(AppStatus status) noexcept;
    void NotifyActionableStates();
    void ShowTrayNotification(const std::wstring& title, const std::wstring& message) const;
    void ShowError(const wchar_t* title, const std::wstring& message);
    void ShowErrorCode(const wchar_t* title, HRESULT result);

    bool m_startHidden = false;
    bool m_initializationFailed = false;
    std::atomic_bool m_shuttingDown = false;
    MainView m_mainView;
    CIcon m_largeIcon;
    CIcon m_smallIcon;
    ApplicationController m_application;
    ApplicationListPresenter m_listPresenter;
    TrayIcon m_trayIcon;
    UpdateChecker m_updateChecker;
    std::wstring m_pendingReleaseUrl;
    bool m_updateCheckRunning = false;
    bool m_updateCheckInteractive = false;
    std::unordered_map<std::wstring, AppStatus> m_notifiedActionableStates;
    std::atomic_bool m_stateNotificationPosted = false;
};

int RunMainFrame(CMessageLoop& messageLoop, bool startHidden);
