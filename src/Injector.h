#pragma once

#include "ArchitectureDetector.h"
#include "InjectionStatus.h"

#include <filesystem>
#include <memory>
#include <string>

struct InjectionResult {
    InjectionStatus status = InjectionStatus::Failed;
    ProcessArchitecture architecture = ProcessArchitecture::Unknown;
    std::wstring error;
    std::shared_ptr<InjectionOperation> pendingOperation;

    bool IsSuccess() const noexcept {
        return status == InjectionStatus::Succeeded;
    }
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
