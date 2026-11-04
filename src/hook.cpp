#include "hook.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "system.h"

namespace tuki::hook {

namespace {

// Marks events Tuki injects so the hook lets them through untouched.
constexpr ULONG_PTR kTag = 0x54554B49;  // "TUKI"

// ---------------------------------------------------------------- shared state

std::atomic<const Config*> g_cfg{nullptr};
std::mutex g_cfgMx;
std::deque<std::unique_ptr<Config>> g_cfgHistory;  // keeps recent snapshots alive for readers

std::atomic<bool> g_paused{false};
std::atomic<bool> g_fgExcluded{false};
std::atomic<HWND> g_notify{nullptr};

std::atomic<uint64_t> g_clicks{0}, g_notches{0}, g_corrected{0}, g_ghost{0}, g_jitter{0};

// Hook-thread only.
WheelState g_wheel;
ButtonState g_buttons;

std::mutex g_fgMx;
std::wstring g_fgExe;

std::thread g_hookThread;
DWORD g_hookThreadId = 0;

// ---------------------------------------------------------------- injector
// SendInput runs on its own thread so the hook callback returns immediately.

std::mutex g_qMx;
std::condition_variable g_qCv;
std::deque<std::vector<INPUT>> g_queue;
bool g_qStop = false;
std::thread g_injectThread;

void enqueue(std::vector<INPUT> inputs) {
    if (inputs.empty()) return;
    {
        std::lock_guard lock(g_qMx);
        g_queue.push_back(std::move(inputs));
    }
    g_qCv.notify_one();
}

void injectorMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        std::vector<INPUT> batch;
        {
            std::unique_lock lock(g_qMx);
            g_qCv.wait(lock, [] { return g_qStop || !g_queue.empty(); });
            if (g_qStop && g_queue.empty()) return;
            batch = std::move(g_queue.front());
            g_queue.pop_front();
        }
        SendInput(static_cast<UINT>(batch.size()), batch.data(), sizeof(INPUT));
    }
}

INPUT mouseInput(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    in.mi.dwExtraInfo = kTag;
    return in;
}

INPUT buttonInput(Button b, bool down) {
    switch (b) {
        case Button::Left: return mouseInput(down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
        case Button::Right: return mouseInput(down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        case Button::Middle: return mouseInput(down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP);
        case Button::X1: return mouseInput(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON1);
        case Button::X2: return mouseInput(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, XBUTTON2);
        default: return mouseInput(0);
    }
}

bool isExtendedKey(WORD vk) {
    switch (vk) {
        case VK_LWIN: case VK_RWIN:
        case VK_VOLUME_UP: case VK_VOLUME_DOWN: case VK_VOLUME_MUTE:
        case VK_MEDIA_PLAY_PAUSE: case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK:
            return true;
        default:
            return false;
    }
}

INPUT keyInput(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (isExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = kTag;
    return in;
}

std::vector<INPUT> combo(std::initializer_list<WORD> keys) {
    std::vector<INPUT> v;
    for (WORD k : keys) v.push_back(keyInput(k, false));
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) v.push_back(keyInput(*it, true));
    return v;
}

std::vector<INPUT> actionInputs(Action a) {
    switch (a) {
        case Action::DoubleClick:
            return {buttonInput(Button::Left, true), buttonInput(Button::Left, false),
                    buttonInput(Button::Left, true), buttonInput(Button::Left, false)};
        case Action::Copy: return combo({VK_CONTROL, 'C'});
        case Action::Paste: return combo({VK_CONTROL, 'V'});
        case Action::Undo: return combo({VK_CONTROL, 'Z'});
        case Action::CloseTab: return combo({VK_CONTROL, 'W'});
        case Action::PlayPause: return combo({VK_MEDIA_PLAY_PAUSE});
        case Action::NextTrack: return combo({VK_MEDIA_NEXT_TRACK});
        case Action::PrevTrack: return combo({VK_MEDIA_PREV_TRACK});
        case Action::VolumeUp: return combo({VK_VOLUME_UP});
        case Action::VolumeDown: return combo({VK_VOLUME_DOWN});
        case Action::Mute: return combo({VK_VOLUME_MUTE});
        case Action::ShowDesktop: return combo({VK_LWIN, 'D'});
        case Action::TaskView: return combo({VK_LWIN, VK_TAB});
        case Action::Screenshot: return combo({VK_LWIN, VK_SHIFT, 'S'});
        default: return {};
    }
}

// ---------------------------------------------------------------- foreground app

void recomputeExcluded() {
    const Config* cfg = g_cfg.load(std::memory_order_acquire);
    std::lock_guard lock(g_fgMx);
    bool excluded = false;
    if (cfg && !g_fgExe.empty())
        excluded = std::find(cfg->excludedApps.begin(), cfg->excludedApps.end(), g_fgExe) != cfg->excludedApps.end();
    g_fgExcluded.store(excluded, std::memory_order_relaxed);
}

void updateForeground(HWND hwnd) {
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    std::wstring exe = sys::processExeName(pid);
    {
        std::lock_guard lock(g_fgMx);
        g_fgExe = std::move(exe);
    }
    recomputeExcluded();
}

void CALLBACK foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (idObject == OBJID_WINDOW) updateForeground(hwnd);
}

// ---------------------------------------------------------------- hook callback

void notifyWheel(int raw, int out, LPARAM flags) {
    HWND h = g_notify.load(std::memory_order_relaxed);
    if (!h) return;
    const auto clamp16 = [](int v) { return static_cast<WORD>(static_cast<short>(std::clamp(v, -32000, 32000))); };
    PostMessageW(h, WM_TUKI_WHEEL, MAKEWPARAM(clamp16(raw), clamp16(out)), flags);
}

void notifyButton(Button b, LPARAM flags) {
    HWND h = g_notify.load(std::memory_order_relaxed);
    if (h) PostMessageW(h, WM_TUKI_BUTTON, static_cast<WPARAM>(b), flags);
}

bool decodeButton(WPARAM msg, const MSLLHOOKSTRUCT* ms, Button& b, bool& down) {
    switch (msg) {
        case WM_LBUTTONDOWN: b = Button::Left; down = true; return true;
        case WM_LBUTTONUP: b = Button::Left; down = false; return true;
        case WM_RBUTTONDOWN: b = Button::Right; down = true; return true;
        case WM_RBUTTONUP: b = Button::Right; down = false; return true;
        case WM_MBUTTONDOWN: b = Button::Middle; down = true; return true;
        case WM_MBUTTONUP: b = Button::Middle; down = false; return true;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            b = HIWORD(ms->mouseData) == XBUTTON1 ? Button::X1 : Button::X2;
            down = msg == WM_XBUTTONDOWN;
            return true;
        default: return false;
    }
}

LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
    if ((ms->flags & LLMHF_INJECTED) && ms->dwExtraInfo == kTag) return CallNextHookEx(nullptr, code, wp, lp);

    const Config* cfg = g_cfg.load(std::memory_order_acquire);
    if (!cfg) return CallNextHookEx(nullptr, code, wp, lp);
    const bool active = cfg->enabled && !g_paused.load(std::memory_order_relaxed) &&
                        !g_fgExcluded.load(std::memory_order_relaxed);

    if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) {
        const bool hIn = wp == WM_MOUSEHWHEEL;
        const int raw = static_cast<short>(HIWORD(ms->mouseData));
        g_notches.fetch_add(1, std::memory_order_relaxed);
        if (!active) {
            notifyWheel(raw, raw, (hIn ? kWheelHorizontalIn | kWheelHorizontalOut : 0) | kWheelInactive);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const WheelDecision d = processWheel(*cfg, g_wheel, raw, hIn, ms->time, shift);

        if (d.dropped) {
            g_jitter.fetch_add(1, std::memory_order_relaxed);
        } else if (d.inject && !hIn && (d.horizontal || (d.delta < 0) != (raw < 0))) {
            g_corrected.fetch_add(1, std::memory_order_relaxed);
        }

        LPARAM flags = hIn ? kWheelHorizontalIn : 0;
        const bool hOut = d.block ? d.horizontal : hIn;
        if (hOut) flags |= kWheelHorizontalOut;
        if (d.dropped) flags |= kWheelDropped;
        notifyWheel(raw, d.block ? d.delta : raw, flags);

        if (d.inject)
            enqueue({mouseInput(d.horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL, static_cast<DWORD>(d.delta))});
        if (d.block) return 1;
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    Button b;
    bool down;
    if (decodeButton(wp, ms, b, down)) {
        const size_t i = static_cast<size_t>(b);
        ButtonDecision d;
        if (!active && down) {
            g_buttons.pendingUp[i] = ButtonDecision{};  // release must pass through too
            g_clicks.fetch_add(1, std::memory_order_relaxed);
            notifyButton(b, kButtonDown | kButtonInactive);
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        // Releases always go through the engine so a remapped press is released correctly.
        d = processButton(*cfg, g_buttons, b, down, ms->time);
        if (down && !d.debounced) g_clicks.fetch_add(1, std::memory_order_relaxed);
        if (d.debounced) g_ghost.fetch_add(1, std::memory_order_relaxed);

        LPARAM flags = (down ? kButtonDown : 0) | (d.block ? kButtonBlocked : 0) | (d.debounced ? kButtonDebounced : 0);
        switch (d.inject) {
            case ButtonDecision::Inject::ButtonDown:
            case ButtonDecision::Inject::ButtonUp:
                enqueue({buttonInput(d.button, d.inject == ButtonDecision::Inject::ButtonDown)});
                flags |= (static_cast<LPARAM>(d.button) + 1) << 8;
                break;
            case ButtonDecision::Inject::Action:
                enqueue(actionInputs(d.action));
                flags |= kButtonAction | (static_cast<LPARAM>(d.action) << 8);
                break;
            default: break;
        }
        notifyButton(b, flags);
        if (d.block) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void hookThreadMain(std::promise<bool> ready) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the message queue

    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
    HWINEVENTHOOK fg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, foregroundProc, 0,
                                       0, WINEVENT_OUTOFCONTEXT);
    updateForeground(GetForegroundWindow());
    ready.set_value(hook != nullptr);
    if (!hook) {
        if (fg) UnhookWinEvent(fg);
        return;
    }

    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (fg) UnhookWinEvent(fg);
    UnhookWindowsHookEx(hook);
}

}  // namespace

void setConfig(const Config& cfg) {
    {
        std::lock_guard lock(g_cfgMx);
        g_cfgHistory.push_back(std::make_unique<Config>(cfg));
        g_cfg.store(g_cfgHistory.back().get(), std::memory_order_release);
        // A hook callback holds a snapshot for microseconds; 32 publishes later it is long gone.
        while (g_cfgHistory.size() > 32) g_cfgHistory.pop_front();
    }
    recomputeExcluded();
}

bool start(const Config& cfg, const Stats& initial) {
    setConfig(cfg);
    g_clicks = initial.clicks;
    g_notches = initial.notches;
    g_corrected = initial.corrected;
    g_ghost = initial.ghostClicks;
    g_jitter = initial.jitterDropped;

    g_qStop = false;
    g_injectThread = std::thread(injectorMain);

    std::promise<bool> ready;
    auto ok = ready.get_future();
    g_hookThread = std::thread(hookThreadMain, std::move(ready));
    g_hookThreadId = GetThreadId(g_hookThread.native_handle());
    if (!ok.get()) {
        stop();
        return false;
    }
    return true;
}

void stop() {
    if (g_hookThread.joinable()) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        g_hookThread.join();
    }
    if (g_injectThread.joinable()) {
        {
            std::lock_guard lock(g_qMx);
            g_qStop = true;
        }
        g_qCv.notify_one();
        g_injectThread.join();
    }
}

void setPaused(bool p) { g_paused.store(p, std::memory_order_relaxed); }
bool paused() { return g_paused.load(std::memory_order_relaxed); }
void setNotifyWindow(HWND hwnd) { g_notify.store(hwnd, std::memory_order_relaxed); }

Stats stats() {
    Stats s;
    s.clicks = g_clicks.load();
    s.notches = g_notches.load();
    s.corrected = g_corrected.load();
    s.ghostClicks = g_ghost.load();
    s.jitterDropped = g_jitter.load();
    return s;
}

void resetStats() {
    g_clicks = 0;
    g_notches = 0;
    g_corrected = 0;
    g_ghost = 0;
    g_jitter = 0;
}

}  // namespace tuki::hook
