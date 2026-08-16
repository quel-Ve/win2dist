/**
 * Frosted DWM — MSVC build.
 * Uses proper ID2D1Effect COM interface (no vtable guessing).
 * Compile: cl /LD dllmain_msvc.cpp minhook/*.cpp /Fe:libfrosted_dwm.dll
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <d2d1_1.h>
#include <d2d1effects.h>

#include "MinHook.h"

// Offsets for Win10 19045, dwmcore 10.0.19041.320
#define OFF_CD2DContext_FillEffect            0xCE6C0
#define OFF_CCustomBlur_BuildEffect           0x40380
#define OFF_CCustomBlur_DetermineOutputScale  0x40304

// CCustomBlur member offsets (from DWMBlurGlass DWMStruct.h)
#define OFF_DIRBLUR_KERNEL_X  0x30
#define OFF_DIRBLUR_KERNEL_Y  0x38

static float g_blurRadius = 30.0f;

// Config file: win2blur writes radius, DLL reads it.
// File IPC avoids all ACL/UIPI/session issues (dwm = SYSTEM can read any file).
#define CONFIG_FILE "C:\\Temp\\frosted_dwm_config.txt"
static DWORD g_lastFileCheck = 0;

static void Log(const char* msg); // forward

static void ReadBlurFromFile() {
    DWORD now = GetTickCount();
    if (now - g_lastFileCheck < 200) return;  // throttle: max 5 reads/sec
    g_lastFileCheck = now;

    HANDLE f = CreateFileA(CONFIG_FILE, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[64] = {0};
    DWORD rd = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &rd, NULL);
    CloseHandle(f);
    int r = atoi(buf);
    if (r >= 1 && r <= 100) {
        if (g_blurRadius != (float)r) {
            g_blurRadius = (float)r;
            char line[64];
            sprintf_s(line, "config: blur -> %.1f", g_blurRadius);
            Log(line);
        }
    }
}

static void Log(const char* msg) {
    HANDLE f = CreateFileA("C:\\Temp\\frosted_dwm.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr; SYSTEMTIME st; GetLocalTime(&st);
        char line[512];
        int n = sprintf_s(line, "[%02d:%02d:%02d.%03d] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        WriteFile(f, line, n, &wr, NULL);
        CloseHandle(f);
    }
}

// ===== BuildEffect hook =====
typedef float   (WINAPI *fn_DetermineOutputScale)(float, float, int);
typedef DWORD64 (WINAPI *fn_BuildEffect)(void*, void*, const void*,
                    const void*, int, const void*, void*);

static fn_DetermineOutputScale g_origScale = nullptr;
static fn_BuildEffect g_origBuild = nullptr;

static LONG g_scaleCalls = 0, g_buildCalls = 0, g_fillCalls = 0;

static float WINAPI Hook_DetermineOutputScale(float size, float blurAmount, int opt) {
    if (InterlockedIncrement(&g_scaleCalls) == 1)
        Log(">> DetermineOutputScale FIRED");
    return 1.0f;
}

static DWORD64 WINAPI Hook_BuildEffect(void* This, void* backdrop, const void* srcRect,
    const void* kSize, int a5, const void* a6, void* a7)
{
    ReadBlurFromFile(); // pick up latest value from win2blur

    LONG n = InterlockedIncrement(&g_buildCalls);
    if (n <= 3) {
        char buf[128];
        sprintf_s(buf, ">> BuildEffect #%d This=0x%p", n, This);
        Log(buf);
    }

    DWORD64 ret = g_origBuild(This, backdrop, srcRect, kSize, a5, a6, a7);

    // EVERY call: override kernel size with current config value
    ID2D1Effect* kx = *(ID2D1Effect**)((BYTE*)This + OFF_DIRBLUR_KERNEL_X);
    ID2D1Effect* ky = *(ID2D1Effect**)((BYTE*)This + OFF_DIRBLUR_KERNEL_Y);
    if (kx) kx->SetValue(0, g_blurRadius);
    if (ky) ky->SetValue(0, g_blurRadius);

    if (n <= 3) {
        char buf[256];
        if (kx) {
            UINT32 cnt = kx->GetPropertyCount();
            wchar_t pname[128]; kx->GetPropertyName(0, pname, 128);
            sprintf_s(buf, "  KernelX=0x%p propCount=%u prop[0]=%S blur=%.1f", kx, cnt, pname, g_blurRadius);
            Log(buf);
        }
    }

    return ret;
}

// ===== FillEffect hook =====
typedef DWORD64 (WINAPI *fn_FillEffect)(void*, const void*, ID2D1Effect*,
    const D2D_RECT_F*, const D2D_POINT_2F*, D2D1_INTERPOLATION_MODE, D2D1_COMPOSITE_MODE);

static fn_FillEffect g_origFill = nullptr;

static DWORD64 WINAPI Hook_FillEffect(void* self, const void* a2, ID2D1Effect* effect,
    const D2D_RECT_F* srcRect, const D2D_POINT_2F* dstPoint,
    D2D1_INTERPOLATION_MODE interp, D2D1_COMPOSITE_MODE comp)
{
    LONG n = InterlockedIncrement(&g_fillCalls);
    if (n == 1) Log(">> FillEffect FIRED");

    // EVERY call: override BlurBehind effects (HostBackdrop has 4 props, skip)
    if (effect && n > 1) {
        UINT32 cnt = effect->GetPropertyCount();
        if (cnt != 4 && cnt >= 1 && cnt <= 10) {
            effect->SetValue(0, g_blurRadius);
        }
    }
    return g_origFill(self, a2, effect, srcRect, dstPoint, interp, comp);
}

// ===== Install =====
static void InstallHooks() {
    HMODULE dwmcore = GetModuleHandleW(L"dwmcore.dll");
    if (!dwmcore) { Log("ERROR: dwmcore not loaded"); return; }

    MH_Initialize();

    void* pScale  = (BYTE*)dwmcore + OFF_CCustomBlur_DetermineOutputScale;
    void* pBuild  = (BYTE*)dwmcore + OFF_CCustomBlur_BuildEffect;
    void* pFill   = (BYTE*)dwmcore + OFF_CD2DContext_FillEffect;

    MH_CreateHook(pScale, Hook_DetermineOutputScale, (void**)&g_origScale);
    MH_EnableHook(pScale);

    MH_CreateHook(pBuild, Hook_BuildEffect, (void**)&g_origBuild);
    MH_EnableHook(pBuild);

    MH_CreateHook(pFill, Hook_FillEffect, (void**)&g_origFill);
    MH_EnableHook(pFill);

    Log("=== FrostedDWM MSVC: 3 hooks active ===");
}

static void RemoveHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        InstallHooks();
    } else if (reason == DLL_PROCESS_DETACH) {
        RemoveHooks();
    }
    return TRUE;
}
