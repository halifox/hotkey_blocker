#include "HotkeyPolicyTransport.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>

namespace {

constexpr wchar_t kPolicyMappingName[] = L"Local\\HotkeyBlocker.HotkeyPolicies";
constexpr std::uint32_t kPolicyMagic = 0x484B4250u;
constexpr std::uint32_t kPolicyVersion = 1u;
constexpr std::size_t kMaxPolicyEntries = 256;

#pragma pack(push, 1)
struct WireHotkeySpec {
    std::uint32_t modifiers = 0;
    std::uint32_t virtualKey = 0;
};

struct WirePolicyEntry {
    std::uint32_t processId = 0;
    std::uint32_t mode = 0;
    std::uint32_t hotkeyCount = 0;
    std::uint32_t valid = 0;
    WireHotkeySpec hotkeys[HotkeyPolicyConstants::kMaxHotkeysPerPolicy]{};
};

struct WirePolicyTable {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t reserved = 0;
    WirePolicyEntry entries[kMaxPolicyEntries]{};
};
#pragma pack(pop)

static_assert(sizeof(WireHotkeySpec) == sizeof(HotkeySpec));
static_assert(sizeof(WirePolicyTable) < (std::numeric_limits<DWORD>::max)());

constexpr DWORD MappingSize() noexcept {
    return static_cast<DWORD>(sizeof(WirePolicyTable));
}

std::wstring ErrorText(const wchar_t* operation) {
    const DWORD errorCode = GetLastError();
    wchar_t buffer[256]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, errorCode, 0, buffer,
                                       static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])),
                                       nullptr);
    std::wstring result = operation;
    result += L"（错误码 ";
    result += std::to_wstring(errorCode);
    result += L"）";
    if (length != 0) {
        DWORD trimmedLength = length;
        while (trimmedLength > 0 &&
               (buffer[trimmedLength - 1] == L'\r' ||
                buffer[trimmedLength - 1] == L'\n')) {
            buffer[trimmedLength - 1] = L'\0';
            --trimmedLength;
        }
        result += L"：";
        result += buffer;
    }
    return result;
}

const WirePolicyTable* AsTable(const void* view) {
    return static_cast<const WirePolicyTable*>(view);
}

WirePolicyTable* AsTable(void* view) {
    return static_cast<WirePolicyTable*>(view);
}

bool IsTableValid(const WirePolicyTable& table) noexcept {
    return table.magic == kPolicyMagic && table.version == kPolicyVersion &&
           table.entryCount <= kMaxPolicyEntries;
}

}  // namespace

HotkeyPolicyRegistry::~HotkeyPolicyRegistry() {
    Stop();
}

bool HotkeyPolicyRegistry::Start(std::wstring& error) {
    std::lock_guard lock(m_mutex);
    if (m_mapping != nullptr && m_view != nullptr) {
        return true;
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        MappingSize(), kPolicyMappingName);
    if (mapping == nullptr) {
        error = ErrorText(L"创建快捷键策略共享内存失败");
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        error = L"快捷键策略共享内存已经存在";
        return false;
    }

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, MappingSize());
    if (view == nullptr) {
        error = ErrorText(L"映射快捷键策略共享内存失败");
        CloseHandle(mapping);
        return false;
    }

    auto* table = AsTable(view);
    std::memset(table, 0, sizeof(*table));
    table->magic = kPolicyMagic;
    table->version = kPolicyVersion;
    table->entryCount = 0;
    m_mapping = mapping;
    m_view = view;
    error.clear();
    return true;
}

void HotkeyPolicyRegistry::Stop() {
    std::lock_guard lock(m_mutex);
    if (m_view != nullptr) {
        UnmapViewOfFile(m_view);
        m_view = nullptr;
    }
    if (m_mapping != nullptr) {
        CloseHandle(m_mapping);
        m_mapping = nullptr;
    }
}

bool HotkeyPolicyRegistry::Publish(DWORD processId, const HotkeyPolicy& policy,
                                   std::wstring& error) {
    HotkeyPolicy normalized = policy;
    NormalizeHotkeyPolicy(normalized);
    if (!ValidateHotkeyPolicy(normalized)) {
        error = L"快捷键策略无效或快捷键数量超过限制";
        return false;
    }

    std::lock_guard lock(m_mutex);
    if (m_view == nullptr) {
        error = L"快捷键策略共享内存尚未启动";
        return false;
    }

    auto* table = AsTable(m_view);
    WirePolicyEntry* entry = nullptr;
    for (std::size_t index = 0; index < table->entryCount; ++index) {
        if (table->entries[index].valid != 0 && table->entries[index].processId == processId) {
            entry = &table->entries[index];
            break;
        }
    }
    if (entry == nullptr) {
        for (std::size_t index = 0; index < table->entryCount; ++index) {
            if (table->entries[index].valid == 0) {
                entry = &table->entries[index];
                break;
            }
        }
    }
    if (entry == nullptr && table->entryCount < kMaxPolicyEntries) {
        entry = &table->entries[table->entryCount++];
    }
    if (entry == nullptr) {
        error = L"快捷键策略共享内存中的进程条目已满";
        return false;
    }

    entry->valid = 0;
    entry->processId = processId;
    entry->mode = static_cast<std::uint32_t>(normalized.mode);
    entry->hotkeyCount = static_cast<std::uint32_t>(normalized.hotkeys.size());
    std::memset(entry->hotkeys, 0, sizeof(entry->hotkeys));
    for (std::size_t index = 0; index < normalized.hotkeys.size(); ++index) {
        entry->hotkeys[index].modifiers = normalized.hotkeys[index].modifiers;
        entry->hotkeys[index].virtualKey = normalized.hotkeys[index].virtualKey;
    }
    entry->valid = 1;
    error.clear();
    return true;
}

void HotkeyPolicyRegistry::Remove(DWORD processId) {
    std::lock_guard lock(m_mutex);
    if (m_view == nullptr) {
        return;
    }

    auto* table = AsTable(m_view);
    for (std::size_t index = 0; index < table->entryCount; ++index) {
        WirePolicyEntry& entry = table->entries[index];
        if (entry.valid != 0 && entry.processId == processId) {
            entry.valid = 0;
            entry.hotkeyCount = 0;
            return;
        }
    }
}

bool LoadHotkeyPolicyForCurrentProcess(HotkeyPolicy& policy) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kPolicyMappingName);
    if (mapping == nullptr) {
        return false;
    }

    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, MappingSize());
    if (view == nullptr) {
        CloseHandle(mapping);
        return false;
    }

    const auto* table = AsTable(view);
    bool found = false;
    HotkeyPolicy loaded;
    const DWORD processId = GetCurrentProcessId();
    if (IsTableValid(*table)) {
        for (std::size_t index = 0; index < table->entryCount; ++index) {
            const WirePolicyEntry& entry = table->entries[index];
            if (entry.valid == 0 || entry.processId != processId ||
                entry.hotkeyCount > HotkeyPolicyConstants::kMaxHotkeysPerPolicy ||
                !IsValidHotkeyMode(static_cast<HotkeyMode>(entry.mode))) {
                continue;
            }

            loaded.mode = static_cast<HotkeyMode>(entry.mode);
            loaded.hotkeys.reserve(entry.hotkeyCount);
            for (std::size_t hotkeyIndex = 0; hotkeyIndex < entry.hotkeyCount; ++hotkeyIndex) {
                loaded.hotkeys.push_back(
                    {entry.hotkeys[hotkeyIndex].modifiers, entry.hotkeys[hotkeyIndex].virtualKey});
            }
            NormalizeHotkeyPolicy(loaded);
            if (ValidateHotkeyPolicy(loaded)) {
                found = true;
            }
            break;
        }
    }

    UnmapViewOfFile(view);
    CloseHandle(mapping);
    if (found) {
        policy = std::move(loaded);
    }
    return found;
}
