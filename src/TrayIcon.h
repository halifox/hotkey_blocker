#pragma once

#include "WindowsTarget.h"

#include <string>

class TrayIcon final {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Add(HWND borrowedOwner, UINT callbackMessage, HICON borrowedIcon,
             const std::wstring& tooltip);
    bool Restore();
    void Remove() noexcept;

    bool IsAdded() const noexcept;
    bool ShowNotification(const std::wstring& title, const std::wstring& message) const;

private:
    bool AddStoredIcon();

    // Borrowed from MainFrame; this class never creates or destroys the window.
    HWND m_borrowedOwnerWindow = nullptr;
    UINT m_callbackMessage = 0;
    // Borrowed from MainFrame and valid until Remove() unregisters the shell icon.
    HICON m_borrowedIcon = nullptr;
    std::wstring m_tooltip;
    bool m_added = false;
};
