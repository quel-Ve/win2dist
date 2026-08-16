# win2blur

Real-time window transparency + Acrylic frosted glass for Windows 10/11.

Zero flicker. Zero external dependencies. Custom blur radius (self-built
DWM hook, no third-party engine).

<p align="center">
  <img src="screenshot.png" width="600" alt="win2blur welcome demo">
</p>

## How it Works

```
Top:    Target window (semi-transparent via SetLayeredWindowAttributes)
Middle: Acrylic overlay (our native C++ window, DWM compositor blur)
Bottom: Desktop background
       → You see through the window at a blurred background = frosted glass
```

The overlay sits directly below your window in the z-order stack and blurs
whatever is behind it. Since v2.6, blur radius is controlled by our own
`libfrosted_dwm.dll` injected into dwm.exe (MinHook + hardcoded offsets,
built with MSVC) — no DWMBlurGlass, no third-party dependency.

## Features

- **Per-window transparency** — real-time control with global hotkeys
- **Acrylic blur** — native Windows Acrylic effect, zero-flicker
- **Custom blur radius** — Settings slider: 2/4/8/12/15/20/30/40/50, instant
- **Auto-frost** — 常用软件（Obsidian/VS Code/Edge/微信等 8 项预置）打开即自动 85% 透明 + 模糊，30s 内生效，可在 Settings 管理
- **Session restore** — keep your effects across restarts
- **Portable** — copy-and-run, no installation (one-time UAC for DWM hook)
- **180 Hz smooth** — overlay tracks window movement at your display's refresh rate

## Shortcuts

| Hotkey | Action |
|--------|--------|
| `ALT + ←` | More transparent (-step%) |
| `ALT + →` | Less transparent (+step%) |
| `ALT + ↑` | Toggle transparency on/off |
| `ALT + ↓` | Toggle Acrylic blur (2% black tint) |

## Tray Menu (right-click icon)

| Option | What it does |
|--------|-------------|
| **Settings** | Change the transparency step size (1–20%), blur radius, and manage the auto-frost list |
| **Restore && Exit** | Undo all effects, then quit |
| **Keep && Exit** | Leave effects active, auto-restore on next launch |

## Requirements

- **Windows 10** version 1803 or later (Windows 11 supported)
- No external dependencies

> Elevation: `install.bat` (run once) registers a logon task, so win2blur starts
> elevated and silent at every logon — zero UAC prompts. Portable copies without
> the task fall back to a single elevation consent at launch.

## Download

Get the latest `win2blur.exe` from the [Releases](../../releases) page.

The single `.exe` bundles everything — native C++ tray app, acrylic overlay
engine, and welcome demo. 493KB, portable.

## Build from Source

```bash
# Prerequisites: MinGW-w64 (g++ 13+), CMake 3.16+

cd native
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
cmake --build .

# Output: build/win2blur.exe (~493KB)
# Child exes (acrylic_overlay.exe, welcome_demo.exe) are embedded as
# RCDATA resources and extracted to %TEMP% at runtime.
```

## Architecture

```
├── native/src/
│   ├── tray_app.cpp          # C++ tray app (hotkeys, tray, settings)
│   ├── acrylic_overlay.cpp   # Zero-flicker acrylic window
│   ├── welcome_demo.cpp      # Startup demo with effect cycling
│   ├── resource.rc           # Icons + embedded child exes
│   └── app.ico               # Application icon
├── win2blur/                 # Python helper modules
│   ├── diagnose.py           # DWMBlurGlass pipeline diagnostic
│   ├── blur_controller.py    # Programmatic blur API (reference)
│   ├── tray_app.py           # Original Python tray app (reference)
│   ├── frosted_glass.py      # Acrylic overlay launcher (reference)
│   └── dist/                 # Distribution
│       ├── win2blur.exe      # Production build (~1.1MB)
│       ├── readme.md
│       └── install.bat / uninstall.bat
├── README.md
└── .gitignore
```

### Routes (how we explored the problem space)

| Route | Method | Flicker | Blur radius | Status |
|-------|--------|---------|-------------|--------|
| **A — Transparency** | `SetLayeredWindowAttributes` | zero | N/A | ✅ Production |
| **C — Acrylic Overlay** | Own DWM acrylic window behind target | zero | system-fixed | ✅ Production |
| E — DDA Capture | Desktop Duplication + GPU blur | 1 frame | custom | ⚠️ Experimental |
| **F — DWM Hook** | Self-built MSVC DLL + MinHook | zero | **custom** | ✅ v2.6 Production |

Route F details: hardcoded offsets extracted from DWMBlurGlass's shared
section (PDB symbol lookup fails — `CCustomBlur::*` are private symbols).
Built with MSVC for correct `ID2D1Effect` COM calls. Config via file IPC
(`C:\Temp\frosted_dwm_config.txt` — named shared memory is denied to dwm.exe).
Offsets are for Win10 build 19045 (dwmcore 10.0.19041.320).

## Roadmap

What we'd like to add in future releases:

- [x] **Customizable blur radius** — done in v2.6 (Settings slider, self-built
  DWM hook, DWMBlurGlass removed).
- [x] **Auto-frost presets** — done in v2.7 (8 built-in app presets auto-apply
  the default effect within 30s; managed from Settings).
- [ ] **Per-window blur radius** — extend config file to `hwnd=radius` lines;
  DLL resolves window via `CVisual::GetHwnd` (+0xE30F0).
- [ ] **Background contrast/saturation boost** — enhance colors behind
  the blur for a richer frosted glass look. Requires Route E (DDA capture)
  with our own Gaussian blur shader.
- [ ] **Per-region blur masks** — different blur radii across different
  areas of the window, creating a fluid gradient effect.
- [ ] **Multi-monitor DPI awareness** — proper scaling on mixed-DPI setups.
- [ ] **Tint profiles** — save and switch between tint presets via hotkey.

## Acknowledgments

This project stands on two excellent open-source foundations:

### [DWMBlurGlass](https://github.com/Maplespe/DWMBlurGlass) by Maplespe

DWMBlurGlass pioneered the technique of injecting a DLL into `dwm.exe` and
using MinHook to intercept DWM's internal blur rendering functions
(`CRenderingTechnique::ExecuteBlur`, `CCustomBlur::BuildEffect`, and
`CD2DContext_FillEffect`). We used its source code to:

- **Understand DWM's blur pipeline** — how the desktop compositor renders
  acrylic, blur, and mica effects, and which functions control blur radius
- **Study the MinHook injection chain** — `DWMBlurGlassHost.dll` (renamed
  EXE) → `DWMBlurGlassExt.dll` → `dwm.exe`, including the PDB symbol-based
  function offset resolution via `MHostLoadProcOffsetList()`
- **Build our diagnostic tool** (`window2clear/diagnose.py`) — checks every
  link in the DWMBlurGlass injection pipeline
- **Learn the IPC mechanism** — `WM_APP + 20` messages between host and DLL
  via `MDWMBlurGlassHostNotify` / `MDWMBlurGlassExtNotify` message-only windows

Route F (DWM Hook) in this project is a direct adaptation of DWMBlurGlass's
approach. It failed on Windows 10 build 19045.6466 due to PDB symbol
incompatibility, which motivated the development of our zero-injection
Route C (Acrylic Overlay).

### [Window2Clear](https://github.com/iwill123/Window2Clear) by iwill123

The original Window2Clear is a lightweight C++ Win32 tool for per-window
transparency control. We used its source to:

- **Validate our transparency approach** — `SetLayeredWindowAttributes` with
  `LWA_ALPHA` for real-time opacity control
- **Study the system-tray + global-hotkey pattern** — `RegisterHotKey` in a
  hidden message-only window with `Shell_NotifyIcon`
- **Cross-reference API behavior** — how `WS_EX_LAYERED` interacts with
  DWM compositor and `SetLayeredWindowAttributes` on Win10 vs Win7

Our v1.x Python tray app (`window2clear/tray_app.py`) was directly inspired
by its interaction model. The v2.0 C++ rewrite (`native/src/tray_app.cpp`)
is a clean-room implementation of the same pattern, extended with acrylic
overlay management, session save/restore, and resource embedding.

Thank you to Maplespe and Blinue for their excellent, well-documented code.

## FAQ

**Why not use DWMBlurGlass?**
DWMBlurGlass is a great project, but we had compatibility issues with
certain Windows builds. win2blur takes a simpler approach: instead of
injecting into `dwm.exe`, we insert our own acrylic window behind the
target. Same visual result, zero compatibility headaches.

**Does it work with everything?**
Yes — any standard Win32 window (Notepad, Obsidian, VS Code, browsers,
etc.). Some UWP apps with custom title bars may not respond to
transparency changes.

**Is it safe? Will it break my windows?**
`Restore && Exit` undoes everything. All effects are runtime-only — no
persistent system changes. The worst case: restart your computer and
everything is back to normal.

## License

MIT — see [LICENSE](LICENSE) for details.

---

*win2blur was originally named "Window2Blur" — renamed to avoid confusion
with the unrelated open-source project of the same name.*
