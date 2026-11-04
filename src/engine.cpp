#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tuki {

namespace {

constexpr int kSign(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }

// Time window in which consecutive notches count as a "fast spin" for acceleration.
constexpr uint32_t kAccelWindowMs = 150;

int takeWhole(double& value) {
    // Keep the fractional part for the next event so slow speeds (0.25x) still scroll.
    double whole = std::trunc(value);
    value -= whole;
    whole = std::clamp(whole, -32000.0, 32000.0);
    return static_cast<int>(whole);
}

}  // namespace

void sanitize(Config& cfg) {
    cfg.scrollSpeed = std::clamp(cfg.scrollSpeed, 0.25, 5.0);
    cfg.scrollAccelStrength = std::clamp(cfg.scrollAccelStrength, 0.25, 3.0);
    cfg.wheelJitterMs = std::clamp<uint32_t>(cfg.wheelJitterMs, 20, 300);
    cfg.debounceMs = std::clamp<uint32_t>(cfg.debounceMs, 10, 250);
    // Remapping the primary buttons could lock the user out of the mouse.
    cfg.buttonActions[static_cast<size_t>(Button::Left)] = Action::Default;
    cfg.buttonActions[static_cast<size_t>(Button::Right)] = Action::Default;
    for (auto& a : cfg.buttonActions) {
        if (static_cast<uint8_t>(a) >= static_cast<uint8_t>(Action::Count)) a = Action::Default;
    }
}

int applyScrollMode(ScrollMode mode, int delta) {
    switch (mode) {
        case ScrollMode::Normal: return delta;
        case ScrollMode::Inverted: return -delta;
        case ScrollMode::AlwaysDown: return -std::abs(delta);
        case ScrollMode::AlwaysUp: return std::abs(delta);
    }
    return delta;
}

WheelDecision processWheel(const Config& cfg, WheelState& st, int rawDelta,
                           bool horizontalIn, uint32_t timeMs, bool shiftDown) {
    WheelDecision out;
    if (rawDelta == 0) return out;

    if (horizontalIn) {
        double v = rawDelta * (cfg.invertHorizontal ? -1.0 : 1.0) * cfg.scrollSpeed;
        if ((v > 0 && st.hRemainder < 0) || (v < 0 && st.hRemainder > 0)) st.hRemainder = 0.0;
        st.hRemainder += v;
        int d = takeWhole(st.hRemainder);
        if (d == rawDelta) return out;  // nothing changed: let the original through
        out.block = true;
        if (d != 0) {
            out.inject = true;
            out.horizontal = true;
            out.delta = d;
        }
        return out;
    }

    const int sign = kSign(rawDelta);
    const uint32_t dt = st.hasLast ? timeMs - st.lastTime : UINT32_MAX;

    // Worn encoders emit a stray notch in the opposite direction mid-spin.
    if (cfg.wheelJitterFilter && st.hasLast && sign != st.lastSign && dt < cfg.wheelJitterMs) {
        out.block = true;
        out.dropped = true;
        return out;
    }
    st.hasLast = true;
    st.lastSign = sign;
    st.lastTime = timeMs;

    const int directed = applyScrollMode(cfg.scrollMode, rawDelta);

    double accel = 1.0;
    if (cfg.scrollAccel && dt < kAccelWindowMs) {
        const double fast = static_cast<double>(kAccelWindowMs - dt) / kAccelWindowMs;  // 0..1
        accel = 1.0 + 2.0 * cfg.scrollAccelStrength * fast;
    }

    double v = directed * cfg.scrollSpeed * accel;
    if ((v > 0 && st.remainder < 0) || (v < 0 && st.remainder > 0)) st.remainder = 0.0;
    st.remainder += v;
    int d = takeWhole(st.remainder);

    const bool toHorizontal = cfg.shiftHorizontal && shiftDown;
    if (toHorizontal) d = -d;  // wheel towards you (negative) scrolls right (positive)

    if (!toHorizontal && d == rawDelta) return out;  // pass through untouched

    out.block = true;
    if (d != 0) {
        out.inject = true;
        out.horizontal = toHorizontal;
        out.delta = d;
    }
    return out;
}

bool isClickAction(Action a) {
    switch (a) {
        case Action::LeftClick:
        case Action::RightClick:
        case Action::MiddleClick:
        case Action::Back:
        case Action::Forward: return true;
        default: return false;
    }
}

Button clickActionButton(Action a) {
    switch (a) {
        case Action::LeftClick: return Button::Left;
        case Action::RightClick: return Button::Right;
        case Action::MiddleClick: return Button::Middle;
        case Action::Back: return Button::X1;
        case Action::Forward: return Button::X2;
        default: return Button::Left;
    }
}

ButtonDecision processButton(const Config& cfg, ButtonState& st, Button b, bool down,
                             uint32_t timeMs) {
    const size_t i = static_cast<size_t>(b);
    ButtonDecision d;

    if (!down) {
        st.hasUp[i] = true;
        st.lastUp[i] = timeMs;
        d = st.pendingUp[i];
        st.pendingUp[i] = ButtonDecision{};
        return d;
    }

    // Ghost click: a new press too soon after the last release.
    if (cfg.debounce[i] && st.hasUp[i] && timeMs - st.lastUp[i] < cfg.debounceMs) {
        d.block = true;
        d.debounced = true;
        st.pendingUp[i] = ButtonDecision{true};
        return d;
    }

    if (cfg.swapLeftRight && (b == Button::Left || b == Button::Right)) {
        const Button other = b == Button::Left ? Button::Right : Button::Left;
        d.block = true;
        d.inject = ButtonDecision::Inject::ButtonDown;
        d.button = other;
        ButtonDecision up;
        up.block = true;
        up.inject = ButtonDecision::Inject::ButtonUp;
        up.button = other;
        st.pendingUp[i] = up;
        return d;
    }

    const Action a = cfg.buttonActions[i];
    if (b == Button::Left || b == Button::Right || a == Action::Default) {
        st.pendingUp[i] = ButtonDecision{};
        return d;  // pass through
    }

    d.block = true;
    if (a == Action::Disabled) {
        st.pendingUp[i] = ButtonDecision{true};
        return d;
    }

    if (isClickAction(a)) {
        const Button target = clickActionButton(a);
        if (target == b) {  // mapped to itself: just pass it along
            st.pendingUp[i] = ButtonDecision{};
            return ButtonDecision{};
        }
        d.inject = ButtonDecision::Inject::ButtonDown;
        d.button = target;
        ButtonDecision up;
        up.block = true;
        up.inject = ButtonDecision::Inject::ButtonUp;
        up.button = target;
        st.pendingUp[i] = up;
        return d;
    }

    // Keyboard / system actions fire once on press; the release is swallowed.
    d.inject = ButtonDecision::Inject::Action;
    d.action = a;
    st.pendingUp[i] = ButtonDecision{true};
    return d;
}

// ---------------------------------------------------------------- names

namespace {
struct ActionEntry { Action action; const char* name; };
constexpr ActionEntry kActions[] = {
    {Action::Default, "default"},       {Action::Disabled, "disabled"},
    {Action::LeftClick, "leftClick"},   {Action::RightClick, "rightClick"},
    {Action::MiddleClick, "middleClick"}, {Action::Back, "back"},
    {Action::Forward, "forward"},       {Action::DoubleClick, "doubleClick"},
    {Action::Copy, "copy"},             {Action::Paste, "paste"},
    {Action::Undo, "undo"},             {Action::CloseTab, "closeTab"},
    {Action::PlayPause, "playPause"},   {Action::NextTrack, "nextTrack"},
    {Action::PrevTrack, "prevTrack"},   {Action::VolumeUp, "volumeUp"},
    {Action::VolumeDown, "volumeDown"}, {Action::Mute, "mute"},
    {Action::ShowDesktop, "showDesktop"}, {Action::TaskView, "taskView"},
    {Action::Screenshot, "screenshot"},
};
}  // namespace

const char* scrollModeName(ScrollMode m) {
    switch (m) {
        case ScrollMode::Normal: return "normal";
        case ScrollMode::Inverted: return "inverted";
        case ScrollMode::AlwaysDown: return "down";
        case ScrollMode::AlwaysUp: return "up";
    }
    return "normal";
}

ScrollMode scrollModeFromName(const std::string& s) {
    if (s == "inverted") return ScrollMode::Inverted;
    if (s == "down") return ScrollMode::AlwaysDown;
    if (s == "up") return ScrollMode::AlwaysUp;
    return ScrollMode::Normal;
}

const char* actionName(Action a) {
    for (const auto& e : kActions)
        if (e.action == a) return e.name;
    return "default";
}

Action actionFromName(const std::string& s) {
    for (const auto& e : kActions)
        if (s == e.name) return e.action;
    return Action::Default;
}

const char* buttonName(Button b) {
    switch (b) {
        case Button::Left: return "left";
        case Button::Right: return "right";
        case Button::Middle: return "middle";
        case Button::X1: return "x1";
        case Button::X2: return "x2";
        default: return "left";
    }
}

}  // namespace tuki
