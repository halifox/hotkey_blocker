#include <windows.h>

#include <string>

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
                 bool afterSelected) {
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
        "\nafter_selected=" + std::to_string(afterSelected ? 1 : 0) + "\n";
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

    const bool baselineAllowed = TryRegister(MOD_NOREPEAT, VK_F24, kAllowedHotKeyId);
    const bool baselineSelected =
        TryRegister(MOD_NOREPEAT | MOD_CONTROL | MOD_ALT, VK_F24, kSelectedHotKeyId);
    SetEvent(ready);

    const DWORD waitResult = WaitForSingleObject(release, 15000);
    if (waitResult != WAIT_OBJECT_0) {
        CloseHandle(ready);
        CloseHandle(release);
        return 4;
    }

    const bool afterAllowed = TryRegister(MOD_NOREPEAT, VK_F24, kAllowedHotKeyId);
    const bool afterSelected =
        TryRegister(MOD_NOREPEAT | MOD_CONTROL | MOD_ALT, VK_F24, kSelectedHotKeyId);

    const bool baselineSucceeded = baselineAllowed && baselineSelected;
    const bool blocked = blacklistMode ? !afterSelected : !afterAllowed && !afterSelected;
    const bool expectedAfter = blacklistMode ? afterAllowed && !afterSelected
                                             : !afterAllowed && !afterSelected;
    const bool success = WriteResult(argv[3], baselineSucceeded, blocked, baselineAllowed,
                                     afterAllowed, baselineSelected, afterSelected) &&
                         baselineSucceeded && expectedAfter;
    CloseHandle(ready);
    CloseHandle(release);
    return success ? 0 : 5;
}
