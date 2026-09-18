#include "ConfigStore.h"

#include "Win32Support.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

constexpr std::size_t kMaxConfigBytes = 16u * 1024u * 1024u;

std::wstring Trim(std::wstring value) {
    const auto isWhitespace = [](wchar_t character) {
        return std::iswspace(static_cast<wint_t>(character)) != 0;
    };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                             [&isWhitespace](wchar_t character) {
                                                 return !isWhitespace(character);
                                             }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&isWhitespace](wchar_t character) {
                                 return !isWhitespace(character);
                             })
                    .base(),
                value.end());
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

bool Utf8ToWide(const std::string& input, std::wstring& output, std::wstring& error) {
    if (input.empty()) {
        output.clear();
        return true;
    }

    if (input.find('\0') != std::string::npos) {
        error = L"配置文件包含无效的 NUL 字符";
        return false;
    }

    if (input.size() > static_cast<std::size_t>(INT_MAX)) {
        error = L"配置文件过大";
        return false;
    }

    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) {
        error = L"配置文件不是有效的 UTF-8 文本";
        return false;
    }

    output.resize(static_cast<std::size_t>(required));
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), required) != required) {
        error = L"配置文件 UTF-8 解码失败";
        output.clear();
        return false;
    }

    if (!output.empty() && output.front() == L'\ufeff') {
        output.erase(output.begin());
    }
    return true;
}

bool WideToUtf8(const std::wstring& input, std::string& output, std::wstring& error) {
    if (input.empty()) {
        output.clear();
        return true;
    }

    if (input.size() > static_cast<std::size_t>(INT_MAX)) {
        error = L"配置内容过大";
        return false;
    }

    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                             static_cast<int>(input.size()), nullptr, 0, nullptr,
                                             nullptr);
    if (required <= 0) {
        error = L"配置内容无法编码为 UTF-8";
        return false;
    }

    output.resize(static_cast<std::size_t>(required));
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                            static_cast<int>(input.size()), output.data(), required, nullptr,
                            nullptr) != required) {
        error = L"配置内容 UTF-8 编码失败";
        output.clear();
        return false;
    }
    return true;
}

bool ParseInteger(const std::wstring& text, int& value) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }

    wchar_t* end = nullptr;
    const long parsed = wcstol(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || *end != L'\0' || parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ParseUnsigned(const std::wstring& text, std::uint32_t& value) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }

    const wchar_t* begin = trimmed.c_str();
    int base = 10;
    if (trimmed.size() > 2 && trimmed[0] == L'0' &&
        (trimmed[1] == L'x' || trimmed[1] == L'X')) {
        begin += 2;
        base = 16;
    }
    if (*begin == L'\0') {
        return false;
    }

    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(begin, &end, base);
    if (end == begin || *end != L'\0' || parsed > UINT32_MAX) {
        return false;
    }
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseBoolean(const std::wstring& text, bool& value) {
    const std::wstring normalized = AsciiLower(Trim(text));
    if (normalized == L"1" || normalized == L"true" || normalized == L"yes") {
        value = true;
        return true;
    }
    if (normalized == L"0" || normalized == L"false" || normalized == L"no") {
        value = false;
        return true;
    }
    return false;
}

bool ParsePositiveIndex(const std::wstring& text, int& value) {
    const std::wstring trimmed = Trim(text);
    if (trimmed.empty()) {
        return false;
    }

    unsigned long long parsed = 0;
    for (const wchar_t character : trimmed) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        parsed = parsed * 10u + static_cast<unsigned long long>(character - L'0');
        if (parsed > static_cast<unsigned long long>(INT_MAX) || parsed == 0) {
            return false;
        }
    }
    value = static_cast<int>(parsed);
    return true;
}

bool ParseHotkeyIndex(std::wstring_view key, int& index) {
    constexpr std::wstring_view kPrefix = L"hotkey.";
    if (key.size() <= kPrefix.size() || key.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    return ParsePositiveIndex(std::wstring(key.substr(kPrefix.size())), index);
}

bool ParseHotkeyMode(const std::wstring& text, HotkeyMode& mode) {
    const std::wstring normalized = AsciiLower(Trim(text));
    if (normalized == L"block_all" || normalized == L"all") {
        mode = HotkeyMode::BlockAll;
        return true;
    }
    if (normalized == L"blacklist") {
        mode = HotkeyMode::Blacklist;
        return true;
    }
    if (normalized == L"whitelist") {
        mode = HotkeyMode::Whitelist;
        return true;
    }
    return false;
}

const wchar_t* HotkeyModeText(HotkeyMode mode) {
    switch (mode) {
        case HotkeyMode::BlockAll:
            return L"block_all";
        case HotkeyMode::Blacklist:
            return L"blacklist";
        case HotkeyMode::Whitelist:
            return L"whitelist";
        default:
            return L"";
    }
}

bool ParseHotkeySpec(const std::wstring& text, HotkeySpec& hotkey) {
    const std::wstring trimmed = Trim(text);
    const std::size_t separator = trimmed.find(L':');
    if (separator == std::wstring::npos) {
        return false;
    }

    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;
    if (!ParseUnsigned(trimmed.substr(0, separator), modifiers) ||
        !ParseUnsigned(trimmed.substr(separator + 1), virtualKey)) {
        return false;
    }

    hotkey = NormalizeHotkey({modifiers, virtualKey});
    return IsValidHotkey(hotkey);
}

struct IniSection {
    std::wstring name;
    std::map<std::wstring, std::wstring> values;
};

bool ParseIni(const std::wstring& text, std::vector<IniSection>& sections,
              std::wstring& error) {
    std::wistringstream stream(text);
    std::wstring line;
    IniSection* current = nullptr;
    std::size_t lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }

        const std::wstring trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == L';' || trimmed.front() == L'#') {
            continue;
        }

        if (trimmed.front() == L'[') {
            if (trimmed.size() < 3 || trimmed.back() != L']') {
                error = L"配置文件第 " + std::to_wstring(lineNumber) + L" 行的节格式无效";
                return false;
            }

            const std::wstring sectionName = Trim(trimmed.substr(1, trimmed.size() - 2));
            if (sectionName.empty()) {
                error = L"配置文件第 " + std::to_wstring(lineNumber) + L" 行的节名称为空";
                return false;
            }

            sections.push_back({sectionName, {}});
            current = &sections.back();
            continue;
        }

        if (current == nullptr) {
            error = L"配置文件第 " + std::to_wstring(lineNumber) + L" 行缺少节名称";
            return false;
        }

        const std::size_t separator = trimmed.find(L'=');
        if (separator == std::wstring::npos) {
            error = L"配置文件第 " + std::to_wstring(lineNumber) + L" 行缺少 '='";
            return false;
        }

        const std::wstring key = AsciiLower(Trim(trimmed.substr(0, separator)));
        if (key.empty()) {
            error = L"配置文件第 " + std::to_wstring(lineNumber) + L" 行的键为空";
            return false;
        }
        if (current->values.find(key) != current->values.end()) {
            error = L"配置文件第 " + std::to_wstring(lineNumber) + L" 行重复定义键 " + key;
            return false;
        }
        current->values.emplace(key, Trim(trimmed.substr(separator + 1)));
    }
    return true;
}

const std::wstring* FindValue(const IniSection& section, std::wstring_view key) {
    const auto iterator = section.values.find(std::wstring(key));
    return iterator == section.values.end() ? nullptr : &iterator->second;
}

bool IsSection(const IniSection& section, std::wstring_view name) {
    return AsciiLower(section.name) == name;
}

bool IsAppSection(const IniSection& section, int& index) {
    constexpr std::wstring_view kPrefix = L"app.";
    const std::wstring name = AsciiLower(section.name);
    if (name.size() <= kPrefix.size() || name.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    return ParsePositiveIndex(name.substr(kPrefix.size()), index);
}

bool ReadFileBytes(const std::filesystem::path& path, std::string& bytes, std::wstring& error,
                   DWORD& systemError) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
                                                           FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        systemError = GetLastError();
        error = Win32Support::ErrorMessage(L"读取配置文件", systemError);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        systemError = GetLastError();
        error = Win32Support::ErrorMessage(L"获取配置文件大小", systemError);
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > kMaxConfigBytes) {
        systemError = ERROR_FILE_TOO_LARGE;
        error = L"配置文件超过 16 MiB 限制";
        CloseHandle(file);
        return false;
    }

    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr)) {
            systemError = GetLastError();
            error = Win32Support::ErrorMessage(L"读取配置文件", systemError);
            CloseHandle(file);
            return false;
        }
        if (read == 0) {
            systemError = ERROR_HANDLE_EOF;
            error = L"读取配置文件时遇到意外的文件结尾";
            CloseHandle(file);
            return false;
        }
        offset += read;
    }

    CloseHandle(file);
    return true;
}

bool WriteFileBytes(const std::filesystem::path& path, const std::string& bytes,
                    std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Win32Support::ErrorMessage(L"创建临时配置文件");
        return false;
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, request, &written, nullptr)) {
            error = Win32Support::ErrorMessage(L"写入临时配置文件");
            CloseHandle(file);
            return false;
        }
        if (written == 0) {
            error = L"写入临时配置文件时未写入数据";
            CloseHandle(file);
            return false;
        }
        offset += written;
    }

    if (!FlushFileBuffers(file)) {
        error = Win32Support::ErrorMessage(L"刷新临时配置文件");
        CloseHandle(file);
        return false;
    }

    if (!CloseHandle(file)) {
        error = Win32Support::ErrorMessage(L"关闭临时配置文件");
        return false;
    }
    return true;
}

std::wstring SerializeIni(const AppConfig& config) {
    std::wstring output;
    output.reserve(256 + config.apps.size() * 240);
    output += L"[Settings]\r\n";
    output += L"Version=" + std::to_wstring(config.version) + L"\r\n";

    for (std::size_t index = 0; index < config.apps.size(); ++index) {
        output += L"\r\n[App." + std::to_wstring(index + 1) + L"]\r\n";
        if (!config.apps[index].displayName.empty()) {
            output += L"Name=" + config.apps[index].displayName + L"\r\n";
        }
        output += L"Path=" + config.apps[index].path + L"\r\n";
        output += L"Enabled=" + std::to_wstring(config.apps[index].enabled ? 1 : 0) + L"\r\n";
        output += L"Kind=" +
                  std::wstring(config.apps[index].kind == RuleKind::Directory ? L"directory"
                                                                                : L"executable") +
                  L"\r\n";
        output += L"HotkeyMode=" +
                  std::wstring(HotkeyModeText(config.apps[index].hotkeyPolicy.mode)) + L"\r\n";
        for (std::size_t hotkeyIndex = 0;
             hotkeyIndex < config.apps[index].hotkeyPolicy.hotkeys.size(); ++hotkeyIndex) {
            const HotkeySpec& hotkey = config.apps[index].hotkeyPolicy.hotkeys[hotkeyIndex];
            output += L"Hotkey." + std::to_wstring(hotkeyIndex + 1) + L"=" +
                      std::to_wstring(hotkey.modifiers) + L":" +
                      std::to_wstring(hotkey.virtualKey) + L"\r\n";
        }
    }
    return output;
}

}  // namespace

ConfigStore::ConfigStore() : m_path(DefaultPath()) {}

ConfigStore::ConfigStore(std::filesystem::path path) : m_path(std::move(path)) {}

const std::filesystem::path& ConfigStore::Path() const noexcept {
    return m_path;
}

bool ConfigStore::Load(AppConfig& config, std::wstring& error) const {
    config = AppConfig{};
    error.clear();

    std::string bytes;
    DWORD systemError = ERROR_SUCCESS;
    if (!ReadFileBytes(m_path, bytes, error, systemError)) {
        const DWORD errorCode = systemError;
        if (errorCode == ERROR_FILE_NOT_FOUND || errorCode == ERROR_PATH_NOT_FOUND) {
            error.clear();
            return true;
        }
        return false;
    }

    std::wstring text;
    if (!Utf8ToWide(bytes, text, error)) {
        return false;
    }

    std::vector<IniSection> sections;
    if (!ParseIni(text, sections, error)) {
        return false;
    }

    bool settingsFound = false;
    for (const IniSection& section : sections) {
        if (!IsSection(section, L"settings")) {
            continue;
        }

        if (settingsFound) {
            error = L"配置文件重复定义 Settings 节";
            return false;
        }
        settingsFound = true;

        const std::wstring* version = FindValue(section, L"version");
        if (version == nullptr || !ParseInteger(*version, config.version)) {
            error = L"Settings.Version 不是有效整数";
            return false;
        }
    }

    if (!settingsFound) {
        error = L"配置文件缺少 Settings 节";
        return false;
    }
    if (config.version != 1 && config.version != kCurrentVersion) {
        error = L"配置版本不受支持：" + std::to_wstring(config.version);
        return false;
    }

    const bool legacyConfig = config.version == 1;

    struct IndexedRule {
        int index;
        AppRule rule;
    };
    std::vector<IndexedRule> indexedRules;

    for (const IniSection& section : sections) {
        int index = 0;
        if (!IsAppSection(section, index)) {
            continue;
        }

        const std::wstring* path = FindValue(section, L"path");
        if (path == nullptr || path->empty()) {
            error = section.name + L" 缺少 Path";
            return false;
        }

        AppRule rule{*path, true};
        if (const std::wstring* name = FindValue(section, L"name"); name != nullptr) {
            rule.displayName = *name;
        }
        if (const std::wstring* enabled = FindValue(section, L"enabled");
            enabled != nullptr && !ParseBoolean(*enabled, rule.enabled)) {
            error = section.name + L".Enabled 不是有效布尔值";
            return false;
        }
        const std::wstring* kind = FindValue(section, L"kind");
        if (kind == nullptr) {
            error = section.name + L" 缺少 Kind";
            return false;
        }
        const std::wstring normalizedKind = AsciiLower(Trim(*kind));
        if (normalizedKind == L"executable") {
            rule.kind = RuleKind::Executable;
        } else if (normalizedKind == L"directory") {
            rule.kind = RuleKind::Directory;
        } else {
            error = section.name + L".Kind 不是有效规则类型";
            return false;
        }

        if (const std::wstring* mode = FindValue(section, L"hotkeymode"); mode != nullptr) {
            if (!ParseHotkeyMode(*mode, rule.hotkeyPolicy.mode)) {
                error = section.name + L".HotkeyMode 不是有效快捷键策略";
                return false;
            }
        } else if (!legacyConfig) {
            rule.hotkeyPolicy.mode = HotkeyMode::BlockAll;
        }

        std::vector<std::pair<int, HotkeySpec>> indexedHotkeys;
        for (const auto& [key, value] : section.values) {
            int hotkeyIndex = 0;
            if (!ParseHotkeyIndex(key, hotkeyIndex)) {
                continue;
            }

            HotkeySpec hotkey;
            if (!ParseHotkeySpec(value, hotkey)) {
                error = section.name + L"." + key + L" 不是有效快捷键";
                return false;
            }
            indexedHotkeys.push_back({hotkeyIndex, hotkey});
        }
        std::sort(indexedHotkeys.begin(), indexedHotkeys.end(),
                  [](const auto& left, const auto& right) { return left.first < right.first; });
        for (std::size_t hotkeyIndex = 1; hotkeyIndex < indexedHotkeys.size(); ++hotkeyIndex) {
            if (indexedHotkeys[hotkeyIndex - 1].first == indexedHotkeys[hotkeyIndex].first) {
                error = section.name + L" 重复定义快捷键序号";
                return false;
            }
        }
        for (const auto& indexedHotkey : indexedHotkeys) {
            rule.hotkeyPolicy.hotkeys.push_back(indexedHotkey.second);
        }
        NormalizeHotkeyPolicy(rule.hotkeyPolicy);
        if (!ValidateHotkeyPolicy(rule.hotkeyPolicy)) {
            error = section.name + L" 的快捷键数量超过限制";
            return false;
        }
        indexedRules.push_back({index, std::move(rule)});
    }

    std::stable_sort(indexedRules.begin(), indexedRules.end(),
                     [](const IndexedRule& left, const IndexedRule& right) {
                         return left.index < right.index;
                     });
    for (std::size_t index = 1; index < indexedRules.size(); ++index) {
        if (indexedRules[index - 1].index == indexedRules[index].index) {
            error = L"配置文件重复定义 App." + std::to_wstring(indexedRules[index].index);
            return false;
        }
    }
    config.apps.reserve(indexedRules.size());
    for (IndexedRule& indexedRule : indexedRules) {
        config.apps.push_back(std::move(indexedRule.rule));
    }
    config.version = kCurrentVersion;
    return true;
}

bool ConfigStore::Save(const AppConfig& config, std::wstring& error) const {
    error.clear();

    if (config.version != kCurrentVersion) {
        error = L"不能写入不受支持的配置版本：" + std::to_wstring(config.version);
        return false;
    }

    AppConfig normalizedConfig = config;
    for (AppRule& rule : normalizedConfig.apps) {
        NormalizeHotkeyPolicy(rule.hotkeyPolicy);
        if (!ValidateHotkeyPolicy(rule.hotkeyPolicy)) {
            error = L"配置包含无效快捷键策略或快捷键数量超过限制";
            return false;
        }
    }

    std::wstring text = SerializeIni(normalizedConfig);
    std::string bytes;
    if (!WideToUtf8(text, bytes, error)) {
        return false;
    }
    if (bytes.size() > kMaxConfigBytes) {
        error = L"配置内容超过 16 MiB 限制";
        return false;
    }

    const std::filesystem::path parent = m_path.parent_path();
    std::error_code fileSystemError;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, fileSystemError);
        if (fileSystemError) {
            error = L"创建配置目录失败：" + std::wstring(parent.c_str()) + L"（错误码 " +
                    std::to_wstring(fileSystemError.value()) + L"）";
            return false;
        }
    }

    const std::filesystem::path temporaryPath = m_path.wstring() + L".tmp";
    if (!WriteFileBytes(temporaryPath, bytes, error)) {
        DeleteFileW(temporaryPath.c_str());
        return false;
    }

    if (!MoveFileExW(temporaryPath.c_str(), m_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = Win32Support::ErrorMessage(L"替换配置文件");
        DeleteFileW(temporaryPath.c_str());
        return false;
    }
    return true;
}

std::filesystem::path ConfigStore::DefaultPath() {
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr,
                                       &localAppData)) &&
        localAppData != nullptr) {
        const std::filesystem::path result =
            std::filesystem::path(localAppData) / L"HotkeyBlocker" / L"config.ini";
        CoTaskMemFree(localAppData);
        return result;
    }

    if (localAppData != nullptr) {
        CoTaskMemFree(localAppData);
    }

    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(buffer) / L"HotkeyBlocker" / L"config.ini";
    }

    return std::filesystem::path(L"HotkeyBlocker") / L"config.ini";
}
