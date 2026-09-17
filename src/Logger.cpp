#include "Logger.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>

namespace {

constexpr unsigned long long kMaxLogBytes = 2ull * 1024ull * 1024ull;

std::wstring TrimLineEnd(std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
}

}  // namespace

Logger::Logger() : m_path(DefaultPath()) {}

Logger::Logger(std::filesystem::path logPath) : m_path(std::move(logPath)) {}

void Logger::Info(std::wstring_view message) {
    Write(L"INFO", message);
}

void Logger::Error(std::wstring_view message) {
    Write(L"ERROR", message);
}

const std::filesystem::path& Logger::Path() const noexcept {
    return m_path;
}

void Logger::Write(std::wstring_view level, std::wstring_view message) {
    const std::string line = ToUtf8(Timestamp() + L" [" + std::wstring(level) + L"] " +
                                    std::wstring(message) + L"\r\n");
    if (line.empty()) {
        return;
    }

    std::lock_guard lock(m_mutex);
    const std::filesystem::path parent = m_path.parent_path();
    std::error_code fileSystemError;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fileSystemError);
        if (fileSystemError) {
            return;
        }
    }
    RotateIfNeeded();

    HANDLE file = CreateFileW(m_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const char* data = line.data();
    std::size_t offset = 0;
    while (offset < line.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            line.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, data + offset, request, &written, nullptr) || written == 0) {
            break;
        }
        offset += written;
    }
    CloseHandle(file);
}

std::filesystem::path Logger::DefaultPath() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                       &localAppData)) &&
        localAppData != nullptr) {
        const std::filesystem::path result =
            std::filesystem::path(localAppData) / L"HotkeyBlocker" / L"logs" / L"hkb.log";
        CoTaskMemFree(localAppData);
        return result;
    }
    if (localAppData != nullptr) {
        CoTaskMemFree(localAppData);
    }

    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(buffer) / L"HotkeyBlocker" / L"logs" / L"hkb.log";
    }
    return std::filesystem::path(L"HotkeyBlocker") / L"logs" / L"hkb.log";
}

std::string Logger::ToUtf8(std::wstring_view text) {
    if (text.empty() || text.size() > static_cast<std::size_t>(INT_MAX)) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), required, nullptr,
                            nullptr) != required) {
        return {};
    }
    return result;
}

std::wstring Logger::Timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

void Logger::RotateIfNeeded() {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(m_path.c_str(), GetFileExInfoStandard, &attributes)) {
        return;
    }

    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    if (size.QuadPart < kMaxLogBytes) {
        return;
    }

    const std::filesystem::path firstBackup = m_path.wstring() + L".1";
    const std::filesystem::path secondBackup = m_path.wstring() + L".2";
    DeleteFileW(secondBackup.c_str());
    MoveFileExW(firstBackup.c_str(), secondBackup.c_str(), MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(m_path.c_str(), firstBackup.c_str(), MOVEFILE_REPLACE_EXISTING);
}
