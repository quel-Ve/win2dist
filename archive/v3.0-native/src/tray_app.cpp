/**
 * win2blur Tray App — native Win32 C++ rewrite
 * =============================================
 * System tray + global hotkeys + transparency + acrylic overlay management.
 * v2.5: self-built MSVC DLL for blur radius (no DWMBlurGlass dependency).
 * Build: g++ tray_app.cpp -o win2blur.exe -ldwmapi -mwindows -O2 -s
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdlib>
using std::max;
using std::min;

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

// v3.0: [Apps] profile-line parser (task 1). WIN32_LEAN_AND_MEAN already
// defined above, so profile.h's guard is a no-op; UNICODE arrives after
// windows.h — tray_app.cpp uses explicit W-suffixed APIs throughout.
#include "profile.h"

// ==================== Constants ====================
#define WM_TRAYICON (WM_APP + 1)
#define ID_UP 1
#define ID_DOWN 2
#define ID_TOGGLE 3
#define ID_ACRYLIC 4
#define ID_SETTINGS 1001
#define ID_EXIT_RESTORE 1002
#define ID_EXIT_KEEP 1003
#define DEFAULT_STEP 5
#define DEFAULT_BLUR_RADIUS 30
#define DEFAULT_EFFECT_MODE 0   // 0 = Acrylic (DWM), 1 = Crisp (DDA wash, text-preserving)
#define DEFAULT_TINT_LEVEL 0    // 0-100 black level; 0 = pure blur (no tint)

static const int g_blurPresets[] = {2, 4, 8, 12, 15, 20, 30, 40, 50};
static const int g_blurPresetCount = sizeof(g_blurPresets) / sizeof(g_blurPresets[0]);

// ==================== Shared Memory IPC ====================
#define SHM_NAME L"Global\\FrostedDWM_Config"
#define SHM_MAGIC 0x4652444D

// ==================== Globals ====================
static HWND g_hwnd = nullptr;
static NOTIFYICONDATAW g_nid = {};
static int g_step = DEFAULT_STEP;
static int g_blurRadius = DEFAULT_BLUR_RADIUS;
static bool g_hasBlurControl = false;
static std::wstring g_injectorPath;
static std::wstring g_dllPath;
static HANDLE g_hShm = nullptr;
static DWORD* g_pShmData = nullptr;
static std::set<HWND> g_modified;
static std::set<HWND> g_autoApplied;   // windows the monitor auto-applied (for revert on removal)
static std::map<HWND, int> g_lastAlpha;
static std::map<HWND, PROCESS_INFORMATION> g_overlays;
// v3.0: per-window effect params. alpha = opacity/wash 0-255; radius/tint = -1
// when unset by a profile (resolve to globals g_blurRadius / g_tintLevel in
// launch_overlay, so hotkey-only windows keep following the global sliders).
struct WinParams { int alpha = 255, radius = -1, tint = -1; int mode = -1; bool circle = false; };
static std::map<HWND, WinParams> g_winAlpha;  // v3.0: per-window params (was int opacity)
static std::map<HWND, DWORD> g_winPid;    // v2.8: PID at overlay launch (HWND-recycling guard)
static std::map<HWND, std::pair<int, DWORD>> g_restartCount;  // v3.0 task 8: crash-guard restart bookkeeping (count, first-attempt tick)
static std::wstring g_configPath, g_overlayPath, g_crispPath, g_welcomePath, g_tempDir;
static int g_effectMode = DEFAULT_EFFECT_MODE;
static int g_tintLevel = DEFAULT_TINT_LEVEL;
// v3.0 crisp overlay launch params ([Settings] CircleRadius/CircleBoost/CircleBand/FocusFactor)
static int g_circleRadius = 50;   // mouse-follow circle size px (0 = off)
static int g_circleBoost = 200;   // circle blur radius = radius * boost / 100
static int g_circleBand = 30;     // circle edge transition band px
static int g_focusFactor = 60;    // background window reduction % (100 = off)
static HICON g_hIcon = nullptr;
static int g_transpMod = MOD_ALT, g_transpKeyUp = VK_LEFT, g_transpKeyDown = VK_RIGHT;
static int g_toggleMod = MOD_ALT, g_toggleKey = VK_UP;
static int g_acrylicMod = MOD_ALT, g_acrylicKey = VK_DOWN;
static bool g_enableUp = true, g_enableDown = true, g_enableToggle = true, g_enableAcrylic = true;
static CRITICAL_SECTION g_fxLock;
// v3.0 fix: per-window mode override (-1 = inherit global g_effectMode).
// Previously apply_profile() wrote profile mode into the GLOBAL g_effectMode,
// so profiles with different modes overwrote each other and hotkeys/Settings/
// session used whichever window matched last (feature-matrix risk A, 2026-08-08).
static int mode_of(HWND hwnd) {
    int m = g_effectMode;
    EnterCriticalSection(&g_fxLock);
    if (g_winAlpha.count(hwnd) && g_winAlpha[hwnd].mode >= 0) m = g_winAlpha[hwnd].mode;
    LeaveCriticalSection(&g_fxLock);
    return m;
}

// ==================== Resource extraction ====================
std::wstring extract_resource(int resId, const wchar_t* name) {
    if (!g_tempDir.empty()) {
        std::wstring path = g_tempDir + L"\\" + name;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    }
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), (LPCWSTR)RT_RCDATA);
    if (!hRes) return L"";
    HGLOBAL hData = LoadResource(nullptr, hRes);
    if (!hData) return L"";
    DWORD size = SizeofResource(nullptr, hRes);
    void* data = LockResource(hData);
    if (!data) return L"";
    if (g_tempDir.empty()) {
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        wchar_t dir[MAX_PATH];
        wsprintfW(dir, L"%swin2blur_%u\\", tmp, GetCurrentProcessId());
        CreateDirectoryW(dir, nullptr);
        g_tempDir = dir;
    }
    std::wstring path = g_tempDir + name;
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return L"";
    DWORD written; WriteFile(hFile, data, size, &written, nullptr);
    CloseHandle(hFile);
    return path;
}
void cleanup_temp() {
    if (!g_tempDir.empty()) {
        DeleteFileW((g_tempDir + L"acrylic_overlay.exe").c_str());
        DeleteFileW((g_tempDir + L"welcome_demo.exe").c_str());
        RemoveDirectoryW(g_tempDir.c_str());
    }
}

// ==================== Config ====================
std::wstring config_path() {
    if (!g_configPath.empty()) return g_configPath;
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* last = wcsrchr(path, L'\\');
    if (last) *(last + 1) = 0;
    g_configPath = std::wstring(path) + L"config.ini";
    return g_configPath;
}
std::wstring exe_dir() {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* last = wcsrchr(path, L'\\');
    if (last) *(last + 1) = 0;
    return std::wstring(path);
}
int read_int(const wchar_t* key, int def) {
    return GetPrivateProfileIntW(L"Settings", key, def, config_path().c_str());
}
void write_int(const wchar_t* key, int val) {
    wchar_t buf[32]; wsprintfW(buf, L"%d", val);
    WritePrivateProfileStringW(L"Settings", key, buf, config_path().c_str());
}
int read_hotkey(const wchar_t* sec, const wchar_t* key, int def) {
    return GetPrivateProfileIntW(sec, key, def, config_path().c_str());
}
bool read_switch(const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(L"Switches", key, def ? 1 : 0, config_path().c_str()) != 0;
}
void load_config() {
    g_effectMode = read_int(L"EffectMode", DEFAULT_EFFECT_MODE);
    if (g_effectMode < 0 || g_effectMode > 1) g_effectMode = DEFAULT_EFFECT_MODE;
    g_tintLevel = read_int(L"TintLevel", DEFAULT_TINT_LEVEL);
    if (g_tintLevel < 0 || g_tintLevel > 100) g_tintLevel = DEFAULT_TINT_LEVEL;
    // v3.0: crisp overlay launch params (clamped to crisp_overlay's accepted ranges)
    g_circleRadius = read_int(L"CircleRadius", 50);
    if (g_circleRadius < 0 || g_circleRadius > 200) g_circleRadius = 50;
    g_circleBoost = read_int(L"CircleBoost", 200);
    if (g_circleBoost < 100 || g_circleBoost > 400) g_circleBoost = 200;
    g_circleBand = read_int(L"CircleBand", 30);
    if (g_circleBand < 5 || g_circleBand > 100) g_circleBand = 30;
    g_focusFactor = read_int(L"FocusFactor", 60);
    if (g_focusFactor < 10 || g_focusFactor > 100) g_focusFactor = 60;
    g_step = read_int(L"TransparencyStep", DEFAULT_STEP);
    if (g_step < 1 || g_step > 20) g_step = DEFAULT_STEP;
    g_transpMod = read_hotkey(L"Hotkeys", L"TransparencyUpModifiers", MOD_ALT);
    g_transpKeyUp = read_hotkey(L"Hotkeys", L"TransparencyUpKey", VK_LEFT);
    g_transpKeyDown = read_hotkey(L"Hotkeys", L"TransparencyDownKey", VK_RIGHT);
    g_toggleMod = read_hotkey(L"Hotkeys", L"TransparencyToggleModifiers", MOD_ALT);
    g_toggleKey = read_hotkey(L"Hotkeys", L"TransparencyToggleKey", VK_UP);
    g_acrylicMod = read_hotkey(L"Hotkeys", L"AcrylicToggleModifiers", MOD_ALT);
    g_acrylicKey = read_hotkey(L"Hotkeys", L"AcrylicToggleKey", VK_DOWN);
    g_enableUp = read_switch(L"EnableTransparencyUp", true);
    g_enableDown = read_switch(L"EnableTransparencyDown", true);
    g_enableToggle = read_switch(L"EnableTransparencyToggle", true);
    g_enableAcrylic = read_switch(L"EnableAcrylicToggle", true);
}

// ==================== Debug log (evidence gathering) ====================
void debug_log(const wchar_t* fmt, ...) {
    static CRITICAL_SECTION lg; static bool lgInit = false;
    if (!lgInit) { InitializeCriticalSection(&lg); lgInit = true; }
    EnterCriticalSection(&lg);
    HANDLE f = CreateFileW(L"C:\\Temp\\win2blur_debug.log", FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t buf[1024]; va_list ap; va_start(ap, fmt);
        wsprintfW(buf, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        wvsprintfW(buf + wcslen(buf), fmt, ap); va_end(ap);
        wcscat(buf, L"\r\n");
        char buf8[2048];
        int n8 = WideCharToMultiByte(CP_UTF8, 0, buf, -1, buf8, 2048, nullptr, nullptr);
        DWORD wr; if (n8 > 1) WriteFile(f, buf8, n8 - 1, &wr, nullptr);
        CloseHandle(f);
    }
    LeaveCriticalSection(&lg);
}
const wchar_t* cls_of(HWND h) { static wchar_t c[64]; GetClassNameW(h, c, 63); return c; }

// ==================== Auto-frost ====================
// v3.0: AutoApp comes from profile.h (same fields as ProfileEntry);
// -1 / false = unset -> inherit globals at apply time.
struct AutoFrostConfig {
    bool enabled = false;
    int defaultAlpha = 217;   // 85%
    bool defaultBlur = true;
    std::vector<AutoApp> apps;
};
static AutoFrostConfig g_auto;
static const AutoApp g_autoPresets[] = {
    {L"Obsidian.exe", L""},
    {L"WindowsTerminal.exe", L""},
    {L"msedge.exe", L""},
    {L"explorer.exe", L"CabinetWClass"},
    {L"cloudmusic.exe", L""},
    {L"WeChat.exe", L""},
    {L"Cherry Studio.exe", L""},
    {L"Code.exe", L""},
};

// v3.0: profile table lives in [Apps] (design spec). Values are profile lines
// ("exe = mode=.. alpha=.. radius=.. tint=.. circle=on/off") parsed by
// parse_profile_line; lines without fields parse with defaults. Legacy v2.8
// [AutoFrost] App_N="exe|cls" pipe entries load as a fallback when [Apps] is
// absent, so existing configs keep working until first Apply migrates them.
void load_autofrost() {
    auto cfg = config_path();
    wchar_t probe[8];
    bool sectionExists = GetPrivateProfileStringW(L"AutoFrost", L"Enabled", L"", probe, 8, cfg.c_str()) != 0;
    if (!sectionExists) {
        // first run — seed presets as plain [Apps] profile lines
        for (int i = 0; i < (int)(sizeof(g_autoPresets) / sizeof(g_autoPresets[0])); i++) {
            wchar_t key[16], val[192];
            wsprintfW(key, L"App_%d", i);
            if (g_autoPresets[i].cls.empty())
                wsprintfW(val, L"%s", g_autoPresets[i].exe.c_str());
            else
                wsprintfW(val, L"%s = cls=%s", g_autoPresets[i].exe.c_str(), g_autoPresets[i].cls.c_str());
            WritePrivateProfileStringW(L"Apps", key, val, cfg.c_str());
        }
        WritePrivateProfileStringW(L"AutoFrost", L"Enabled", L"1", cfg.c_str());
        WritePrivateProfileStringW(L"AutoFrost", L"DefaultAlpha", L"217", cfg.c_str());
        WritePrivateProfileStringW(L"AutoFrost", L"DefaultBlur", L"1", cfg.c_str());
    }
    g_auto.enabled = GetPrivateProfileIntW(L"AutoFrost", L"Enabled", 0, cfg.c_str()) != 0;
    g_auto.defaultAlpha = GetPrivateProfileIntW(L"AutoFrost", L"DefaultAlpha", 217, cfg.c_str());
    if (g_auto.defaultAlpha < 1 || g_auto.defaultAlpha > 255) g_auto.defaultAlpha = 217;
    g_auto.defaultBlur = GetPrivateProfileIntW(L"AutoFrost", L"DefaultBlur", 1, cfg.c_str()) != 0;
    g_auto.apps.clear();
    wchar_t buf[512];
    for (int i = 0; i < 32; i++) {                 // v3.0: [Apps] profile table
        wchar_t key[16]; wsprintfW(key, L"App_%d", i);
        if (!GetPrivateProfileStringW(L"Apps", key, L"", buf, 512, cfg.c_str()) || !buf[0]) break;
        ProfileEntry pe = parse_profile_line(buf);
        if (pe.exe.empty()) continue;
        AutoApp a;
        a.exe = pe.exe; a.cls = pe.cls;
        a.mode = pe.mode; a.alpha = pe.alpha; a.radius = pe.radius;
        a.tint = pe.tint; a.circle = pe.circle;
        g_auto.apps.push_back(a);
    }
    if (g_auto.apps.empty()) {                     // legacy v2.8 pipe format fallback
        wchar_t lp[8];
        if (GetPrivateProfileStringW(L"AutoFrost", L"App_0", L"", lp, 8, cfg.c_str()) != 0) {
            wchar_t lbuf[128];
            for (int i = 0; i < 32; i++) {
                wchar_t key[16]; wsprintfW(key, L"App_%d", i);
                if (!GetPrivateProfileStringW(L"AutoFrost", key, L"", lbuf, 128, cfg.c_str()) || !lbuf[0]) break;
                std::wstring s(lbuf);
                auto p = s.find(L'|');
                AutoApp a;
                a.exe = (p == s.npos) ? s : s.substr(0, p);
                a.cls = (p == s.npos) ? L"" : s.substr(p + 1);
                if (!a.exe.empty()) g_auto.apps.push_back(a);
            }
        }
    }
}
// v3.0: build_profile_line lives in profile.cpp beside its parser (single
// serialization path; Task 7's profile editor writes through the same helper).
void save_autofrost() {
    auto cfg = config_path();
    WritePrivateProfileStringW(L"AutoFrost", nullptr, nullptr, cfg.c_str()); // wipe legacy section (migrates pipe list out)
    WritePrivateProfileStringW(L"Apps", nullptr, nullptr, cfg.c_str());       // wipe profile table
    WritePrivateProfileStringW(L"AutoFrost", L"Enabled", g_auto.enabled ? L"1" : L"0", cfg.c_str());
    wchar_t b[16]; wsprintfW(b, L"%d", g_auto.defaultAlpha);
    WritePrivateProfileStringW(L"AutoFrost", L"DefaultAlpha", b, cfg.c_str());
    WritePrivateProfileStringW(L"AutoFrost", L"DefaultBlur", g_auto.defaultBlur ? L"1" : L"0", cfg.c_str());
    for (size_t i = 0; i < g_auto.apps.size() && i < 32; i++) {
        wchar_t key[16], val[256];
        wsprintfW(key, L"App_%u", (unsigned)i);
        build_profile_line(g_auto.apps[i], val);
        WritePrivateProfileStringW(L"Apps", key, val, cfg.c_str());
    }
}

// ==================== Self-built DLL + Shared Memory ====================
void detect_self_dll() {
    std::wstring dir = exe_dir();
    const wchar_t* paths[] = {
        L"injector.exe", L"libfrosted_dwm.dll",
        L"native\\dwm_inject\\build\\injector.exe", L"native\\dwm_inject\\build\\libfrosted_dwm.dll",
    };
    for (int i = 0; i < 4; i += 2) {
        if (GetFileAttributesW((dir + paths[i]).c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW((dir + paths[i+1]).c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_injectorPath = dir + paths[i];
            g_dllPath = dir + paths[i+1];
            g_hasBlurControl = true;
            // Create shared memory
            g_hShm = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 8, SHM_NAME);
            if (g_hShm) {
                g_pShmData = (DWORD*)MapViewOfFile(g_hShm, FILE_MAP_WRITE, 0, 0, 8);
                if (g_pShmData) { g_pShmData[0] = SHM_MAGIC; g_pShmData[1] = g_blurRadius; }
            }
            return;
        }
    }
    g_hasBlurControl = false;
}

static bool is_elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te = {};
    DWORD sz = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz) && te.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}

void inject_self_dll() {
    if (g_injectorPath.empty() || g_dllPath.empty()) return;
    // win2blur runs elevated (requireAdministrator manifest) -> plain launch, no UAC prompt.
    // runas fallback kept for non-manifest builds (e.g. debug copies run unelevated).
    if (is_elevated()) {
        wchar_t cmd[MAX_PATH * 2];
        wsprintfW(cmd, L"\"%s\" \"%s\"", g_injectorPath.c_str(), g_dllPath.c_str());
        STARTUPINFOW si = {sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi)) {
            if (pi.hProcess) {
                WaitForSingleObject(pi.hProcess, 5000);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
            }
            return;
        }
    }
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = g_injectorPath.c_str();
    sei.lpParameters = g_dllPath.c_str();
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 5000);
        CloseHandle(sei.hProcess);
    }
}

// Config file path — must match DLL's CONFIG_FILE
#define BLUR_CONFIG_FILE L"C:\\Temp\\frosted_dwm_config.txt"

void set_blur_radius(int presetIdx) {
    if (presetIdx < 0) presetIdx = 0;
    if (presetIdx >= g_blurPresetCount) presetIdx = g_blurPresetCount - 1;
    g_blurRadius = g_blurPresets[presetIdx];

    // Write config file — DLL in dwm.exe reads it (file IPC avoids ACL issues)
    HANDLE f = CreateFileW(BLUR_CONFIG_FILE, GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[16];
        wsprintfA(buf, "%d", g_blurRadius);
        DWORD wr;
        WriteFile(f, buf, (DWORD)strlen(buf), &wr, nullptr);
        CloseHandle(f);
    }
}

// ==================== Session ====================
void save_session(bool clear) {
    std::wstring cfg = config_path();
    if (clear) { WritePrivateProfileStringW(L"Session", nullptr, nullptr, cfg.c_str()); return; }
    WritePrivateProfileStringW(L"Session", nullptr, nullptr, cfg.c_str());
    EnterCriticalSection(&g_fxLock);
    int i = 0;
    for (auto hwnd : g_modified) {
        wchar_t buf[512], key[32];
        int alpha = 255;
        if (g_winAlpha.count(hwnd)) alpha = g_winAlpha[hwnd].alpha;   // crisp: tracked value
        else { BYTE b; DWORD flags;
            if (GetLayeredWindowAttributes(hwnd, nullptr, &b, &flags) && (flags & LWA_ALPHA)) alpha = b; }
        wchar_t title[128] = {}, cls[64] = {};
        GetWindowTextW(hwnd, title, 127);
        GetClassNameW(hwnd, cls, 63);
        // effect field: 0 = none, 1 = acrylic overlay, 2 = crisp overlay.
        // v3.0: radius/tint/circle appended AFTER effect (old 4-field lines
        // still parse; -1 = unset -> global default on restore).
        int effect = g_overlays.count(hwnd) ? mode_of(hwnd) + 1 : 0;   // per-window mode (fix risk A)
        WinParams wp = g_winAlpha.count(hwnd) ? g_winAlpha[hwnd] : WinParams();
        wsprintfW(buf, L"%s|%s|%d|%d|%d|%d|%d", title, cls, alpha, effect,
                  wp.radius, wp.tint, wp.circle ? 1 : 0);
        wsprintfW(key, L"window_%d", i++);
        WritePrivateProfileStringW(L"Session", key, buf, cfg.c_str());
    }
    LeaveCriticalSection(&g_fxLock);
}
// fields: title|cls|alpha|effect|radius|tint|circle  (radius/tint/circle appended in v3.0)
struct SessionItem { std::wstring title, cls; int alpha, effect, radius, tint; bool circle; };
static std::vector<SessionItem> g_sessionItems;
void load_session_items() {
    g_sessionItems.clear();
    wchar_t buf[512]; auto cfg = config_path();
    for (int i = 0; i < 50; i++) {
        wchar_t key[32]; wsprintfW(key, L"window_%d", i);
        if (!GetPrivateProfileStringW(L"Session", key, L"", buf, 512, cfg.c_str()) || !buf[0]) break;
        std::wstring s(buf);
        auto p1 = s.find(L'|'); if (p1 == s.npos) continue;
        auto p2 = s.find(L'|', p1+1); if (p2 == s.npos) continue;
        auto p3 = s.find(L'|', p2+1);
        auto p4 = s.find(L'|', p3+1);
        auto p5 = s.find(L'|', p4+1);
        auto p6 = s.find(L'|', p5+1);
        SessionItem si;
        si.title = s.substr(0, p1);
        si.cls = s.substr(p1+1, p2-p1-1);
        si.alpha = _wtoi(s.substr(p2+1, p3-p2-1).c_str());
        si.effect = (p3 != s.npos) ? _wtoi(s.substr(p3+1, (p4 != s.npos) ? p4-p3-1 : s.npos).c_str()) : -1;
        si.radius = (p4 != s.npos) ? _wtoi(s.substr(p4+1, (p5 != s.npos) ? p5-p4-1 : s.npos).c_str()) : -1;
        si.tint = (p5 != s.npos) ? _wtoi(s.substr(p5+1, (p6 != s.npos) ? p6-p5-1 : s.npos).c_str()) : -1;
        si.circle = (p6 != s.npos) ? (_wtoi(s.substr(p6+1).c_str()) != 0) : false;
        g_sessionItems.push_back(si);
    }
}
HWND find_window_by_title_class(const wchar_t* title, const wchar_t* cls) {
    struct Ctx { const wchar_t* title; const wchar_t* cls; HWND found; } ctx = {title, cls, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t t[256], cl[64];
        GetWindowTextW(h, t, 255); GetClassNameW(h, cl, 63);
        if (wcscmp(t, c->title) == 0 && wcscmp(cl, c->cls) == 0) { c->found = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}
unsigned tint_argb();           // forward decl — defined in Acrylic Overlay section

void restore_session() {
    load_session_items();
    if (g_sessionItems.empty()) return;
    for (auto& si : g_sessionItems) {
        HWND h = find_window_by_title_class(si.title.c_str(), si.cls.c_str());
        if (!h) continue;
        EnterCriticalSection(&g_fxLock);
        g_modified.insert(h);
        g_winAlpha[h].alpha = si.alpha;              // v3.0: per-window params (radius/tint only
        if (si.radius >= 0) g_winAlpha[h].radius = si.radius;   // when the session recorded them)
        if (si.tint >= 0) g_winAlpha[h].tint = si.tint;
        g_winAlpha[h].circle = si.circle;
        if (si.effect > 0) g_winAlpha[h].mode = si.effect - 1;   // per-window mode (fix risk A)
        LeaveCriticalSection(&g_fxLock);
        if (si.effect != 2) {   // crisp windows are never made layered
            LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
            if (!(ex & WS_EX_LAYERED)) SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
            SetLayeredWindowAttributes(h, 0, (BYTE)si.alpha, LWA_ALPHA);
        }
        if (si.effect > 0) {   // effect: 1 = acrylic, 2 = crisp
            int mode = si.effect - 1;
            wchar_t cmd[512];
            if (mode == 1) {
                if (g_crispPath.empty()) g_crispPath = extract_resource(103, L"crisp_overlay.exe");
                int wash = 255 - max(0, min(255, si.alpha));
                if (wash <= 0 && !si.circle) continue;   // v3.0: circle peephole survives wash=0
                int radius = si.radius >= 0 ? si.radius : g_blurRadius;
                int tint = si.tint >= 0 ? si.tint : g_tintLevel;
                int circlePx = si.circle ? g_circleRadius : 0;
                wsprintfW(cmd, L"\"%s\" 0x%08X %d %d %d %d %d %d %d",
                          g_crispPath.c_str(), (DWORD)(ULONG_PTR)h, wash, tint, radius,
                          circlePx, g_circleBoost, g_circleBand, g_focusFactor);
            } else {
                if (g_overlayPath.empty()) g_overlayPath = extract_resource(101, L"acrylic_overlay.exe");
                int tint = (si.tint >= 0) ? si.tint : g_tintLevel;
                wsprintfW(cmd, L"\"%s\" 0x%08X 0x%08X", g_overlayPath.c_str(),
                          (DWORD)(ULONG_PTR)h, ((unsigned)tint * 255 / 100) << 24);
            }
            STARTUPINFOW si2 = {sizeof(si2)};
            si2.dwFlags = STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi)) {
                DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
                EnterCriticalSection(&g_fxLock);
                g_overlays[h] = pi;
                g_winPid[h] = pid;
                LeaveCriticalSection(&g_fxLock);
            }
        }
    }
    save_session(true);
}

// ==================== Transparency ====================
// Current opacity of a window (0-255). Crisp mode: tracked value, since the
// target window itself is never made layered. Acrylic mode: window attribute.
int current_alpha(HWND hwnd) {
    if (mode_of(hwnd) == 1) {          // per-window mode (fix risk A)
        EnterCriticalSection(&g_fxLock);
        int a = g_winAlpha.count(hwnd) ? g_winAlpha[hwnd].alpha : 255;
        LeaveCriticalSection(&g_fxLock);
        return a;
    }
    BYTE alpha = 255; DWORD flags;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED)
        GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags);
    if (alpha == 0) alpha = 255;
    return alpha;
}

void launch_overlay(HWND hwnd);  // forward decl — defined in Acrylic Overlay section below
void kill_overlay(HWND hwnd);   // forward decl — same
void guard_overlays();          // forward decl — same (crash guard, v3.0 task 8)
unsigned tint_argb();           // forward decl — same

void apply_transparency(HWND hwnd, int alpha) {
    EnterCriticalSection(&g_fxLock);
    g_winAlpha[hwnd].alpha = alpha;
    g_modified.insert(hwnd);
    bool hasOverlay = g_overlays.count(hwnd) != 0;
    LeaveCriticalSection(&g_fxLock);
    if (mode_of(hwnd) == 1) {          // per-window mode (fix risk A)
        // Crisp mode: target stays 100% opaque (text stays crisp) — the
        // "transparency" value becomes background wash strength instead.
        // Relaunch the overlay to apply the new wash level (or create one).
        if (hasOverlay) kill_overlay(hwnd);
        launch_overlay(hwnd);
        return;
    }
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
}

// v3.0: autofrost entry — apply one [Apps] profile to a window. -1 profile
// fields inherit globals (defaultAlpha / g_effectMode / g_blurRadius /
// g_tintLevel). mode override is session-scoped (NOT persisted). Per-window
// radius/tint/circle are stored for launch_overlay to build the overlay args.
void apply_profile(HWND hwnd, const AutoApp& app) {
    int alpha = app.alpha >= 0 ? app.alpha : g_auto.defaultAlpha;
    EnterCriticalSection(&g_fxLock);
    g_winAlpha[hwnd].alpha = alpha;
    if (app.mode >= 0) g_winAlpha[hwnd].mode = app.mode;   // per-window override (NOT global — fix risk A)
    if (app.radius >= 0) g_winAlpha[hwnd].radius = app.radius;
    if (app.tint >= 0) g_winAlpha[hwnd].tint = app.tint;
    g_winAlpha[hwnd].circle = app.circle;
    g_modified.insert(hwnd);
    LeaveCriticalSection(&g_fxLock);
    if (mode_of(hwnd) == 1) {
        if (g_overlays.count(hwnd)) kill_overlay(hwnd);
        launch_overlay(hwnd);
    } else {
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (!(ex & WS_EX_LAYERED)) SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
        // acrylic-profile windows keep the DWM acrylic overlay when blur is
        // wanted (radius is DWM-fixed — documented limit; profile radius>0
        // means "blur on", otherwise follow the defaultBlur switch)
        bool wantBlur = (app.radius > 0) || (app.radius < 0 && g_auto.defaultBlur);
        if (wantBlur) {
            if (g_overlays.count(hwnd)) kill_overlay(hwnd);
            launch_overlay(hwnd);
        }
    }
}

// ==================== Auto-frost monitor ====================
static const int AUTOFROST_POLL_MS = 10000; // matching cadence — was 800ms debug value; 10s = cheap, new windows blur within ≤10s
// v3.0 task 8: crash-guard cadence — own ~1s timer, independent of the
// autofrost matching poll (v3.1: heartbeat loop below keeps the guard at
// ~1s while the autofrost round sleeps the full AUTOFROST_POLL_MS).
static const int CRASHGUARD_POLL_MS = 1000;
static volatile LONG g_shuttingDown = 0;

std::wstring exe_name_of(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";
    wchar_t path[1024] = {};
    DWORD len = 1024;
    if (!QueryFullProcessImageNameW(proc, 0, path, &len)) { CloseHandle(proc); return L""; }
    CloseHandle(proc);
    wchar_t* base = wcsrchr(path, L'\\');
    return base ? base + 1 : path;
}

void prune_stale() {
    EnterCriticalSection(&g_fxLock);
    for (auto it = g_modified.begin(); it != g_modified.end(); ) {
        if (!IsWindow(*it)) { g_winAlpha.erase(*it); it = g_modified.erase(it); }  // v3.0: drop per-window params too
        else ++it;
    }
    for (auto it = g_autoApplied.begin(); it != g_autoApplied.end(); ) {
        if (!IsWindow(*it)) it = g_autoApplied.erase(it); else ++it;
    }
    // v2.8: kill overlays when the target is GONE (destroyed or HWND recycled).
    // v3.0 FIX (2026-08-08): do NOT kill on IsWindowVisible=false anymore —
    // crisp overlay's dual-hide capture briefly hides the target (~100ms), and
    // with 6-8 overlays the 800ms prune poll would periodically catch a target
    // mid-capture, kill its overlay, clear g_autoApplied/g_modified, and the
    // next autofrost round would re-apply + re-capture = "windows repeatedly
    // close and reopen" loop (user report). Hidden targets are handled by the
    // overlay itself (tick() hides its window when the target is hidden and
    // shows it again on EVENT_OBJECT_SHOW), so no state cleanup is needed here.
    for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
        HWND h = it->first;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        bool recycled = (g_winPid.count(h) != 0 && pid != g_winPid[h]);
        if (!IsWindow(h) || recycled) {
            TerminateProcess(it->second.hProcess, 0);
            CloseHandle(it->second.hProcess); CloseHandle(it->second.hThread);
            g_modified.erase(h); g_autoApplied.erase(h); g_winAlpha.erase(h);
            it = g_overlays.erase(it);
        } else ++it;
    }
    LeaveCriticalSection(&g_fxLock);
}

bool autofrost_match(const AutoApp& app, HWND hwnd) {
    std::wstring exe = exe_name_of(hwnd);
    if (exe.empty()) return false;
    if (_wcsicmp(exe.c_str(), app.exe.c_str()) != 0) return false;
    if (app.cls.empty()) return true;
    wchar_t cls[64];
    GetClassNameW(hwnd, cls, 63);
    // case-insensitive: parse_profile_line lowercases every profile token,
    // including cls= values (e.g. "cls=CabinetWClass" -> "cabinetwclass")
    return _wcsicmp(cls, app.cls.c_str()) == 0;
}

// Reload config only when the file changed (mtime) — cheap check each round, full ini read on change
void load_cfg_if_changed() {
    static FILETIME g_cfgTime = {0, 0};
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!GetFileAttributesExW(config_path().c_str(), GetFileExInfoStandard, &fad)) return;
    if (fad.ftLastWriteTime.dwHighDateTime == g_cfgTime.dwHighDateTime &&
        fad.ftLastWriteTime.dwLowDateTime == g_cfgTime.dwLowDateTime) return;
    g_cfgTime = fad.ftLastWriteTime;
    load_autofrost();
}

// Revert windows the monitor auto-applied that are no longer in the list (or autofrost disabled)
void revert_auto_applied(const AutoFrostConfig& cfg) {
    EnterCriticalSection(&g_fxLock);
    std::vector<HWND> toRevert;
    for (HWND h : g_autoApplied) {
        if (!IsWindow(h)) continue;
        if (!cfg.enabled) { toRevert.push_back(h); continue; }
        std::wstring exe = exe_name_of(h);   // few windows — short lock hold
        bool matched = false;
        for (auto& app : cfg.apps)
            if (_wcsicmp(exe.c_str(), app.exe.c_str()) == 0) { matched = true; break; }
        if (!matched) toRevert.push_back(h);
    }
    for (HWND h : toRevert) { g_autoApplied.erase(h); g_modified.erase(h); g_winAlpha.erase(h); }
    LeaveCriticalSection(&g_fxLock);
    for (HWND h : toRevert) {
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) { SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA); SetWindowLongW(h, GWL_EXSTYLE, ex & ~WS_EX_LAYERED); }
        kill_overlay(h);
    }
}

DWORD WINAPI autofrost_thread(LPVOID) {
    // v3.0 task 8: crash-guard runs on its own ~1s timer (independent of the
    // autofrost matching cadence — it must keep firing if autofrost is off).
    // v3.1: the autofrost round fires every AUTOFROST_POLL_MS (10s); the 500ms
    // heartbeat keeps the crash guard at ~1s so a dead overlay is restarted
    // promptly while the expensive EnumWindows round sleeps long.
    DWORD lastGuard = GetTickCount();
    DWORD nextMatch = GetTickCount();       // first round runs right away
    for (;;) {
        Sleep(500);
        if (g_shuttingDown) break;
        DWORD now = GetTickCount();
        if (now - lastGuard >= CRASHGUARD_POLL_MS) {
            lastGuard = now;
            guard_overlays();
        }
        if ((int)(now - nextMatch) < 0) continue;   // wrap-safe: not due yet
        nextMatch = now + AUTOFROST_POLL_MS;
        prune_stale();
        EnterCriticalSection(&g_fxLock);
        load_cfg_if_changed();          // config file touched (Apply) → picked up next round
        AutoFrostConfig cfg = g_auto;   // snapshot under lock
        LeaveCriticalSection(&g_fxLock);
        revert_auto_applied(cfg);
        if (!cfg.enabled) continue;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            if (g_shuttingDown) return FALSE;
            if (!IsWindowVisible(h)) return TRUE;
            if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
            if (GetWindowTextLengthW(h) == 0) return TRUE;
            auto* cfg = (AutoFrostConfig*)lp;
            for (auto& app : cfg->apps) {
                if (!autofrost_match(app, h)) continue;
                EnterCriticalSection(&g_fxLock);
                bool done = g_modified.count(h) != 0;
                LeaveCriticalSection(&g_fxLock);
                if (!done) {
                    apply_profile(h, app);          // v3.0: profile fields, not bare defaults
                    EnterCriticalSection(&g_fxLock);
                    g_autoApplied.insert(h);
                    LeaveCriticalSection(&g_fxLock);
                    debug_log(L"autofrost_apply hwnd=0x%08X exe=%s", (unsigned)(ULONG_PTR)h, exe_name_of(h).c_str());
                }
                break;
            }
            return TRUE;
        }, (LPARAM)&cfg);
    }
    return 0;
}

// ==================== Acrylic Overlay ====================
unsigned tint_argb() {
    int t = g_tintLevel; if (t < 0) t = 0; if (t > 100) t = 100;
    return ((unsigned)t * 255 / 100) << 24;   // black level; 0 = pure blur
}

void launch_overlay(HWND hwnd) {
    // v2.8: effect mode selects the overlay exe and its arguments.
    // v3.0: crisp gets the full 8-arg cmd — radius/tint/circle resolve per
    // window (profile values; -1/unset falls back to the global sliders).
    wchar_t cmd[1024];
    int alpha = 255;
    int radius = g_blurRadius, tint = g_tintLevel, circlePx = 0;
    EnterCriticalSection(&g_fxLock);
    bool dup = g_overlays.count(hwnd) != 0;
    if (g_winAlpha.count(hwnd)) {
        alpha = g_winAlpha[hwnd].alpha;
        if (g_winAlpha[hwnd].radius >= 0) radius = g_winAlpha[hwnd].radius;
        if (g_winAlpha[hwnd].tint >= 0) tint = g_winAlpha[hwnd].tint;
        if (g_winAlpha[hwnd].circle) circlePx = g_circleRadius;
    }
    LeaveCriticalSection(&g_fxLock);
    if (dup) return;
    if (mode_of(hwnd) == 1) {          // per-window mode (fix risk A)
        if (g_crispPath.empty()) g_crispPath = extract_resource(103, L"crisp_overlay.exe");
        int wash = 255 - max(0, min(255, alpha));          // opacity -> wash strength
        // v3.0 peephole: wash=0 (alpha=255) still launches when the circle is
        // enabled — the overlay then renders per-pixel alpha (mask only), so
        // the window shows 100% original outside the circle.
        if (wash <= 0 && circlePx <= 0) return;             // fully opaque, no circle: nothing to render
        // CLI order: <hwnd> <wash> <tint> <radius> <circle_px> <boost_x100> <band_px> <focus_pct>
        wsprintfW(cmd, L"\"%s\" 0x%08X %d %d %d %d %d %d %d",
                  g_crispPath.c_str(), (DWORD)(ULONG_PTR)hwnd, wash, tint, radius,
                  circlePx, g_circleBoost, g_circleBand, g_focusFactor);
    } else {
        if (g_overlayPath.empty()) g_overlayPath = extract_resource(101, L"acrylic_overlay.exe");
        wsprintfW(cmd, L"\"%s\" 0x%08X 0x%08X", g_overlayPath.c_str(),
                  (DWORD)(ULONG_PTR)hwnd, ((unsigned)tint * 255 / 100) << 24);
    }
    STARTUPINFOW si = {sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        EnterCriticalSection(&g_fxLock);
        auto it = g_overlays.find(hwnd);
        if (it != g_overlays.end()) {
            LeaveCriticalSection(&g_fxLock);
            TerminateProcess(pi.hProcess, 0);   // 并发启动者先到 — 杀自己刚建的
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        } else {
            g_overlays[hwnd] = pi;
            g_winPid[hwnd] = pid;               // record PID for recycling guard
            LeaveCriticalSection(&g_fxLock);
        }
    }
}
void kill_overlay(HWND hwnd) {
    EnterCriticalSection(&g_fxLock);
    auto it = g_overlays.find(hwnd);
    if (it == g_overlays.end()) { LeaveCriticalSection(&g_fxLock); return; }
    PROCESS_INFORMATION pi = it->second;
    g_overlays.erase(it);
    LeaveCriticalSection(&g_fxLock);
    // Find THIS overlay's window by PID (multi-overlay safe — FindWindowW by
    // class returns the FIRST matching overlay globally; with several alive it
    // would WM_CLOSE the wrong one, whose "crash" then fed the crash-guard
    // relaunch loop and cascaded across all windows).
    HWND ov = nullptr;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD wpid = 0; GetWindowThreadProcessId(h, &wpid);
        if (wpid == (DWORD)lp) { *reinterpret_cast<HWND*>(lp) = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ov));
    if (ov) { PostMessageW(ov, WM_CLOSE, 0, 0); DWORD w = WaitForSingleObject(pi.hProcess, 500);
        if (w == WAIT_OBJECT_0) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return; }
        DestroyWindow(ov); }
    // Hard-kill path: if the overlay died while dual-hiding the target during
    // capture, the target may be stuck hidden — restore it before killing.
    if (IsWindow(hwnd) && !IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
}

// ==================== Crash guard (v3.0 task 8) ====================
void tray_balloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    nid.szInfoTitle[0] = 0; wcsncpy(nid.szInfoTitle, title, 63); nid.szInfoTitle[63] = 0;
    nid.szInfo[0] = 0; wcsncpy(nid.szInfo, text, 127); nid.szInfo[127] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// Monitor pass for dead overlay processes: relaunch, capped at 3 restarts per
// 30s per window (cap trips -> tray balloon + no relaunch; count resets after
// the 30s window). Runs on the autofrost thread at CRASHGUARD_POLL_MS. Dead
// list is collected under g_fxLock, actions outside it (g_fxLock discipline).
void guard_overlays() {
    DWORD now = GetTickCount();
    std::vector<HWND> dead;
    EnterCriticalSection(&g_fxLock);
    for (auto& kv : g_overlays) {
        if (WaitForSingleObject(kv.second.hProcess, 0) == WAIT_OBJECT_0) dead.push_back(kv.first);
    }
    LeaveCriticalSection(&g_fxLock);
    for (HWND h : dead) {
        // Re-verify under lock: a hotkey/settings relaunch may have replaced
        // the dead process between collection and here — don't kill the fresh one.
        EnterCriticalSection(&g_fxLock);
        auto it = g_overlays.find(h);
        bool stillDead = (it != g_overlays.end()) &&
                         (WaitForSingleObject(it->second.hProcess, 0) == WAIT_OBJECT_0);
        LeaveCriticalSection(&g_fxLock);
        if (!stillDead) continue;
        kill_overlay(h);   // erases map entries + closes handles
        if (g_shuttingDown || !IsWindow(h)) continue;
        auto& rc = g_restartCount[h];
        if (rc.second == 0) rc.second = now;
        if (rc.first >= 3 && now - rc.second < 30000) {
            tray_balloon(L"overlay crash loop", L"overlay crashed 3x in 30s — stopped");
            debug_log(L"crashguard cap hit hwnd=0x%08X — relaunches stopped", (unsigned)(ULONG_PTR)h);
            g_restartCount.erase(h);
            // v3.0 fix: the crashed overlay may have died mid-dual-hide, leaving
            // the target stuck hidden ("software terminated" ghost symptom) —
            // no relaunch will come to restore it, so restore it here.
            if (IsWindow(h) && !IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
            continue;
        }
        if (now - rc.second >= 30000) rc = {0, now};
        rc.first++;
        debug_log(L"crashguard relaunch hwnd=0x%08X attempt=%d", (unsigned)(ULONG_PTR)h, rc.first);
        launch_overlay(h);
    }
}
void toggle_acrylic(HWND hwnd) {
    EnterCriticalSection(&g_fxLock);
    bool has = g_overlays.count(hwnd) != 0;
    LeaveCriticalSection(&g_fxLock);
    if (has) kill_overlay(hwnd); else launch_overlay(hwnd);
}

// ==================== Hotkeys ====================
void on_hotkey(int id) {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return;
    int alpha = current_alpha(hwnd);
    int step = g_step * 255 / 100;
    switch (id) {
    case ID_UP: alpha = (BYTE)max(10, (int)alpha - step); apply_transparency(hwnd, alpha); break;
    case ID_DOWN: alpha = (BYTE)min(255, (int)alpha + step); apply_transparency(hwnd, alpha); break;
    case ID_TOGGLE:
        if (alpha < 255) { g_lastAlpha[hwnd] = alpha; apply_transparency(hwnd, 255); }
        else { int last = g_lastAlpha.count(hwnd) ? g_lastAlpha[hwnd] : 255 - step;
               if (last >= 255) last = 255 - step; apply_transparency(hwnd, last); }
        break;
    case ID_ACRYLIC: toggle_acrylic(hwnd); break;
    }
}

// ==================== Settings Window ====================
struct SettingsParams { int* pStep; int* pBlur; bool hasBlur; AutoFrostConfig* pAuto; HWND fgWindow;
                        int* pEffect; int* pTint;
                        // v3.0: circle/focus launch params (crisp overlay)
                        int* pCircleRadius; int* pCircleBoost; int* pCircleBand; int* pFocusFactor; };

// Alpha slider label depends on mode: acrylic = window opacity, crisp = bg wash
static void relabel_alpha(HWND hTitle, HWND hTrack, int mode) {
    if (!hTitle || !hTrack) return;
    int v = (int)SendMessageW(hTrack, TBM_GETPOS, 0, 0);
    wchar_t b[64];
    if (mode == 1) wsprintfW(b, L"Background wash: %d%%", 100 - v);
    else wsprintfW(b, L"Default transparency: %d%%", v);
    SetWindowTextW(hTitle, b);
}

void refill_app_list(HWND hList, AutoFrostConfig* p) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (auto& a : p->apps) {
        std::wstring s = a.exe + (a.cls.empty() ? L"" : (L"  (" + a.cls + L")"));
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
}

// Status line under the app list — always shows what "Add current" would capture
static void status_show_target(HWND hStatus, HWND target) {
    if (!hStatus) return;
    std::wstring exe = target ? exe_name_of(target) : L"";
    if (exe.empty()) SetWindowTextW(hStatus, L"Add current: (none - activate your app)");
    else SetWindowTextW(hStatus, (std::wstring(L"Add current: ") + exe).c_str());
}

// v3.0: settings dialog state lives at file scope so the profile-editor
// helpers below can share it with SettingsWndProc (reset in WM_DESTROY).
static HWND hTrack = nullptr, hStepTitle = nullptr;
static HWND hBlurTrack = nullptr, hBlurTitle = nullptr;
static HWND hAlphaTrack = nullptr, hAlphaTitle = nullptr, hEnableCk = nullptr, hBlurCk = nullptr, hAppList = nullptr;
static HWND hModeAcrylic = nullptr, hModeCrisp = nullptr;
static HWND hTintTrack = nullptr, hTintTitle = nullptr;
static HWND hStatus = nullptr;          // feedback line under the app list
static HWND s_lastActive = nullptr;     // last non-own window the user activated (Add current target)
// v3.0: circle/focus global sliders + per-app profile editor (right column)
static HWND hCrTitle = nullptr, hCbTitle = nullptr, hCbandTitle = nullptr, hFocusTitle = nullptr;
static HWND hCircleRadiusTrack = nullptr, hCircleBoostTrack = nullptr, hCircleBandTrack = nullptr, hFocusTrack = nullptr;
static HWND hModeCombo = nullptr, hAlphaPTrack = nullptr, hRadiusPTrack = nullptr, hTintPTrack = nullptr;
static HWND hAlphaPTitle = nullptr, hRadiusPTitle = nullptr, hTintPTitle = nullptr;
static HWND hCircleCk = nullptr, hInheritCk = nullptr;
static SettingsParams* p = nullptr;

// Editor controls are meaningful only when "Inherit global" is unchecked
// (checked = the whole profile resolves to the global values on Save).
static void settings_set_editor_enabled(bool en) {
    EnableWindow(hModeCombo, en);
    EnableWindow(hAlphaPTrack, en);
    EnableWindow(hRadiusPTrack, en);
    EnableWindow(hTintPTrack, en);
    EnableWindow(hCircleCk, en);
}
// v3.0: refresh the three profile-editor labels from the current trackbar
// positions. TBM_SETPOS fires no WM_HSCROLL to the parent, so populate and
// scroll share this one formatting path — labels would otherwise stay blank
// (first selection) or keep the previous app's values.
static void settings_refresh_editor_labels() {
    wchar_t b[64];
    int v = (int)SendMessageW(hAlphaPTrack, TBM_GETPOS, 0, 0);
    wsprintfW(b, L"Profile alpha: %d", v); SetWindowTextW(hAlphaPTitle, b);
    v = (int)SendMessageW(hRadiusPTrack, TBM_GETPOS, 0, 0);
    wsprintfW(b, L"Profile radius: %d px", v); SetWindowTextW(hRadiusPTitle, b);
    v = (int)SendMessageW(hTintPTrack, TBM_GETPOS, 0, 0);
    wsprintfW(b, L"Profile tint: %d%%", v); SetWindowTextW(hTintPTitle, b);
}
// v3.0: editor with no valid selection — controls inert and stale state
// cleared (Inherit unchecked, status back to the "Add current" default).
static void settings_disable_editor() {
    settings_set_editor_enabled(false);
    SendMessageW(hInheritCk, BM_SETCHECK, BST_UNCHECKED, 0);
    status_show_target(hStatus, s_lastActive);
}
// v3.0: push the selected app's profile fields into the editor controls.
// -1 fields (inherit) display the current global value; the "Inherit global"
// checkbox is checked only when the whole profile is pure inherit.
static void settings_populate_editor() {
    if (!p) return;
    int sel = (int)SendMessageW(hAppList, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)p->pAuto->apps.size()) { settings_disable_editor(); return; }
    AutoApp& a = p->pAuto->apps[sel];
    bool inh = (a.mode < 0 && a.alpha < 0 && a.radius < 0 && a.tint < 0 && !a.circle);
    SendMessageW(hInheritCk, BM_SETCHECK, inh ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(hModeCombo, CB_SETCURSEL, a.mode >= 0 ? a.mode : *p->pEffect, 0);
    int alpha = a.alpha >= 0 ? a.alpha : p->pAuto->defaultAlpha;
    SendMessageW(hAlphaPTrack, TBM_SETPOS, TRUE, alpha);
    int radius = a.radius >= 0 ? a.radius : *p->pBlur;
    SendMessageW(hRadiusPTrack, TBM_SETPOS, TRUE, radius);
    int tint = a.tint >= 0 ? a.tint : *p->pTint;
    SendMessageW(hTintPTrack, TBM_SETPOS, TRUE, tint);
    settings_refresh_editor_labels();   // TBM_SETPOS fires no WM_HSCROLL — labels set here
    SendMessageW(hCircleCk, BM_SETCHECK, a.circle ? BST_CHECKED : BST_UNCHECKED, 0);
    settings_set_editor_enabled(!inh);
    SetWindowTextW(hStatus, (std::wstring(L"Editing profile: ") + a.exe).c_str());
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp; p = (SettingsParams*)cs->lpCreateParams;
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        wchar_t buf[64];
        // Value shown in the title static — right column (x>=200) is reserved for Apply/Close buttons
        hStepTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 12, 200, 20, hwnd, nullptr, hi, nullptr);
        wsprintfW(buf, L"Transparency step: %d%%", *p->pStep); SetWindowTextW(hStepTitle, buf);
        hTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 12, 32, 200, 28, hwnd, (HMENU)101, hi, nullptr);
        SendMessageW(hTrack, TBM_SETRANGE, TRUE, MAKELONG(1, 20));
        SendMessageW(hTrack, TBM_SETPOS, TRUE, *p->pStep);
        int y = 70;
        if (p->hasBlur) {
            hBlurTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, y, 200, 20, hwnd, nullptr, hi, nullptr);
            y += 20;
            hBlurTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 12, y, 200, 28, hwnd, (HMENU)102, hi, nullptr);
            SendMessageW(hBlurTrack, TBM_SETRANGE, TRUE, MAKELONG(0, g_blurPresetCount - 1));
            int curIdx = 0;
            for (int i = 0; i < g_blurPresetCount; i++) {
                if (g_blurPresets[i] == *p->pBlur) { curIdx = i; break; }
                if (g_blurPresets[i] < *p->pBlur) curIdx = i;
            }
            SendMessageW(hBlurTrack, TBM_SETPOS, TRUE, curIdx);
            wsprintfW(buf, L"Blur radius: %d px", g_blurPresets[curIdx]); SetWindowTextW(hBlurTitle, buf);
            y += 32;
        }
        // v2.8: effect mode + tint (black level; 0 = pure blur — fixes old
        // "cannot set tint to 0" gap where the tint cycle had no 0 entry)
        static HWND hModeTitle = nullptr, hAutoTitle = nullptr;   // section headers (bold)
        hModeTitle = CreateWindowW(L"STATIC", L"Effect mode", WS_CHILD | WS_VISIBLE, 12, y + 8, 150, 20, hwnd, nullptr, hi, nullptr);
        hModeAcrylic = CreateWindowW(L"BUTTON", L"Acrylic (DWM)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 12, y + 28, 110, 20, hwnd, (HMENU)108, hi, nullptr);
        hModeCrisp = CreateWindowW(L"BUTTON", L"Crisp (readable)", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON, 128, y + 28, 130, 20, hwnd, (HMENU)109, hi, nullptr);
        SendMessageW(hModeAcrylic, BM_SETCHECK, *p->pEffect == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(hModeCrisp, BM_SETCHECK, *p->pEffect == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
        hTintTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, y + 50, 220, 20, hwnd, nullptr, hi, nullptr);
        hTintTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 12, y + 68, 200, 28, hwnd, (HMENU)110, hi, nullptr);
        SendMessageW(hTintTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(hTintTrack, TBM_SETTICFREQ, 10, 0);   // scale every 10%
        SendMessageW(hTintTrack, TBM_SETPAGESIZE, 0, 10);  // coarse steps (user: no need for fine precision)
        SendMessageW(hTintTrack, TBM_SETPOS, TRUE, *p->pTint);
        wsprintfW(buf, L"Tint (black level): %d%%", *p->pTint); SetWindowTextW(hTintTitle, buf);
        y += 96;
        // v3.0: circle/focus + per-app profile editor live in the right column
        // (x=282+, clear of the Apply/Close buttons at x 200-260 and the Add
        // current/Remove buttons at x 210-278) so the dialog only grows in
        // width, not in height.
        static HWND hCfgTitle = nullptr, hEditTitle = nullptr;   // section headers (bold)
        hCfgTitle = CreateWindowW(L"STATIC", L"Circle & focus", WS_CHILD | WS_VISIBLE, 282, 24, 170, 20, hwnd, nullptr, hi, nullptr);
        hCrTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 282, 46, 170, 20, hwnd, nullptr, hi, nullptr);
        hCircleRadiusTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 282, 66, 165, 28, hwnd, (HMENU)120, hi, nullptr);
        SendMessageW(hCircleRadiusTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 200));   // 0 = off
        SendMessageW(hCircleRadiusTrack, TBM_SETTICFREQ, 20, 0);
        SendMessageW(hCircleRadiusTrack, TBM_SETPAGESIZE, 0, 10);  // coarse steps
        SendMessageW(hCircleRadiusTrack, TBM_SETPOS, TRUE, *p->pCircleRadius);
        wsprintfW(buf, *p->pCircleRadius ? L"Circle radius: %d px" : L"Circle radius: 0 px (off)", *p->pCircleRadius);
        SetWindowTextW(hCrTitle, buf);
        hCbTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 282, 96, 170, 20, hwnd, nullptr, hi, nullptr);
        hCircleBoostTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 282, 116, 165, 28, hwnd, (HMENU)121, hi, nullptr);
        SendMessageW(hCircleBoostTrack, TBM_SETRANGE, TRUE, MAKELONG(100, 400));
        SendMessageW(hCircleBoostTrack, TBM_SETTICFREQ, 30, 0);
        SendMessageW(hCircleBoostTrack, TBM_SETPAGESIZE, 0, 15);  // coarse steps
        SendMessageW(hCircleBoostTrack, TBM_SETPOS, TRUE, *p->pCircleBoost);
        wsprintfW(buf, L"Circle boost: %d%%", *p->pCircleBoost); SetWindowTextW(hCbTitle, buf);
        hCbandTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 282, 146, 170, 20, hwnd, nullptr, hi, nullptr);
        hCircleBandTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 282, 166, 165, 28, hwnd, (HMENU)122, hi, nullptr);
        SendMessageW(hCircleBandTrack, TBM_SETRANGE, TRUE, MAKELONG(5, 100));
        SendMessageW(hCircleBandTrack, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hCircleBandTrack, TBM_SETPAGESIZE, 0, 10);  // coarse steps
        SendMessageW(hCircleBandTrack, TBM_SETPOS, TRUE, *p->pCircleBand);
        wsprintfW(buf, L"Circle band: %d px", *p->pCircleBand); SetWindowTextW(hCbandTitle, buf);
        hFocusTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 282, 196, 170, 20, hwnd, nullptr, hi, nullptr);
        hFocusTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 282, 216, 165, 28, hwnd, (HMENU)123, hi, nullptr);
        SendMessageW(hFocusTrack, TBM_SETRANGE, TRUE, MAKELONG(10, 100));
        SendMessageW(hFocusTrack, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hFocusTrack, TBM_SETPAGESIZE, 0, 10);  // coarse steps
        SendMessageW(hFocusTrack, TBM_SETPOS, TRUE, *p->pFocusFactor);
        wsprintfW(buf, L"Focus factor: %d%%", *p->pFocusFactor); SetWindowTextW(hFocusTitle, buf);
        // per-app profile editor: mode combo + alpha/radius/tint + circle.
        // radius range is 1-120 (Task 1 constraint — 0 would parse as -1
        // "inherit" and per-app "no blur" is unexpressible by design).
        hEditTitle = CreateWindowW(L"STATIC", L"Profile editor", WS_CHILD | WS_VISIBLE, 282, 248, 170, 20, hwnd, nullptr, hi, nullptr);
        hModeCombo = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 282, 270, 165, 200, hwnd, (HMENU)124, hi, nullptr);
        SendMessageW(hModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Acrylic (DWM)");
        SendMessageW(hModeCombo, CB_ADDSTRING, 0, (LPARAM)L"Crisp (readable)");
        SendMessageW(hModeCombo, CB_SETCURSEL, 0, 0);
        hInheritCk = CreateWindowW(L"BUTTON", L"Inherit global", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 282, 296, 170, 20, hwnd, (HMENU)130, hi, nullptr);
        hAlphaPTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 282, 318, 170, 20, hwnd, nullptr, hi, nullptr);
        hAlphaPTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 282, 338, 165, 28, hwnd, (HMENU)125, hi, nullptr);
        SendMessageW(hAlphaPTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 255));
        SendMessageW(hAlphaPTrack, TBM_SETTICFREQ, 15, 0);
        SendMessageW(hAlphaPTrack, TBM_SETPAGESIZE, 0, 15);  // coarse steps
        hRadiusPTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 282, 368, 170, 20, hwnd, nullptr, hi, nullptr);
        hRadiusPTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 282, 388, 165, 28, hwnd, (HMENU)126, hi, nullptr);
        SendMessageW(hRadiusPTrack, TBM_SETRANGE, TRUE, MAKELONG(1, 120));
        SendMessageW(hRadiusPTrack, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hRadiusPTrack, TBM_SETPAGESIZE, 0, 10);  // coarse steps
        hTintPTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 282, 418, 170, 20, hwnd, nullptr, hi, nullptr);
        hTintPTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 282, 438, 165, 28, hwnd, (HMENU)127, hi, nullptr);
        SendMessageW(hTintPTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
        SendMessageW(hTintPTrack, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hTintPTrack, TBM_SETPAGESIZE, 0, 10);  // coarse steps
        hCircleCk = CreateWindowW(L"BUTTON", L"Circle follow", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 282, 468, 120, 20, hwnd, (HMENU)128, hi, nullptr);
        // Save sits on its own row — the 165px column cannot hold checkbox +
        // button side by side without them overlapping.
        CreateWindowW(L"BUTTON", L"Save to profile", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 282, 492, 98, 24, hwnd, (HMENU)129, hi, nullptr);
        settings_disable_editor();   // nothing selected yet (hStatus not created yet — helper null-guards)
        hAutoTitle = CreateWindowW(L"STATIC", L"Auto-frost", WS_CHILD | WS_VISIBLE, 12, y + 8, 150, 20, hwnd, nullptr, hi, nullptr);
        hEnableCk = CreateWindowW(L"BUTTON", L"Enable auto-frost", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, y + 28, 150, 20, hwnd, (HMENU)104, hi, nullptr);
        SendMessageW(hEnableCk, BM_SETCHECK, p->pAuto->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        hAlphaTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, y + 50, 200, 20, hwnd, nullptr, hi, nullptr);
        hAlphaTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 12, y + 68, 200, 28, hwnd, (HMENU)103, hi, nullptr);
        SendMessageW(hAlphaTrack, TBM_SETRANGE, TRUE, MAKELONG(50, 100));
        SendMessageW(hAlphaTrack, TBM_SETTICFREQ, 10, 0);
        SendMessageW(hAlphaTrack, TBM_SETPAGESIZE, 0, 10);  // coarse steps   // scale every 10%
        int pct = (p->pAuto->defaultAlpha * 100 + 127) / 255;
        SendMessageW(hAlphaTrack, TBM_SETPOS, TRUE, pct);
        relabel_alpha(hAlphaTitle, hAlphaTrack, *p->pEffect);
        hBlurCk = CreateWindowW(L"BUTTON", L"Blur", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, y + 100, 100, 20, hwnd, (HMENU)105, hi, nullptr);
        SendMessageW(hBlurCk, BM_SETCHECK, p->pAuto->defaultBlur ? BST_CHECKED : BST_UNCHECKED, 0);
        hAppList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 12, y + 126, 190, 96, hwnd, nullptr, hi, nullptr);   // v3.0: LBS_NOTIFY so LBN_SELCHANGE feeds the profile editor
        refill_app_list(hAppList, p->pAuto);
        CreateWindowW(L"BUTTON", L"Add current", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, y + 126, 68, 24, hwnd, (HMENU)106, hi, nullptr);
        CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, y + 156, 68, 24, hwnd, (HMENU)107, hi, nullptr);
        hStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, y + 226, 260, 20, hwnd, nullptr, hi, nullptr);
        y += 226;
        CreateWindowW(L"STATIC", L"ALT+LEFT  more transparent\r\nALT+RIGHT less transparent\r\nALT+UP    toggle on/off\r\nALT+DOWN  toggle blur mode", WS_CHILD | WS_VISIBLE, 12, y + 22, 240, 80, hwnd, nullptr, hi, nullptr);
        // "Add current" target = the window the user last activated that is not our own
        s_lastActive = p->fgWindow;
        DWORD ownPid = 0;
        if (!s_lastActive || (GetWindowThreadProcessId(s_lastActive, &ownPid) && ownPid == GetCurrentProcessId()))
            s_lastActive = nullptr;
        status_show_target(hStatus, s_lastActive);
        // v3.0 layout: Apply/Close bottom-right, flush with the bottom edge
        // (Windows settings-dialog convention), not top-right.
        RECT cr; GetClientRect(hwnd, &cr);
        int by = cr.bottom - 24 - 8;               // button row, 8px above bottom
        int bx = cr.right - 60 - 8;                // Close at far right
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx, by, 60, 24, hwnd, (HMENU)IDCANCEL, hi, nullptr);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, bx - 68, by, 60, 24, hwnd, (HMENU)IDOK, hi, nullptr);
        HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Candara");
        if (hFont) EnumChildWindows(hwnd, [](HWND h, LPARAM l) -> BOOL { SendMessageW(h, WM_SETFONT, (WPARAM)l, TRUE); return TRUE; }, (LPARAM)hFont);
        // bold section headers for hierarchy (Candara Bold 15)
        HFONT hBold = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Candara");
        if (hBold) { SendMessageW(hModeTitle, WM_SETFONT, (WPARAM)hBold, TRUE); SendMessageW(hAutoTitle, WM_SETFONT, (WPARAM)hBold, TRUE);
            SendMessageW(hCfgTitle, WM_SETFONT, (WPARAM)hBold, TRUE); SendMessageW(hEditTitle, WM_SETFONT, (WPARAM)hBold, TRUE); }
        return 0;
    }
    case WM_ACTIVATE:
        // lParam = window being deactivated; when the user clicks another app and comes
        // back, this tracks which app they were working with -> "Add current" target
        if (LOWORD(wp) == WA_ACTIVE) {
            HWND deact = (HWND)lp;
            DWORD pid = 0;
            if (deact && GetWindowThreadProcessId(deact, &pid) && pid != GetCurrentProcessId())
                s_lastActive = deact;
            status_show_target(hStatus, s_lastActive);
        }
        return 0;
    case WM_HSCROLL: {
        int id = GetDlgCtrlID((HWND)lp);
        if (id == 101) { int v = (int)SendMessageW(hTrack, TBM_GETPOS, 0, 0); wchar_t b[64];
            wsprintfW(b, L"Transparency step: %d%%", v); SetWindowTextW(hStepTitle, b); }
        else if (id == 102 && hBlurTrack) { int idx = (int)SendMessageW(hBlurTrack, TBM_GETPOS, 0, 0);
            if (idx >= 0 && idx < g_blurPresetCount) { wchar_t b[64];
                wsprintfW(b, L"Blur radius: %d px", g_blurPresets[idx]); SetWindowTextW(hBlurTitle, b); } }
        else if (id == 103 && hAlphaTrack) relabel_alpha(hAlphaTitle, hAlphaTrack, *p->pEffect);
        else if (id == 110 && hTintTrack) { int t = (int)SendMessageW(hTintTrack, TBM_GETPOS, 0, 0);
            wchar_t b[64]; wsprintfW(b, L"Tint (black level): %d%%", t);
            SetWindowTextW(hTintTitle, b); }
        // v3.0: circle/focus global sliders + profile editor trackbars
        else if (id == 120 && hCircleRadiusTrack) { int v = (int)SendMessageW(hCircleRadiusTrack, TBM_GETPOS, 0, 0);
            wchar_t b[64]; wsprintfW(b, v ? L"Circle radius: %d px" : L"Circle radius: 0 px (off)", v);
            SetWindowTextW(hCrTitle, b); }
        else if (id == 121 && hCircleBoostTrack) { int v = (int)SendMessageW(hCircleBoostTrack, TBM_GETPOS, 0, 0);
            wchar_t b[64]; wsprintfW(b, L"Circle boost: %d%%", v); SetWindowTextW(hCbTitle, b); }
        else if (id == 122 && hCircleBandTrack) { int v = (int)SendMessageW(hCircleBandTrack, TBM_GETPOS, 0, 0);
            wchar_t b[64]; wsprintfW(b, L"Circle band: %d px", v); SetWindowTextW(hCbandTitle, b); }
        else if (id == 123 && hFocusTrack) { int v = (int)SendMessageW(hFocusTrack, TBM_GETPOS, 0, 0);
            wchar_t b[64]; wsprintfW(b, L"Focus factor: %d%%", v); SetWindowTextW(hFocusTitle, b); }
        else if ((id == 125 && hAlphaPTrack) || (id == 126 && hRadiusPTrack) || (id == 127 && hTintPTrack))
            settings_refresh_editor_labels();   // shared with populate — one formatting path
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int v = (int)SendMessageW(hTrack, TBM_GETPOS, 0, 0);
            if (v >= 1 && v <= 20) { *p->pStep = v; g_step = v; write_int(L"TransparencyStep", v); }
            if (p->hasBlur && hBlurTrack) {
                int idx = (int)SendMessageW(hBlurTrack, TBM_GETPOS, 0, 0);
                if (idx >= 0 && idx < g_blurPresetCount) set_blur_radius(idx);
            }
            EnterCriticalSection(&g_fxLock);
            p->pAuto->enabled = SendMessageW(hEnableCk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            p->pAuto->defaultBlur = SendMessageW(hBlurCk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int pct2 = (int)SendMessageW(hAlphaTrack, TBM_GETPOS, 0, 0);
            if (pct2 >= 50 && pct2 <= 100) p->pAuto->defaultAlpha = pct2 * 255 / 100;
            save_autofrost();
            LeaveCriticalSection(&g_fxLock);
            // v2.8: effect mode / tint changed -> write config + relaunch every
            // overlay under the new mode. g_winAlpha preserves per-window
            // opacity across mode switches (crisp strips WS_EX_LAYERED so the
            // target renders natively again — ClearType text stays sharp).
            int mode = SendMessageW(hModeCrisp, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
            int tint = (int)SendMessageW(hTintTrack, TBM_GETPOS, 0, 0);
            if (tint < 0) tint = 0; if (tint > 100) tint = 100;
            // v3.0: circle/focus launch params — persist, and treat any change
            // like a mode/tint change so crisp overlays relaunch with the new
            // args (launch_overlay reads the globals below).
            int cr = (int)SendMessageW(hCircleRadiusTrack, TBM_GETPOS, 0, 0);
            int cb = (int)SendMessageW(hCircleBoostTrack, TBM_GETPOS, 0, 0);
            int cband = (int)SendMessageW(hCircleBandTrack, TBM_GETPOS, 0, 0);
            int ff = (int)SendMessageW(hFocusTrack, TBM_GETPOS, 0, 0);
            bool circleChanged = (cr != g_circleRadius || cb != g_circleBoost ||
                                  cband != g_circleBand || ff != g_focusFactor);
            if (circleChanged) {
                g_circleRadius = cr; write_int(L"CircleRadius", cr);
                g_circleBoost = cb; write_int(L"CircleBoost", cb);
                g_circleBand = cband; write_int(L"CircleBand", cband);
                g_focusFactor = ff; write_int(L"FocusFactor", ff);
                // v3.0 UX: circle only activates per-profile ("Circle follow"
                // checkbox). If no profile enables it, the sliders do nothing —
                // tell the user instead of silently ignoring their change.
                bool anyCircle = false;
                EnterCriticalSection(&g_fxLock);
                for (auto& a : p->pAuto->apps) if (a.circle) { anyCircle = true; break; }
                LeaveCriticalSection(&g_fxLock);
                if (!anyCircle && hStatus)
                    SetWindowTextW(hStatus, L"Circle sliders saved — enable \"Circle follow\" in a profile to see it");
            }
            if (mode != g_effectMode || tint != g_tintLevel || circleChanged) {
                *p->pEffect = mode; g_effectMode = mode; write_int(L"EffectMode", mode);
                *p->pTint = tint; g_tintLevel = tint; write_int(L"TintLevel", tint);
                std::vector<HWND> affected;
                EnterCriticalSection(&g_fxLock);
                for (auto& kv : g_overlays) affected.push_back(kv.first);
                LeaveCriticalSection(&g_fxLock);
                for (HWND h : affected) {
                    if (mode == 1) {
                        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
                        if (ex & WS_EX_LAYERED) SetWindowLongW(h, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
                    }
                    int a = 255;
                    EnterCriticalSection(&g_fxLock);
                    if (g_winAlpha.count(h)) a = g_winAlpha[h].alpha;
                    LeaveCriticalSection(&g_fxLock);
                    apply_transparency(h, a);   // relaunches overlay per mode
                }
            }
            return 0;
        } else if (LOWORD(wp) == 108 || LOWORD(wp) == 109) {   // effect mode radios
            relabel_alpha(hAlphaTitle, hAlphaTrack, LOWORD(wp) == 109 ? 1 : 0);
        } else if (LOWORD(wp) == 106) {           // Add current
            EnterCriticalSection(&g_fxLock);
            HWND target = s_lastActive ? s_lastActive : p->fgWindow;
            std::wstring exe = target ? exe_name_of(target) : L"";
            bool own = false;
            if (target) { DWORD pid = 0; GetWindowThreadProcessId(target, &pid); own = (pid == GetCurrentProcessId()); }
            bool dup = false;
            for (auto& a : p->pAuto->apps) if (_wcsicmp(a.exe.c_str(), exe.c_str()) == 0) dup = true;
            debug_log(L"add_current fg=0x%08X cls=%s exe=%s dup=%d empty=%d own=%d",
                      (unsigned)(ULONG_PTR)target, target ? cls_of(target) : L"<null>",
                      exe.empty() ? L"<none>" : exe.c_str(), dup ? 1 : 0, exe.empty() ? 1 : 0, own ? 1 : 0);
            if (exe.empty() || own) {
                SetWindowTextW(hStatus, L"No target app - activate the app first");
            } else if (dup) {
                SetWindowTextW(hStatus, (std::wstring(L"Already in list: ") + exe).c_str());
            } else {
                p->pAuto->apps.push_back({exe, L""});
                refill_app_list(hAppList, p->pAuto);
                int last = (int)p->pAuto->apps.size() - 1;   // select + scroll to the new item
                SendMessageW(hAppList, LB_SETCURSEL, (WPARAM)last, 0);
                SendMessageW(hAppList, LB_SETTOPINDEX, (WPARAM)last, 0);
                SetWindowTextW(hStatus, (std::wstring(L"Added: ") + exe).c_str());
                settings_populate_editor();                 // v3.0: fresh profile feeds the editor
            }
            LeaveCriticalSection(&g_fxLock);
            return 0;
        } else if (LOWORD(wp) == 107) {           // Remove selected
            EnterCriticalSection(&g_fxLock);
            int sel = (int)SendMessageW(hAppList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)p->pAuto->apps.size()) {
                std::wstring removed = p->pAuto->apps[sel].exe;
                p->pAuto->apps.erase(p->pAuto->apps.begin() + sel);
                refill_app_list(hAppList, p->pAuto);
                int n = (int)p->pAuto->apps.size();          // keep selection near the removed slot
                if (n > 0) {
                    int news = sel < n ? sel : n - 1;
                    SendMessageW(hAppList, LB_SETCURSEL, (WPARAM)news, 0);
                    SendMessageW(hAppList, LB_SETTOPINDEX, (WPARAM)news, 0);
                    settings_populate_editor();             // v3.0: editor follows the new selection
                } else {
                    settings_disable_editor();               // v3.0: no apps left — editor inert, stale state cleared
                }
                SetWindowTextW(hStatus, (std::wstring(L"Removed: ") + removed).c_str());
            }
            LeaveCriticalSection(&g_fxLock);
            return 0;
        } else if (HIWORD(wp) == LBN_SELCHANGE && (HWND)lp == hAppList) {   // v3.0: app selected -> load profile into editor
            EnterCriticalSection(&g_fxLock);   // autofrost thread may rewrite g_auto.apps
            settings_populate_editor();
            LeaveCriticalSection(&g_fxLock);
            return 0;
        } else if (LOWORD(wp) == 130) {           // v3.0: Inherit global toggle
            settings_set_editor_enabled(SendMessageW(hInheritCk, BM_GETCHECK, 0, 0) != BST_CHECKED);
            return 0;
        } else if (LOWORD(wp) == 129) {           // v3.0: Save to profile — write editor fields into the selected app
            EnterCriticalSection(&g_fxLock);
            int sel = (int)SendMessageW(hAppList, LB_GETCURSEL, 0, 0);
            if (sel < 0 || sel >= (int)p->pAuto->apps.size()) {
                SetWindowTextW(hStatus, L"Select an app first");
            } else {
                AutoApp& a = p->pAuto->apps[sel];
                if (SendMessageW(hInheritCk, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    a.mode = -1; a.alpha = -1; a.radius = -1; a.tint = -1; a.circle = false;   // inherit everything
                } else {
                    a.mode = (int)SendMessageW(hModeCombo, CB_GETCURSEL, 0, 0);
                    if (a.mode != 1) a.mode = 0;   // -1 (nothing chosen) -> acrylic
                    a.alpha = (int)SendMessageW(hAlphaPTrack, TBM_GETPOS, 0, 0);
                    a.radius = (int)SendMessageW(hRadiusPTrack, TBM_GETPOS, 0, 0);
                    a.tint = (int)SendMessageW(hTintPTrack, TBM_GETPOS, 0, 0);
                    a.circle = SendMessageW(hCircleCk, BM_GETCHECK, 0, 0) == BST_CHECKED;
                }
                save_autofrost();   // full [Apps] rewrite through build_profile_line (omits -1 fields)
                SetWindowTextW(hStatus, (std::wstring(L"Saved profile: ") + a.exe).c_str());
            }
            LeaveCriticalSection(&g_fxLock);
            return 0;
        } else if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: hTrack = nullptr; hStepTitle = nullptr; hBlurTrack = nullptr; hBlurTitle = nullptr;
        hAlphaTrack = nullptr; hAlphaTitle = nullptr; hEnableCk = nullptr; hBlurCk = nullptr; hAppList = nullptr;
        hModeAcrylic = nullptr; hModeCrisp = nullptr; hTintTrack = nullptr; hTintTitle = nullptr;
        hStatus = nullptr; s_lastActive = nullptr;
        hCrTitle = nullptr; hCbTitle = nullptr; hCbandTitle = nullptr; hFocusTitle = nullptr;
        hCircleRadiusTrack = nullptr; hCircleBoostTrack = nullptr; hCircleBandTrack = nullptr; hFocusTrack = nullptr;
        hModeCombo = nullptr; hAlphaPTrack = nullptr; hRadiusPTrack = nullptr; hTintPTrack = nullptr;
        hAlphaPTitle = nullptr; hRadiusPTitle = nullptr; hTintPTitle = nullptr;
        hCircleCk = nullptr; hInheritCk = nullptr; p = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void show_settings(HINSTANCE hInst) {
    const wchar_t* CN = L"win2blurSettings";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = SettingsWndProc; wc.hInstance = hInst;
    wc.lpszClassName = CN; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!GetClassInfoW(hInst, CN, &wc)) RegisterClassW(&wc);
    SettingsParams sp = { &g_step, &g_blurRadius, g_hasBlurControl, &g_auto, GetForegroundWindow(), &g_effectMode, &g_tintLevel,
                          &g_circleRadius, &g_circleBoost, &g_circleBand, &g_focusFactor };
    std::wstring fgExe = sp.fgWindow ? exe_name_of(sp.fgWindow) : L"";
    debug_log(L"settings_open fg=0x%08X cls=%s exe=%s", (unsigned)(ULONG_PTR)sp.fgWindow, sp.fgWindow ? cls_of(sp.fgWindow) : L"<null>", fgExe.empty() ? L"<none>" : fgExe.c_str());
    // v3.0: width 290 -> 460 (right column x=282+ = circle/focus + profile
    // editor, clear of the Apply/Close x 200-260 and Add/Remove x 210-278).
    // Height 604: client ~573 — Save-to-profile bottom (516) clears the
    // bottom-right Apply/Close row (y ~541, 8px above client bottom) by 25px;
    // the no-blur left column (hotkey text ends 494) fits with margin.
    int h = 604;
    EnterCriticalSection(&g_fxLock);   // WM_CREATE reads g_auto initial values and iterates pAuto->apps
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, CN, L"win2blur - Settings", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 460, h, nullptr, nullptr, hInst, &sp);
    LeaveCriticalSection(&g_fxLock);
    RECT rc; GetWindowRect(hwnd, &rc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int w = rc.right - rc.left;
    SetWindowPos(hwnd, nullptr, (sw-w)/2, (sh-h)/2, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    MSG msg;
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
}

// ==================== Tray menu ====================
void show_tray_menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT_RESTORE, L"Restore && Exit");
    AppendMenuW(menu, MF_STRING, ID_EXIT_KEEP, L"Keep && Exit");
    POINT pt; GetCursorPos(&pt);
    HWND fgBefore = GetForegroundWindow();
    debug_log(L"tray_menu fg_before_setfg=0x%08X cls=%s", (unsigned)(ULONG_PTR)fgBefore, fgBefore ? cls_of(fgBefore) : L"<null>");
    SetForegroundWindow(g_hwnd);
    debug_log(L"tray_menu fg_after_setfg=0x%08X cls=%s", (unsigned)(ULONG_PTR)GetForegroundWindow(), cls_of(GetForegroundWindow()));
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    debug_log(L"tray_menu fg_after_menu=0x%08X cls=%s", (unsigned)(ULONG_PTR)GetForegroundWindow(), cls_of(GetForegroundWindow()));
    DestroyMenu(menu);
}

// ==================== Shutdown ====================
void close_orphan_class(const wchar_t* cls) {
    HWND ov = nullptr;
    while ((ov = FindWindowExW(nullptr, ov, cls, nullptr)) != nullptr) {
        if (wcscmp(cls, L"AcrylicOverlayClass") == 0) {
            // Disable DWM blur state first (critical on Win11, else the acrylic
            // texture lingers after the window is gone)
            HMODULE u32 = GetModuleHandleW(L"user32.dll");
            if (u32) {
                auto SetAccent = (BOOL (WINAPI*)(HWND, void*))GetProcAddress(u32, "SetWindowCompositionAttribute");
                if (SetAccent) {
                    struct { int state; int flags; unsigned int color; int anim; } policy = {};
                    struct { int attr; void* data; int size; } data = {19, &policy, sizeof(policy)};
                    SetAccent(ov, &data);
                }
            }
            DWM_BLURBEHIND bb = {}; bb.dwFlags = DWM_BB_ENABLE; bb.fEnable = FALSE;
            DwmEnableBlurBehindWindow(ov, &bb);
        }
        PostMessageW(ov, WM_CLOSE, 0, 0);
        DestroyWindow(ov);
    }
}
void cleanup_orphan_overlays() {
    close_orphan_class(L"AcrylicOverlayClass");
    close_orphan_class(L"CrispOverlayClass");
}
void shutdown(bool restore) {
    InterlockedExchange(&g_shuttingDown, 1);
    if (restore) {
        EnterCriticalSection(&g_fxLock);
        for (auto hwnd : g_modified) {
            LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if (ex & WS_EX_LAYERED) { SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA); SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED); }
        }
        g_modified.clear();
        g_autoApplied.clear();
        for (auto& kv : g_overlays) { TerminateProcess(kv.second.hProcess, 0); CloseHandle(kv.second.hProcess); CloseHandle(kv.second.hThread); }
        g_overlays.clear();
        LeaveCriticalSection(&g_fxLock);
        save_session(true);
        cleanup_orphan_overlays();
    } else { save_session(false); }
    UnregisterHotKey(g_hwnd, ID_UP); UnregisterHotKey(g_hwnd, ID_DOWN);
    UnregisterHotKey(g_hwnd, ID_TOGGLE); UnregisterHotKey(g_hwnd, ID_ACRYLIC);
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_pShmData) { UnmapViewOfFile(g_pShmData); g_pShmData = nullptr; }
    if (g_hShm) { CloseHandle(g_hShm); g_hShm = nullptr; }
    cleanup_temp();
    DestroyWindow(g_hwnd);
}

// ==================== WinMain ====================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_nid.cbSize = sizeof(NOTIFYICONDATAW); g_nid.hWnd = hwnd; g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = g_hIcon ? g_hIcon : LoadIcon(nullptr, IDI_APPLICATION);
        wcscpy(g_nid.szTip, L"win2blur");
        Shell_NotifyIconW(NIM_ADD, &g_nid);
        if (g_enableUp) RegisterHotKey(hwnd, ID_UP, g_transpMod, g_transpKeyUp);
        if (g_enableDown) RegisterHotKey(hwnd, ID_DOWN, g_transpMod, g_transpKeyDown);
        if (g_enableToggle) RegisterHotKey(hwnd, ID_TOGGLE, g_toggleMod, g_toggleKey);
        if (g_enableAcrylic) RegisterHotKey(hwnd, ID_ACRYLIC, g_acrylicMod, g_acrylicKey);
        g_overlayPath = extract_resource(101, L"acrylic_overlay.exe");
        g_welcomePath = extract_resource(102, L"welcome_demo.exe");
        if (!g_welcomePath.empty()) {
            STARTUPINFOW si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
            CreateProcessW(g_welcomePath.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            // v3.0 fix (risk D): NEVER wait INFINITE on the demo — if it hangs
            // the whole app (hotkeys, session restore, AutoFrost thread) stays
            // blocked and "everything is unusable" after launch. Wait 5s then
            // proceed regardless; demo exit is best-effort.
            if (pi.hProcess) { WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
        }
        // Inject self-built DLL (one-time admin prompt)
        inject_self_dll();
        restore_session();
        CreateThread(nullptr, 0, autofrost_thread, nullptr, 0, nullptr);
        return 0;
    }
    case WM_HOTKEY: on_hotkey((int)wp); return 0;
    case WM_TRAYICON: if (LOWORD(lp) == WM_RBUTTONUP) show_tray_menu(); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SETTINGS) show_settings((HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
        else if (LOWORD(wp) == ID_EXIT_RESTORE) { shutdown(true); PostQuitMessage(0); }
        else if (LOWORD(wp) == ID_EXIT_KEEP) { shutdown(false); PostQuitMessage(0); }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // Single instance — a logon task + Start Menu shortcut may both try to launch;
    // the second one exits silently (handle kept for process lifetime, released on exit).
    HANDLE hSingle = CreateMutexW(nullptr, TRUE, L"win2blur_single");
    if (hSingle && GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    InitializeCriticalSection(&g_fxLock);
    InitCommonControls();
    g_hIcon = LoadIconW(hInst, L"APP_ICON");
    load_config();
    load_autofrost();
    detect_self_dll();
    WNDCLASSW wc = {}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = L"win2blurTray";
    if (!RegisterClassW(&wc)) return 1;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
