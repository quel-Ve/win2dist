# Auto-frost v2.7 Implementation Plan

> **执行记录（2026-08-05）：** 5 任务全部完成并经逐任务审查（Spec ✅ + 质量 ✅）。最终全分支审查（opus）：1 项 Important（Settings 窗口高度 420/390→460/410，热键提示裁剪）已修复并复审通过；18 项 Minor 全部裁决延迟/不修（台账已销毁，裁决摘要：M1 播种探测可延迟、M2 RAII 可延迟、M3 kill_overlay 类名匹配可延迟、M4 save_session 锁内 I/O 可延迟、M5-M8 线程生命周期/TOCTOU/重播种可延迟、M9-M11 UI 细节不修、M12-M14 验证缺口/README 措辞可延迟、M15 DeleteCriticalSection 不修、M16 取整方向不修）。v2.7 tag 移至含高度修复的 0cd8982。发行包 dist_release_2.7 已更新。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 常用软件自动应用默认效果（85% 透明 + Acrylic 模糊），30s 轮询发现新窗口，每窗口只自动一次。

**Architecture:** 全部改动集中在 `native/src/tray_app.cpp` 单文件（现有 517 行 Win32 C++）。新增 `[AutoFrost]` ini 配置节 + 后台监视线程（30s 轮询 `EnumWindows` → exe 名匹配 → 复用现有 `apply_transparency`/`launch_overlay`）。新增全局 `CRITICAL_SECTION` 保护跨线程共享集合。

**Tech Stack:** MinGW g++ / Win32 API（EnumWindows, QueryFullProcessImageNameW, GetPrivateProfileStringW）/ CMake (build5)。无 DLL 改动，DWM hook 不动。

## Global Constraints

- 只改 `native/src/tray_app.cpp`（含新增），不改其他源文件、不改 DLL/injector
- 保持现有风格：`g_` 前缀全局、`wsprintfW`、`GetPrivateProfile*W` ini API、Candara 字体
- 轮询间隔 `AUTOFROST_POLL_MS = 30000`
- 每窗口只自动一次：命中窗口不在 `g_modified` 才应用，之后手动调整永不被抢回
- `DefaultAlpha` 默认 217（=85%），合法范围 1–255；UI 滑块显示百分比 50–100
- 预置 8 项（首次运行写入）：Obsidian.exe / WindowsTerminal.exe / msedge.exe / explorer.exe|CabinetWClass / cloudmusic.exe / WeChat.exe / CherryStudio.exe / Code.exe
- 应用列表上限 32 项
- 所有 `g_modified` / `g_overlays` / `g_auto` 访问必须持有 `g_fxLock`（CRITICAL_SECTION）
- 构建：`cd native/build5 && cmake --build .`（MinGW）

---

### Task 1: `[AutoFrost]` 配置层

**Files:**
- Modify: `native/src/tray_app.cpp` — 新增结构体、预置表、load/save 函数；`load_config()` 之后调用 `load_autofrost()`

**Interfaces:**
- Consumes: 现有 `config_path()`（返回 exe 旁 config.ini 路径）
- Produces: `struct AutoApp { std::wstring exe, cls; }`、`struct AutoFrostConfig { bool enabled; int defaultAlpha; bool defaultBlur; std::vector<AutoApp> apps; }`、全局 `static AutoFrostConfig g_auto;`、`void load_autofrost()`、`void save_autofrost()` — 供 Task 3（监视线程）、Task 4（UI）使用

- [ ] **Step 1: 添加结构体与预置表**（插在 `// ==================== Self-built DLL + Shared Memory ====================` 段之前）

```cpp
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
    {L"CherryStudio.exe", L""},
    {L"Code.exe", L""},
};
```

- [ ] **Step 2: 添加 load/save 函数**（接在上一步代码后）

```cpp
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
        wsprintfW(key, L"App_%u", (unsigned)i);   // %zu 不被 wsprintfW 支持（修复轮确认）
        wsprintfW(val, L"%s|%s", g_auto.apps[i].exe.c_str(), g_auto.apps[i].cls.c_str());
        WritePrivateProfileStringW(L"AutoFrost", key, val, cfg.c_str());
    }
}
```

- [ ] **Step 3: 启动时加载** — 在 `WinMain` 的 `load_config();` 之后加一行：

```cpp
    load_config();
    load_autofrost();
```

- [ ] **Step 4: 构建验证**

Run: `cd native/build5 && cmake --build .`
Expected: 编译通过，无警告新增。

- [ ] **Step 5: 运行验证首启预置**

Run: 从 build5 运行 win2blur.exe，退出（Keep & Exit），检查 exe 旁 config.ini
Expected: `[AutoFrost]` 节含 Enabled=1、DefaultAlpha=217、DefaultBlur=1、App_0..App_7 八个预置项（explorer.exe 带 `|CabinetWClass`）。

- [ ] **Step 6: Commit**

```bash
git add native/src/tray_app.cpp
git commit -m "feat: autofrost config layer with first-run presets"
```

---

### Task 2: 线程安全改造（g_fxLock）

**Files:**
- Modify: `native/src/tray_app.cpp` — 新增 `g_fxLock`、改造 `apply_transparency` / `launch_overlay` / `kill_overlay` / `save_session` / `restore_session` / `shutdown`

**Interfaces:**
- Consumes: 现有集合 `g_modified` (std::set\<HWND\>)、`g_overlays` (std::map\<HWND, PROCESS_INFORMATION\>)
- Produces: 全局 `static CRITICAL_SECTION g_fxLock;`（WinMain 中 `InitializeCriticalSection`），不变量「g_modified / g_overlays 只在持有 g_fxLock 时访问」— Task 3 的监视线程依赖此不变量
- 说明: `launch_overlay` / `kill_overlay` / `save_session` 内部加锁（调用方不需要加锁）；`apply_transparency` 只锁 `g_modified.insert`；`restore_session` / `shutdown` 的集合遍历处加锁

- [ ] **Step 1: 声明锁** — 在 globals 段（`static bool g_enableAcrylic...` 之后）加：

```cpp
static CRITICAL_SECTION g_fxLock;
```

- [ ] **Step 2: 改造 `apply_transparency`**

```cpp
void apply_transparency(HWND hwnd, int alpha) {
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) SetWindowLongW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)alpha, LWA_ALPHA);
    EnterCriticalSection(&g_fxLock);
    g_modified.insert(hwnd);
    LeaveCriticalSection(&g_fxLock);
}
```

- [ ] **Step 3: 改造 `launch_overlay`**

```cpp
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
        g_overlays[hwnd] = pi;
        LeaveCriticalSection(&g_fxLock);
    }
}
```

- [ ] **Step 4: 改造 `kill_overlay`**（锁内取出并 erase，锁外等待/终止，行为不变）

```cpp
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
```

- [ ] **Step 5: 改造 `save_session`** — 完整替换函数体（`clear` 分支提前 return 不受影响，锁只包循环）：

```cpp
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
```

- [ ] **Step 6: 改造 `restore_session`** — 两处集合写入加锁。第一处（透明度写入后）：

```cpp
        SetLayeredWindowAttributes(h, 0, (BYTE)si.alpha, LWA_ALPHA);
        EnterCriticalSection(&g_fxLock);
        g_modified.insert(h);
        LeaveCriticalSection(&g_fxLock);
```

第二处（overlay 启动成功后）：

```cpp
            if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi)) {
                EnterCriticalSection(&g_fxLock);
                g_overlays[h] = pi;
                LeaveCriticalSection(&g_fxLock);
            }
```

- [ ] **Step 7: 改造 `shutdown`** — `restore` 分支的两个循环用锁包裹（`SetLayeredWindowAttributes` 等窗口调用在锁内也安全）：

```cpp
    if (restore) {
        EnterCriticalSection(&g_fxLock);
        for (auto hwnd : g_modified) {
            LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if (ex & WS_EX_LAYERED) { SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA); SetWindowLongW(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED); }
        }
        g_modified.clear();
        for (auto& kv : g_overlays) { TerminateProcess(kv.second.hProcess, 0); CloseHandle(kv.second.hProcess); CloseHandle(kv.second.hThread); }
        g_overlays.clear();
        LeaveCriticalSection(&g_fxLock);
        save_session(true);
        cleanup_orphan_overlays();
    } else { save_session(false); }
```

注意死锁纪律：**任何持锁路径不得再调用会加锁的函数**（如 `apply_transparency`/`launch_overlay`/`save_session`）。上述各函数的锁都是局部短持有；monitor 线程调用 `apply_transparency`/`launch_overlay` 时自身不持锁，无嵌套。

- [ ] **Step 8: 初始化锁** — `WinMain` 开头（`InitCommonControls();` 之前）加：

```cpp
    InitializeCriticalSection(&g_fxLock);
```

- [ ] **Step 9: 构建 + 回归验证**

Run: `cd native/build5 && cmake --build .`，运行 win2blur.exe
Expected: 编译通过。热键 ALT+←/→/↑/↓ 全部照常工作；Restore & Exit 与 Keep & Exit 正常；无崩溃、无死锁（Keep & Exit 后重启能恢复会话）。

- [ ] **Step 10: Commit**

```bash
git add native/src/tray_app.cpp
git commit -m "refactor: protect shared window sets with critical section"
```

---

### Task 3: 监视线程

**Files:**
- Modify: `native/src/tray_app.cpp` — 新增 `exe_name_of` / `prune_stale` / `autofrost_match` / `autofrost_thread`，在 `WM_CREATE` 的 `restore_session()` 后启动线程

**Interfaces:**
- Consumes: Task 1 的 `g_auto` / `load_autofrost()`；Task 2 的 `g_fxLock` 及改造后的 `apply_transparency` / `launch_overlay`；现有 `config_path()`
- Produces: `static const int AUTOFROST_POLL_MS = 30000;`、`static volatile LONG g_shuttingDown = 0;`、`std::wstring exe_name_of(HWND)`（Task 4 的 Add-current 按钮复用）、`void prune_stale()`、`DWORD WINAPI autofrost_thread(LPVOID)` — 在 `restore_session();` 之后调用 `CreateThread(nullptr, 0, autofrost_thread, nullptr, 0, nullptr);`

- [ ] **Step 1: 添加工具函数与线程**（插在 `// ==================== Transparency ====================` 段之后）

```cpp
// ==================== Auto-frost monitor ====================
static const int AUTOFROST_POLL_MS = 30000;
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

DWORD WINAPI autofrost_thread(LPVOID) {
    for (;;) {
        Sleep(AUTOFROST_POLL_MS);
        if (g_shuttingDown) break;
        prune_stale();
        EnterCriticalSection(&g_fxLock);
        load_autofrost();               // hot-read config each round
        AutoFrostConfig cfg = g_auto;   // snapshot under lock
        LeaveCriticalSection(&g_fxLock);
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
                }
                break;
            }
            return TRUE;
        }, (LPARAM)&cfg);
    }
    return 0;
}
```

- [ ] **Step 2: 启动线程** — `WM_CREATE` 中 `restore_session();` 之后加：

```cpp
        restore_session();
        CreateThread(nullptr, 0, autofrost_thread, nullptr, 0, nullptr);
```

- [ ] **Step 3: 关闭标志** — `shutdown()` 函数开头（第一个 if 之前）加：

```cpp
    InterlockedExchange(&g_shuttingDown, 1);
```

- [ ] **Step 4: 开发期加速测试** — 临时把 `AUTOFROST_POLL_MS` 改为 `3000`，构建运行：打开 Obsidian/记事本对照验证（预期 ≤3s 自动 85%+模糊）；验证完成后**改回 30000**。

- [ ] **Step 5: 全链路验证（30s 正式值）**

Run: 恢复 30000 后构建运行，逐项检查：
1. 启动 win2blur → 打开 Obsidian → ≤30s 自动 85% + 模糊
2. ALT+↑ 手动调回 100% → 下一轮不被抢回（窗口在 g_modified 中）
3. 关闭 Obsidian 重开 → 新窗口再次自动应用
4. 打开文件资源管理器 → 自动应用；任务栏/桌面**无**变化（CabinetWClass 类名过滤）
5. 运行 `tasklist | findstr /i "explorer"` 确认 explorer.exe 进程仍在正常显示桌面

- [ ] **Step 6: Commit**

```bash
git add native/src/tray_app.cpp
git commit -m "feat: autofrost 30s monitor thread auto-applies effects to matched apps"
```

---

### Task 4: Settings UI — Auto-frost 节

**Files:**
- Modify: `native/src/tray_app.cpp` — `SettingsParams` 结构、`SettingsWndProc` WM_CREATE/WM_HSCROLL/WM_COMMAND、`show_settings`

**Interfaces:**
- Consumes: Task 1 的 `g_auto` / `save_autofrost()`；Task 3 的 `exe_name_of()`；现有 `show_settings`
- Produces: 扩展后的 `struct SettingsParams { int* pStep; int* pBlur; bool hasBlur; AutoFrostConfig* pAuto; HWND fgWindow; }` — 供窗口创建传参

- [ ] **Step 1: 扩展 SettingsParams**

```cpp
struct SettingsParams { int* pStep; int* pBlur; bool hasBlur; AutoFrostConfig* pAuto; HWND fgWindow; };
```

- [ ] **Step 2: 添加列表刷新助手**（`SettingsWndProc` 之前）

```cpp
void refill_app_list(HWND hList, AutoFrostConfig* p) {
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (auto& a : p->apps) {
        std::wstring s = a.exe + (a.cls.empty() ? L"" : (L"  (" + a.cls + L")"));
        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
}
```

- [ ] **Step 3: WM_CREATE 追加 Auto-frost 控件** — 在现有 blur 块 `y += 32;` 之后、hotkey 提示 STATIC 之前插入（新控件 id：103=alpha% 滑块、104=Enable 勾选、105=Blur 勾选、106=Add 按钮、107=Remove 按钮；静态变量 `hAlphaTrack/hAlphaLabel/hEnableCk/hBlurCk/hAppList` 加到函数顶部现有 static 声明处）：

```cpp
        CreateWindowW(L"STATIC", L"Auto-frost", WS_CHILD | WS_VISIBLE, 12, y + 8, 150, 20, hwnd, nullptr, hi, nullptr);
        hEnableCk = CreateWindowW(L"BUTTON", L"Enable auto-frost", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, y + 28, 150, 20, hwnd, (HMENU)104, hi, nullptr);
        SendMessageW(hEnableCk, BM_SETCHECK, p->pAuto->enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        CreateWindowW(L"STATIC", L"Default transparency", WS_CHILD | WS_VISIBLE, 12, y + 50, 200, 20, hwnd, nullptr, hi, nullptr);
        hAlphaTrack = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_TOOLTIPS | TBS_NOTICKS, 12, y + 68, 200, 28, hwnd, (HMENU)103, hi, nullptr);
        SendMessageW(hAlphaTrack, TBM_SETRANGE, TRUE, MAKELONG(50, 100));
        int pct = (p->pAuto->defaultAlpha * 100 + 127) / 255;
        SendMessageW(hAlphaTrack, TBM_SETPOS, TRUE, pct);
        hAlphaLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 220, y + 70, 50, 20, hwnd, nullptr, hi, nullptr);
        wsprintfW(buf, L"%d%%", pct); SetWindowTextW(hAlphaLabel, buf);
        hBlurCk = CreateWindowW(L"BUTTON", L"Blur", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 12, y + 100, 100, 20, hwnd, (HMENU)105, hi, nullptr);
        SendMessageW(hBlurCk, BM_SETCHECK, p->pAuto->defaultBlur ? BST_CHECKED : BST_UNCHECKED, 0);
        hAppList = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL, 12, y + 126, 190, 96, hwnd, nullptr, hi, nullptr);
        refill_app_list(hAppList, p->pAuto);
        CreateWindowW(L"BUTTON", L"Add current", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, y + 126, 68, 24, hwnd, (HMENU)106, hi, nullptr);
        CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 210, y + 156, 68, 24, hwnd, (HMENU)107, hi, nullptr);
        y += 226;
```

- [ ] **Step 4: WM_HSCROLL 加 alpha 滑块分支** — 现有 `else if (id == 102 && hBlurTrack)` 之后加：

```cpp
        else if (id == 103 && hAlphaTrack) { int v = (int)SendMessageW(hAlphaTrack, TBM_GETPOS, 0, 0);
            wchar_t b[16]; wsprintfW(b, L"%d%%", v); SetWindowTextW(hAlphaLabel, b); }
```

- [ ] **Step 5: WM_COMMAND 处理 Add/Remove/Apply** — IDOK 分支末尾追加 Auto-frost 写入；`IDCANCEL` 分支前加两个按钮分支：

```cpp
        if (LOWORD(wp) == IDOK) {
            int v = (int)SendMessageW(hTrack, TBM_GETPOS, 0, 0);
            if (v >= 1 && v <= 20) { *p->pStep = v; g_step = v; write_int(L"TransparencyStep", v); }
            if (p->hasBlur && hBlurTrack) {
                int idx = (int)SendMessageW(hBlurTrack, TBM_GETPOS, 0, 0);
                if (idx >= 0 && idx < g_blurPresetCount) set_blur_radius(idx);
            }
            p->pAuto->enabled = SendMessageW(hEnableCk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            p->pAuto->defaultBlur = SendMessageW(hBlurCk, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int pct2 = (int)SendMessageW(hAlphaTrack, TBM_GETPOS, 0, 0);
            if (pct2 >= 50 && pct2 <= 100) p->pAuto->defaultAlpha = pct2 * 255 / 100;
            save_autofrost();
            return 0;
        } else if (LOWORD(wp) == 106) {           // Add current
            std::wstring exe = exe_name_of(p->fgWindow);
            bool dup = false;
            for (auto& a : p->pAuto->apps) if (_wcsicmp(a.exe.c_str(), exe.c_str()) == 0) dup = true;
            if (!exe.empty() && !dup) {
                p->pAuto->apps.push_back({exe, L""});
                refill_app_list(hAppList, p->pAuto);
            }
            return 0;
        } else if (LOWORD(wp) == 107) {           // Remove selected
            int sel = (int)SendMessageW(hAppList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < (int)p->pAuto->apps.size()) {
                p->pAuto->apps.erase(p->pAuto->apps.begin() + sel);
                refill_app_list(hAppList, p->pAuto);
            }
            return 0;
        } else if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); }
```

- [ ] **Step 6: show_settings 传参 + 窗口加高** — 修改两处：

```cpp
    SettingsParams sp = { &g_step, &g_blurRadius, g_hasBlurControl, &g_auto, GetForegroundWindow() };
    int h = g_hasBlurControl ? 420 : 390;
```

- [ ] **Step 7: 构建 + 手测**

Run: `cd native/build5 && cmake --build .`，运行 win2blur.exe → 托盘 Settings
Expected:
1. 新控件出现（Candara 字体生效），Enable 默认勾选、滑块显示 85%、列表显示 8 项（explorer.exe 带 (CabinetWClass)）
2. 激活记事本 → 打开 Settings → Add current → 列表出现 notepad.exe → Apply → 打开记事本另一实例 → ≤30s 自动应用
3. Remove 选中 notepad.exe → Apply → config.ini 中 App_8 消失
4. 取消 Enable → Apply → config.ini Enabled=0；重新勾选 → Apply → Enabled=1
5. 滑块拖到 70 → Apply → config.ini DefaultAlpha=178（70*255/100=178）
6. Apply 不关窗（现有行为保持）

- [ ] **Step 8: Commit**

```bash
git add native/src/tray_app.cpp
git commit -m "feat: autofrost settings UI (enable/alpha/blur/app list)"
```

---

### Task 5: 回归、打包 v2.7、更新文档

**Files:**
- Modify: `README.md`（Features 加 Auto-frost、Tray Menu 说明、Roadmap 勾选）
- Modify: `local/CLAUDE.md`（快捷键表加说明、架构树注释 v2.7）
- Create: `dist_release_2.7/`（5 文件：win2blur.exe + injector.exe + libfrosted_dwm.dll + acrylic_overlay.exe + welcome_demo.exe）

**Interfaces:**
- Consumes: 全部 Task 1–4 产物

- [ ] **Step 1: 完整回归**（30s 轮询正式值下）

Run: 构建后全量手测 — 热键四件套、Settings 全部控件、Add/Remove/Enable、Keep & Exit 后重启恢复、Restore & Exit 还原、自动应用 8 项预置逐一开软件验证、任务栏/桌面不受影响
Expected: 全部通过，无崩溃。

- [ ] **Step 2: 打包**

Run:
```bash
mkdir -p dist_release_2.7
cp native/build5/win2blur.exe dist_release_2.7/
cp native/dwm_inject/build/injector.exe dist_release_2.7/
cp native/dwm_inject/build/libfrosted_dwm.dll dist_release_2.7/
cp native/build5/acrylic_overlay.exe dist_release_2.7/
cp native/build5/welcome_demo.exe dist_release_2.7/
```
（若 acrylic/welcome 不在 build5，从 dist_release_2.6 复制——内容未变）
Expected: `ls dist_release_2.7` 5 个文件。

- [ ] **Step 3: 更新 README.md**

- Features 加一行：`- **Auto-frost** — 常用软件（Obsidian/VS Code/Edge/微信等 8 项预置）打开即自动 85% 透明 + 模糊，30s 内生效，可在 Settings 管理`
- Tray Menu Settings 行说明加 "auto-frost list"
- Roadmap 加已勾选项：`- [x] **Auto-frost presets** — done in v2.7`

- [ ] **Step 4: 更新 local/CLAUDE.md**

- 架构树 `tray_app.cpp` 注释改为 `# ★ 主程序 — 托盘+热键+透明度+overlay+模糊滑块+Auto-frost 监视线程`
- 快捷键表下方加 Auto-frost 段（30s 轮询、每窗口一次、config.ini [AutoFrost] 节、线程安全 g_fxLock）

- [ ] **Step 5: Commit + 标记发布**

```bash
git add native/src/tray_app.cpp README.md local/CLAUDE.md
git commit -m "v2.7: auto-frost — auto apply default effect to frequently used apps"
git tag v2.7
```

- [ ] **Step 6: 交付说明**

向用户报告：构建产物位置、手测结果、config.ini [AutoFrost] 节内容、vault 文档同步（project-sync）待用户触发 `sync proj`。
