#pragma once

#include <string>

class StartupManager final {
public:
    bool GetEnabled(bool& enabled, std::wstring& error) const;
    bool SetEnabled(bool enabled, std::wstring& error) const;
};
