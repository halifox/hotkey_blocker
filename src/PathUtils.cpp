#include "PathUtils.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <utility>

namespace PathUtils {
namespace {

bool IsExePath(const std::wstring& path) {
    const std::wstring extension = std::filesystem::path(path).extension().wstring();
    return extension.size() == 4 && extension[0] == L'.' &&
           (extension[1] == L'e' || extension[1] == L'E') &&
           (extension[2] == L'x' || extension[2] == L'X') &&
           (extension[3] == L'e' || extension[3] == L'E');
}

std::wstring DefaultDisplayName(const std::wstring& path) {
    const std::filesystem::path value(path);
    const std::wstring name = value.stem().wstring();
    return name.empty() ? value.filename().wstring() : name;
}

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

bool IsPathUnderDirectory(const std::wstring& path, const std::wstring& directory) {
    const std::wstring normalizedPath = NormalizePath(path);
    std::wstring normalizedDirectory = NormalizePath(directory);
    if (normalizedPath.empty() || normalizedDirectory.empty()) {
        return false;
    }

    if (normalizedDirectory.back() != L'\\') {
        normalizedDirectory.push_back(L'\\');
    }
    return normalizedPath.size() > normalizedDirectory.size() &&
           CompareStringOrdinal(normalizedPath.c_str(),
                                static_cast<int>(normalizedDirectory.size()),
                                normalizedDirectory.c_str(),
                                static_cast<int>(normalizedDirectory.size()), TRUE) ==
               CSTR_EQUAL;
}

bool NormalizeRule(AppRule& rule, std::wstring& error) {
    error.clear();
    rule.path = NormalizePath(rule.path);
    if (rule.path.empty()) {
        error = L"规则路径无效";
        return false;
    }

    const DWORD attributes = GetFileAttributesW(rule.path.c_str());
    if (rule.kind == RuleKind::Executable) {
        if (!IsExePath(rule.path)) {
            error = L"规则路径必须是 .exe 文件";
            return false;
        }
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            error = L"规则路径不能是文件夹";
            return false;
        }
    } else {
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            error = L"文件夹规则路径必须是文件夹";
            return false;
        }
    }

    if (rule.displayName.empty()) {
        rule.displayName = DefaultDisplayName(rule.path);
    }
    if (rule.displayName.empty()) {
        rule.displayName = rule.path;
    }
    return true;
}

bool Matches(const AppRule& rule, const std::wstring& path) {
    if (rule.kind == RuleKind::Executable) {
        return SamePath(rule.path, path);
    }
    return IsExePath(path) && IsPathUnderDirectory(path, rule.path);
}

bool Overlaps(const AppRule& left, const AppRule& right) {
    if (left.kind == RuleKind::Executable) {
        return Matches(right, left.path);
    }
    if (right.kind == RuleKind::Executable) {
        return Matches(left, right.path);
    }
    return SamePath(left.path, right.path) ||
           IsPathUnderDirectory(left.path, right.path) ||
           IsPathUnderDirectory(right.path, left.path);
}

}  // namespace PathUtils
