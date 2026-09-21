#pragma once

#include "HotkeyPolicy.h"
#include "ProcessIdentity.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <cstdint>
#include <string>

class HotkeyPolicyRegistry final {
public:
    HotkeyPolicyRegistry() = default;
    ~HotkeyPolicyRegistry();

    HotkeyPolicyRegistry(const HotkeyPolicyRegistry&) = delete;
    HotkeyPolicyRegistry& operator=(const HotkeyPolicyRegistry&) = delete;

    bool Start(std::wstring& error);
    void Stop();
    bool Publish(const ProcessIdentity& process, const HotkeyPolicy& policy,
                 std::wstring& error);
    void Remove(const ProcessIdentity& process);
    std::shared_ptr<void> RetainMappingLifetime(std::wstring& error) const;

private:
    mutable std::mutex m_mutex;
    HANDLE m_mapping = nullptr;
    void* m_view = nullptr;
    std::uint64_t m_ownerToken = 0;
    ULONGLONG m_ownerCreationTime = 0;
};

bool LoadHotkeyPolicyForCurrentProcess(HotkeyPolicy& policy);
bool RetainHotkeyPolicyMappingForCurrentProcess();
void ReleaseHotkeyPolicyMappingForCurrentProcess();
