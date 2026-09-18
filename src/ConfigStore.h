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
    int version = 2;
    std::vector<AppRule> apps;
};

class ConfigStore final {
public:
    static constexpr int kCurrentVersion = 2;

    ConfigStore();
    explicit ConfigStore(std::filesystem::path path);

    const std::filesystem::path& Path() const noexcept;

    // A missing file is treated as an empty configuration.
    bool Load(AppConfig& config, std::wstring& error) const;
    bool Save(const AppConfig& config, std::wstring& error) const;

private:
    static std::filesystem::path DefaultPath();

    std::filesystem::path m_path;
};
