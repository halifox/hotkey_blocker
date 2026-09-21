#include <windows.h>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "UpdateChecker.h"
#include "Version.h"

namespace {

constexpr char kGitHubReleaseUrl[] =
    "https://api.github.com/repos/halifox/hotkey_blocker/releases/latest";

struct VersionNumber {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t patch = 0;
};

UpdateCheckResult ErrorResult(std::wstring message) {
    UpdateCheckResult result;
    result.error = std::move(message);
    return result;
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

    try {
        m_thread = std::thread([this, callback = std::move(callback)]() mutable {
            UpdateCheckResult result = CheckLatestRelease();
            if (!m_stopRequested.load(std::memory_order_acquire) && callback) {
                callback(std::move(result));
            }
        });
    } catch (...) {
        return false;
    }
    return true;
}

void UpdateChecker::Stop() {
    m_stopRequested.store(true, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

UpdateCheckResult UpdateChecker::CheckLatestRelease() {
    VersionNumber currentVersion;
    if (!ParseVersion(hkb::version::kString, currentVersion)) {
        return ErrorResult(L"当前程序版本格式无效");
    }

    std::string response;
    const cpr::Response httpResponse = cpr::Get(
        cpr::Url{kGitHubReleaseUrl},
        cpr::Header{{"Accept", "application/vnd.github+json"},
                    {"User-Agent", "HotkeyBlocker"}},
        cpr::Timeout{5000}, cpr::ConnectTimeout{3000},
        cpr::WriteCallback{[&response](std::string_view chunk, intptr_t) {
            response.append(chunk.data(), chunk.size());
            return true;
        }});

    if (httpResponse.error.code != cpr::ErrorCode::OK) {
        std::wstring message = L"版本检查请求失败";
        const std::wstring detail = Utf8ToWide(httpResponse.error.message);
        if (!detail.empty()) {
            message += L"：";
            message += detail;
        }
        return ErrorResult(std::move(message));
    }
    if (httpResponse.status_code != 200) {
        const nlohmann::json errorResponse = nlohmann::json::parse(response, nullptr, false);
        if (errorResponse.is_object()) {
            const auto messageIt = errorResponse.find("message");
            if (messageIt != errorResponse.end() && messageIt->is_string()) {
                const std::wstring message = Utf8ToWide(messageIt->get<std::string>());
                if (!message.empty()) {
                    return ErrorResult(message);
                }
            }
        }
        return ErrorResult(L"GitHub 返回 HTTP " + std::to_wstring(httpResponse.status_code));
    }

    const nlohmann::json release = nlohmann::json::parse(response, nullptr, false);
    if (release.is_discarded() || !release.is_object()) {
        return ErrorResult(L"GitHub 返回的 JSON 格式无效");
    }

    const auto tagIt = release.find("tag_name");
    const auto urlIt = release.find("html_url");
    if (tagIt == release.end() || !tagIt->is_string() ||
        urlIt == release.end() || !urlIt->is_string()) {
        return ErrorResult(L"GitHub 返回的发布信息不完整");
    }

    const std::string tag = tagIt->get<std::string>();
    const std::string releaseUrl = urlIt->get<std::string>();
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
    result.currentVersion = hkb::version::kString;
    result.latestVersion = latestVersion;
    result.releaseUrl = latestUrl;
    result.updateAvailable = IsNewer(latestVersionNumber, currentVersion);
    return result;
}
