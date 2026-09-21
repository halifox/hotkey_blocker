#pragma once

#include "HotkeyPolicy.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

enum class RuleKind {
    Executable,
    Directory,
};

namespace ConfigSchema {
inline constexpr int kLegacyVersion = 1;
inline constexpr int kCurrentVersion = 2;
}  // namespace ConfigSchema

struct AppRule {
    AppRule() = default;
    AppRule(std::wstring rulePath, bool ruleEnabled = true)
        : path(std::move(rulePath)), enabled(ruleEnabled) {}

    std::wstring path;
    bool enabled = true;
    std::wstring displayName;
    RuleKind kind = RuleKind::Executable;
    HotkeyPolicy hotkeyPolicy;
};

struct AppConfig {
    int version = ConfigSchema::kCurrentVersion;
    std::vector<AppRule> apps;
};

class ConfigStore final {
public:
    ConfigStore();
    explicit ConfigStore(std::filesystem::path path);

    // A missing file is treated as an empty configuration.
    bool Load(AppConfig& config, std::wstring& error) const;
    bool Save(const AppConfig& config, std::wstring& error) const;

private:
    static std::filesystem::path DefaultPath();

    std::filesystem::path m_path;
};
