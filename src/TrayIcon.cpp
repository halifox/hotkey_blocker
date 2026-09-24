#include "WindowsTarget.h"

#include <windows.h>
#include <shellapi.h>

#include <iterator>

#include "TrayIcon.h"

namespace {

constexpr UINT kTrayIconId = 1;

}  // namespace

TrayIcon::~TrayIcon() {
    Remove();
}

bool TrayIcon::Add(HWND owner, UINT callbackMessage, HICON icon,
                   const std::wstring& tooltip) {
    Remove();
    m_owner = owner;
    m_callbackMessage = callbackMessage;
    m_icon = icon;
    m_tooltip = tooltip;
    return AddStoredIcon();
}

bool TrayIcon::Restore() {
    m_added = false;
    return AddStoredIcon();
}

void TrayIcon::Remove() noexcept {
    if (m_added && m_owner != nullptr) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = m_owner;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    m_added = false;
}

bool TrayIcon::IsAdded() const noexcept {
    return m_added;
}

bool TrayIcon::ShowNotification(const std::wstring& title,
                                const std::wstring& message) const {
    if (!m_added) {
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_owner;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    data.uTimeout = 5000;
    wcsncpy_s(data.szInfoTitle, std::size(data.szInfoTitle), title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, std::size(data.szInfo), message.c_str(), _TRUNCATE);
    return Shell_NotifyIconW(NIM_MODIFY, &data) == TRUE;
}

bool TrayIcon::AddStoredIcon() {
    if (m_owner == nullptr || m_icon == nullptr || m_callbackMessage == 0) {
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_owner;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = m_callbackMessage;
    data.hIcon = m_icon;
    wcsncpy_s(data.szTip, std::size(data.szTip), m_tooltip.c_str(), _TRUNCATE);
    m_added = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    return m_added;
}
