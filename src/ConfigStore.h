#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct AppRule {
    std::wstring path;
    bool enabled = true;
};

struct AppConfig {
    int version = 1;
    bool autoStart = false;
    std::vector<AppRule> apps;
};

class ConfigStore final {
public:
    static constexpr int kCurrentVersion = 1;

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
