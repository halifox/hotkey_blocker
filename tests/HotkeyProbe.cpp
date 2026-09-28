#include <windows.h>

#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kAllowedHotKeyId = 0x7ffe;
constexpr int kSelectedHotKeyId = 0x7fff;

bool TryRegister(UINT modifiers, UINT virtualKey, int identifier) {
    const BOOL result = RegisterHotKey(nullptr, identifier, modifiers, virtualKey);
    if (result) {
        UnregisterHotKey(nullptr, identifier);
    }
    return result == TRUE;
}

bool WriteResult(const std::wstring& path, bool baselineSucceeded, bool blocked,
                 bool baselineAllowed, bool afterAllowed, bool baselineSelected,
                 bool afterSelected, bool earlySurvived) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::string text =
        "baseline=" + std::to_string(baselineSucceeded ? 1 : 0) +
        "\nblocked=" + std::to_string(blocked ? 1 : 0) +
        "\nbaseline_allowed=" + std::to_string(baselineAllowed ? 1 : 0) +
        "\nafter_allowed=" + std::to_string(afterAllowed ? 1 : 0) +
        "\nbaseline_selected=" + std::to_string(baselineSelected ? 1 : 0) +
        "\nafter_selected=" + std::to_string(afterSelected ? 1 : 0) +
        "\nearly_survived=" + std::to_string(earlySurvived ? 1 : 0) + "\n";
    DWORD written = 0;
    const bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                                   nullptr) &&
                         written == text.size();
    CloseHandle(file);
    return success;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4 && argc != 5) {
        return 2;
    }
    const bool blacklistMode = argc == 5 && _wcsicmp(argv[4], L"--blacklist") == 0;
    if (argc == 5 && !blacklistMode) {
        return 2;
    }

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[1]);
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
    if (ready == nullptr || release == nullptr) {
        if (ready != nullptr) {
            CloseHandle(ready);
        }
        if (release != nullptr) {
            CloseHandle(release);
        }
        return 3;
    }

    // Register immediately at startup and keep ownership across hook installation.
    const bool earlyRegistered = RegisterHotKey(nullptr, 200,
        MOD_NOREPEAT | MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F19) == TRUE;
    const bool baselineAllowed = TryRegister(MOD_NOREPEAT, VK_F24, kAllowedHotKeyId);
    const bool baselineSelected =
        TryRegister(MOD_NOREPEAT | MOD_CONTROL | MOD_ALT, VK_F24, kSelectedHotKeyId);
    // Keep other target threads inside RegisterHotKey while the hook is installed.
    std::vector<std::jthread> callers;
    for (UINT index = 0; index < 4; ++index) {
        callers.emplace_back([index](std::stop_token stop) {
            while (!stop.stop_requested()) {
                TryRegister(MOD_NOREPEAT | MOD_SHIFT, VK_F20 + index, 100 + index);
                SwitchToThread();
            }
        });
    }
    SetEvent(ready);

    const DWORD waitResult = WaitForSingleObject(release, 15000);
    for (auto& caller : callers) caller.request_stop();
    callers.clear();
    if (waitResult != WAIT_OBJECT_0) {
        CloseHandle(ready);
        CloseHandle(release);
        return 4;
    }

    const bool afterAllowed = TryRegister(MOD_NOREPEAT, VK_F24, kAllowedHotKeyId);
    const bool afterSelected =
        TryRegister(MOD_NOREPEAT | MOD_CONTROL | MOD_ALT, VK_F24, kSelectedHotKeyId);
    const bool earlySurvived = UnregisterHotKey(nullptr, 200) == TRUE;

    const bool baselineSucceeded = baselineAllowed && baselineSelected;
    const bool blocked = blacklistMode ? !afterSelected : !afterAllowed && !afterSelected;
    const bool expectedAfter = blacklistMode ? afterAllowed && !afterSelected
                                             : !afterAllowed && !afterSelected;
    const bool success = WriteResult(argv[3], baselineSucceeded, blocked, baselineAllowed,
                                     afterAllowed, baselineSelected, afterSelected, earlySurvived) &&
                         baselineSucceeded && expectedAfter && earlyRegistered && earlySurvived;
    CloseHandle(ready);
    CloseHandle(release);
    return success ? 0 : 5;
}
