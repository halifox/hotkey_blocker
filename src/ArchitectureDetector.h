#pragma once

#include <windows.h>

#include <string>

enum class ProcessArchitecture {
    X86,
    X64,
    Unknown,
};

struct ArchitectureResult {
    ProcessArchitecture architecture = ProcessArchitecture::Unknown;
    std::wstring error;
};

ArchitectureResult DetectProcessArchitecture(DWORD pid);
const wchar_t* ArchitectureName(ProcessArchitecture architecture);
