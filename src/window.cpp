#include "window.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>

#include <algorithm>
#include <string>

#include "WebView2.h"
#include "config.h"
#include "hook.h"
#include "resource.h"
#include "system.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using nlohmann::json;

namespace tuki::ui {

namespace {

constexpr wchar_t kClass[] = L"TukiSettings";
constexpr wchar_t kHost[] = L"tuki.local";
constexpr wchar_t kStartUrl[] = L"https://tuki.local/index.html";
constexpr int kWidthDip = 1040;
constexpr int kHeightDip = 720;
constexpr int kMinWidthDip = 760;
constexpr int kMinHeightDip = 560;

// Must match --surface-0 in ui/styles.css so there is no white flash on open.
constexpr COLORREF kLightBase = RGB(0xF4, 0xF5, 0xF2);
constexpr COLORREF kDarkBase = RGB(0x1A, 0x1C, 0x1F);

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
MessageHandler g_onMessage;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
bool g_pageReady = false;

int scale(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }

void applyFrameTheme(HWND hwnd) {
    const BOOL dark = sys::systemUsesDarkTheme() ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = dark ? kDarkBase : kLightBase;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    if (g_controller) {
        ComPtr<ICoreWebView2Controller2> c2;
        if (SUCCEEDED(g_controller.As(&c2))) {
            COREWEBVIEW2_COLOR bg{255, GetRValue(caption), GetGValue(caption), GetBValue(caption)};
            c2->put_DefaultBackgroundColor(bg);
        }
    }
}

void resizeWebView() {
    if (!g_controller || !g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

void reportMissingRuntime() {
    const int choice = MessageBoxW(g_hwnd,
                                   L"O Tuki precisa do Microsoft Edge WebView2 Runtime para mostrar as configurações.\n\n"
                                   L"O ajuste do mouse continua funcionando. Deseja abrir a página de download?",
                                   L"Tuki", MB_YESNO | MB_ICONINFORMATION);
    if (choice == IDYES)
        ShellExecuteW(nullptr, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr,
                      SW_SHOWNORMAL);
    if (g_hwnd) DestroyWindow(g_hwnd);
}

HRESULT onControllerCreated(HWND hwnd, HRESULT result, ICoreWebView2Controller* controller) {
    if (hwnd != g_hwnd) return S_OK;  // window was closed while WebView2 was starting
    if (FAILED(result) || !controller) {
        reportMissingRuntime();
        return S_OK;
    }
    g_controller = controller;
    g_controller->get_CoreWebView2(&g_webview);
    applyFrameTheme(hwnd);

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(g_webview->get_Settings(&settings))) {
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
#ifdef NDEBUG
        settings->put_AreDevToolsEnabled(FALSE);
        ComPtr<ICoreWebView2Settings3> s3;
        if (SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
#endif
    }

    ComPtr<ICoreWebView2_3> wv3;
    if (SUCCEEDED(g_webview.As(&wv3))) {
        const std::wstring uiDir = sys::exeDir() + L"\\ui";
        wv3->SetVirtualHostNameToFolderMapping(kHost, uiDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
    }

    EventRegistrationToken token;
    g_webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
                json msg = json::parse(toUtf8(raw), nullptr, false);
                CoTaskMemFree(raw);
                if (msg.is_object() && g_onMessage) {
                    if (msg.value("type", "") == "ready") g_pageReady = true;
                    g_onMessage(msg);
                }
                return S_OK;
            })
            .Get(),
        &token);

    // The UI never navigates; keep it on the bundled pages only.
    g_webview->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri) {
                    if (std::wstring(uri).rfind(L"https://tuki.local/", 0) != 0) args->put_Cancel(TRUE);
                    CoTaskMemFree(uri);
                }
                return S_OK;
            })
            .Get(),
        &token);

    g_webview->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                args->put_Handled(TRUE);
                return S_OK;
            })
            .Get(),
        &token);

    resizeWebView();
    g_webview->Navigate(kStartUrl);
    return S_OK;
}

void createWebView(HWND hwnd) {
    auto onController = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            return onControllerCreated(hwnd, result, controller);
        });

    if (g_env) {
        g_env->CreateCoreWebView2Controller(hwnd, onController.Get());
        return;
    }
    const std::wstring dataDir = localDataDir() + L"\\WebView2";
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, onController](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (hwnd != g_hwnd) return S_OK;
                if (FAILED(result) || !env) {
                    reportMissingRuntime();
                    return S_OK;
                }
                g_env = env;
                g_env->CreateCoreWebView2Controller(hwnd, onController.Get());
                return S_OK;
            })
            .Get());
    if (FAILED(hr)) reportMissingRuntime();
}

json wheelToJson(WPARAM wp, LPARAM lp) {
    return {{"type", "wheel"},
            {"raw", static_cast<short>(LOWORD(wp))},
            {"out", static_cast<short>(HIWORD(wp))},
            {"hIn", (lp & kWheelHorizontalIn) != 0},
            {"hOut", (lp & kWheelHorizontalOut) != 0},
            {"dropped", (lp & kWheelDropped) != 0},
            {"inactive", (lp & kWheelInactive) != 0}};
}

json buttonToJson(WPARAM wp, LPARAM lp) {
    json j = {{"type", "button"},
              {"button", buttonName(static_cast<Button>(wp))},
              {"down", (lp & kButtonDown) != 0},
              {"blocked", (lp & kButtonBlocked) != 0},
              {"debounced", (lp & kButtonDebounced) != 0},
              {"inactive", (lp & kButtonInactive) != 0}};
    const int extra = static_cast<int>((lp >> 8) & 0xFF);
    if (lp & kButtonAction) {
        j["action"] = actionName(static_cast<Action>(extra));
    } else if (extra > 0) {
        j["mappedTo"] = buttonName(static_cast<Button>(extra - 1));
    }
    return j;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            resizeWebView();
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            const UINT dpi = GetDpiForWindow(hwnd);
            mm->ptMinTrackSize = {scale(kMinWidthDip, dpi), scale(kMinHeightDip, dpi)};
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) applyFrameTheme(hwnd);
            break;
        case WM_MOVE:
        case WM_MOVING:
            if (g_controller) g_controller->NotifyParentWindowPositionChanged();
            break;
        case WM_TUKI_WHEEL:
            send(wheelToJson(wp, lp));
            return 0;
        case WM_TUKI_BUTTON:
            send(buttonToJson(wp, lp));
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            hook::setNotifyWindow(nullptr);
            if (g_controller) g_controller->Close();
            g_webview.Reset();
            g_controller.Reset();
            g_pageReady = false;
            g_hwnd = nullptr;
            // Hand the WebView2 memory back: the tray app stays tiny while the window is closed.
            SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void init(HINSTANCE inst, MessageHandler onMessage) {
    g_inst = inst;
    g_onMessage = std::move(onMessage);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_TUKI), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_TUKI), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hbrBackground = CreateSolidBrush(sys::systemUsesDarkTheme() ? kDarkBase : kLightBase);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
}

void show() {
    if (g_hwnd) {
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        return;
    }
    const UINT dpi = GetDpiForSystem();
    MONITORINFO mi{sizeof(mi)};
    POINT cursor;
    GetCursorPos(&cursor);
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& wa = mi.rcWork;
    const int w = std::min<int>(scale(kWidthDip, dpi), wa.right - wa.left);
    const int h = std::min<int>(scale(kHeightDip, dpi), wa.bottom - wa.top);
    const int x = wa.left + (wa.right - wa.left - w) / 2;
    const int y = wa.top + (wa.bottom - wa.top - h) / 2;

    g_hwnd = CreateWindowExW(0, kClass, L"Tuki", WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!g_hwnd) return;
    applyFrameTheme(g_hwnd);
    hook::setNotifyWindow(g_hwnd);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    createWebView(g_hwnd);
}

void close() {
    if (g_hwnd) DestroyWindow(g_hwnd);
}

bool isOpen() { return g_hwnd != nullptr; }

void send(const json& msg) {
    if (!g_webview || !g_pageReady) return;
    g_webview->PostWebMessageAsJson(fromUtf8(msg.dump()).c_str());
}

}  // namespace tuki::ui
