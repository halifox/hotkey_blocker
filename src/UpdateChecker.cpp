#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "UpdateChecker.h"
#include "Version.h"

namespace {

constexpr wchar_t kGitHubHost[] = L"api.github.com";
constexpr wchar_t kGitHubReleasePath[] =
    L"/repos/halifox/hotkey_blocker/releases/latest";
constexpr std::size_t kMaxResponseBytes = 1024 * 1024;

struct WinHttpHandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
};

using WinHttpHandle = std::unique_ptr<void, WinHttpHandleCloser>;

struct VersionNumber {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

UpdateCheckResult ErrorResult(std::wstring message) {
    UpdateCheckResult result;
    result.currentVersion = hkb::version::kString;
    result.error = std::move(message);
    return result;
}

std::wstring WinHttpError(const wchar_t* operation) {
    const DWORD errorCode = GetLastError();
    wchar_t systemMessage[256]{};
    const DWORD messageLength = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, errorCode, 0,
        systemMessage, static_cast<DWORD>(std::size(systemMessage)), nullptr);

    std::wstring message = operation;
    message += L"失败";
    if (messageLength > 0) {
        message += L"：";
        message.append(systemMessage, messageLength);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    }
    message += L"（错误码 " + std::to_wstring(errorCode) + L"）";
    return message;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }

    const int sourceLength = static_cast<int>(text.size());
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), sourceLength,
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), sourceLength, result.data(),
                            length) <= 0) {
        return {};
    }
    return result;
}

std::string ExtractJsonString(std::string_view json, std::string_view key) {
    const std::string marker = "\"" + std::string(key) + "\"";
    std::size_t keyPosition = json.find(marker);
    while (keyPosition != std::string_view::npos) {
        const std::size_t colonPosition = json.find(':', keyPosition + marker.size());
        if (colonPosition == std::string_view::npos) {
            return {};
        }

        std::size_t valuePosition = colonPosition + 1;
        while (valuePosition < json.size() &&
               (json[valuePosition] == ' ' || json[valuePosition] == '\t' ||
                json[valuePosition] == '\r' || json[valuePosition] == '\n')) {
            ++valuePosition;
        }
        if (valuePosition >= json.size() || json[valuePosition] != '"') {
            keyPosition = json.find(marker, keyPosition + marker.size());
            continue;
        }

        std::string value;
        for (std::size_t index = valuePosition + 1; index < json.size(); ++index) {
            const char character = json[index];
            if (character == '"') {
                return value;
            }
            if (character != '\\' || index + 1 >= json.size()) {
                value.push_back(character);
                continue;
            }

            const char escaped = json[++index];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                default:
                    value.push_back(escaped);
                    break;
            }
        }
        return {};
    }
    return {};
}

bool ParseVersion(std::wstring_view text, VersionNumber& version) {
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }

    std::uint32_t* components[] = {&version.major, &version.minor, &version.patch};
    std::size_t start = 0;
    for (std::size_t component = 0; component < std::size(components); ++component) {
        const std::size_t separator = text.find(L'.', start);
        const std::size_t end = separator == std::wstring_view::npos ? text.size() : separator;
        if (start == end) {
            return false;
        }

        std::uint32_t value = 0;
        for (std::size_t index = start; index < end; ++index) {
            const wchar_t character = text[index];
            if (character < L'0' || character > L'9') {
                return false;
            }
            const std::uint32_t digit = static_cast<std::uint32_t>(character - L'0');
            if (value > (UINT32_MAX - digit) / 10) {
                return false;
            }
            value = value * 10 + digit;
        }
        *components[component] = value;

        if (component + 1 < std::size(components)) {
            if (separator == std::wstring_view::npos) {
                return false;
            }
            start = separator + 1;
        } else if (separator != std::wstring_view::npos) {
            return false;
        }
    }
    return true;
}

bool IsNewer(const VersionNumber& candidate, const VersionNumber& current) {
    if (candidate.major != current.major) {
        return candidate.major > current.major;
    }
    if (candidate.minor != current.minor) {
        return candidate.minor > current.minor;
    }
    return candidate.patch > current.patch;
}

}  // namespace

UpdateChecker::~UpdateChecker() {
    Stop();
}

bool UpdateChecker::Start(CompletionCallback callback) {
    Stop();
    m_stopRequested.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);

    try {
        m_thread = std::thread([this, callback = std::move(callback)]() mutable {
            UpdateCheckResult result = CheckLatestRelease();
            if (!m_stopRequested.load(std::memory_order_acquire) && callback) {
                callback(std::move(result));
            }
            m_running.store(false, std::memory_order_release);
        });
    } catch (...) {
        m_running.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void UpdateChecker::Stop() {
    m_stopRequested.store(true, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false, std::memory_order_release);
}

bool UpdateChecker::IsRunning() const noexcept {
    return m_running.load(std::memory_order_acquire);
}

UpdateCheckResult UpdateChecker::CheckLatestRelease() {
    VersionNumber currentVersion;
    if (!ParseVersion(hkb::version::kString, currentVersion)) {
        return ErrorResult(L"当前程序版本格式无效");
    }

    WinHttpHandle session(WinHttpOpen(L"HotkeyBlocker", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return ErrorResult(WinHttpError(L"初始化网络请求"));
    }
    if (!WinHttpSetTimeouts(session.get(), 3000, 3000, 5000, 5000)) {
        return ErrorResult(WinHttpError(L"设置网络请求超时"));
    }

    WinHttpHandle connection(WinHttpConnect(session.get(), kGitHubHost,
                                            INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        return ErrorResult(WinHttpError(L"连接 GitHub"));
    }

    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", kGitHubReleasePath, nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        return ErrorResult(WinHttpError(L"创建网络请求"));
    }

    constexpr wchar_t kHeaders[] =
        L"Accept: application/vnd.github+json\r\nUser-Agent: HotkeyBlocker\r\n";
    if (!WinHttpAddRequestHeaders(request.get(), kHeaders, -1L,
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        return ErrorResult(WinHttpError(L"设置网络请求头"));
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return ErrorResult(WinHttpError(L"发送版本检查请求"));
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        return ErrorResult(WinHttpError(L"接收版本检查响应"));
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
                             WINHTTP_NO_HEADER_INDEX)) {
        return ErrorResult(WinHttpError(L"读取版本检查响应状态"));
    }
    if (statusCode != 200) {
        return ErrorResult(L"GitHub 返回 HTTP " + std::to_wstring(statusCode));
    }

    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            return ErrorResult(WinHttpError(L"读取版本检查响应长度"));
        }
        if (available == 0) {
            break;
        }
        if (response.size() + available > kMaxResponseBytes) {
            return ErrorResult(L"版本检查响应过大");
        }

        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), available, &read)) {
            return ErrorResult(WinHttpError(L"读取版本检查响应内容"));
        }
        response.append(buffer.data(), read);
    }

    const std::string tag = ExtractJsonString(response, "tag_name");
    const std::string releaseUrl = ExtractJsonString(response, "html_url");
    const std::wstring latestVersion = Utf8ToWide(tag);
    const std::wstring latestUrl = Utf8ToWide(releaseUrl);
    VersionNumber latestVersionNumber;
    if (latestVersion.empty() || !ParseVersion(latestVersion, latestVersionNumber)) {
        return ErrorResult(L"GitHub 返回的版本号格式无效");
    }
    if (latestUrl.empty()) {
        return ErrorResult(L"GitHub 返回的发布页面地址无效");
    }

    UpdateCheckResult result;
    result.success = true;
    result.currentVersion = hkb::version::kString;
    result.latestVersion = latestVersion;
    result.releaseUrl = latestUrl;
    result.updateAvailable = IsNewer(latestVersionNumber, currentVersion);
    return result;
}
