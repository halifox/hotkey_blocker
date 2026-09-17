#include "ApplicationDiscovery.h"

#include "PathUtils.h"

#include <windows.h>
#include <propsys.h>
#include <propkey.h>
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

    const auto tryPath = [](std::wstring candidate) {
        candidate = ExpandEnvironmentVariables(Trim(std::move(candidate)));
        if (!IsExePath(candidate) || !IsRegularFile(candidate)) {
            return std::wstring{};
        }
        return PathUtils::NormalizePath(candidate);
    };

    if (result.front() == L'"') {
        const std::size_t closingQuote = result.find(L'"', 1);
        if (closingQuote == std::wstring::npos) {
            return {};
        }
        result = result.substr(1, closingQuote - 1);
        return tryPath(result);
    }

    // DisplayIcon and App Paths commonly store an unquoted executable path.
    // Do not split at the first space: paths such as "C:\\Program Files\\..."
    // are valid. First try the complete value, then progressively try each
    // .exe boundary to support an icon index or command-line arguments.
    if (const std::wstring executable = tryPath(result); !executable.empty()) {
        return executable;
    }

    const std::wstring expanded = ExpandEnvironmentVariables(result);
    for (std::size_t position = 0; position + 4 <= expanded.size(); ++position) {
        if (CompareStringOrdinal(expanded.c_str() + position, 4, L".exe", 4, TRUE) !=
            CSTR_EQUAL) {
            continue;
        }
        if (const std::wstring executable = tryPath(expanded.substr(0, position + 4));
            !executable.empty()) {
            return executable;
        }
    }

    return {};
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

std::wstring ShellItemProperty(IShellItem2* item, REFPROPERTYKEY key) {
    if (item == nullptr) {
        return {};
    }

    CComPtr<IPropertyStore> propertyStore;
    if (FAILED(item->GetPropertyStore(GPS_DEFAULT, IID_PPV_ARGS(&propertyStore)))) {
        return {};
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = propertyStore->GetValue(key, &value);
    std::wstring text;
    if (SUCCEEDED(result)) {
        if (value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
            text = value.pwszVal;
        } else if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
            text = value.bstrVal;
        }
    }
    PropVariantClear(&value);
    return text;
}

std::wstring ShellItemDisplayName(IShellItem2* item) {
    if (item == nullptr) {
        return {};
    }
    PWSTR rawName = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &rawName)) || rawName == nullptr) {
        return {};
    }
    std::wstring name(rawName);
    CoTaskMemFree(rawName);
    return name;
}

std::wstring ShellItemExecutable(IShellItem2* item) {
    if (item == nullptr) {
        return {};
    }

    PWSTR rawPath = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath != nullptr) {
        const std::wstring path = ParseExecutableValue(rawPath);
        CoTaskMemFree(rawPath);
        if (!path.empty()) {
            return path;
        }
    }

    const PROPERTYKEY keys[] = {PKEY_Link_TargetParsingPath, PKEY_ParsingPath};
    for (const PROPERTYKEY& key : keys) {
        const std::wstring value = ShellItemProperty(item, key);
        if (const std::wstring path = ParseExecutableValue(value); !path.empty()) {
            return path;
        }
    }
    return {};
}

void ScanAppsFolder(std::vector<DiscoveredApplication>& applications,
                    DiscoveryStats* stats, const std::atomic_bool* cancellation) {
    if (IsCancelled(cancellation)) {
        return;
    }

    PIDLIST_ABSOLUTE folderId = nullptr;
    if (FAILED(SHParseDisplayName(L"shell:AppsFolder", nullptr, &folderId, 0, nullptr)) ||
        folderId == nullptr) {
        return;
    }

    CComPtr<IShellFolder> folder;
    HRESULT result = SHBindToObject(nullptr, folderId, nullptr, IID_PPV_ARGS(&folder));
    if (FAILED(result) || folder == nullptr) {
        CoTaskMemFree(folderId);
        return;
    }

    CComPtr<IEnumIDList> enumerator;
    result = folder->EnumObjects(nullptr,
                                 SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN,
                                 &enumerator);
    if (FAILED(result) || enumerator == nullptr) {
        CoTaskMemFree(folderId);
        return;
    }

    for (;;) {
        if (IsCancelled(cancellation)) {
            break;
        }

        PITEMID_CHILD childId = nullptr;
        ULONG fetched = 0;
        result = enumerator->Next(1, &childId, &fetched);
        if (result != S_OK || fetched == 0 || childId == nullptr) {
            break;
        }
        ++stats->appsFolderEntries;

        CComPtr<IShellItem2> item;
        result = SHCreateItemWithParent(folderId, folder, childId, IID_PPV_ARGS(&item));
        if (FAILED(result) || item == nullptr) {
            ++stats->unresolvedEntries;
            CoTaskMemFree(childId);
            continue;
        }

        const std::wstring executable = ShellItemExecutable(item);
        if (executable.empty()) {
            ++stats->unresolvedEntries;
            CoTaskMemFree(childId);
            continue;
        }

        DiscoveredApplication application;
        application.path = executable;
        application.displayName = ShellItemDisplayName(item);
        AddOrMerge(applications, std::move(application));
        CoTaskMemFree(childId);
    }
    CoTaskMemFree(folderId);
}

void ScanStartMenuDirectory(const std::filesystem::path& root,
                            std::vector<DiscoveredApplication>& applications,
                            DiscoveryStats* stats,
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

        ++stats->startMenuShortcuts;
        const std::wstring target = ShortcutTarget(iterator->path());
        if (target.empty()) {
            ++stats->unresolvedEntries;
            continue;
        }
        DiscoveredApplication application;
        application.path = target;
        application.displayName = iterator->path().stem().wstring();
        AddOrMerge(applications, std::move(application));
    }
}

void ScanAppPaths(HKEY root, REGSAM view, std::vector<DiscoveredApplication>& applications,
                  DiscoveryStats* stats,
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
        ++stats->appPathsEntries;

        HKEY entry = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &entry) != ERROR_SUCCESS) {
            ++stats->unresolvedEntries;
            continue;
        }
        const std::wstring path = ParseExecutableValue(ReadRegistryString(entry, nullptr));
        RegCloseKey(entry);
        if (path.empty()) {
            ++stats->unresolvedEntries;
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
                   DiscoveryStats* stats,
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
        ++stats->uninstallEntries;

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
        } else {
            ++stats->unresolvedEntries;
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
                                       const std::atomic_bool* cancellation,
                                       DiscoveryStats* stats) {
    std::vector<DiscoveredApplication> applications;
    warning.clear();
    DiscoveryStats localStats;
    if (stats == nullptr) {
        stats = &localStats;
    } else {
        *stats = {};
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        AppendWarning(warning, L"初始化应用发现组件失败");
    }

    ScanAppsFolder(applications, stats, cancellation);
    ScanStartMenuDirectory(KnownFolderPath(FOLDERID_Programs), applications, stats, cancellation);
    ScanStartMenuDirectory(KnownFolderPath(FOLDERID_CommonPrograms), applications, stats,
                           cancellation);

    constexpr REGSAM views[] = {KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    const HKEY roots[] = {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
    for (const REGSAM view : views) {
        for (const HKEY root : roots) {
            if (IsCancelled(cancellation)) {
                break;
            }
            ScanAppPaths(root, view, applications, stats, cancellation);
            ScanUninstall(root, view, applications, stats, cancellation);
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
    stats->applications = applications.size();
    return applications;
}

}  // namespace ApplicationDiscovery
