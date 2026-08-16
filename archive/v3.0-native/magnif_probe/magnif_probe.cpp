// Magnification API probe: does MagSetWindowFilterList EXCLUDE the target
// window from a MagSetImageScalingCallback capture? If yes, we can capture the
// background BEHIND a window without hiding it (no flicker, no taskbar reorder).
//
// Plan:
//  1. MagInitialize
//  2. Create a hidden MagWindow (magnifier host)
//  3. MagSetImageScalingCallback -> we get the source bitmap each frame
//  4. MagSetWindowFilterList(EXCLUDE) with the target window
//  5. MagSetWindowSource to the target's rect
//  6. Compare: with target in exclude list, the bitmap should show BACKGROUND
//     (target area empty); without exclude, it shows the target itself.
#include <windows.h>
#include <cstdio>
#include <vector>

// ---- Magnification API (MinGW has no usable header — declare manually) ----
extern "C" {
    typedef struct { int width; int height; int stride; DWORD format; DWORD cbSize; } MAGIMAGEHEADER;
    typedef BOOL (CALLBACK *MagImageScalingCallback)(HWND, void*, MAGIMAGEHEADER, void*, RECT*, RECT*);
    BOOL MagInitialize();
    BOOL MagUninitialize();
    BOOL MagSetWindowSource(HWND hwndMag, RECT srcRect);
    BOOL MagSetImageScalingCallback(HWND hwndMag, MagImageScalingCallback callback);
    #define MW_FILTERMODE_EXCLUDE 0
    BOOL MagSetWindowFilterList(HWND hwndMag, DWORD dwFilterMode, int count, const HWND* pHWND);
}

static bool g_hasFrame = false;
static int g_callbackCount = 0;
static RECT g_lastSrc = {};
static std::vector<BYTE> g_lastFrame;

// MagImageScalingCallback: called each frame with the source bitmap
static BOOL CALLBACK ScaleCallback(HWND hwnd, void* srcdata, MAGIMAGEHEADER header,
    void* dstdata, RECT* srcrect, RECT* dstrect) {
    g_callbackCount++;
    g_lastSrc = *srcrect;
    size_t bytes = (size_t)header.cbSize;
    if (srcdata && bytes > 0) {
        g_lastFrame.assign((BYTE*)srcdata, (BYTE*)srcdata + bytes);
        g_hasFrame = true;
    }
    return TRUE;
}

// LoadMagnification.dll dynamically (no MinGW import lib)
static BOOL (WINAPI *pMagInitialize)();
static BOOL (WINAPI *pMagUninitialize)();
static BOOL (WINAPI *pMagSetWindowSource)(HWND, RECT);
static BOOL (WINAPI *pMagSetImageScalingCallback)(HWND, MagImageScalingCallback);
static BOOL (WINAPI *pMagSetWindowFilterList)(HWND, DWORD, int, const HWND*);
#define MagInitialize() pMagInitialize()
#define MagUninitialize() pMagUninitialize()
#define MagSetWindowSource(a,b) pMagSetWindowSource(a,b)
#define MagSetImageScalingCallback(a,b) pMagSetImageScalingCallback(a,b)
#define MagSetWindowFilterList(a,b,c,d) pMagSetWindowFilterList(a,b,c,d)

int main() {
    HMODULE mag = LoadLibraryW(L"Magnification.dll");
    if (!mag) { printf("LoadLibrary Magnification.dll failed %lu\n", GetLastError()); return 1; }
    pMagInitialize = (BOOL(WINAPI*)())GetProcAddress(mag, "MagInitialize");
    pMagUninitialize = (BOOL(WINAPI*)())GetProcAddress(mag, "MagUninitialize");
    pMagSetWindowSource = (BOOL(WINAPI*)(HWND,RECT))GetProcAddress(mag, "MagSetWindowSource");
    pMagSetImageScalingCallback = (BOOL(WINAPI*)(HWND,MagImageScalingCallback))GetProcAddress(mag, "MagSetImageScalingCallback");
    pMagSetWindowFilterList = (BOOL(WINAPI*)(HWND,DWORD,int,const HWND*))GetProcAddress(mag, "MagSetWindowFilterList");
    if (!pMagInitialize || !pMagSetWindowSource || !pMagSetImageScalingCallback || !pMagSetWindowFilterList) {
        printf("GetProcAddress failed %lu\n", GetLastError()); return 1;
    }
    printf("Magnification.dll loaded OK\n");
    if (!MagInitialize()) { printf("MagInitialize FAILED err=%lu\n", GetLastError()); return 1; }
    printf("MagInitialize OK\n");

    // target = explorer window (find a CabinetWClass window)
    HWND target = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t cls[64]; GetClassNameW(h, cls, 63);
        if (wcscmp(cls, L"CabinetWClass") == 0) { *(HWND*)lp = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&target);
    if (!target) { printf("no explorer window\n"); MagUninitialize(); return 1; }
    RECT tr; GetWindowRect(target, &tr);
    printf("target 0x%p rect %ld,%ld %ldx%ld\n", target, tr.left, tr.top,
           tr.right - tr.left, tr.bottom - tr.top);

    // Create a hidden mag window (host)
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"MagProbeHost";
    RegisterClassExW(&wc);
    HWND magHost = CreateWindowExW(0, L"MagProbeHost", L"", WS_POPUP,
                                   0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!magHost) { printf("host create failed %lu\n", GetLastError()); MagUninitialize(); return 1; }
    printf("mag host 0x%p\n", magHost);

    // MagSetImageScalingCallback is NOT supported for non-magnifier callers
    // (ERROR_NOT_SUPPORTED). Alternative: make the mag window VISIBLE (small,
    // corner) so DWM composites it, then grab its content with BitBlt.
    // The exclude list applies to the mag window's rendered output.
    SetWindowPos(magHost, HWND_TOPMOST, 0, 0, 400, 300, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    SetWindowPos(magHost, HWND_BOTTOM, 0, 0, 400, 300, SWP_NOACTIVATE);
    // mag window must be visible for compositing; place at bottom-right corner
    RECT wr = {0,0,400,300};
    SetWindowPos(magHost, HWND_TOP, 1920-420, 1080-320, 400, 300, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    printf("mag host visible at corner (1920-420,1080-320)\n");

    // Set source = target rect. With NO exclude first: expect target itself.
    if (!MagSetWindowSource(magHost, tr)) { printf("source set failed %lu\n", GetLastError()); }
    Sleep(800);

    // grab mag window content via BitBlt (screenshots of the mag window)
    auto grabMag = [&](const char* tag) {
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        HBITMAP bmp = CreateCompatibleBitmap(scr, 400, 300);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, 400, 300, scr, 1920-420, 1080-320, SRCCOPY);
        // sample center pixel
        COLORREF c = GetPixel(mem, 200, 150);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, scr);
        printf("%s: mag window center pixel RGB=(%u,%u,%u)\n",
               tag, GetRValue(c), GetGValue(c), GetBValue(c));
        return c;
    };

    grabMag("WITHOUT exclude");

    // Now exclude the target -> capture should show BACKGROUND behind it
    MagSetWindowFilterList(magHost, MW_FILTERMODE_EXCLUDE, 1, &target);
    Sleep(800);
    grabMag("WITH exclude");

    // compare with the real target window content at its center
    HDC scr = GetDC(nullptr);
    COLORREF tgt = GetPixel(scr, (tr.left+tr.right)/2, (tr.top+tr.bottom)/2);
    ReleaseDC(nullptr, scr);
    printf("target center pixel RGB=(%u,%u,%u)\n", GetRValue(tgt), GetGValue(tgt), GetBValue(tgt));

    MagSetWindowFilterList(magHost, MW_FILTERMODE_EXCLUDE, 0, nullptr);
    DestroyWindow(magHost);
    MagUninitialize();
    printf("done\n");
    return 0;
}
