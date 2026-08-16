# win2blur v2.6 — Zero-Dependency Release

**2026-08-05**

## The Big Change: DWMBlurGlass REMOVED

Previous releases (v2.5) relied on DWMBlurGlass for blur radius control.
This release replaces it with our **self-built MSVC DLL** (`libfrosted_dwm.dll`)
that hooks DWM's internal blur rendering directly.

```
v2.5 (old):  win2blur → DWMBlurGlass config.ini → restart GUI → UAC every time
v2.6 (new):  win2blur → C:\Temp\frosted_dwm_config.txt → injected DLL reads it → instant
```

## What's in the box (5 files, ~1.5MB total)

| File | Role |
|------|------|
| `win2blur.exe` | Tray app — transparency, acrylic overlay, blur radius slider |
| `libfrosted_dwm.dll` | **Self-built** DWM hook — MinHook + hardcoded offsets (MSVC) |
| `injector.exe` | Injects the DLL into dwm.exe (needs admin, one-time per boot) |
| `acrylic_overlay.exe` | Acrylic overlay child process |
| `welcome_demo.exe` | Welcome demo child process |

## Blur Radius Control

- Settings → **Blur radius** slider: 2 / 4 / 8 / 12 / 15 / 20 / 30 / 40 / 50
- **Instant** — writes a config file, DLL picks it up on next effect build
- **No DWMBlurGlass** — no GUI, no extra install, no title-bar side effects

## How the injection works

1. `win2blur.exe` launches → detects `injector.exe` + `libfrosted_dwm.dll` beside it
2. Launches injector (first time: UAC prompt for admin) → injects DLL into dwm.exe
3. DLL hooks 3 DWM functions (Win10 19045, dwmcore 10.0.19041.320):
   - `CCustomBlur::DetermineOutputScale` → force 1.0 (kill 2.5x prescale)
   - `CCustomBlur::BuildEffect` → SetValue kernel StandardDeviation
   - `CD2DContext::FillEffect` → SetValue on BlurBehind effects
4. Blur slider writes `C:\Temp\frosted_dwm_config.txt` → DLL reads it every BuildEffect

## Known limits

- Offsets are for **Win10 build 19045** (dwmcore 10.0.19041.320). OS updates may
  require re-extracting offsets (see `native/dwm_inject/` for the extraction tooling).
- UAC prompt once per boot for injection (dwm.exe is PPL-protected).
- If `libfrosted_dwm.dll` / `injector.exe` are missing, blur slider hides and
  everything else works normally (graceful degradation).

## Source

- `native/dwm_inject/src/dllmain_msvc.cpp` — the injected DLL (MSVC build)
- `native/dwm_inject/src/injector.c` — injector (MinGW build)
- `native/dwm_inject/build_msvc.bat` — DLL build script
- Build: `D:\Garage\Software\ccproject\12window2clear\native\build5\`
