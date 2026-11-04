// End-to-end check against a running Tuki.exe.
// Opens a small window under the cursor, injects real mouse input with SendInput and
// prints what the window actually receives (after Tuki's hook has processed it).
//
// Expected Tuki config for this test: scroll mode "down", middle button "disabled".
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <vector>

namespace {

std::vector<int> g_wheel;
int g_middleDowns = 0;
int g_leftDowns = 0;

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_MOUSEWHEEL: g_wheel.push_back(GET_WHEEL_DELTA_WPARAM(wp)); return 0;
        case WM_MBUTTONDOWN: ++g_middleDowns; return 0;
        case WM_LBUTTONDOWN: ++g_leftDowns; return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

void pump(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    MSG msg;
    while (std::chrono::steady_clock::now() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    }
}

void send(DWORD flags, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof(in));
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TukiE2E";
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
    RegisterClassW(&wc);

    POINT pt;
    GetCursorPos(&pt);
    HWND w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"tuki-e2e", WS_POPUP | WS_VISIBLE,
                             pt.x - 150, pt.y - 150, 300, 300, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(w);
    pump(300);

    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    };

    // Wheel pushed away (up): Tuki must turn it into a downward scroll.
    g_wheel.clear();
    auto t0 = std::chrono::steady_clock::now();
    send(MOUSEEVENTF_WHEEL, static_cast<DWORD>(120));
    while (g_wheel.empty() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1)) pump(1);
    const double latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    pump(100);
    std::printf("wheel up   -> received:");
    for (int d : g_wheel) std::printf(" %d", d);
    std::printf("   (%.1f ms)\n", latencyMs);
    expect(g_wheel.size() == 1 && g_wheel[0] == -120, "wheel up arrives as one notch down");

    // Wheel pulled (down): stays down.
    g_wheel.clear();
    send(MOUSEEVENTF_WHEEL, static_cast<DWORD>(-120));
    pump(250);
    std::printf("wheel down -> received:");
    for (int d : g_wheel) std::printf(" %d", d);
    std::printf("\n");
    expect(g_wheel.size() == 1 && g_wheel[0] == -120, "wheel down arrives unchanged");

    // A fast burst of ten "up" notches: all must come out as "down".
    g_wheel.clear();
    for (int i = 0; i < 10; ++i) send(MOUSEEVENTF_WHEEL, static_cast<DWORD>(120));
    pump(400);
    int total = 0;
    bool allDown = !g_wheel.empty();
    for (int d : g_wheel) {
        total += d;
        allDown = allDown && d < 0;
    }
    std::printf("burst x10  -> %zu events, total %d\n", g_wheel.size(), total);
    expect(allDown && total == -1200, "burst of up notches all scroll down");

    // Middle button is disabled in the test config.
    send(MOUSEEVENTF_MIDDLEDOWN);
    send(MOUSEEVENTF_MIDDLEUP);
    pump(250);
    expect(g_middleDowns == 0, "middle click is swallowed");

    // Left button is untouched.
    send(MOUSEEVENTF_LEFTDOWN);
    send(MOUSEEVENTF_LEFTUP);
    pump(250);
    expect(g_leftDowns == 1, "left click passes through");

    DestroyWindow(w);
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
