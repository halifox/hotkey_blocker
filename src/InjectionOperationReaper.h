#pragma once

#include "InjectionStatus.h"

#include <memory>

class InjectionOperationReaper final {
public:
    static void Retain(std::shared_ptr<InjectionOperation> operation,
                       std::shared_ptr<void> policyMappingLifetime = {});
};
