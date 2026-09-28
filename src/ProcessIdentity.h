#pragma once

#include <windows.h>

struct ProcessIdentity {
    DWORD pid = 0;
    ULONGLONG creationTime = 0;
};

inline bool QueryProcessIdentity(HANDLE process, ProcessIdentity& identity) noexcept {
    FILETIME creation{}, exit{}, kernel{}, user{};
    const DWORD pid = GetProcessId(process);
    if (pid == 0 || !GetProcessTimes(process, &creation, &exit, &kernel, &user)) return false;
    ULARGE_INTEGER value{};
    value.LowPart = creation.dwLowDateTime;
    value.HighPart = creation.dwHighDateTime;
    identity = {pid, value.QuadPart};
    return identity.creationTime != 0;
}
