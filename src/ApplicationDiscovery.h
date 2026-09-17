#pragma once

#include "ConfigStore.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

struct DiscoveredApplication {
    std::wstring id;
    std::wstring displayName;
    std::wstring path;
    std::wstring publisher;
    std::wstring version;
    std::wstring installLocation;
    AppSource source = AppSource::Installed;
};

struct DiscoveryStats {
    std::size_t fixedExecutableEntries = 0;
    std::size_t unresolvedEntries = 0;
    std::size_t filteredEntries = 0;
    std::size_t applications = 0;
};

namespace ApplicationDiscovery {

std::vector<DiscoveredApplication> Scan(
    std::wstring& warning, const std::atomic_bool* cancellation = nullptr,
    DiscoveryStats* stats = nullptr);

}  // namespace ApplicationDiscovery
