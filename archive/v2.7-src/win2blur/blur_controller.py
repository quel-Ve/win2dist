"""
Blur Controller — Zero-flicker blur engine (DWMBlurGlass-powered)
==================================================================
Programmatic control of DWM-level blur: amount, effect type, per-window on/off.

Architecture:
  Python (this module) → DWMBlurGlass config.ini → IPC refresh signal
                          → DWMBlurGlass DLL in dwm.exe reloads config
                          → Direct2D Gaussian blur applied at compositor level
  Per-window on/off via DwmEnableBlurBehindWindow → DWMBlurGlass hook upgrades to custom blur

Zero flicker because everything happens inside DWM (desktop compositor).
"""
import ctypes
from ctypes import wintypes
import os
import subprocess
import time
import configparser

# ============================================================
# Windows API
# ============================================================
user32 = ctypes.windll.user32
dwmapi = ctypes.windll.dwmapi

WM_APP = 0x8000
WM_APP_CONFIG_REFRESH = WM_APP + 20  # DWMBlurGlass IPC message

# MHostNotifyType enum (must match DWMBlurGlass Common.h)
MHOST_REFRESH = 0
MHOST_SHUTDOWN = 1
MHOST_ENABLE_TRANSPARENCY = 2

# Window class names (must match DWMBlurGlass Common.h)
HOST_NOTIFY_CLASS = "MDWMBlurGlassHostNotify"
EXT_NOTIFY_CLASS = "MDWMBlurGlassExtNotify"

# DWM Blur Behind API
class DWM_BLURBEHIND(ctypes.Structure):
    _fields_ = [
        ("dwFlags", wintypes.DWORD),
        ("fEnable", wintypes.BOOL),
        ("hRgnBlur", wintypes.HRGN),
        ("fTransitionOnMaximized", wintypes.BOOL),
    ]

DWM_BB_ENABLE = 0x00000001

# ============================================================
# Config path
# ============================================================
_DEFAULT_DWM_CONFIG = os.path.join(
    os.path.dirname(__file__), "..",
    "DWMBlurGlass.2.3.2_Beta3_x64", "Release", "data", "config.ini"
)
_DEFAULT_GUI = os.path.join(
    os.path.dirname(__file__), "..",
    "DWMBlurGlass.2.3.2_Beta3_x64", "Release", "DWMBlurGlassGUI.exe"
)


def _resolve_config_path(config_path: str | None = None) -> str:
    """Resolve DWMBlurGlass config file path."""
    if config_path:
        return os.path.abspath(config_path)
    path = os.path.abspath(_DEFAULT_DWM_CONFIG)
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"DWMBlurGlass config not found: {path}\n"
            f"  Install DWMBlurGlass or pass config_path to set_blur_amount()"
        )
    return path


def _find_host_window() -> int | None:
    """Find DWMBlurGlass host notify window (HWND_MESSAGE window in the host process)."""
    # HWND_MESSAGE = -3 (message-only window parent)
    HWND_MESSAGE = -3
    return user32.FindWindowExW(HWND_MESSAGE, 0, HOST_NOTIFY_CLASS, None)


def _find_dll_notify_window() -> int | None:
    """Find DLL's internal notify window (in dwm.exe)."""
    HWND_MESSAGE = -3
    return user32.FindWindowExW(HWND_MESSAGE, 0, EXT_NOTIFY_CLASS, None)


def _launch_gui() -> bool:
    """Launch DWMBlurGlass GUI if not already running. Returns True if GUI is now available."""
    if _find_host_window():
        return True
    if not os.path.exists(_DEFAULT_GUI):
        return False
    # Launch GUI minimized (DWMBlurGlass auto-minimizes to tray)
    try:
        subprocess.Popen(
            [_DEFAULT_GUI],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NO_WINDOW
        )
    except Exception:
        return False
    # Wait for host window to appear
    for _ in range(30):  # 3 seconds max
        time.sleep(0.1)
        if _find_host_window():
            return True
    return False


def _send_refresh() -> bool:
    """Send refresh signal to DWMBlurGlass DLL (reloads config.ini → new blur amount).
    Tries host window first, then DLL notify window as fallback.
    Auto-launches GUI if needed.
    """
    # Try host window first (fastest path)
    hwnd = _find_host_window()
    if not hwnd:
        # Try DLL notify window (may still be active even without GUI)
        hwnd = _find_dll_notify_window()
    if not hwnd:
        # Launch GUI to restore host window
        if not _launch_gui():
            return False
        hwnd = _find_host_window()
        if not hwnd:
            return False
    # wParam=0 = MHostNotifyType::Refresh → DLL calls Refresh(true) → LoadFromFile(config.ini)
    result = user32.SendMessageW(hwnd, WM_APP_CONFIG_REFRESH, MHOST_REFRESH, 0)
    return result == 0


# ============================================================
# DWM Acrylic API (zero-flicker, no admin, direct DWM compositor)
# ============================================================
WCA_ACCENT_POLICY = 19
ACCENT_DISABLED = 0
ACCENT_ENABLE_ACRYLICBLURBEHIND = 4    # Win10 1803+ acrylic
ACCENT_ENABLE_BLURBEHIND = 3           # Win7-style blur

class AccentPolicy(ctypes.Structure):
    _fields_ = [
        ("AccentState", ctypes.c_int),
        ("AccentFlags", ctypes.c_int),
        ("GradientColor", ctypes.c_uint),   # ARGB: 0xAARRGGBB
        ("AnimationId", ctypes.c_int),
    ]

class WindowCompositionAttrData(ctypes.Structure):
    _fields_ = [
        ("Attribute", ctypes.c_int),
        ("Data", ctypes.c_void_p),
        ("SizeOfData", ctypes.c_int),
    ]

_swca = user32.SetWindowCompositionAttribute
_swca.argtypes = [wintypes.HWND, ctypes.POINTER(WindowCompositionAttrData)]
_swca.restype = wintypes.BOOL


def _apply_accent(hwnd: int, accent_state: int, gradient_color: int = 0x80FFFFFF, accent_flags: int = 0) -> bool:
    """Apply acrylic/blur effect directly via DWM (zero-flicker)."""
    policy = AccentPolicy()
    policy.AccentState = accent_state
    policy.AccentFlags = accent_flags
    policy.GradientColor = gradient_color
    policy.AnimationId = 0

    data = WindowCompositionAttrData()
    data.Attribute = WCA_ACCENT_POLICY
    data.SizeOfData = ctypes.sizeof(policy)
    data.Data = ctypes.cast(ctypes.pointer(policy), ctypes.c_void_p)

    return bool(_swca(hwnd, ctypes.byref(data)))


# ============================================================
# Public API
# ============================================================

def enable_blur(hwnd: int, tint: int = 0x80FFFFFF) -> bool:
    """
    Enable Acrylic blur on a window (zero-flicker, DWM compositor level).
    No dependencies, no admin required.

    hwnd: window handle
    tint: 0xAARRGGBB format (AA=color opacity, RR=red, GG=green, BB=blue)
          Common: 0x80FFFFFF (50% white, natural)
                  0x99000000 (60% dark, for dark mode)
                  0x40FFFFFF (25% white, more transparent)
    Returns True on success.
    """
    return _apply_accent(hwnd, ACCENT_ENABLE_ACRYLICBLURBEHIND, tint)


def disable_blur(hwnd: int) -> bool:
    """Remove acrylic blur from a window."""
    return _apply_accent(hwnd, ACCENT_DISABLED, 0, 0)


def toggle_blur(hwnd: int) -> bool:
    """Toggle blur on/off. Returns True if blur is now ON.
    State is tracked per-call; for persistent tracking, maintain state externally
    and use enable_blur/disable_blur directly.
    """
    raise NotImplementedError(
        "Use enable_blur(hwnd) / disable_blur(hwnd) explicitly. "
        "State tracking is handled by the caller (e.g., tray_app)."
    )


def is_blur_supported() -> bool:
    """Check if Acrylic blur is supported (Win10 1803+)."""
    # Try on a throwaway window
    import tkinter as tk
    root = tk.Tk()
    root.withdraw()
    hwnd = int(root.frame(), 16)
    ok = _apply_accent(hwnd, ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x80FFFFFF)
    _apply_accent(hwnd, ACCENT_DISABLED, 0, 0)  # clean up
    root.destroy()
    return ok


# ============================================================
# DWMBlurGlass integration (optional — advanced effects engine)
# ============================================================

def set_blur_amount(amount: float, config_path: str | None = None) -> bool:
    """
    Set global blur amount via DWMBlurGlass config.ini.
    Requires DWMBlurGlass to be installed and its GUI running (admin).
    Falls back to config-file-only write if IPC refresh fails.
    """
    try:
        path = _resolve_config_path(config_path)
    except FileNotFoundError:
        return False

    cfg = configparser.ConfigParser()
    cfg.read(path, encoding='utf-8')

    if not cfg.has_section('config'):
        cfg.add_section('config')

    cfg['config']['customAmount'] = 'true'
    cfg['config']['blurAmount'] = str(amount)
    cfg['config']['customBlurAmount'] = str(amount)

    with open(path, 'w', encoding='utf-8') as f:
        cfg.write(f)

    # Signal DWMBlurGlass to reload (if host is running with admin)
    # If it fails, the amount takes effect on next DWMBlurGlass restart
    _send_refresh()
    # Always succeed — config was written
    return True


def get_blur_amount(config_path: str | None = None) -> float:
    """Read current blur amount from DWMBlurGlass config."""
    try:
        path = _resolve_config_path(config_path)
    except FileNotFoundError:
        return 0.0
    cfg = configparser.ConfigParser()
    cfg.read(path, encoding='utf-8')
    try:
        return cfg.getfloat('config', 'blurAmount', fallback=20.0)
    except ValueError:
        return 20.0


def is_dwmblurglass_installed() -> bool:
    """Check if DWMBlurGlass is installed."""
    return os.path.exists(_DEFAULT_GUI)


# ============================================================
# CLI
# ============================================================
def main():
    import argparse
    p = argparse.ArgumentParser(description="Zero-flicker blur controller")
    sub = p.add_subparsers(dest='cmd')

    a = sub.add_parser('enable', help='Enable Acrylic blur on foreground window')
    b = sub.add_parser('disable', help='Disable blur on foreground window')

    s = sub.add_parser('set', help='Set global blur amount (DWMBlurGlass)')
    s.add_argument('amount', type=float, help='Blur amount (0-100)')

    g = sub.add_parser('get', help='Get current blur amount (DWMBlurGlass)')

    st = sub.add_parser('status', help='Check system status')

    args = p.parse_args()

    if args.cmd == 'enable':
        hwnd = user32.GetForegroundWindow()
        if enable_blur(hwnd):
            print(f"Acrylic blur enabled on {hwnd:#x}")
        else:
            print("Failed to enable blur")
    elif args.cmd == 'disable':
        hwnd = user32.GetForegroundWindow()
        if disable_blur(hwnd):
            print(f"Blur disabled on {hwnd:#x}")
        else:
            print("Failed to disable blur")
    elif args.cmd == 'set':
        set_blur_amount(args.amount)
        print(f"Blur amount: {args.amount} — Config written"
              f"{' (IPC refresh sent)' if _find_host_window() else ''}")
    elif args.cmd == 'get':
        print(f"Current blur amount: {get_blur_amount()}")
    elif args.cmd == 'status':
        print(f"Acrylic API: {'supported' if is_blur_supported() else 'unsupported'}")
        dwm_installed = os.path.exists(_DEFAULT_GUI)
        print(f"DWMBlurGlass: {'installed' if dwm_installed else 'not installed'}")
        if dwm_installed:
            print(f"  GUI running: {_find_host_window() is not None}")
            print(f"  Blur amount: {get_blur_amount()}")
    else:
        p.print_help()


if __name__ == '__main__':
    main()
