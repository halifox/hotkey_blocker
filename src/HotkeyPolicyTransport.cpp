#include "HotkeyPolicyTransport.h"

#include <cstdint>
#include <limits>
#include <atomic>
#include <utility>

namespace {

constexpr wchar_t kPolicyMappingName[] = L"Local\\HotkeyBlocker.HotkeyPolicies";
constexpr wchar_t kPolicyMutexName[] = L"Local\\HotkeyBlocker.HotkeyPolicies.Lock";
constexpr std::uint32_t kPolicyMagic = 0x484B4250u;
constexpr std::uint32_t kPolicyVersion = 4u;
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
    std::uint64_t processCreationTime = 0;
    WireHotkeySpec hotkeys[HotkeyPolicyConstants::kMaxHotkeysPerPolicy]{};
};

struct WirePolicyTable {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t entryCount = 0;
    std::uint32_t ownerProcessId = 0;
    std::uint64_t ownerCreationTime = 0;
    std::uint64_t ownerToken = 0;
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
           table.entryCount <= kMaxPolicyEntries && table.ownerProcessId != 0 &&
           table.ownerCreationTime != 0 && table.ownerToken != 0;
}

bool IsOwnedBy(const WirePolicyTable& table, DWORD processId,
               ULONGLONG creationTime, std::uint64_t ownerToken) noexcept {
    return IsTableValid(table) && table.ownerProcessId == processId &&
           table.ownerCreationTime == creationTime && table.ownerToken == ownerToken;
}

class ScopedPolicyTableLock final {
public:
    ScopedPolicyTableLock() {
        m_mutex = CreateMutexW(nullptr, FALSE, kPolicyMutexName);
        if (m_mutex == nullptr) {
            return;
        }
        const DWORD waitResult = WaitForSingleObject(m_mutex, INFINITE);
        m_acquired = waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED;
    }

    ~ScopedPolicyTableLock() {
        if (m_acquired) {
            ReleaseMutex(m_mutex);
        }
        if (m_mutex != nullptr) {
            CloseHandle(m_mutex);
        }
    }

    bool Acquired() const noexcept {
        return m_acquired;
    }

private:
    HANDLE m_mutex = nullptr;
    bool m_acquired = false;
};

std::uint64_t NewOwnerToken() {
    static std::atomic_uint64_t nextToken{1};
    return nextToken.fetch_add(1, std::memory_order_relaxed);
}

bool GetCreationTime(HANDLE process, ULONGLONG& creationTime) {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
        return false;
    }
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    creationTime = value.QuadPart;
    return creationTime != 0;
}

bool IsProcessInstanceAlive(const ProcessIdentity& identity) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                 identity.pid);
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }

    ULONGLONG creationTime = 0;
    const bool queried = GetCreationTime(process, creationTime);
    const DWORD waitResult = WaitForSingleObject(process, 0);
    CloseHandle(process);
    if (waitResult == WAIT_OBJECT_0) {
        return false;
    }
    if (!queried) {
        return true;
    }
    return creationTime == identity.creationTime;
}

struct RetainedMapping final {
    HANDLE mapping = nullptr;
    void* view = nullptr;

    ~RetainedMapping() {
        if (view != nullptr) {
            UnmapViewOfFile(view);
        }
        if (mapping != nullptr) {
            CloseHandle(mapping);
        }
    }
};

HANDLE g_readerMapping = nullptr;
const WirePolicyTable* g_readerTable = nullptr;

}  // namespace

HotkeyPolicyRegistry::~HotkeyPolicyRegistry() {
    Stop();
}

bool HotkeyPolicyRegistry::Start(std::wstring& error) {
    std::lock_guard lock(m_mutex);
    if (m_mapping != nullptr && m_view != nullptr) {
        return true;
    }
    ScopedPolicyTableLock tableLock;
    if (!tableLock.Acquired()) {
        error = ErrorText(L"锁定快捷键策略共享表失败");
        return false;
    }
    if (m_ownerToken == 0) {
        m_ownerToken = NewOwnerToken();
    }

    ULONGLONG ownerCreationTime = 0;
    if (!GetCreationTime(GetCurrentProcess(), ownerCreationTime)) {
        error = ErrorText(L"查询当前进程标识失败");
        return false;
    }

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        MappingSize(), kPolicyMappingName);
    if (mapping == nullptr) {
        error = ErrorText(L"创建快捷键策略共享内存失败");
        return false;
    }
    const DWORD creationError = GetLastError();
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, MappingSize());
    if (view == nullptr) {
        error = ErrorText(L"映射快捷键策略共享内存失败");
        CloseHandle(mapping);
        return false;
    }

    auto* table = AsTable(view);
    if (creationError == ERROR_ALREADY_EXISTS) {
        if (!IsTableValid(*table)) {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            error = L"快捷键策略共享内存已经被其他实例占用";
            return false;
        }
        const ProcessIdentity owner{table->ownerProcessId, table->ownerCreationTime};
        const DWORD currentPid = GetCurrentProcessId();
        const bool sameOwner = owner.pid == currentPid &&
                               owner.creationTime == ownerCreationTime &&
                               table->ownerToken == m_ownerToken;
        if (!sameOwner && IsProcessInstanceAlive(owner)) {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            error = L"快捷键策略共享内存已经被其他实例占用";
            return false;
        }
        table->ownerProcessId = currentPid;
        table->ownerCreationTime = ownerCreationTime;
        table->ownerToken = m_ownerToken;
        for (std::size_t index = 0; index < table->entryCount; ++index) {
            WirePolicyEntry& entry = table->entries[index];
            if (entry.valid != 0 &&
                !IsProcessInstanceAlive({entry.processId, entry.processCreationTime})) {
                entry.valid = 0;
            }
        }
    } else {
        table->magic = kPolicyMagic;
        table->version = kPolicyVersion;
        table->entryCount = 0;
        table->ownerProcessId = GetCurrentProcessId();
        table->ownerCreationTime = ownerCreationTime;
        table->ownerToken = m_ownerToken;
    }
    m_mapping = mapping;
    m_view = view;
    m_ownerCreationTime = ownerCreationTime;
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

bool HotkeyPolicyRegistry::Publish(const ProcessIdentity& process,
                                   const HotkeyPolicy& policy,
                                   std::wstring& error) {
    if (process.pid == 0 || process.creationTime == 0) {
        error = L"目标进程标识无效";
        return false;
    }

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
    ScopedPolicyTableLock tableLock;
    if (!tableLock.Acquired()) {
        error = ErrorText(L"锁定快捷键策略共享表失败");
        return false;
    }

    auto* table = AsTable(m_view);
    if (!IsOwnedBy(*table, GetCurrentProcessId(), m_ownerCreationTime, m_ownerToken)) {
        error = L"快捷键策略共享表所有权已变化";
        return false;
    }
    WirePolicyEntry* entry = nullptr;
    for (std::size_t index = 0; index < table->entryCount; ++index) {
        if (table->entries[index].valid != 0 &&
            table->entries[index].processId == process.pid) {
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
    entry->processId = process.pid;
    entry->processCreationTime = process.creationTime;
    entry->mode = static_cast<std::uint32_t>(normalized.mode);
    entry->hotkeyCount = static_cast<std::uint32_t>(normalized.hotkeys.size());
    for (std::size_t index = 0; index < normalized.hotkeys.size(); ++index) {
        entry->hotkeys[index].modifiers = normalized.hotkeys[index].modifiers;
        entry->hotkeys[index].virtualKey = normalized.hotkeys[index].virtualKey;
    }
    entry->valid = 1;
    error.clear();
    return true;
}

void HotkeyPolicyRegistry::Remove(const ProcessIdentity& process) {
    std::lock_guard lock(m_mutex);
    if (m_view == nullptr) {
        return;
    }
    ScopedPolicyTableLock tableLock;
    if (!tableLock.Acquired()) {
        return;
    }

    auto* table = AsTable(m_view);
    if (!IsOwnedBy(*table, GetCurrentProcessId(), m_ownerCreationTime, m_ownerToken)) {
        return;
    }
    for (std::size_t index = 0; index < table->entryCount; ++index) {
        WirePolicyEntry& entry = table->entries[index];
        if (entry.valid != 0 && entry.processId == process.pid &&
            entry.processCreationTime == process.creationTime) {
            entry.valid = 0;
            return;
        }
    }
}

std::shared_ptr<void> HotkeyPolicyRegistry::RetainMappingLifetime(std::wstring& error) const {
    std::lock_guard lock(m_mutex);
    if (m_mapping == nullptr || m_view == nullptr) {
        error = L"快捷键策略共享内存尚未启动";
        return {};
    }

    HANDLE mapping = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), m_mapping, GetCurrentProcess(), &mapping, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) {
        error = ErrorText(L"保留快捷键策略共享内存句柄失败");
        return {};
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, MappingSize());
    if (view == nullptr) {
        error = ErrorText(L"保留快捷键策略共享内存视图失败");
        CloseHandle(mapping);
        return {};
    }

    try {
        auto retained = std::make_shared<RetainedMapping>();
        retained->mapping = mapping;
        retained->view = view;
        error.clear();
        return retained;
    } catch (...) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        error = L"无法保留快捷键策略共享内存生命周期";
        return {};
    }
}

bool RetainHotkeyPolicyMappingForCurrentProcess() {
    if (g_readerMapping != nullptr && g_readerTable != nullptr) {
        return true;
    }

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, kPolicyMappingName);
    if (mapping == nullptr) {
        return false;
    }
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, MappingSize());
    if (view == nullptr) {
        CloseHandle(mapping);
        return false;
    }

    g_readerMapping = mapping;
    g_readerTable = AsTable(view);
    return true;
}

void ReleaseHotkeyPolicyMappingForCurrentProcess() {
    if (g_readerTable != nullptr) {
        UnmapViewOfFile(g_readerTable);
        g_readerTable = nullptr;
    }
    if (g_readerMapping != nullptr) {
        CloseHandle(g_readerMapping);
        g_readerMapping = nullptr;
    }
}

bool LoadHotkeyPolicyForCurrentProcess(HotkeyPolicy& policy) {
    if (!RetainHotkeyPolicyMappingForCurrentProcess()) {
        return false;
    }

    const auto* table = g_readerTable;
    bool found = false;
    HotkeyPolicy loaded;
    const DWORD processId = GetCurrentProcessId();
    ULONGLONG processCreationTime = 0;
    if (!GetCreationTime(GetCurrentProcess(), processCreationTime)) {
        return false;
    }
    if (IsTableValid(*table)) {
        for (std::size_t index = 0; index < table->entryCount; ++index) {
            const WirePolicyEntry& entry = table->entries[index];
            if (entry.valid == 0 || entry.processId != processId ||
                entry.processCreationTime != processCreationTime ||
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
            found = true;
            break;
        }
    }

    if (found) {
        policy = std::move(loaded);
    }
    return found;
}
