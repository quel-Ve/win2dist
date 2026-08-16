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

// ==================== Constants ====================
#define WM_TRAYICON (WM_APP + 1)
#define ID_UP 1
#define ID_DOWN 2
#define ID_TOGGLE 3
#define ID_ACRYLIC 4
#define ID_SETTINGS 1001
#define ID_EXIT_RESTORE 1002
#define ID_EXIT_KEEP 1003
#define TINT 0x1A000000
#define DEFAULT_STEP 5
#define DEFAULT_BLUR_RADIUS 30

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
static std::wstring g_configPath, g_overlayPath, g_welcomePath, g_tempDir;
static HICON g_hIcon = nullptr;
static int g_transpMod = MOD_ALT, g_transpKeyUp = VK_LEFT, g_transpKeyDown = VK_RIGHT;
static int g_toggleMod = MOD_ALT, g_toggleKey = VK_UP;
static int g_acrylicMod = MOD_ALT, g_acrylicKey = VK_DOWN;
static bool g_enableUp = true, g_enableDown = true, g_enableToggle = true, g_enableAcrylic = true;
static CRITICAL_SECTION g_fxLock;

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
struct AutoApp { std::wstring exe, cls; };
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

void load_autofrost() {
    auto cfg = config_path();
    wchar_t probe[8];
    bool sectionExists = GetPrivateProfileStringW(L"AutoFrost", L"Enabled", L"", probe, 8, cfg.c_str()) != 0;
    if (!sectionExists) {
        // first run — seed presets
        for (int i = 0; i < (int)(sizeof(g_autoPresets) / sizeof(g_autoPresets[0])); i++) {
            wchar_t key[16], val[128];
            wsprintfW(key, L"App_%d", i);
            wsprintfW(val, L"%s|%s", g_autoPresets[i].exe.c_str(), g_autoPresets[i].cls.c_str());
            WritePrivateProfileStringW(L"AutoFrost", key, val, cfg.c_str());
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
    wchar_t buf[128];
    for (int i = 0; i < 32; i++) {
        wchar_t key[16]; wsprintfW(key, L"App_%d", i);
        if (!GetPrivateProfileStringW(L"AutoFrost", key, L"", buf, 128, cfg.c_str()) || !buf[0]) break;
        std::wstring s(buf);
        auto p = s.find(L'|');
        AutoApp a;
        a.exe = (p == s.npos) ? s : s.substr(0, p);
        a.cls = (p == s.npos) ? L"" : s.substr(p + 1);
        if (!a.exe.empty()) g_auto.apps.push_back(a);
    }
}
void save_autofrost() {
    auto cfg = config_path();
    WritePrivateProfileStringW(L"AutoFrost", nullptr, nullptr, cfg.c_str()); // wipe section
    WritePrivateProfileStringW(L"AutoFrost", L"Enabled", g_auto.enabled ? L"1" : L"0", cfg.c_str());
    wchar_t b[16]; wsprintfW(b, L"%d", g_auto.defaultAlpha);
    WritePrivateProfileStringW(L"AutoFrost", L"DefaultAlpha", b, cfg.c_str());
    WritePrivateProfileStringW(L"AutoFrost", L"DefaultBlur", g_auto.defaultBlur ? L"1" : L"0", cfg.c_str());
    for (size_t i = 0; i < g_auto.apps.size() && i < 32; i++) {
        wchar_t key[16], val[256];
        wsprintfW(key, L"App_%u", (unsigned)i);
        wsprintfW(val, L"%s|%s", g_auto.apps[i].exe.c_str(), g_auto.apps[i].cls.c_str());
        WritePrivateProfileStringW(L"AutoFrost", key, val, cfg.c_str());
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
        wchar_t buf[256], key[32];
        int alpha = 255;
        BYTE b; DWORD flags;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &b, &flags) && (flags & LWA_ALPHA)) alpha = b;
        wchar_t title[128] = {}, cls[64] = {};
        GetWindowTextW(hwnd, title, 127);
        GetClassNameW(hwnd, cls, 63);
        wsprintfW(buf, L"%s|%s|%d|%d", title, cls, alpha, g_overlays.count(hwnd) ? 0 : -1);
        wsprintfW(key, L"window_%d", i++);
        WritePrivateProfileStringW(L"Session", key, buf, cfg.c_str());
    }
    LeaveCriticalSection(&g_fxLock);
}
struct SessionItem { std::wstring title, cls; int alpha, tint; };
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
        SessionItem si;
        si.title = s.substr(0, p1);
        si.cls = s.substr(p1+1, p2-p1-1);
        si.alpha = _wtoi(s.substr(p2+1, p3-p2-1).c_str());
        si.tint = (p3 != s.npos) ? _wtoi(s.substr(p3+1).c_str()) : -1;
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
void restore_session() {
    load_session_items();
    if (g_sessionItems.empty()) return;
    for (auto& si : g_sessionItems) {
        HWND h = find_window_by_title_class(si.title.c_str(), si.cls.c_str());
        if (!h) continue;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (!(ex & WS_EX_LAYERED)) SetWindowLongW(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(h, 0, (BYTE)si.alpha, LWA_ALPHA);
        EnterCriticalSection(&g_fxLock);
        g_modified.insert(h);
        LeaveCriticalSection(&g_fxLock);
        if (si.tint >= 0 && !g_overlayPath.empty()) {
            wchar_t cmd[512], arg[32];
            wsprintfW(arg, L"0x%08X 0x%08X", (DWORD)(ULONG_PTR)h, TINT);
            wsprintfW(cmd, L"\"%s\" %s", g_overlayPath.c_str(), arg);
            STARTUPINFOW si2 = {sizeof(si2)};
            si2.dwFlags = STARTF_USESHOWWINDOW; si2.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi)) {
                EnterCriticalSection(&g_fxLock);
                g_overlays[h] = pi;
                LeaveCriticalSection(&g_fxLock);
            }
        }
    }
    save_session(true);
}

// ==================== Transparency ====================
void apply_transparency(HWND hwnd, int alpha) {
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    EnterCriticalSection(&g_fxLock);
    g_modified.insert(hwnd);
    LeaveCriticalSection(&g_fxLock);
}

void launch_overlay(HWND hwnd);  // forward decl — defined in Acrylic Overlay section below
void kill_overlay(HWND hwnd);   // forward decl — same

// ==================== Auto-frost monitor ====================
static const int AUTOFROST_POLL_MS = 10000; // matching cadence — was 800ms debug value; 10s = cheap, new windows blur within ≤10s
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
        if (!IsWindow(*it)) it = g_modified.erase(it); else ++it;
    }
    for (auto it = g_autoApplied.begin(); it != g_autoApplied.end(); ) {
        if (!IsWindow(*it)) it = g_autoApplied.erase(it); else ++it;
    }
    for (auto it = g_overlays.begin(); it != g_overlays.end(); ) {
        if (!IsWindow(it->first)) {
            TerminateProcess(it->second.hProcess, 0);
            CloseHandle(it->second.hProcess); CloseHandle(it->second.hThread);
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
    return wcscmp(cls, app.cls.c_str()) == 0;
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
    for (HWND h : toRevert) { g_autoApplied.erase(h); g_modified.erase(h); }
    LeaveCriticalSection(&g_fxLock);
    for (HWND h : toRevert) {
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) { SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA); SetWindowLongW(h, GWL_EXSTYLE, ex & ~WS_EX_LAYERED); }
        kill_overlay(h);
    }
}

DWORD WINAPI autofrost_thread(LPVOID) {
    for (;;) {
        Sleep(AUTOFROST_POLL_MS);
        if (g_shuttingDown) break;
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
                    apply_transparency(h, cfg->defaultAlpha);
                    if (cfg->defaultBlur) launch_overlay(h);
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
void launch_overlay(HWND hwnd) {
    if (g_overlayPath.empty()) g_overlayPath = extract_resource(101, L"acrylic_overlay.exe");
    EnterCriticalSection(&g_fxLock);
    bool dup = g_overlays.count(hwnd) != 0;
    LeaveCriticalSection(&g_fxLock);
    if (dup) return;
    wchar_t cmd[512], arg[32];
    wsprintfW(arg, L"0x%08X 0x%08X", (DWORD)(ULONG_PTR)hwnd, TINT);
    wsprintfW(cmd, L"\"%s\" %s", g_overlayPath.c_str(), arg);
    STARTUPINFOW si = {sizeof(si)}; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        EnterCriticalSection(&g_fxLock);
        auto it = g_overlays.find(hwnd);
        if (it != g_overlays.end()) {
            LeaveCriticalSection(&g_fxLock);
            TerminateProcess(pi.hProcess, 0);   // 并发启动者先到 — 杀自己刚建的
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        } else {
            g_overlays[hwnd] = pi;
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
    HWND ov = FindWindowW(L"AcrylicOverlayClass", nullptr);
    if (ov) { PostMessageW(ov, WM_CLOSE, 0, 0); DWORD w = WaitForSingleObject(pi.hProcess, 500);
        if (w == WAIT_OBJECT_0) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); return; }
        DestroyWindow(ov); }
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
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
    BYTE alpha = 255; DWORD flags;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags);
    if (alpha == 0) alpha = 255;
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
struct SettingsParams { int* pStep; int* pBlur; bool hasBlur; AutoFrostConfig* pAuto; HWND fgWindow; };

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

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static HWND hTrack = nullptr, hStepTitle = nullptr;
    static HWND hBlurTrack = nullptr, hBlurTitle = nullptr;
    static HWND hAlphaTrack = nullptr, hAlphaTitle = nullptr, hEnableCk = nullptr, hBlurCk = nullptr, hAppList = nullptr;
    static HWND hStatus = nullptr;          // feedback line under the app list
    static HWND s_lastActive = nullptr;     // last non-own window the user activated (Add current target)
    static SettingsParams* p = nullptr;
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp; p = (SettingsParams*)cs->lpCreateParams;
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        wchar_t buf[64];
        // Value shown in the title static — right column (x>=200) is reserved for Apply/Close buttons
        hStepTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, 12, 200, 20, hwnd, nullptr, hi, nullptr);
        wsprintfW(buf, L"Transparency step (1-20%%): %d%%", *p->pStep); SetWindowTextW(hStepTitle, buf);
        hTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS, 12, 32, 200, 28, hwnd, (HMENU)101, hi, nullptr);
        SendMessageW(hTrack, TBM_SETRANGE, TRUE, MAKELONG(1, 20));
        SendMessageW(hTrack, TBM_SETPOS, TRUE, *p->pStep);
        int y = 70;
        if (p->hasBlur) {
            hBlurTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, y, 200, 20, hwnd, nullptr, hi, nullptr);
            y += 20;
            hBlurTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS | TBS_NOTICKS, 12, y, 200, 28, hwnd, (HMENU)102, hi, nullptr);
            SendMessageW(hBlurTrack, TBM_SETRANGE, TRUE, MAKELONG(0, g_blurPresetCount - 1));
            int curIdx = 0;
            for (int i = 0; i < g_blurPresetCount; i++) {
                if (g_blurPresets[i] == *p->pBlur) { curIdx = i; break; }
                if (g_blurPresets[i] < *p->pBlur) curIdx = i;
            }
            SendMessageW(hBlurTrack, TBM_SETPOS, TRUE, curIdx);
            wsprintfW(buf, L"Blur radius: %d", g_blurPresets[curIdx]); SetWindowTextW(hBlurTitle, buf);
            y += 32;
        }
        CreateWindowW(L"STATIC", L"Auto-frost", WS_CHILD | WS_VISIBLE, 12, y + 8, 150, 20, hwnd, nullptr, hi, nullptr);
        hEnableCk = CreateWindowW(L"BUTTON", L"Enable auto-frost", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, y + 28, 150, 20, hwnd, (HMENU)104, hi, nullptr);
        SendMessageW(hEnableCk, BM_SETCHECK, p->pAuto->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        hAlphaTitle = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, y + 50, 200, 20, hwnd, nullptr, hi, nullptr);
        hAlphaTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS | TBS_NOTICKS, 12, y + 68, 200, 28, hwnd, (HMENU)103, hi, nullptr);
        SendMessageW(hAlphaTrack, TBM_SETRANGE, TRUE, MAKELONG(50, 100));
        int pct = (p->pAuto->defaultAlpha * 100 + 127) / 255;
        SendMessageW(hAlphaTrack, TBM_SETPOS, TRUE, pct);
        wsprintfW(buf, L"Default transparency: %d%%", pct); SetWindowTextW(hAlphaTitle, buf);
        hBlurCk = CreateWindowW(L"BUTTON", L"Blur", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, y + 100, 100, 20, hwnd, (HMENU)105, hi, nullptr);
        SendMessageW(hBlurCk, BM_SETCHECK, p->pAuto->defaultBlur ? BST_CHECKED : BST_UNCHECKED, 0);
        hAppList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 12, y + 126, 190, 96, hwnd, nullptr, hi, nullptr);
        refill_app_list(hAppList, p->pAuto);
        CreateWindowW(L"BUTTON", L"Add current", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, y + 126, 68, 24, hwnd, (HMENU)106, hi, nullptr);
        CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, y + 156, 68, 24, hwnd, (HMENU)107, hi, nullptr);
        hStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 12, y + 226, 260, 20, hwnd, nullptr, hi, nullptr);
        y += 226;
        CreateWindowW(L"STATIC", L"ALT+LEFT  more transparent\r\nALT+RIGHT less transparent\r\nALT+UP    toggle on/off\r\nALT+DOWN  acrylic blur", WS_CHILD | WS_VISIBLE, 12, y + 22, 240, 80, hwnd, nullptr, hi, nullptr);
        // "Add current" target = the window the user last activated that is not our own
        s_lastActive = p->fgWindow;
        DWORD ownPid = 0;
        if (!s_lastActive || (GetWindowThreadProcessId(s_lastActive, &ownPid) && ownPid == GetCurrentProcessId()))
            s_lastActive = nullptr;
        status_show_target(hStatus, s_lastActive);
        CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 200, 24, 60, 24, hwnd, (HMENU)IDOK, hi, nullptr);
        CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 200, 56, 60, 24, hwnd, (HMENU)IDCANCEL, hi, nullptr);
        HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Candara");
        if (hFont) EnumChildWindows(hwnd, [](HWND h, LPARAM l) -> BOOL { SendMessageW(h, WM_SETFONT, (WPARAM)l, TRUE); return TRUE; }, (LPARAM)hFont);
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
            wsprintfW(b, L"Transparency step (1-20%%): %d%%", v); SetWindowTextW(hStepTitle, b); }
        else if (id == 102 && hBlurTrack) { int idx = (int)SendMessageW(hBlurTrack, TBM_GETPOS, 0, 0);
            if (idx >= 0 && idx < g_blurPresetCount) { wchar_t b[64];
                wsprintfW(b, L"Blur radius: %d", g_blurPresets[idx]); SetWindowTextW(hBlurTitle, b); } }
        else if (id == 103 && hAlphaTrack) { int v = (int)SendMessageW(hAlphaTrack, TBM_GETPOS, 0, 0);
            wchar_t b[64]; wsprintfW(b, L"Default transparency: %d%%", v); SetWindowTextW(hAlphaTitle, b); }
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
            return 0;
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
                }
                SetWindowTextW(hStatus, (std::wstring(L"Removed: ") + removed).c_str());
            }
            LeaveCriticalSection(&g_fxLock);
            return 0;
        } else if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: hTrack = nullptr; hStepTitle = nullptr; hBlurTrack = nullptr; hBlurTitle = nullptr;
        hAlphaTrack = nullptr; hAlphaTitle = nullptr; hEnableCk = nullptr; hBlurCk = nullptr; hAppList = nullptr;
        hStatus = nullptr; s_lastActive = nullptr; return 0;
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
    SettingsParams sp = { &g_step, &g_blurRadius, g_hasBlurControl, &g_auto, GetForegroundWindow() };
    std::wstring fgExe = sp.fgWindow ? exe_name_of(sp.fgWindow) : L"";
    debug_log(L"settings_open fg=0x%08X cls=%s exe=%s", (unsigned)(ULONG_PTR)sp.fgWindow, sp.fgWindow ? cls_of(sp.fgWindow) : L"<null>", fgExe.empty() ? L"<none>" : fgExe.c_str());
    int h = g_hasBlurControl ? 460 : 410;
    EnterCriticalSection(&g_fxLock);   // WM_CREATE reads g_auto initial values and iterates pAuto->apps
    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, CN, L"win2blur - Settings", WS_CAPTION | WS_SYSMENU | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 290, h, nullptr, nullptr, hInst, &sp);
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
void cleanup_orphan_overlays() {
    HWND ov = nullptr;
    while ((ov = FindWindowExW(nullptr, ov, L"AcrylicOverlayClass", nullptr)) != nullptr) {
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
        PostMessageW(ov, WM_CLOSE, 0, 0);
        DestroyWindow(ov);
    }
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
            if (pi.hProcess) { WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
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
