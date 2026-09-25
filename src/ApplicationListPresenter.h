#pragma once

#include <vector>

#include "ApplicationListView.h"
#include "BlockerService.h"
#include "ConfigStore.h"

class ApplicationListPresenter final {
public:
    std::vector<ApplicationListRow> BuildRows(
        const std::vector<AppRule>& rules,
        const std::vector<RuntimeRuleState>& runtimeStates) const;
};
