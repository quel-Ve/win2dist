"""
Frosted Glass PoC — Python control layer
=========================================
Calls native blur_backdrop.exe to create a blurred backdrop behind a target window.
Monitors target window position/size and manages the native helper lifecycle.

Usage:
  python -m window2clear.frosted_glass                 # Use foreground window
  python -m window2clear.frosted_glass --title Notepad  # Find by title
"""
import ctypes
from ctypes import wintypes
import subprocess
import sys
import os
import time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

user32 = ctypes.windll.user32

# Path to native helper — prefer acrylic_overlay (zero-flicker) over blur_backdrop (DDA)
_OVERLAY_EXE = os.path.join(os.path.dirname(__file__),
                            "..", "native", "build2", "acrylic_overlay.exe")
_BACKDROP_EXE = os.path.join(os.path.dirname(__file__),
                             "..", "native", "build2", "blur_backdrop.exe")
_NATIVE_EXE = _OVERLAY_EXE if os.path.exists(_OVERLAY_EXE) else _BACKDROP_EXE


# Unsafe window classes - never target these
_UNSAFE_CLASSES = {
    "Progman", "Shell_TrayWnd", "Shell_SecondaryTrayWnd",
    "Windows.UI.Core.CoreWindow", "ApplicationFrameWindow",
    "Button", "Static",
}


def _get_class(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(hwnd, buf, 64)
    return buf.value


def _get_title(hwnd: int) -> str:
    length = user32.GetWindowTextLengthW(hwnd)
    if length == 0:
        return "<untitled>"
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def _is_safe_target(hwnd: int) -> bool:
    """Reject console, system shell, and UI control windows."""
    cls = _get_class(hwnd)
    if cls in _UNSAFE_CLASSES:
        return False
    if cls in ("ConsoleWindowClass", "CASCADIA_HOSTING_WINDOW_CLASS"):
        return False
    return True


def find_foreground() -> int | None:
    """Get HWND of the foreground window (excludes console/system windows)."""
    hwnd = user32.GetForegroundWindow()
    if hwnd and not _is_safe_target(hwnd):
        print(f"[!] Foreground window skipped (unsafe class: {_get_class(hwnd)})")
        return None
    return hwnd


def find_by_title(substr: str) -> list[tuple[int, str, str]]:
    """Find windows by title substring. Returns [(hwnd, title, class), ...]."""
    results = []
    def enum_cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            if length > 0:
                buf = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buf, length + 1)
                if substr.lower() in buf.value.lower():
                    if _is_safe_target(hwnd):
                        results.append((hwnd, buf.value, _get_class(hwnd)))
        return True
    WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
    return results


def run_backdrop(hwnd: int, tint: int = 0x00000000) -> subprocess.Popen | None:
    """Launch native acrylic overlay. tint: 0x00000000=pure blur 0x80FFFFFF=50% white."""
    if not os.path.exists(_NATIVE_EXE):
        print(f"[x] Native helper not found: {_NATIVE_EXE}")
        print(f"    Build it: cd native && mkdir build2 && cd build2 && cmake -G 'MinGW Makefiles' .. && cmake --build .")
        return None

    hwnd_hex = f"0x{hwnd:08X}"
    tint_hex = f"0x{tint:08X}"
    print(f"[*] Launching acrylic overlay for HWND {hwnd_hex} (tint={tint_hex})")
    proc = subprocess.Popen(
        [_NATIVE_EXE, hwnd_hex, tint_hex],
        stdout=None, stderr=None,
    )
    return proc


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--title", "-t", help="Window title substring")
    p.add_argument("--tint", type=lambda x: int(x, 16), default=0x00000000,
                   help="Tint ARGB (default: 0x00000000 = pure blur, 0x80FFFFFF = 50%% white)")
    args = p.parse_args()

    if args.title:
        matches = find_by_title(args.title)
        if not matches:
            print(f"[x] No safe window matching '{args.title}'")
            return
        if len(matches) > 1:
            print(f"Found {len(matches)} matches (pick one with --title):")
            for hwnd, title, cls in matches:
                print(f"  0x{hwnd:08X}  [{cls}]  {title}")
            return
        hwnd, title, cls = matches[0]
        print(f"Target: {title}  [{cls}]")
    else:
        print("Click the window you want to blur, then come back here and press Enter...")
        # Poll foreground while waiting - remember the last safe non-console window
        import msvcrt
        last_safe_hwnd = None
        while True:
            if msvcrt.kbhit():
                key = msvcrt.getch()
                if key in (b'\r', b'\n'):
                    break
            hwnd = user32.GetForegroundWindow()
            if hwnd and _is_safe_target(hwnd):
                last_safe_hwnd = hwnd
            time.sleep(0.05)
        if not last_safe_hwnd:
            print("[x] No safe window detected. Try --title instead.")
            return
        hwnd = last_safe_hwnd
        print(f"Target: {_get_title(hwnd)}  [{_get_class(hwnd)}]")

    proc = run_backdrop(hwnd, args.tint)
    if not proc:
        return

    print("Press Enter to stop...")
    try:
        input()
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        proc.terminate()
        proc.wait()
        print("Blur backdrop stopped")


if __name__ == "__main__":
    main()
