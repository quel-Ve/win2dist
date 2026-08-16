# win2blur

Window transparency + Acrylic frosted glass. Zero flicker, portable.

No UAC prompts: run `install.bat` once (one-time consent) to register a
logon task — win2blur then auto-starts elevated and silent at every logon.

## Quick start

1. Double-click `win2blur.exe`
2. Click any window, then use the hotkeys:

| Shortcut | Effect |
|----------|--------|
| `ALT + ←` | More transparent |
| `ALT + →` | Less transparent |
| `ALT + ↑` | Toggle transparency on/off |
| `ALT + ↓` | Toggle Acrylic blur |

Right-click the tray icon for settings and exit options.

## Start Menu shortcut

Double-click `install.bat` to add win2blur to the Start Menu.
Double-click `uninstall.bat` to remove it.

## Exit modes

- **Restore && Exit** — undo all effects, clean quit
- **Keep && Exit** — keep effects active, auto-restore on next launch

## Requirements

- Windows 10 (version 1803+) or Windows 11
- No installation — `install.bat` once, then zero prompts

## Uninstall

Delete the `win2blur` folder. If you created a desktop shortcut, run `uninstall.bat` first.

---

win2blur uses the native Windows DWM compositor to create a zero-flicker
acrylic overlay behind your windows — no DLL injection, no system hooks.
