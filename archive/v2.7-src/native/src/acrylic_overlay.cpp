/**
 * Acrylic Overlay — Zero-flicker frosted glass without DWM injection
 * ====================================================================
 * Inserts a WS_EX_LAYERED acrylic window directly BELOW the target in
 * z-order. DWM handles the blur natively.
 *
 * No DWM hooking, no PDB symbols, no DWMBlurGlass dependency.
 * Works on Win10 1803+ and Win11.
 *
 * Usage: acrylic_overlay.exe <hwnd_hex> [tint_hex]
 *        tint_hex: gradient color (0x00RRGGBB), default 0x00000000 (pure blur)
 *                 0x80FFFFFF = 50% white (standard acrylic)
 *                 0x00000000 = pure blur, no tint (clearest)
 */

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdlib>

#ifndef DWMWA_NCRENDERING_POLICY
#define DWMWA_NCRENDERING_POLICY 2
#define DWMNCRP_ENABLED 2
#endif

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000
#endif

// Undocumented SetWindowCompositionAttribute
struct AccentPolicy {
    int AccentState;
    int AccentFlags;
    unsigned int GradientColor;  // ARGB
    int AnimationId;
};
struct WinCompAttrData {
    int Attribute;       // WCA_ACCENT_POLICY = 19
    void* Data;
    int SizeOfData;
};
enum AccentState {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};

typedef BOOL (WINAPI *SetWindowCompositionAttribute_t)(HWND, WinCompAttrData*);

// Globals — loaded once, used across main loop
static SetWindowCompositionAttribute_t g_setAccent = nullptr;
static HMODULE g_dwm = nullptr;
static decltype(&DwmGetWindowAttribute) g_pDwmGetWindowAttr = nullptr;
static decltype(&DwmEnableBlurBehindWindow) g_pDwmEnableBlurBehind = nullptr;
static HWND g_target = nullptr;
static HWND g_overlay = nullptr;
static HWINEVENTHOOK g_hook = nullptr;
static bool g_running = true;
static bool g_isWin11 = false;

// Triple-call z-order fix.
// Call 1: instant (synchronous), Call 2: 200ms, Call 3: 600ms.
// Catches late reordering by taskbar animations and DWM compositor queue.
#define ZORDER_GAP_MS 200
#define ZORDER_GAP_MS2 600
static DWORD g_secondCallAt = 0;
static DWORD g_thirdCallAt = 0;

// ---- Helpers ----

bool detect_win11() {
    // RtlGetVersion is not affected by manifest compatibility shims
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto RtlGetVersion = (LONG (WINAPI*)(PRTL_OSVERSIONINFOW))
        GetProcAddress(ntdll, "RtlGetVersion");
    if (!RtlGetVersion) return false;
    RTL_OSVERSIONINFOW vi = {sizeof(vi)};
    if (RtlGetVersion(&vi) != 0) return false;
    return vi.dwBuildNumber >= 22000;  // Win11 starts at build 22000
}

void do_set_window_pos() {
    SetWindowPos(g_overlay, g_target, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);  // synchronous
}

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                           LONG, LONG, DWORD, DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd == g_target && g_overlay) {
        do_set_window_pos();
        g_secondCallAt = GetTickCount() + ZORDER_GAP_MS;
        g_thirdCallAt = GetTickCount() + ZORDER_GAP_MS2;
    }
}

void apply_accent(HWND hwnd, int state, unsigned int tint = 0x00000000) {
    if (!g_setAccent) {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        g_setAccent = (SetWindowCompositionAttribute_t)
            GetProcAddress(u32, "SetWindowCompositionAttribute");
        if (!g_setAccent) return;
    }
    AccentPolicy policy = {};
    policy.AccentState = state;
    policy.GradientColor = tint;
    WinCompAttrData data = {19, &policy, sizeof(policy)};  // 19 = WCA_ACCENT_POLICY
    g_setAccent(hwnd, &data);
}

void disable_blur_behind(HWND hwnd) {
    if (!g_pDwmEnableBlurBehind) return;
    DWM_BLURBEHIND bb = {};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = FALSE;
    g_pDwmEnableBlurBehind(hwnd, &bb);
}

RECT get_frame(HWND hwnd) {
    RECT r = {};
    if (!GetWindowRect(hwnd, &r)) {
        // Window may be in a transitional state — return empty rect
        return r;
    }

    // Try DwmGetWindowAttribute for extended frame bounds (more accurate)
    if (g_pDwmGetWindowAttr) {
        RECT f = {};
        HRESULT hr = g_pDwmGetWindowAttr(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                          &f, sizeof(f));
        if (SUCCEEDED(hr) && f.right > f.left && f.bottom > f.top) {
            r = f;
        }
    }
    return r;
}

bool is_valid_frame(const RECT& r) {
    LONG w = r.right - r.left;
    LONG h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;
    // Sanity check: overlay shouldn't exceed screen bounds unreasonably
    LONG sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    LONG sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w > sw * 2 || h > sh * 2) return false;
    return true;
}

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_running = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Main ----

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: acrylic_overlay.exe <hwnd_hex> [tint_hex]\n");
        fprintf(stderr, "  tint: 0x00000000 = pure blur  0x80FFFFFF = 50%% white\n");
        return 1;
    }
    g_target = (HWND)(ULONG_PTR)strtoull(argv[1], nullptr, 16);
    if (!IsWindow(g_target)) {
        fprintf(stderr, "[x] Invalid HWND\n");
        return 1;
    }
    unsigned int tint = 0x00000000;  // default: pure blur, no tint
    if (argc >= 3) {
        tint = (unsigned int)strtoull(argv[2], nullptr, 16);
    }

    // ---- Load DLLs once ----
    g_dwm = LoadLibraryW(L"dwmapi.dll");
    if (g_dwm) {
        g_pDwmGetWindowAttr = (decltype(&DwmGetWindowAttribute))
            GetProcAddress(g_dwm, "DwmGetWindowAttribute");
        g_pDwmEnableBlurBehind = (decltype(&DwmEnableBlurBehindWindow))
            GetProcAddress(g_dwm, "DwmEnableBlurBehindWindow");
    }
    g_isWin11 = detect_win11();

    // ---- Create overlay window ----
    const wchar_t* CN = L"AcrylicOverlayClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = CN;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);  // Don't paint!
    RegisterClassW(&wc);

    RECT r = get_frame(g_target);
    if (!is_valid_frame(r)) {
        // Fallback: try GetWindowRect directly
        if (GetWindowRect(g_target, &r) && !is_valid_frame(r)) {
            fprintf(stderr, "[x] Cannot determine target window frame\n");
            if (g_dwm) FreeLibrary(g_dwm);
            return 1;
        }
    }

    LONG ow = r.right - r.left;
    LONG oh = r.bottom - r.top;
    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CN, L"", WS_POPUP,
        r.left, r.top, ow, oh,
        nullptr, nullptr, GetModuleHandle(nullptr), nullptr
    );

    if (!g_overlay) {
        fprintf(stderr, "[x] Failed to create overlay (err=%lu)\n", GetLastError());
        if (g_dwm) FreeLibrary(g_dwm);
        return 1;
    }

    // ---- Apply blur ----
    // 1. Enable DWM frame extension into client area
    int policy = DWMNCRP_ENABLED;
    DwmSetWindowAttribute(g_overlay, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));

    // 2. On Win11, ensure the window does NOT use redirection surface
    //    (WS_EX_NOREDIRECTIONBITMAP alone may be insufficient on Win11 24H2+)
    if (g_isWin11 && g_pDwmEnableBlurBehind) {
        // Explicitly disable blur-behind first (prevents DWM from
        // applying blur to the entire redirection surface)
        disable_blur_behind(g_overlay);
    }

    // 3. Apply acrylic blur via undocumented API
    apply_accent(g_overlay, ACCENT_ENABLE_ACRYLICBLURBEHIND, tint);

    // 4. Position overlay BELOW target in z-order
    SetWindowPos(g_overlay, g_target, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    wchar_t title[256];
    GetWindowTextW(g_target, title, 256);
    wprintf(L"[*] Acrylic Overlay active\n");
    wprintf(L"    Target: %s\n", title);
    wprintf(L"    Overlay: %dx%d at (%d,%d)\n", ow, oh, r.left, r.top);
    wprintf(L"    OS: %s\n", g_isWin11 ? L"Windows 11" : L"Windows 10");
    wprintf(L"    Tint: 0x%08X\n", tint);
    fflush(stdout);

    // Register WinEvent hook: refresh z-order when target regains focus
    g_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, WinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // ---- Main loop ----
    MSG msg;
    RECT lastRect = r;
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { g_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        // Deferred z-order calls
        if (g_secondCallAt && GetTickCount() >= g_secondCallAt) {
            g_secondCallAt = 0;
            if (g_overlay && IsWindow(g_target)) do_set_window_pos();
        }
        if (g_thirdCallAt && GetTickCount() >= g_thirdCallAt) {
            g_thirdCallAt = 0;
            if (g_overlay && IsWindow(g_target)) do_set_window_pos();
        }

        if (!IsWindow(g_target)) {
            wprintf(L"[!] Target destroyed, exiting\n");
            break;
        }

        r = get_frame(g_target);
        if (is_valid_frame(r) &&
            (r.left != lastRect.left || r.top != lastRect.top ||
             r.right != lastRect.right || r.bottom != lastRect.bottom)) {
            lastRect = r;
            SetWindowPos(g_overlay, g_target,
                r.left, r.top, r.right - r.left, r.bottom - r.top,
                SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOZORDER);
        }

        Sleep(5);  // ~180 fps — match 180Hz display
    }

    // ---- Cleanup ----
    if (g_hook) UnhookWinEvent(g_hook);

    // Disable all blur state before destroying window (critical for Win11)
    apply_accent(g_overlay, ACCENT_DISABLED);
    if (g_pDwmEnableBlurBehind) {
        disable_blur_behind(g_overlay);
    }
    // Let DWM finish processing the state change
    DwmFlush();
    Sleep(50);

    DestroyWindow(g_overlay);
    if (g_dwm) FreeLibrary(g_dwm);
    wprintf(L"[*] Overlay destroyed\n");
    return 0;
}
