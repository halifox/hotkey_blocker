#pragma once

#include "ConfigStore.h"

#include <atomic>
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

namespace ApplicationDiscovery {

std::vector<DiscoveredApplication> Scan(
    std::wstring& warning, const std::atomic_bool* cancellation = nullptr);

}  // namespace ApplicationDiscovery
