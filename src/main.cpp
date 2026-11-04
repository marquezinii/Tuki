// Tuki: universal mouse tweaks for Windows.
// Tray app that owns the global mouse hook and opens the settings window on demand.
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <string>

#include "config.h"
#include "hook.h"
#include "resource.h"
#include "system.h"
#include "window.h"

using nlohmann::json;
using namespace tuki;

namespace {

constexpr wchar_t kHostClass[] = L"TukiHost";
constexpr wchar_t kMutexName[] = L"Local\\Tuki.SingleInstance";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_TUKI_SHOW = WM_APP + 2;
constexpr int kHotkeyId = 1;
constexpr UINT_PTR kSaveTimer = 1;
constexpr UINT kSaveIntervalMs = 60'000;
constexpr wchar_t kHotkeyLabel[] = L"Ctrl+Alt+F12";

enum MenuId : UINT {
    kMenuOpen = 100,
    kMenuModeNormal = 110,
    kMenuModeInverted,
    kMenuModeDown,
    kMenuModeUp,
    kMenuPause = 120,
    kMenuExit = 199,
};

HINSTANCE g_inst = nullptr;
HWND g_host = nullptr;
UINT g_taskbarCreated = 0;
HICON g_iconOn = nullptr;
HICON g_iconPaused = nullptr;
Config g_config;
bool g_hotkeyRegistered = false;
bool g_trayAdded = false;

// ---------------------------------------------------------------- tray

const wchar_t* modeLabel(ScrollMode m) {
    switch (m) {
        case ScrollMode::Normal: return L"rolagem normal";
        case ScrollMode::Inverted: return L"rolagem invertida";
        case ScrollMode::AlwaysDown: return L"rolagem sempre para baixo";
        case ScrollMode::AlwaysUp: return L"rolagem sempre para cima";
    }
    return L"";
}

bool effectivelyPaused() { return hook::paused() || !g_config.enabled; }

void fillTrayData(NOTIFYICONDATAW& nid) {
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_host;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = effectivelyPaused() ? g_iconPaused : g_iconOn;
    std::wstring tip = effectivelyPaused() ? L"Tuki pausado" : std::wstring(L"Tuki: ") + modeLabel(g_config.scrollMode);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
}

void addTray() {
    NOTIFYICONDATAW nid{};
    fillTrayData(nid);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void updateTray() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    fillTrayData(nid);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void trayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    fillTrayData(nid);
    nid.uFlags |= NIF_INFO;
    nid.dwInfoFlags = NIIF_USER | NIIF_NOSOUND | NIIF_LARGE_ICON;
    nid.hBalloonIcon = nid.hIcon;
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void removeTray() {
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_host;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// ---------------------------------------------------------------- state

json stateJson() {
    const auto pointer = sys::getPointer();
    return {
        {"type", "state"},
        {"config", configToJson(g_config)},
        {"stats", statsToJson(hook::stats())},
        {"paused", hook::paused()},
        {"pointer", {{"speed", pointer.speed}, {"enhancePrecision", pointer.enhancePrecision}}},
        {"version", kVersion},
        {"elevated", sys::isElevated()},
        {"hotkey", toUtf8(kHotkeyLabel)},
        {"hotkeyAvailable", !g_config.hotkeyEnabled || g_hotkeyRegistered},
        {"configPath", toUtf8(configPath())},
    };
}

void syncHotkey() {
    if (g_config.hotkeyEnabled && !g_hotkeyRegistered) {
        g_hotkeyRegistered = RegisterHotKey(g_host, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12) != FALSE;
    } else if (!g_config.hotkeyEnabled && g_hotkeyRegistered) {
        UnregisterHotKey(g_host, kHotkeyId);
        g_hotkeyRegistered = false;
    }
}

void applyConfig(Config next, bool fromUi) {
    sanitize(next);
    const bool startupChanged = next.startWithWindows != g_config.startWithWindows;
    g_config = std::move(next);
    hook::setConfig(g_config);
    saveConfig(g_config);
    if (startupChanged) sys::setStartWithWindows(g_config.startWithWindows);
    syncHotkey();
    updateTray();
    if (!fromUi) ui::send(stateJson());
}

void setPaused(bool paused, bool announce) {
    hook::setPaused(paused);
    updateTray();
    ui::send({{"type", "paused"}, {"value", paused}});
    if (announce)
        trayBalloon(paused ? L"Tuki pausado" : L"Tuki ativo",
                    paused ? L"O mouse voltou ao comportamento padrão do Windows."
                           : L"Seus ajustes de mouse estão valendo de novo.");
}

void onUiMessage(const json& msg) {
    const std::string type = msg.value("type", "");
    if (type == "ready") {
        ui::send(stateJson());
    } else if (type == "setConfig" && msg.contains("config")) {
        applyConfig(configFromJson(msg["config"], g_config), true);
        ui::send({{"type", "saved"}, {"hotkeyAvailable", !g_config.hotkeyEnabled || g_hotkeyRegistered}});
    } else if (type == "setPaused") {
        setPaused(msg.value("value", false), false);
    } else if (type == "listApps") {
        json apps = json::array();
        for (const auto& a : sys::runningWindowApps())
            apps.push_back({{"exe", toUtf8(a.exe)}, {"title", toUtf8(a.title)}});
        ui::send({{"type", "apps"}, {"apps", apps}});
    } else if (type == "getStats") {
        ui::send({{"type", "stats"}, {"stats", statsToJson(hook::stats())}});
    } else if (type == "resetStats") {
        hook::resetStats();
        saveStats(hook::stats());
        ui::send({{"type", "stats"}, {"stats", statsToJson(hook::stats())}});
    } else if (type == "setPointer") {
        sys::PointerSettings p = sys::getPointer();
        p.speed = msg.value("speed", p.speed);
        p.enhancePrecision = msg.value("enhancePrecision", p.enhancePrecision);
        sys::setPointer(p);
        const auto now = sys::getPointer();
        ui::send({{"type", "pointer"}, {"speed", now.speed}, {"enhancePrecision", now.enhancePrecision}});
    } else if (type == "openConfigFolder") {
        ShellExecuteW(nullptr, L"open", appDataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---------------------------------------------------------------- menu

void showTrayMenu(POINT pt) {
    HMENU menu = CreatePopupMenu();
    HMENU modes = CreatePopupMenu();
    AppendMenuW(modes, MF_STRING, kMenuModeNormal, L"Normal");
    AppendMenuW(modes, MF_STRING, kMenuModeInverted, L"Invertida");
    AppendMenuW(modes, MF_STRING, kMenuModeDown, L"Sempre para baixo");
    AppendMenuW(modes, MF_STRING, kMenuModeUp, L"Sempre para cima");
    CheckMenuRadioItem(modes, kMenuModeNormal, kMenuModeUp,
                       kMenuModeNormal + static_cast<UINT>(g_config.scrollMode), MF_BYCOMMAND);

    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Abrir Tuki");
    SetMenuDefaultItem(menu, kMenuOpen, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(modes), L"Direção da rolagem");
    std::wstring pauseLabel = std::wstring(L"Pausar Tuki\t") + kHotkeyLabel;
    AppendMenuW(menu, MF_STRING | (hook::paused() ? MF_CHECKED : 0), kMenuPause, pauseLabel.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Sair");

    SetForegroundWindow(g_host);
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, g_host, nullptr);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);  // also destroys the submenu

    switch (cmd) {
        case kMenuOpen: ui::show(); break;
        case kMenuModeNormal:
        case kMenuModeInverted:
        case kMenuModeDown:
        case kMenuModeUp: {
            Config next = g_config;
            next.scrollMode = static_cast<ScrollMode>(cmd - kMenuModeNormal);
            applyConfig(next, false);
            break;
        }
        case kMenuPause: setPaused(!hook::paused(), false); break;
        case kMenuExit: DestroyWindow(g_host); break;
        default: break;
    }
}

// ---------------------------------------------------------------- host window

LRESULT CALLBACK hostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {  // Explorer restarted
        g_trayAdded = false;
        addTray();
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                    ui::show();
                    break;
                case WM_CONTEXTMENU:
                    showTrayMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
                    break;
            }
            return 0;
        case WM_TUKI_SHOW:
            ui::show();
            return 0;
        case WM_HOTKEY:
            if (wp == kHotkeyId) setPaused(!hook::paused(), true);
            return 0;
        case WM_TIMER:
            if (wp == kSaveTimer) saveStats(hook::stats());
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wp) saveStats(hook::stats());
            return 0;
        case WM_DESTROY:
            ui::close();
            KillTimer(hwnd, kSaveTimer);
            if (g_hotkeyRegistered) UnregisterHotKey(hwnd, kHotkeyId);
            removeTray();
            hook::stop();
            saveStats(hook::stats());
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HICON loadSmallIcon(int id) {
    return static_cast<HICON>(LoadImageW(g_inst, MAKEINTRESOURCEW(id), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), 0));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    const bool startInTray = cmdLine && wcsstr(cmdLine, L"--tray") != nullptr;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startInTray) {
            if (HWND other = FindWindowW(kHostClass, nullptr)) PostMessageW(other, WM_TUKI_SHOW, 0, 0);
        }
        return 0;
    }

    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_config = loadConfig();
    // The registry is the source of truth for autostart (the user may change it in Task Manager).
    g_config.startWithWindows = sys::startWithWindowsEnabled();
    if (g_config.startWithWindows) sys::setStartWithWindows(true);  // refresh path if the exe moved

    if (!hook::start(g_config, loadStats())) {
        MessageBoxW(nullptr, L"Não foi possível instalar o gancho de mouse do Windows.", L"Tuki", MB_ICONERROR);
        return 1;
    }

    g_iconOn = loadSmallIcon(IDI_TUKI);
    g_iconPaused = loadSmallIcon(IDI_TUKI_PAUSED);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = hostProc;
    wc.hInstance = inst;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so the tray menu can take focus.
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, kHostClass, L"Tuki", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_host, g_taskbarCreated, MSGFLT_ALLOW, nullptr);

    ui::init(inst, onUiMessage);
    addTray();
    syncHotkey();
    SetTimer(g_host, kSaveTimer, kSaveIntervalMs, nullptr);

    if (!startInTray) ui::show();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
