"""
Shared window enumeration utilities.
Used by both blur.py and transparency.py for finding windows.
"""
import ctypes
from ctypes import wintypes

user32 = ctypes.windll.user32


def find_by_title(title_substr: str) -> list[tuple[int, str]]:
    """Find all visible windows whose title contains the given substring.
    Returns list of (hwnd, title) tuples.
    """
    results = []

    def enum_callback(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            if length > 0:
                buf = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buf, length + 1)
                if title_substr.lower() in buf.value.lower():
                    results.append((hwnd, buf.value))
        return True

    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows(WNDENUMPROC(enum_callback), 0)
    return results


def list_all() -> list[tuple[int, str]]:
    """List all visible, titled windows. Returns list of (hwnd, title) tuples."""
    results = []

    def enum_callback(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            if length > 0:
                buf = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buf, length + 1)
                results.append((hwnd, buf.value))
        return True

    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows(WNDENUMPROC(enum_callback), 0)
    return results


def get_title(hwnd: int) -> str:
    """Get the title of a window by its handle."""
    length = user32.GetWindowTextLengthW(hwnd)
    if length == 0:
        return "<untitled>"
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def is_visible(hwnd: int) -> bool:
    """Check if a window is currently visible."""
    return bool(user32.IsWindowVisible(hwnd))
