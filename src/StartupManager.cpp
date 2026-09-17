#include "StartupManager.h"

#include <windows.h>

#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"HotkeyBlocker";

std::wstring Win32Error(const wchar_t* operation, DWORD errorCode = GetLastError()) {
    wchar_t buffer[512]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, errorCode, 0, buffer,
                                       static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring result(operation);
    if (length != 0) {
        result += L"：";
        result.append(buffer, length);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
    } else {
        result += L"（错误码 " + std::to_wstring(errorCode) + L"）";
    }
    return result;
}

}  // namespace

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
        error = Win32Error(L"打开开机启动注册表项", static_cast<DWORD>(status));
        return false;
    }

    if (!enabled) {
        status = RegDeleteValueW(key, kValueName);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    } else {
        std::wstring executablePath = ExecutablePath();
        if (executablePath.empty()) {
            RegCloseKey(key);
            error = L"无法获取程序路径";
            return false;
        }
        const std::wstring command = L"\"" + executablePath + L"\" --background";

        DWORD type = 0;
        DWORD bytes = 0;
        status = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes);
        if (status == ERROR_SUCCESS && type == REG_SZ && bytes >= sizeof(wchar_t)) {
            std::vector<wchar_t> existing(bytes / sizeof(wchar_t) + 1, L'\0');
            DWORD existingBytes = bytes;
            status = RegQueryValueExW(key, kValueName, nullptr, &type,
                                      reinterpret_cast<BYTE*>(existing.data()), &existingBytes);
            if (status == ERROR_SUCCESS &&
                std::wstring(existing.data()) == command) {
                RegCloseKey(key);
                return true;
            }
        }

        status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        error = Win32Error(enabled ? L"写入开机启动配置" : L"删除开机启动配置",
                           static_cast<DWORD>(status));
        return false;
    }
    return true;
}

std::wstring StartupManager::ExecutablePath() {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}
