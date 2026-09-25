#include "StartupManager.h"

#include "Win32Support.h"

#include <windows.h>

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"HotkeyBlocker";

}  // namespace

bool StartupManager::GetEnabled(bool& enabled, std::wstring& error) const {
    enabled = false;
    error.clear();

    HKEY key = nullptr;
    const LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (status != ERROR_SUCCESS) {
        error = Win32Support::ErrorMessage(L"打开登录时启动注册表项", static_cast<DWORD>(status));
        return false;
    }

    DWORD type = 0;
    const LSTATUS query = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    if (query == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (query != ERROR_SUCCESS) {
        error = Win32Support::ErrorMessage(L"读取登录时启动配置", static_cast<DWORD>(query));
        return false;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        error = L"登录时启动配置类型无效";
        return false;
    }
    enabled = true;
    return true;
}

bool StartupManager::SetEnabled(bool enabled, std::wstring& error) const {
    error.clear();
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                                   KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND && enabled) {
        status = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                                 KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    }
    if (status != ERROR_SUCCESS) {
        if (!enabled && status == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        error = Win32Support::ErrorMessage(L"打开登录时启动注册表项", static_cast<DWORD>(status));
        return false;
    }

    if (!enabled) {
        status = RegDeleteValueW(key, kValueName);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    } else {
        std::wstring executablePath = Win32Support::ModulePath();
        if (executablePath.empty()) {
            RegCloseKey(key);
            error = L"无法获取程序路径";
            return false;
        }
        const std::wstring command = L"\"" + executablePath + L"\" --background";

        status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        error = Win32Support::ErrorMessage(enabled ? L"写入登录时启动配置" : L"删除登录时启动配置",
                                            static_cast<DWORD>(status));
        return false;
    }
    return true;
}
