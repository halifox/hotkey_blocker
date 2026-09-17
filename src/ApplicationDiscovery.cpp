#include "ApplicationDiscovery.h"

#include "PathUtils.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>
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

std::wstring AsciiLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        if (character >= L'A' && character <= L'Z') {
            return static_cast<wchar_t>(character - L'A' + L'a');
        }
        return character;
    });
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

struct ExecutableVersionInfo {
    std::wstring productName;
    std::wstring fileDescription;
    std::wstring publisher;
    std::wstring version;
};

std::wstring HexWord(WORD value) {
    constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring result(4, L'0');
    for (int index = 3; index >= 0; --index) {
        result[static_cast<std::size_t>(index)] = digits[value & 0x0f];
        value >>= 4;
    }
    return result;
}

std::wstring QueryVersionString(const std::vector<BYTE>& data,
                                const std::wstring& translation,
                                const wchar_t* valueName) {
    const std::wstring query = L"\\StringFileInfo\\" + translation + L"\\" + valueName;
    LPVOID rawValue = nullptr;
    UINT valueLength = 0;
    if (!VerQueryValueW(const_cast<LPBYTE>(data.data()), query.c_str(), &rawValue,
                        &valueLength) ||
        rawValue == nullptr || valueLength == 0) {
        return {};
    }
    return static_cast<LPCWSTR>(rawValue);
}

ExecutableVersionInfo ReadExecutableVersionInfo(const std::wstring& path) {
    ExecutableVersionInfo info;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return info;
    }

    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return info;
    }

    struct Translation {
        WORD language;
        WORD codePage;
    };
    LPVOID rawTranslations = nullptr;
    UINT translationsLength = 0;
    std::vector<std::wstring> translations;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &rawTranslations,
                       &translationsLength) &&
        rawTranslations != nullptr && translationsLength >= sizeof(Translation)) {
        const auto* values = static_cast<const Translation*>(rawTranslations);
        const std::size_t count = translationsLength / sizeof(Translation);
        translations.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            translations.push_back(HexWord(values[index].language) +
                                   HexWord(values[index].codePage));
        }
    }
    if (translations.empty()) {
        // Most Windows desktop programs expose the English/Unicode block.
        translations.push_back(L"040904b0");
    }

    for (const std::wstring& translation : translations) {
        if (info.productName.empty()) {
            info.productName = QueryVersionString(data, translation, L"ProductName");
        }
        if (info.fileDescription.empty()) {
            info.fileDescription = QueryVersionString(data, translation, L"FileDescription");
        }
        if (info.publisher.empty()) {
            info.publisher = QueryVersionString(data, translation, L"CompanyName");
        }
        if (info.version.empty()) {
            info.version = QueryVersionString(data, translation, L"FileVersion");
        }
        if (!info.productName.empty() && !info.fileDescription.empty() &&
            !info.publisher.empty() && !info.version.empty()) {
            break;
        }
    }

    if (info.version.empty()) {
        VS_FIXEDFILEINFO* fixedInfo = nullptr;
        UINT fixedInfoLength = 0;
        if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<LPVOID*>(&fixedInfo),
                           &fixedInfoLength) &&
            fixedInfo != nullptr && fixedInfoLength >= sizeof(VS_FIXEDFILEINFO) &&
            fixedInfo->dwSignature == VS_FFI_SIGNATURE) {
            info.version = std::to_wstring(HIWORD(fixedInfo->dwFileVersionMS)) + L"." +
                           std::to_wstring(LOWORD(fixedInfo->dwFileVersionMS)) + L"." +
                           std::to_wstring(HIWORD(fixedInfo->dwFileVersionLS)) + L"." +
                           std::to_wstring(LOWORD(fixedInfo->dwFileVersionLS));
        }
    }
    return info;
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

bool IsPathUnderDirectory(const std::wstring& path, const std::wstring& directory) {
    if (path.empty() || directory.empty()) {
        return false;
    }

    std::wstring normalizedDirectory = ExpandEnvironmentVariables(directory);
    normalizedDirectory = Trim(std::move(normalizedDirectory));
    if (normalizedDirectory.empty()) {
        return false;
    }
    std::replace(normalizedDirectory.begin(), normalizedDirectory.end(), L'/', L'\\');
    while (normalizedDirectory.size() > 3 && normalizedDirectory.back() == L'\\') {
        normalizedDirectory.pop_back();
    }
    if (normalizedDirectory.back() != L'\\') {
        normalizedDirectory.push_back(L'\\');
    }

    const std::wstring normalizedPath = PathUtils::NormalizePath(path);
    return normalizedPath.size() > normalizedDirectory.size() &&
           CompareStringOrdinal(normalizedPath.c_str(),
                                static_cast<int>(normalizedDirectory.size()),
                                normalizedDirectory.c_str(),
                                static_cast<int>(normalizedDirectory.size()), TRUE) ==
               CSTR_EQUAL;
}

std::wstring WindowsDirectoryPath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(length + 1);
    }
}

bool IsWindowsSystemPath(const std::wstring& path) {
    static const std::wstring windowsDirectory = WindowsDirectoryPath();
    return IsPathUnderDirectory(path, windowsDirectory);
}

bool IsFixedScanHelperExecutable(const std::filesystem::path& path) {
    const std::wstring fileName = AsciiLower(path.stem().wstring());
    constexpr const wchar_t* ignoredFragments[] = {
        L"unins",          L"uninstall",      L"update",   L"updater",
        L"helper",         L"crash",          L"report",   L"service",
        L"daemon",         L"launcher_helper",
    };
    for (const wchar_t* fragment : ignoredFragments) {
        if (fileName.find(fragment) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool HasApplicationPath(const std::vector<DiscoveredApplication>& applications,
                        const std::wstring& path) {
    return std::any_of(applications.begin(), applications.end(), [&path](const auto& application) {
        return PathUtils::SamePath(application.path, path);
    });
}

void AddFixedScanRoot(std::vector<std::filesystem::path>& roots,
                      const std::filesystem::path& root) {
    if (root.empty()) {
        return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        return;
    }

    std::wstring normalized = root.lexically_normal().wstring();
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    if (std::none_of(roots.begin(), roots.end(), [&normalized](const auto& existing) {
            std::wstring existingPath = existing.lexically_normal().wstring();
            std::replace(existingPath.begin(), existingPath.end(), L'/', L'\\');
            return CompareStringOrdinal(existingPath.c_str(), -1, normalized.c_str(), -1, TRUE) ==
                   CSTR_EQUAL;
        })) {
        roots.push_back(root);
    }
}

void ScanFixedProgramDirectory(const std::filesystem::path& root,
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
        if (!iterator->is_regular_file(error) || error ||
            !HasExtension(iterator->path(), L".exe")) {
            error.clear();
            continue;
        }

        ++stats->fixedExecutableEntries;
        if (IsFixedScanHelperExecutable(iterator->path())) {
            ++stats->filteredEntries;
            continue;
        }

        const std::wstring executable = ParseExecutableValue(iterator->path().wstring());
        if (executable.empty()) {
            ++stats->unresolvedEntries;
            continue;
        }
        if (IsWindowsSystemPath(executable)) {
            ++stats->filteredEntries;
            continue;
        }

        const ExecutableVersionInfo versionInfo = ReadExecutableVersionInfo(executable);
        const bool hasIdentity = !versionInfo.productName.empty() ||
                                 !versionInfo.fileDescription.empty();
        // An EXE without ProductName/FileDescription is kept only when another
        // source already identified the same path. This makes it a low-priority
        // metadata supplement instead of turning every helper into an application.
        if (!hasIdentity && !HasApplicationPath(applications, executable)) {
            ++stats->filteredEntries;
            continue;
        }

        DiscoveredApplication application;
        application.path = executable;
        application.displayName = !versionInfo.productName.empty()
                                      ? versionInfo.productName
                                      : (!versionInfo.fileDescription.empty()
                                             ? versionInfo.fileDescription
                                             : iterator->path().stem().wstring());
        application.publisher = versionInfo.publisher;
        application.version = versionInfo.version;
        application.installLocation = iterator->path().parent_path().wstring();
        AddOrMerge(applications, std::move(application));
    }
}

void ScanFixedProgramDirectories(std::vector<DiscoveredApplication>& applications,
                                 DiscoveryStats* stats,
                                 const std::atomic_bool* cancellation) {
    std::vector<std::filesystem::path> roots;
    AddFixedScanRoot(roots, KnownFolderPath(FOLDERID_ProgramFiles));
    AddFixedScanRoot(roots, KnownFolderPath(FOLDERID_ProgramFilesX86));

    const std::wstring localAppData = KnownFolderPath(FOLDERID_LocalAppData);
    if (!localAppData.empty()) {
        AddFixedScanRoot(roots, std::filesystem::path(localAppData) / L"Programs");
    }

    for (const std::filesystem::path& root : roots) {
        if (IsCancelled(cancellation)) {
            break;
        }
        ScanFixedProgramDirectory(root, applications, stats, cancellation);
    }
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

    ScanFixedProgramDirectories(applications, stats, cancellation);

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
