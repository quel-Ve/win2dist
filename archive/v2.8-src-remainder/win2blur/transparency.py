"""
Window transparency via SetLayeredWindowAttributes.
Controls overall window opacity (0-100%), independent of blur effects.
"""
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32

# Window styles
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
LWA_ALPHA = 0x00000002
LWA_COLORKEY = 0x00000001


def make_layered(hwnd: int) -> bool:
    """Add WS_EX_LAYERED style to a window (required for SetLayeredWindowAttributes)."""
    current_style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    if not (current_style & WS_EX_LAYERED):
        user32.SetWindowLongW(hwnd, GWL_EXSTYLE, current_style | WS_EX_LAYERED)
    return True


def set_opacity(hwnd: int, opacity: int) -> bool:
    """
    Set window opacity.

    Args:
        hwnd: Window handle
        opacity: 0-255 (0 = fully transparent, 255 = fully opaque)
    """
    make_layered(hwnd)
    result = user32.SetLayeredWindowAttributes(hwnd, 0, opacity, LWA_ALPHA)
    if not result:
        err = ctypes.get_last_error()
        print(f"[x] SetLayeredWindowAttributes failed: error {err}")
    return bool(result)


def set_opacity_percent(hwnd: int, percent: int) -> bool:
    """Set window opacity as percentage (0-100)."""
    percent = max(0, min(100, percent))
    alpha = int(255 * percent / 100)
    return set_opacity(hwnd, alpha)


def get_opacity(hwnd: int) -> int | None:
    """Get current layered window transparency (0-255), or None if not layered."""
    style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
    if not (style & WS_EX_LAYERED):
        return None
    # There's no direct "get" API; return None as sentinel
    return None


def remove_opacity(hwnd: int) -> bool:
    """Reset window to fully opaque."""
    return set_opacity(hwnd, 255)
