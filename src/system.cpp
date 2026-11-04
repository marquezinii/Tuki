#include "system.h"

#include <dwmapi.h>
#include <shlwapi.h>

#include <algorithm>

#include "config.h"

namespace tuki::sys {

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Tuki";
}  // namespace

std::wstring exePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring exeDir() {
    std::wstring p = exePath();
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::wstring processExeName(DWORD pid) {
    if (pid == 0) return {};
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buf));
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, buf, &size)) name = toLower(PathFindFileNameW(buf));
    CloseHandle(h);
    return name;
}

std::vector<WindowApp> runningWindowApps() {
    std::vector<WindowApp> apps;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* list = reinterpret_cast<std::vector<WindowApp>*>(lp);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
            BOOL cloaked = FALSE;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (cloaked) return TRUE;
            wchar_t title[256];
            if (GetWindowTextW(hwnd, title, 256) == 0) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            std::wstring exe = processExeName(pid);
            if (exe.empty() || exe == L"applicationframehost.exe" || exe == L"textinputhost.exe") return TRUE;
            auto same = [&](const WindowApp& a) { return a.exe == exe; };
            if (std::none_of(list->begin(), list->end(), same)) list->push_back({exe, title});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&apps));
    std::sort(apps.begin(), apps.end(), [](const WindowApp& a, const WindowApp& b) { return a.exe < b.exe; });
    return apps;
}

bool setStartWithWindows(bool on) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return false;
    LSTATUS st;
    if (on) {
        std::wstring cmd = L"\"" + exePath() + L"\" --tray";
        st = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kRunValue);
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

bool startWithWindowsEnabled() {
    DWORD type = 0, size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, &type, nullptr, &size) == ERROR_SUCCESS;
}

PointerSettings getPointer() {
    PointerSettings p;
    int speed = 10;
    if (SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0)) p.speed = speed;
    int accel[3] = {};
    if (SystemParametersInfoW(SPI_GETMOUSE, 0, accel, 0)) p.enhancePrecision = accel[2] != 0;
    return p;
}

bool setPointer(const PointerSettings& p) {
    const int speed = std::clamp(p.speed, 1, 20);
    int accel[3] = {0, 0, 0};
    if (p.enhancePrecision) {
        accel[0] = 6;  // Windows defaults when "Enhance pointer precision" is on
        accel[1] = 10;
        accel[2] = 1;
    }
    const UINT flags = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    BOOL ok = SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(speed)), flags);
    ok = SystemParametersInfoW(SPI_SETMOUSE, 0, accel, flags) && ok;
    return ok != FALSE;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    bool elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) &&
                    elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

bool systemUsesDarkTheme() {
    DWORD value = 1, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return false;
    return value == 0;
}

}  // namespace tuki::sys
