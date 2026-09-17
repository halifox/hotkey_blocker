#pragma once

#include <string>

class StartupManager final {
public:
    bool SetEnabled(bool enabled, std::wstring& error) const;

private:
    static std::wstring ExecutablePath();
};
