#pragma once

#include "InjectionStatus.h"

#include <windows.h>

#include <memory>
#include <string>

struct RemoteInjectionResult {
    InjectionStatus status = InjectionStatus::Failed;
    std::wstring error;
    std::shared_ptr<InjectionOperation> pendingOperation;
};

enum class InjectorHelperExitCode : DWORD {
    Succeeded = 0,
    Failed = 1,
    InvalidArguments = 2,
};

RemoteInjectionResult InjectDllIntoProcess(DWORD pid, const std::wstring& dllPath,
                                           bool waitForCompletion = false);
