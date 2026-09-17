#pragma once

#include <windows.h>

using RegisterHotKeyFunction = BOOL(WINAPI*)(HWND, int, UINT, UINT);

bool InstallRegisterHotKeyHook();
void RemoveRegisterHotKeyHook();
