#pragma once

#include "HotkeyPolicy.h"

#include <windows.h>

#include <mutex>
#include <string>

class HotkeyPolicyRegistry final {
public:
    HotkeyPolicyRegistry() = default;
    ~HotkeyPolicyRegistry();

    HotkeyPolicyRegistry(const HotkeyPolicyRegistry&) = delete;
    HotkeyPolicyRegistry& operator=(const HotkeyPolicyRegistry&) = delete;

    bool Start(std::wstring& error);
    void Stop();
    bool Publish(DWORD processId, const HotkeyPolicy& policy, std::wstring& error);
    void Remove(DWORD processId);

private:
    mutable std::mutex m_mutex;
    HANDLE m_mapping = nullptr;
    void* m_view = nullptr;
};

bool LoadHotkeyPolicyForCurrentProcess(HotkeyPolicy& policy);
