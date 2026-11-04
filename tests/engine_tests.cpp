// Unit tests for the pure decision logic in src/engine.cpp.
#include <cstdio>
#include <cstdlib>

#include "../src/engine.h"

using namespace tuki;

static int g_failed = 0;
static int g_run = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_run;                                                                 \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

static void test_scroll_modes() {
    CHECK(applyScrollMode(ScrollMode::Normal, 120) == 120);
    CHECK(applyScrollMode(ScrollMode::Normal, -120) == -120);
    CHECK(applyScrollMode(ScrollMode::Inverted, 120) == -120);
    CHECK(applyScrollMode(ScrollMode::AlwaysDown, 120) == -120);
    CHECK(applyScrollMode(ScrollMode::AlwaysDown, -120) == -120);
    CHECK(applyScrollMode(ScrollMode::AlwaysUp, -120) == 120);
    CHECK(applyScrollMode(ScrollMode::AlwaysUp, 120) == 120);
}

static void test_always_down_rewrites_up_notches() {
    Config c;
    c.scrollMode = ScrollMode::AlwaysDown;
    WheelState s;
    auto up = processWheel(c, s, 120, false, 1000, false);
    CHECK(up.block && up.inject && !up.horizontal && up.delta == -120);
    auto down = processWheel(c, s, -120, false, 2000, false);
    CHECK(!down.block && !down.inject);  // already down: untouched
}

static void test_always_up() {
    Config c;
    c.scrollMode = ScrollMode::AlwaysUp;
    WheelState s;
    auto r = processWheel(c, s, -120, false, 10, false);
    CHECK(r.block && r.inject && r.delta == 120);
    auto r2 = processWheel(c, s, 120, false, 500, false);
    CHECK(!r2.block);
}

static void test_normal_passes_through() {
    Config c;
    WheelState s;
    auto r = processWheel(c, s, 120, false, 0, false);
    CHECK(!r.block && !r.inject);
}

static void test_speed_multiplier_and_remainder() {
    Config c;
    c.scrollSpeed = 0.25;
    WheelState s;
    int total = 0;
    for (int i = 0; i < 4; ++i) {
        auto r = processWheel(c, s, 120, false, 1000u * (i + 1), false);
        CHECK(r.block);
        if (r.inject) total += r.delta;
    }
    CHECK(total == 120);  // four quarter-notches add up to one notch

    Config f;
    f.scrollSpeed = 3.0;
    WheelState s2;
    auto r = processWheel(f, s2, -120, false, 0, false);
    CHECK(r.inject && r.delta == -360);
}

static void test_acceleration() {
    Config c;
    c.scrollAccel = true;
    c.scrollAccelStrength = 1.0;
    WheelState s;
    auto first = processWheel(c, s, 120, false, 0, false);
    CHECK(!first.block);  // first notch has no history: unchanged
    auto fast = processWheel(c, s, 120, false, 10, false);
    CHECK(fast.inject && fast.delta > 120);
    auto slow = processWheel(c, s, 120, false, 5000, false);
    CHECK(!slow.block);
}

static void test_jitter_filter() {
    Config c;
    c.wheelJitterFilter = true;
    c.wheelJitterMs = 80;
    WheelState s;
    processWheel(c, s, -120, false, 1000, false);
    auto stray = processWheel(c, s, 120, false, 1030, false);
    CHECK(stray.block && stray.dropped && !stray.inject);
    auto next = processWheel(c, s, -120, false, 1060, false);
    CHECK(!next.block);
    auto realReverse = processWheel(c, s, 120, false, 1500, false);
    CHECK(!realReverse.block);
}

static void test_jitter_filter_with_time_wrap() {
    Config c;
    c.wheelJitterFilter = true;
    WheelState s;
    processWheel(c, s, -120, false, 0xFFFFFFF0u, false);
    auto stray = processWheel(c, s, 120, false, 0x10u, false);  // 32 ms later, across wrap
    CHECK(stray.dropped);
}

static void test_shift_horizontal() {
    Config c;
    c.shiftHorizontal = true;
    WheelState s;
    auto r = processWheel(c, s, -120, false, 0, true);
    CHECK(r.block && r.inject && r.horizontal && r.delta == 120);
    auto noShift = processWheel(c, s, -120, false, 1000, false);
    CHECK(!noShift.block);
}

static void test_shift_horizontal_respects_mode() {
    Config c;
    c.shiftHorizontal = true;
    c.scrollMode = ScrollMode::AlwaysDown;
    WheelState s;
    auto r = processWheel(c, s, 120, false, 0, true);
    CHECK(r.horizontal && r.delta == 120);  // "down" maps to "right"
}

static void test_invert_horizontal() {
    Config c;
    c.invertHorizontal = true;
    WheelState s;
    auto r = processWheel(c, s, 120, true, 0, false);
    CHECK(r.block && r.inject && r.horizontal && r.delta == -120);
    Config n;
    WheelState s2;
    CHECK(!processWheel(n, s2, 120, true, 0, false).block);
}

static void test_button_passthrough() {
    Config c;
    ButtonState s;
    auto d = processButton(c, s, Button::Left, true, 0);
    CHECK(!d.block);
    auto u = processButton(c, s, Button::Left, false, 50);
    CHECK(!u.block);
}

static void test_debounce() {
    Config c;
    c.debounce[static_cast<size_t>(Button::Left)] = true;
    c.debounceMs = 60;
    ButtonState s;
    CHECK(!processButton(c, s, Button::Left, true, 0).block);
    CHECK(!processButton(c, s, Button::Left, false, 80).block);
    auto ghost = processButton(c, s, Button::Left, true, 100);  // 20 ms after release
    CHECK(ghost.block && ghost.debounced);
    CHECK(processButton(c, s, Button::Left, false, 110).block);  // its release is eaten too
    auto real = processButton(c, s, Button::Left, true, 400);
    CHECK(!real.block);
}

static void test_swap_left_right() {
    Config c;
    c.swapLeftRight = true;
    ButtonState s;
    auto d = processButton(c, s, Button::Left, true, 0);
    CHECK(d.block && d.inject == ButtonDecision::Inject::ButtonDown && d.button == Button::Right);
    c.swapLeftRight = false;  // config changes while held: release must still match
    auto u = processButton(c, s, Button::Left, false, 10);
    CHECK(u.block && u.inject == ButtonDecision::Inject::ButtonUp && u.button == Button::Right);
}

static void test_remap_to_key_action() {
    Config c;
    c.buttonActions[static_cast<size_t>(Button::X1)] = Action::Copy;
    ButtonState s;
    auto d = processButton(c, s, Button::X1, true, 0);
    CHECK(d.block && d.inject == ButtonDecision::Inject::Action && d.action == Action::Copy);
    auto u = processButton(c, s, Button::X1, false, 30);
    CHECK(u.block && u.inject == ButtonDecision::Inject::None);
}

static void test_remap_to_click() {
    Config c;
    c.buttonActions[static_cast<size_t>(Button::Middle)] = Action::Back;
    ButtonState s;
    auto d = processButton(c, s, Button::Middle, true, 0);
    CHECK(d.inject == ButtonDecision::Inject::ButtonDown && d.button == Button::X1);
    auto u = processButton(c, s, Button::Middle, false, 30);
    CHECK(u.inject == ButtonDecision::Inject::ButtonUp && u.button == Button::X1);
}

static void test_disabled_button() {
    Config c;
    c.buttonActions[static_cast<size_t>(Button::X2)] = Action::Disabled;
    ButtonState s;
    CHECK(processButton(c, s, Button::X2, true, 0).block);
    CHECK(processButton(c, s, Button::X2, false, 5).block);
}

static void test_sanitize() {
    Config c;
    c.scrollSpeed = 99;
    c.debounceMs = 1;
    c.buttonActions[static_cast<size_t>(Button::Left)] = Action::Disabled;
    sanitize(c);
    CHECK(c.scrollSpeed == 5.0);
    CHECK(c.debounceMs == 10);
    CHECK(c.buttonActions[0] == Action::Default);
}

static void test_names_round_trip() {
    for (int i = 0; i < static_cast<int>(Action::Count); ++i) {
        auto a = static_cast<Action>(i);
        CHECK(actionFromName(actionName(a)) == a);
    }
    CHECK(scrollModeFromName(scrollModeName(ScrollMode::AlwaysDown)) == ScrollMode::AlwaysDown);
    CHECK(scrollModeFromName("garbage") == ScrollMode::Normal);
}

int main() {
    test_scroll_modes();
    test_always_down_rewrites_up_notches();
    test_always_up();
    test_normal_passes_through();
    test_speed_multiplier_and_remainder();
    test_acceleration();
    test_jitter_filter();
    test_jitter_filter_with_time_wrap();
    test_shift_horizontal();
    test_shift_horizontal_respects_mode();
    test_invert_horizontal();
    test_button_passthrough();
    test_debounce();
    test_swap_left_right();
    test_remap_to_key_action();
    test_remap_to_click();
    test_disabled_button();
    test_sanitize();
    test_names_round_trip();
    std::printf("%d checks, %d failed\n", g_run, g_failed);
    return g_failed == 0 ? 0 : 1;
}
