// Tuki: pure decision logic for mouse events.
// No Win32 calls here, so everything in this file can be unit-tested.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tuki {

enum class ScrollMode : uint8_t { Normal, Inverted, AlwaysDown, AlwaysUp };

enum class Button : uint8_t { Left = 0, Right, Middle, X1, X2, Count };
constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);

enum class Action : uint8_t {
    Default,      // keep the button's own behaviour
    Disabled,     // swallow the button
    LeftClick,
    RightClick,
    MiddleClick,
    Back,         // sends mouse X1
    Forward,      // sends mouse X2
    DoubleClick,
    Copy,
    Paste,
    Undo,
    CloseTab,
    PlayPause,
    NextTrack,
    PrevTrack,
    VolumeUp,
    VolumeDown,
    Mute,
    ShowDesktop,
    TaskView,
    Screenshot,
    Count
};

struct Config {
    bool enabled = true;

    // Scroll
    ScrollMode scrollMode = ScrollMode::Normal;
    double scrollSpeed = 1.0;          // 0.25 .. 5
    bool scrollAccel = false;
    double scrollAccelStrength = 1.0;  // 0.25 .. 3
    bool shiftHorizontal = false;
    bool invertHorizontal = false;
    bool wheelJitterFilter = false;
    uint32_t wheelJitterMs = 80;       // 20 .. 300

    // Buttons (only Middle, X1 and X2 can be remapped)
    bool swapLeftRight = false;
    std::array<Action, kButtonCount> buttonActions{};

    // Click debounce ("ghost double click" filter)
    std::array<bool, kButtonCount> debounce{};
    uint32_t debounceMs = 60;          // 10 .. 250

    // Apps where Tuki steps aside (lower-case exe names)
    std::vector<std::wstring> excludedApps;

    // General
    bool startWithWindows = false;
    bool hotkeyEnabled = true;
};

// Clamp every numeric field into its valid range and drop remaps on Left/Right.
void sanitize(Config& cfg);

// ---------------------------------------------------------------- wheel

struct WheelState {
    bool hasLast = false;
    int lastSign = 0;
    uint32_t lastTime = 0;
    double remainder = 0.0;   // fractional vertical delta carried to the next notch
    double hRemainder = 0.0;  // same for horizontal
};

struct WheelDecision {
    bool block = false;       // swallow the original event
    bool inject = false;      // send a replacement event
    bool horizontal = false;  // replacement is WM_MOUSEHWHEEL
    int delta = 0;            // replacement delta (WHEEL_DELTA units)
    bool dropped = false;     // removed by the jitter filter
};

// rawDelta: signed delta from the hardware (positive = wheel pushed away / up).
// horizontalIn: event was a WM_MOUSEHWHEEL.
// timeMs: event timestamp (wraps, compared with unsigned subtraction).
WheelDecision processWheel(const Config& cfg, WheelState& st, int rawDelta,
                           bool horizontalIn, uint32_t timeMs, bool shiftDown);

// Direction transform only (used by the UI preview and tests).
int applyScrollMode(ScrollMode mode, int delta);

// ---------------------------------------------------------------- buttons

struct ButtonDecision {
    enum class Inject : uint8_t { None, ButtonDown, ButtonUp, Action };
    bool block = false;
    Inject inject = Inject::None;
    Button button = Button::Left;   // for ButtonDown / ButtonUp
    Action action = Action::Default; // for Action
    bool debounced = false;          // blocked as a ghost click
};

struct ButtonState {
    std::array<bool, kButtonCount> hasUp{};
    std::array<uint32_t, kButtonCount> lastUp{};
    // What to do with the matching "up" event, decided when "down" arrived,
    // so a config change mid-press never leaves a button stuck.
    std::array<ButtonDecision, kButtonCount> pendingUp{};
};

ButtonDecision processButton(const Config& cfg, ButtonState& st, Button b,
                             bool down, uint32_t timeMs);

// True for actions that behave like a held mouse button (down on press, up on release).
bool isClickAction(Action a);
Button clickActionButton(Action a);

// ---------------------------------------------------------------- names

const char* scrollModeName(ScrollMode m);
ScrollMode scrollModeFromName(const std::string& s);
const char* actionName(Action a);
Action actionFromName(const std::string& s);
const char* buttonName(Button b);

}  // namespace tuki
