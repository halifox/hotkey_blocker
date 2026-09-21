#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

class Logger final {
public:
    Logger();
    explicit Logger(std::filesystem::path logPath);

    void Info(std::wstring_view message);
    void Error(std::wstring_view message);

private:
    void Write(std::wstring_view level, std::wstring_view message);
    static std::filesystem::path DefaultPath();
    static std::string ToUtf8(std::wstring_view text);
    static std::wstring Timestamp();
    void RotateIfNeeded();

    mutable std::mutex m_mutex;
    std::filesystem::path m_path;
};
