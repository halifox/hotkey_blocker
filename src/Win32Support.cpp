#include "Win32Support.h"

#include <iterator>
#include <vector>

namespace Win32Support {

std::wstring ErrorMessage(const wchar_t* operation, DWORD errorCode) {
    wchar_t buffer[512]{};
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, errorCode, 0, buffer,
                                        static_cast<DWORD>(std::size(buffer)), nullptr);
    std::wstring result(operation == nullptr ? L"Windows 操作失败" : operation);
    if (length != 0) {
        result += L"：";
        result.append(buffer, length);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
    } else {
        result += L"（错误码 " + std::to_wstring(errorCode) + L"）";
    }
    return result;
}

std::wstring ModulePath() {
    std::vector<wchar_t> buffer(512);
    while (buffer.size() <= 32768) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}

}  // namespace Win32Support
