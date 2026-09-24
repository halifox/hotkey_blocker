#pragma once

#include "WindowsTarget.h"

#include <string>

class TrayIcon final {
public:
    TrayIcon() = default;
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Add(HWND owner, UINT callbackMessage, HICON icon, const std::wstring& tooltip);
    bool Restore();
    void Remove() noexcept;

    bool IsAdded() const noexcept;
    bool ShowNotification(const std::wstring& title, const std::wstring& message) const;

private:
    bool AddStoredIcon();

    HWND m_owner = nullptr;
    UINT m_callbackMessage = 0;
    HICON m_icon = nullptr;
    std::wstring m_tooltip;
    bool m_added = false;
};
