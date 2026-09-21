#pragma once

#include <string>

enum class InjectionStatus {
    Succeeded,
    Failed,
    Pending,
    Indeterminate,
};

struct InjectionCompletion {
    InjectionStatus status = InjectionStatus::Failed;
    std::wstring error;
};

class InjectionOperation {
public:
    virtual ~InjectionOperation() = default;
    // False keeps the operation and any associated resources owned by the caller.
    // An indeterminate completion may include a diagnostic in `completion`.
    virtual bool TryComplete(InjectionCompletion& completion) = 0;
};
