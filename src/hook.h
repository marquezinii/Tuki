// Global low-level mouse hook running on its own high-priority thread.
#pragma once

#include <windows.h>

#include <string>

#include "config.h"

namespace tuki {

// Posted to the notify window (if any) for the live preview in the UI.
// WM_TUKI_WHEEL: wParam = MAKEWPARAM(raw delta, output delta), lParam = WheelFlag bits.
// WM_TUKI_BUTTON: wParam = Button index, lParam = ButtonFlag bits | (mapped index + 1) << 8.
constexpr UINT WM_TUKI_WHEEL = WM_APP + 40;
constexpr UINT WM_TUKI_BUTTON = WM_APP + 41;

enum WheelFlag : LPARAM {
    kWheelHorizontalIn = 1,
    kWheelHorizontalOut = 2,
    kWheelDropped = 4,
    kWheelInactive = 8,
};

enum ButtonFlag : LPARAM {
    kButtonDown = 1,
    kButtonBlocked = 2,
    kButtonDebounced = 4,
    kButtonInactive = 8,
    kButtonAction = 16,  // mapped to a keyboard/system action (bits 8+ hold the Action)
};

namespace hook {

bool start(const Config& cfg, const Stats& initial);
void stop();

void setConfig(const Config& cfg);
void setPaused(bool paused);
bool paused();

void setNotifyWindow(HWND hwnd);

Stats stats();
void resetStats();

}  // namespace hook
}  // namespace tuki
