#include "PathUtils.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>

namespace PathUtils {
namespace {

std::wstring StripExtendedPrefix(std::wstring path) {
    constexpr wchar_t kExtendedPrefix[] = L"\\\\?\\";
    constexpr wchar_t kUncPrefix[] = L"UNC\\";

    if (path.rfind(kExtendedPrefix, 0) != 0) {
        return path;
    }
    path.erase(0, std::size(kExtendedPrefix) - 1);
    if (path.rfind(kUncPrefix, 0) == 0) {
        path.erase(0, std::size(kUncPrefix) - 1);
        path.insert(0, L"\\\\");
    }
    return path;
}

std::wstring ExpandEnvironmentVariables(const std::wstring& path) {
    const DWORD required = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (required == 0) {
        return path;
    }

    std::wstring expanded(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return path;
    }
    expanded.resize(written - 1);
    return expanded;
}

std::wstring FullPath(const std::wstring& path) {
    const std::wstring expanded = ExpandEnvironmentVariables(path);
    DWORD required = GetFullPathNameW(expanded.c_str(), 0, nullptr, nullptr);
    if (required == 0 || required >= MAXDWORD - 1) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD written = GetFullPathNameW(expanded.c_str(),
                                           static_cast<DWORD>(result.size()), result.data(),
                                           nullptr);
    if (written == 0 || written >= result.size()) {
        return {};
    }
    result.resize(written);
    return result;
}

std::wstring FinalPathIfAvailable(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                   FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    const DWORD required = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED);
    if (required == 0) {
        CloseHandle(file);
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(file, result.data(), required + 1,
                                                    FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (written == 0 || written > required) {
        return {};
    }
    result.resize(written);
    return result;
}

}  // namespace

std::wstring NormalizePath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    std::wstring normalized = FullPath(path);
    if (normalized.empty()) {
        return {};
    }

    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    const std::wstring finalPath = FinalPathIfAvailable(normalized);
    if (!finalPath.empty()) {
        normalized = finalPath;
    }
    normalized = StripExtendedPrefix(std::move(normalized));

    while (normalized.size() > 3 && normalized.back() == L'\\') {
        normalized.pop_back();
    }
    return normalized;
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

int FindRuleIndex(const std::vector<AppRule>& rules, const std::wstring& path) {
    for (std::size_t index = 0; index < rules.size(); ++index) {
        if (SamePath(rules[index].path, path)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

}  // namespace PathUtils
