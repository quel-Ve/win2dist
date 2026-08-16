/**
 * Frosted DWM — FillEffect approach.
 * CD2DContext::FillEffect handles BlurBehind (taskbar, start menu).
 * Gets raw ID2D1Effect* — standard COM vtable, no DWM-private layout.
 */
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <cstdio>
#include <initializer_list>

#include "MinHook.h"

#define OFF_CD2DContext_FillEffect            0xCE6C0
#define OFF_CCustomBlur_DetermineOutputScale  0x40304

static float g_blurRadius = 3.0f;

static void Log(const char* msg) {
    HANDLE f = CreateFileW(L"C:\\Temp\\frosted_dwm.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr; SYSTEMTIME st; GetLocalTime(&st);
        char line[512];
        int n = wsprintfA(line, "[%02d:%02d:%02d.%03d] %s\r\n",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
        WriteFile(f, line, n, &wr, NULL);
        CloseHandle(f);
    }
}

// ===== ID2D1Effect vtable types (standard COM) =====
typedef UINT32 (STDMETHODCALLTYPE *GetPropCount_t)(void* self);
typedef HRESULT (STDMETHODCALLTYPE *GetPropName_t)(void* self, UINT32 idx, LPWSTR name, UINT32 count);

// FillEffect signature
typedef DWORD64 (STDMETHODCALLTYPE *FillEffect_t)(void* self, const void* a2, void* inputEffect,
    const void* srcRect, const void* dstPoint, int interMode, int mode);

static FillEffect_t g_origFill = nullptr;
static float   (*g_origScale)(float size, float blurAmount, int optimization) = nullptr;

static float Hook_DetermineOutputScale(float size, float blurAmount, int optimization) {
    return 1.0f;
}

static LONG g_fillCount = 0;
static bool g_dumped = false;

static DWORD64 Hook_FillEffect(void* self, const void* a2, void* inputEffect,
    const void* srcRect, const void* dstPoint, int interMode, int mode)
{
    LONG n = InterlockedIncrement(&g_fillCount);

    // Modify ALL blur effects — set StandardDeviation + KernelRangeFactor
    if (inputEffect && (DWORD64)inputEffect > 0x10000) {
        void** v = *(void***)inputEffect;
        UINT32 cnt = ((GetPropCount_t)v[3])(inputEffect);
        if (cnt >= 1 && cnt <= 10) {
            typedef HRESULT (STDMETHODCALLTYPE *SV3)(void*, UINT32, const BYTE*, UINT32);
            SV3 sv = (SV3)v[9];
            sv(inputEffect, 0, (const BYTE*)&g_blurRadius, sizeof(float)); // StandardDeviation
            if (cnt >= 3)
                sv(inputEffect, 2, (const BYTE*)&g_blurRadius, sizeof(float)); // KernelRangeFactor
        }
    }

    // One-time dump
    if (!g_dumped && n % 500 == 7) {
        g_dumped = true;
        if (inputEffect && (DWORD64)inputEffect > 0x10000) {
            void** v = *(void***)inputEffect;
            UINT32 cnt = ((GetPropCount_t)v[3])(inputEffect);
            char line[256];
            wsprintfA(line, "FillEffect: cnt=%lu (modifying all)", cnt);
            Log(line);
        }
    }

    return g_origFill(self, a2, inputEffect, srcRect, dstPoint, interMode, mode);
}

static void InstallHooks() {
    HMODULE dwmcore = GetModuleHandleW(L"dwmcore.dll");
    if (!dwmcore) return;
    MH_Initialize();
    void* pS = (void*)((DWORD64)dwmcore + OFF_CCustomBlur_DetermineOutputScale);
    void* pF = (void*)((DWORD64)dwmcore + OFF_CD2DContext_FillEffect);
    MH_CreateHook(pS, (void*)Hook_DetermineOutputScale, (void**)&g_origScale);
    MH_EnableHook(pS);
    MH_CreateHook(pF, (void*)Hook_FillEffect, (void**)&g_origFill);
    MH_EnableHook(pF);
    Log("=== FrostedDWM FillEffect probe active ===");
}

static void RemoveHooks() { MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize(); }

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(mod); InstallHooks(); }
    else if (reason == DLL_PROCESS_DETACH) { RemoveHooks(); }
    return TRUE;
}
