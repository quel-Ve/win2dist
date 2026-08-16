/**
 * win2blur Welcome Demo — native Win32 effect cycling
 * =====================================================
 * Cycles native → transparent → acrylic blur every 2s.
 * Acrylic phase: launches acrylic_overlay.exe behind this window.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>

static HFONT g_hfontTitle = nullptr, g_hfontBody = nullptr, g_hfontBtn = nullptr;
static int g_effectIdx = 0;
static PROCESS_INFORMATION g_overlayPi = {};
#define BTN_ID 1001
#define TIMER_ID 1
#define HIDE_TIMER_ID 2
#define TINT 0x1A000000

// ---- Find acrylic_overlay.exe next to our own exe ----
bool find_overlay(wchar_t* buf, int bufSize) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t* last = wcsrchr(self, L'\\');
    if (last) *(last + 1) = 0;
    wcscpy_s(buf, bufSize, self);
    wcscat_s(buf, bufSize, L"acrylic_overlay.exe");
    return GetFileAttributesW(buf) != INVALID_FILE_ATTRIBUTES;
}

void launch_overlay(HWND hwnd) {
    if (g_overlayPi.hProcess) return;
    wchar_t path[MAX_PATH];
    if (!find_overlay(path, MAX_PATH)) return;
    wchar_t cmd[512];
    wcscpy(cmd, L"\"");
    wcscat(cmd, path);
    wcscat(cmd, L"\" ");
    wchar_t arg[32];
    wsprintfW(arg, L"0x%08X 0x%08X", (DWORD)(ULONG_PTR)hwnd, TINT);
    wcscat(cmd, arg);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &g_overlayPi);
    // Wait for overlay window to materialize (max 2s)
    for (int i = 0; i < 40; i++) {
        if (FindWindowW(L"AcrylicOverlayClass", nullptr)) break;
        Sleep(50);
    }
}

void kill_overlay() {
    if (!g_overlayPi.hProcess) return;
    TerminateProcess(g_overlayPi.hProcess, 0);
    CloseHandle(g_overlayPi.hProcess);
    CloseHandle(g_overlayPi.hThread);
    ZeroMemory(&g_overlayPi, sizeof(g_overlayPi));
}

void apply_effect(HWND hwnd, int idx) {
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (!(exStyle & WS_EX_LAYERED))
        SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);

    if (!g_overlayPi.hProcess)
        launch_overlay(hwnd);  // fallback if pre-warm failed

    HWND ov = FindWindowW(L"AcrylicOverlayClass", nullptr);
    if (ov) {
        if (idx == 2) {
            KillTimer(hwnd, HIDE_TIMER_ID);  // cancel pending hide
            ShowWindow(ov, SW_SHOWNOACTIVATE);
        } else {
            // Delay hide until opacity transition completes
            SetTimer(hwnd, HIDE_TIMER_ID, 150, nullptr);
        }
    }

    int alpha = (idx == 0) ? 255 : (idx == 2 ? 200 : 217);  // native / acrylic / 85%
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
}

const wchar_t* effect_label(int idx) {
    switch (idx) {
        case 0: return L"Native  (100% opaque)";
        case 1: return L"Transparent  (~85%)";
        case 2: return L"Acrylic Blur  (2% black)";
        default: return L"";
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ID, 2000, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == HIDE_TIMER_ID) {
            KillTimer(hwnd, HIDE_TIMER_ID);
            HWND ov = FindWindowW(L"AcrylicOverlayClass", nullptr);
            if (ov) ShowWindow(ov, SW_HIDE);
            return 0;
        }
        g_effectIdx = (g_effectIdx + 1) % 3;
        apply_effect(hwnd, g_effectIdx);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == BTN_ID) {
            kill_overlay();
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right;
        HBRUSH hbr = CreateSolidBrush(0x121212);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
        SetBkMode(hdc, TRANSPARENT);
        bool isNative = (g_effectIdx == 0);

        if (isNative) {
            SelectObject(hdc, g_hfontTitle);
            // Multi-layer glow: 5 layers, wider spread, slow attenuation
            struct { int d; COLORREF color; } glow[] = {
                {6, 0x00112200},  // outermost — barely visible
                {5, 0x001A3300},
                {4, 0x00224400},
                {3, 0x00336600},
                {2, 0x00558800},
                {1, 0x00AAC044},
            };
            for (auto& g : glow) {
                SetTextColor(hdc, g.color);
                RECT r;
                r = {30+g.d, 14+g.d, w+g.d, 56+g.d}; DrawTextW(hdc, L"win2blur", -1, &r, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                r = {30-g.d, 14+g.d, w-g.d, 56+g.d}; DrawTextW(hdc, L"win2blur", -1, &r, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                r = {30+g.d, 14-g.d, w+g.d, 56-g.d}; DrawTextW(hdc, L"win2blur", -1, &r, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                r = {30-g.d, 14-g.d, w-g.d, 56-g.d}; DrawTextW(hdc, L"win2blur", -1, &r, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            }
            SetTextColor(hdc, 0x00FFEEDD);
            RECT tr = {30, 14, w, 56};
            DrawTextW(hdc, L"win2blur", -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        } else {
            SetTextColor(hdc, 0x00F7C34F);
            SelectObject(hdc, g_hfontTitle);
            RECT tr = {30, 14, w, 56};
            DrawTextW(hdc, L"win2blur", -1, &tr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        }

        SetTextColor(hdc, 0x909090);
        SelectObject(hdc, g_hfontBody);
        RECT sr = {30, 60, w-30, 80};
        DrawTextW(hdc, L"Transparency + Acrylic Frosted Glass", -1, &sr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        SetTextColor(hdc, isNative ? 0x00FF8844 : 0x00F7C34F);
        RECT er = {30, 92, w-30, 112};
        DrawTextW(hdc, effect_label(g_effectIdx), -1, &er, DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        SetTextColor(hdc, 0x909090);
        SetTextColor(hdc, isNative ? 0x00CCBBAA : 0x00B0B0B0);
        SelectObject(hdc, g_hfontBody);
        struct { const wchar_t* header; const wchar_t* keys[4]; const wchar_t* descs[4]; } sections[] = {
            {L"Shortcuts",
             {L"ALT+LEFT / RIGHT", L"ALT+UP", L"ALT+DOWN", nullptr},
             {L"Transparency step", L"Toggle on/off", L"Acrylic blur cycle", nullptr}},
            {L"Tray Menu (right-click icon)",
             {L"Settings", L"Restore && Exit", L"Keep && Exit", nullptr},
             {L"Change transparency step", L"Restore all windows, then exit", L"Keep effects, resume on next launch", nullptr}},
        };
        int y = 128;
        int colX = 30, descX = 220;
        for (auto& sec : sections) {
            SetTextColor(hdc, isNative ? 0x00FFEEDD : 0x00E0E0E0);
            RECT hr = {colX, y, w-colX, y+26};
            DrawTextW(hdc, sec.header, -1, &hr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            y += 26;
            SetTextColor(hdc, 0x00A0A0A0);
            for (int i = 0; sec.keys[i]; i++) {
                RECT kr = {colX, y, descX-10, y+22};
                DrawTextW(hdc, sec.keys[i], -1, &kr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                RECT dr = {descX, y, w-colX, y+22};
                DrawTextW(hdc, sec.descs[i], -1, &dr, DT_LEFT|DT_VCENTER|DT_SINGLELINE);
                y += 22;
            }
            y += 8; // spacer between sections
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, 0x0A0A0A);
        SetDCBrushColor(hdc, 0x00F7C34F);
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    case WM_DESTROY:
        kill_overlay();
        KillTimer(hwnd, TIMER_ID);
        KillTimer(hwnd, HIDE_TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    g_hfontTitle = CreateFontW(36,0,0,0,FW_BOLD,0,0,0,0,0,0,0,0,L"Segoe UI");
    g_hfontBody  = CreateFontW(17,0,0,0,FW_NORMAL,0,0,0,0,0,0,0,0,L"Segoe UI");
    g_hfontBtn   = CreateFontW(16,0,0,0,FW_SEMIBOLD,0,0,0,0,0,0,0,0,L"Segoe UI");

    const wchar_t* CN = L"win2blurWelcome";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = CN;
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc)) return 1;

    int W = 440, H = 460;
    int x = (GetSystemMetrics(SM_CXSCREEN)-W)/2, y = (GetSystemMetrics(SM_CYSCREEN)-H)/2;
    HWND hwnd = CreateWindowExW(WS_EX_LAYERED, CN, L"win2blur — Welcome",
        WS_OVERLAPPEDWINDOW & ~(WS_MINIMIZEBOX|WS_MAXIMIZEBOX),
        x, y, W, H, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    HWND hBtn = CreateWindowExW(0, L"BUTTON", L"Got it",
        WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_FLAT,
        (W-120)/2, H-120, 120, 36, hwnd, (HMENU)BTN_ID, hInst, nullptr);
    if (hBtn) SendMessageW(hBtn, WM_SETFONT, (WPARAM)g_hfontBtn, TRUE);

    apply_effect(hwnd, 0);
    launch_overlay(hwnd);  // pre-warm: overlay ready before first acrylic phase
    ShowWindow(hwnd, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DeleteObject(g_hfontTitle); DeleteObject(g_hfontBody); DeleteObject(g_hfontBtn);
    return 0;
}
