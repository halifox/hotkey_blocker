#include "ApplicationDiscovery.h"

#include "PathUtils.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atlbase.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

bool IsCancelled(const std::atomic_bool* cancellation) {
    return cancellation != nullptr && cancellation->load(std::memory_order_acquire);
}

bool IsExePath(const std::wstring& path) {
    const std::wstring extension = std::filesystem::path(path).extension().wstring();
    return extension.size() == 4 && extension[0] == L'.' &&
           (extension[1] == L'e' || extension[1] == L'E') &&
           (extension[2] == L'x' || extension[2] == L'X') &&
           (extension[3] == L'e' || extension[3] == L'E');
}

bool HasExtension(const std::filesystem::path& path, const wchar_t* extension) {
    const std::wstring value = path.extension().wstring();
    return CompareStringOrdinal(value.c_str(), -1, extension, -1, TRUE) == CSTR_EQUAL;
}

bool IsRegularFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring Trim(std::wstring value) {
    const auto isWhitespace = [](wchar_t character) {
        return character == L' ' || character == L'\t' || character == L'\r' ||
               character == L'\n';
    };
    while (!value.empty() && isWhitespace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && isWhitespace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring ExpandEnvironmentVariables(const std::wstring& value) {
    const DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (required == 0) {
        return value;
    }
    std::wstring expanded(required, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required);
    if (written == 0 || written > required) {
        return value;
    }
    expanded.resize(written - 1);
    return expanded;
}

std::wstring ParseExecutableValue(const std::wstring& value) {
    std::wstring result = Trim(value);
    if (result.empty()) {
        return {};
    }

    if (result.front() == L'"') {
        const std::size_t closingQuote = result.find(L'"', 1);
        if (closingQuote == std::wstring::npos) {
            return {};
        }
        result = result.substr(1, closingQuote - 1);
    } else {
        const std::size_t separator = result.find(L',');
        if (separator != std::wstring::npos) {
            result.resize(separator);
        } else {
            const std::size_t argument = result.find_first_of(L" \t");
            if (argument != std::wstring::npos) {
                result.resize(argument);
            }
        }
    }

    result = ExpandEnvironmentVariables(Trim(result));
    if (!IsExePath(result) || !IsRegularFile(result)) {
        return {};
    }
    return PathUtils::NormalizePath(result);
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        return {};
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(value.data()), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    if (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return type == REG_EXPAND_SZ ? ExpandEnvironmentVariables(value) : value;
}

std::wstring ShortcutTarget(const std::filesystem::path& shortcut) {
    CComPtr<IShellLinkW> shellLink;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&shellLink)))) {
        return {};
    }

    CComPtr<IPersistFile> persistFile;
    if (FAILED(shellLink->QueryInterface(IID_PPV_ARGS(&persistFile))) ||
        FAILED(persistFile->Load(shortcut.c_str(), STGM_READ))) {
        return {};
    }

    std::vector<wchar_t> buffer(32768);
    WIN32_FIND_DATAW findData{};
    if (FAILED(shellLink->GetPath(buffer.data(), static_cast<int>(buffer.size()), &findData,
                                  SLGP_RAWPATH))) {
        return {};
    }
    return ParseExecutableValue(buffer.data());
}

std::wstring KnownFolderPath(REFKNOWNFOLDERID folderId) {
    PWSTR rawPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath)) ||
        rawPath == nullptr) {
        return {};
    }
    std::wstring path(rawPath);
    CoTaskMemFree(rawPath);
    return path;
}

void AddOrMerge(std::vector<DiscoveredApplication>& applications,
                DiscoveredApplication application) {
    if (application.path.empty()) {
        return;
    }
    application.id = application.path;
    const auto existing = std::find_if(
        applications.begin(), applications.end(), [&application](const auto& item) {
            return PathUtils::SamePath(item.path, application.path);
        });
    if (existing == applications.end()) {
        applications.push_back(std::move(application));
        return;
    }

    if (existing->displayName.empty() ||
        (existing->displayName == std::filesystem::path(existing->path).stem().wstring() &&
         !application.displayName.empty())) {
        existing->displayName = application.displayName;
    }
    if (existing->publisher.empty()) {
        existing->publisher = application.publisher;
    }
    if (existing->version.empty()) {
        existing->version = application.version;
    }
    if (existing->installLocation.empty()) {
        existing->installLocation = application.installLocation;
    }
}

void ScanStartMenuDirectory(const std::filesystem::path& root,
                            std::vector<DiscoveredApplication>& applications,
                            const std::atomic_bool* cancellation) {
    if (root.empty() || IsCancelled(cancellation)) {
        return;
    }

    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        root, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end && !IsCancelled(cancellation); iterator.increment(error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (!iterator->is_regular_file(error) || error || !HasExtension(iterator->path(), L".lnk")) {
            error.clear();
            continue;
        }

        const std::wstring target = ShortcutTarget(iterator->path());
        if (target.empty()) {
            continue;
        }
        DiscoveredApplication application;
        application.path = target;
        application.displayName = iterator->path().stem().wstring();
        AddOrMerge(applications, std::move(application));
    }
}

void ScanAppPaths(HKEY root, REGSAM view, std::vector<DiscoveredApplication>& applications,
                  const std::atomic_bool* cancellation) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths", 0,
                      KEY_READ | view, &key) != ERROR_SUCCESS) {
        return;
    }

    DWORD index = 0;
    wchar_t name[512]{};
    while (!IsCancelled(cancellation)) {
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(key, index++, name, &nameLength, nullptr, nullptr,
                                          nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result != ERROR_SUCCESS) {
            continue;
        }

        HKEY entry = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring path = ParseExecutableValue(ReadRegistryString(entry, nullptr));
        RegCloseKey(entry);
        if (path.empty()) {
            continue;
        }

        DiscoveredApplication application;
        application.path = path;
        application.displayName = std::filesystem::path(path).stem().wstring();
        AddOrMerge(applications, std::move(application));
    }
    RegCloseKey(key);
}

std::wstring FindSingleExecutable(const std::wstring& directory) {
    if (directory.empty()) {
        return {};
    }
    std::error_code error;
    std::vector<std::filesystem::path> executables;
    for (const auto& entry : std::filesystem::directory_iterator(
             directory, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) {
            error.clear();
            continue;
        }
        if (entry.is_regular_file(error) && !error && HasExtension(entry.path(), L".exe")) {
            executables.push_back(entry.path());
        }
    }
    return executables.size() == 1 ? ParseExecutableValue(executables.front().wstring()) :
                                     std::wstring{};
}

void ScanUninstall(HKEY root, REGSAM view, std::vector<DiscoveredApplication>& applications,
                   const std::atomic_bool* cancellation) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                      KEY_READ | view, &key) != ERROR_SUCCESS) {
        return;
    }

    DWORD index = 0;
    wchar_t name[512]{};
    while (!IsCancelled(cancellation)) {
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(key, index++, name, &nameLength, nullptr, nullptr,
                                          nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (result != ERROR_SUCCESS) {
            continue;
        }

        HKEY entry = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) {
            continue;
        }
        const std::wstring displayName = ReadRegistryString(entry, L"DisplayName");
        if (displayName.empty()) {
            RegCloseKey(entry);
            continue;
        }

        std::wstring executable = ParseExecutableValue(ReadRegistryString(entry, L"DisplayIcon"));
        const std::wstring installLocation =
            ExpandEnvironmentVariables(ReadRegistryString(entry, L"InstallLocation"));
        if (executable.empty()) {
            executable = FindSingleExecutable(installLocation);
        }
        if (!executable.empty()) {
            DiscoveredApplication application;
            application.path = executable;
            application.displayName = displayName;
            application.publisher = ReadRegistryString(entry, L"Publisher");
            application.version = ReadRegistryString(entry, L"DisplayVersion");
            application.installLocation = installLocation;
            AddOrMerge(applications, std::move(application));
        }
        RegCloseKey(entry);
    }
    RegCloseKey(key);
}

void AppendWarning(std::wstring& warning, const std::wstring& message) {
    if (!warning.empty()) {
        warning += L"；";
    }
    warning += message;
}

}  // namespace

namespace ApplicationDiscovery {

std::vector<DiscoveredApplication> Scan(std::wstring& warning,
                                       const std::atomic_bool* cancellation) {
    std::vector<DiscoveredApplication> applications;
    warning.clear();

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        AppendWarning(warning, L"初始化应用发现组件失败");
    }

    ScanStartMenuDirectory(KnownFolderPath(FOLDERID_Programs), applications, cancellation);
    ScanStartMenuDirectory(KnownFolderPath(FOLDERID_CommonPrograms), applications, cancellation);

    constexpr REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    for (const REGSAM view : views) {
        for (const HKEY root : roots) {
            if (IsCancelled(cancellation)) {
                break;
            }
            ScanAppPaths(root, view, applications, cancellation);
            ScanUninstall(root, view, applications, cancellation);
        }
    }

    if (shouldUninitialize) {
        CoUninitialize();
    }

    std::sort(applications.begin(), applications.end(), [](const auto& left, const auto& right) {
        const int nameComparison = CompareStringOrdinal(left.displayName.c_str(), -1,
                                                        right.displayName.c_str(), -1, TRUE);
        if (nameComparison != CSTR_EQUAL) {
            return nameComparison == CSTR_LESS_THAN;
        }
        return CompareStringOrdinal(left.path.c_str(), -1, right.path.c_str(), -1, TRUE) ==
               CSTR_LESS_THAN;
    });
    return applications;
}

}  // namespace ApplicationDiscovery
