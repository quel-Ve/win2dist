// dcomp_probe.cpp — DirectComposition viability: the KEY question first —
// can a NON-layered window with a DComp target take WDA_EXCLUDEFROMCAPTURE?
// (layered windows cannot, err 8; a plain window has a real redirection
// surface so it may.) If yes, the overlay never needs hiding -> no flicker.
#include <windows.h>
#include <dcomp.h>
#include <cstdio>
#pragma comment(lib, "dcomp.lib")

static IDCompositionDevice* g_dev = nullptr;
static IDCompositionTarget* g_tgt = nullptr;
static IDCompositionVisual* g_vis = nullptr;

int main() {
    printf("DComp probe start\n");
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"DCompProbe";
    RegisterClassW(&wc);
    HWND ov = CreateWindowExW(0, L"DCompProbe", L"", WS_POPUP,
                              500, 300, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    if (!ov) { printf("window create failed %lu\n", GetLastError()); return 1; }
    ShowWindow(ov, SW_SHOWNOACTIVATE);

    // Create DComp device + target + visual (minimal, no content yet)
    HRESULT hr = DCompositionCreateDevice(nullptr, __uuidof(IDCompositionDevice),
                                          (void**)&g_dev);
    if (FAILED(hr)) { printf("DCompositionCreateDevice failed 0x%08X\n", hr); return 1; }
    hr = g_dev->CreateTargetForHwnd(ov, TRUE, &g_tgt);
    if (FAILED(hr)) { printf("CreateTargetForHwnd failed 0x%08X\n", hr); return 1; }
    hr = g_dev->CreateVisual(&g_vis);
    if (FAILED(hr)) { printf("CreateVisual failed 0x%08X\n", hr); return 1; }
    g_tgt->SetRoot(g_vis);
    g_dev->Commit();
    printf("DComp target+visual created OK\n");

    // KEY: can this non-layered window take WDA?
    DWORD aff0 = 0; GetWindowDisplayAffinity(ov, &aff0);
    printf("affinity before: %lu\n", aff0);
    BOOL ok = SetWindowDisplayAffinity(ov, 0x11);
    printf("SetWindowDisplayAffinity(EXCLUDE): %s err=%lu\n",
           ok ? "YES" : "NO", GetLastError());
    DWORD aff1 = 0; GetWindowDisplayAffinity(ov, &aff1);
    printf("affinity after: %lu %s\n", aff1,
           aff1 == 0x11 ? "*** DCOMP WINDOW CAN TAKE WDA — KEY RESULT ***" : "(failed)");

    printf("window at 500,300 400x300 (should be invisible/blank)\n");
    printf("Press Enter to cleanup\n");
    getchar();
    SetWindowDisplayAffinity(ov, 0);
    DestroyWindow(ov);
    printf("done\n");
    return 0;
}
