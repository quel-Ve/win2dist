# win2blur v2.5

**2026-07-31**

## What's New

### Blur Radius Control
- Integrated DWMBlurGlass blur radius management with discrete presets: **2, 4, 8, 12, 15, 20, 30, 40, 50**
- Instant Apply via IPC — no UI restart, no UAC prompt after initial install
- Automatically minimizes title bar side effects (effecttype=Blur, applyglobal=false)

### UI Improvements
- Candara font replaces system default for a cleaner look
- Apply button keeps Settings window open for iterative tuning
- Close button / X to dismiss

### Z-Order Fix
- Acrylic overlay now uses triple-call z-order correction (0ms / 200ms / 600ms)
- Synchronous SetWindowPos — no more race conditions with async compositor

### Under the Hood
- Route F DWM injection subsystem (experimental, not integrated)
- DLL injector with auto-unload of previous versions
- Hardcoded function offsets for Win10 19045 dwmcore 10.0.19041.320

## Dependencies

**DWMBlurGlass 2.3.2 Beta 3** (bundled) for blur radius control.
First launch requires one-time admin authorization for DWM injection.
Subsequent radius changes are instant with no prompts.

## Files

- `win2blur.exe` — main tray application
- `DWMBlurGlass.2.3.2_Beta3_x64/` — DWMBlurGlass (install once, blur radius via Settings)
