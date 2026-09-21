#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

struct UpdateCheckResult {
    std::wstring currentVersion;
    std::wstring latestVersion;
    std::wstring releaseUrl;
    std::wstring error;
    bool updateAvailable = false;
};

class UpdateChecker final {
public:
    using CompletionCallback = std::function<void(UpdateCheckResult)>;

    UpdateChecker() = default;
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    bool Start(CompletionCallback callback);
    void Stop();

private:
    static UpdateCheckResult CheckLatestRelease();

    std::atomic_bool m_stopRequested = false;
    std::thread m_thread;
};
