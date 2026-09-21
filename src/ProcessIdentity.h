#pragma once

#include <windows.h>

struct ProcessIdentity {
    DWORD pid = 0;
    ULONGLONG creationTime = 0;
};
