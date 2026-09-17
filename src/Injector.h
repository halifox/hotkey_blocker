#pragma once

#include "ArchitectureDetector.h"

#include <filesystem>
#include <string>

struct InjectionResult {
    bool success = false;
    ProcessArchitecture architecture = ProcessArchitecture::Unknown;
    std::wstring error;
};

class Injector final {
public:
    Injector();
    explicit Injector(std::filesystem::path executableDirectory);

    InjectionResult Inject(DWORD pid) const;

private:
    InjectionResult InjectWith32BitHelper(DWORD pid, const std::filesystem::path& dllPath) const;
    static std::wstring QuoteCommandLineArgument(const std::wstring& argument);
    static std::filesystem::path ExecutableDirectory();

    std::filesystem::path m_executableDirectory;
};
