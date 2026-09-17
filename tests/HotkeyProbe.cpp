#include <windows.h>

#include <string>

namespace {

constexpr int kHotKeyId = 0x7ffe;

bool WriteResult(const std::wstring& path, bool baselineSucceeded, bool blocked) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const std::string text = "baseline=" + std::to_string(baselineSucceeded ? 1 : 0) +
                             "\nblocked=" + std::to_string(blocked ? 1 : 0) + "\n";
    DWORD written = 0;
    const bool success = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                                   nullptr) &&
                         written == text.size();
    CloseHandle(file);
    return success;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 4) {
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

    const BOOL baseline = RegisterHotKey(nullptr, kHotKeyId, MOD_NOREPEAT, VK_F24);
    if (baseline) {
        UnregisterHotKey(nullptr, kHotKeyId);
    }
    SetEvent(ready);

    const DWORD waitResult = WaitForSingleObject(release, 15000);
    if (waitResult != WAIT_OBJECT_0) {
        CloseHandle(ready);
        CloseHandle(release);
        return 4;
    }

    const BOOL afterInjection = RegisterHotKey(nullptr, kHotKeyId, MOD_NOREPEAT, VK_F24);
    if (afterInjection) {
        UnregisterHotKey(nullptr, kHotKeyId);
    }

    const bool success = WriteResult(argv[3], baseline == TRUE, afterInjection == FALSE);
    CloseHandle(ready);
    CloseHandle(release);
    return success && baseline && !afterInjection ? 0 : 5;
}
