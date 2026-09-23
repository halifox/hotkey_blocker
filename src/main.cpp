#include "WindowsTarget.h"

#include <windows.h>

#include <atlbase.h>
#include <atlapp.h>

#include "MainWindow.h"

CAppModule _Module;

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comResult)) {
        return 1;
    }

    HRESULT result = _Module.Init(nullptr, instance);
    if (FAILED(result)) {
        CoUninitialize();
        return static_cast<int>(result);
    }

    HANDLE rawInstanceMutex =
        CreateMutexW(nullptr, TRUE, L"Local\\HotkeyBlocker.SingleInstance");
    if (rawInstanceMutex == nullptr) {
        _Module.Term();
        CoUninitialize();
        return 1;
    }
    const DWORD mutexStatus = GetLastError();
    CHandle instanceMutex(rawInstanceMutex);
    if (mutexStatus == ERROR_ALREADY_EXISTS) {
        _Module.Term();
        CoUninitialize();
        return 0;
    }

    AtlInitCommonControls(ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES);
    CMessageLoop messageLoop;
    _Module.AddMessageLoop(&messageLoop);

    const bool startHidden = commandLine != nullptr &&
                             wcsstr(commandLine, L"--background") != nullptr;
    const int exitCode = RunMainWindow(messageLoop, startHidden);

    _Module.RemoveMessageLoop();
    _Module.Term();
    CoUninitialize();
    return exitCode;
}
