// Thin wrappers over Windows settings and process queries.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace tuki::sys {

struct WindowApp {
    std::wstring exe;    // lower-case file name, e.g. "chrome.exe"
    std::wstring title;  // title of one of its visible windows
};

std::wstring exePath();
std::wstring exeDir();
std::wstring processExeName(DWORD pid);           // lower-case file name or empty
std::vector<WindowApp> runningWindowApps();

bool setStartWithWindows(bool on);
bool startWithWindowsEnabled();

struct PointerSettings {
    int speed = 10;              // 1..20, Windows default 10
    bool enhancePrecision = true;
};
PointerSettings getPointer();
bool setPointer(const PointerSettings& p);

bool isElevated();
bool systemUsesDarkTheme();

}  // namespace tuki::sys
