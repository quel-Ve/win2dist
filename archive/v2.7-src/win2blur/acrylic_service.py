"""
Acrylic Blur Service — 激活静止窗口自动毛玻璃
==============================================
后台监听前台窗口切换和移动事件：
  - 窗口激活 + 停止移动 0.5s → 应用 Acrylic 模糊
  - 窗口移动 → 移除模糊（避免拖拽卡顿）
  - 窗口切走 → 移除模糊

默认参数：Acrylic + 50% 白色色调，可通过 config.ini 调整。

用法:
  python -m window2clear.acrylic_service
"""
import ctypes
from ctypes import wintypes
import time
import sys
import os
import threading

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# ============================================================
# Windows API
# ============================================================
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# --- Accent API ---
class AccentPolicy(ctypes.Structure):
    _fields_ = [
        ("AccentState", ctypes.c_int),
        ("AccentFlags", ctypes.c_int),
        ("GradientColor", ctypes.c_uint),
        ("AnimationId", ctypes.c_int),
    ]

class WindowCompositionAttributeData(ctypes.Structure):
    _fields_ = [
        ("Attribute", ctypes.c_int),
        ("Data", ctypes.c_void_p),
        ("SizeOfData", ctypes.c_int),
    ]

WCA_ACCENT_POLICY = 19
ACCENT_DISABLED = 0
ACCENT_ENABLE_BLURBEHIND = 3
ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
ACCENT_ENABLE_HOSTBACKDROP = 5  # Mica (Win11)

SetWindowCompositionAttribute = user32.SetWindowCompositionAttribute
SetWindowCompositionAttribute.argtypes = [wintypes.HWND, ctypes.POINTER(WindowCompositionAttributeData)]
SetWindowCompositionAttribute.restype = wintypes.BOOL


def apply_accent(hwnd: int, accent_state: int, gradient_color: int = 0x80FFFFFF) -> bool:
    """Apply DWM backdrop effect to a window."""
    policy = AccentPolicy()
    policy.AccentState = accent_state
    policy.AccentFlags = 0
    policy.GradientColor = gradient_color
    policy.AnimationId = 0

    data = WindowCompositionAttributeData()
    data.Attribute = WCA_ACCENT_POLICY
    data.SizeOfData = ctypes.sizeof(policy)
    data.Data = ctypes.cast(ctypes.pointer(policy), ctypes.c_void_p)

    return bool(SetWindowCompositionAttribute(hwnd, ctypes.byref(data)))


def remove_accent(hwnd: int) -> bool:
    """Remove DWM backdrop effect."""
    return apply_accent(hwnd, ACCENT_DISABLED)


# --- WinEvent hooks ---
EVENT_SYSTEM_FOREGROUND = 0x0003
EVENT_OBJECT_LOCATIONCHANGE = 0x800B
WINEVENT_OUTOFCONTEXT = 0x0000

WinEventProc = ctypes.WINFUNCTYPE(
    None,  # void return
    wintypes.HANDLE,  # hWinEventHook
    wintypes.DWORD,   # event
    wintypes.HWND,    # hwnd
    wintypes.LONG,    # idObject
    wintypes.LONG,    # idChild
    wintypes.DWORD,   # idEventThread
    wintypes.DWORD,   # dwmsEventTime
)


class AcrylicService:
    """Watches foreground window changes and applies Acrylic blur when settled."""

    def __init__(self,
                 accent_type: str = "acrylic",
                 gradient_color: int = 0x80FFFFFF,
                 settle_delay: float = 0.5):
        self.accent_state = {
            "blur": ACCENT_ENABLE_BLURBEHIND,
            "acrylic": ACCENT_ENABLE_ACRYLICBLURBEHIND,
            "mica": ACCENT_ENABLE_HOSTBACKDROP,
        }[accent_type]
        self.gradient_color = gradient_color
        self.settle_delay = settle_delay

        # State
        self.current_hwnd: int | None = None
        self.current_rect: tuple[int, int, int, int] = (0, 0, 0, 0)
        self.blur_active = False
        self.last_move_time: float = 0.0
        self.settle_timer: threading.Timer | None = None
        self.running = False

    # ---- WinEvent callbacks ----
    def _on_foreground(self, hook, event, hwnd, id_object, id_child, thread, time_ms):
        """Called when foreground window changes."""
        if id_object != 0 or id_child != 0:
            return  # Only care about window-level events
        if hwnd == self.current_hwnd:
            return

        # Remove blur from old window
        if self.current_hwnd and self.blur_active:
            remove_accent(self.current_hwnd)
            print(f"[blur off] 0x{self.current_hwnd:08X} (switched away)")

        # Cancel pending settle timer
        if self.settle_timer:
            self.settle_timer.cancel()

        self.current_hwnd = hwnd
        self._update_rect(hwnd)
        self.blur_active = False
        self._try_settle()

    def _on_location_change(self, hook, event, hwnd, id_object, id_child, thread, time_ms):
        """Called when a window moves or resizes."""
        if id_object != 0 or id_child != 0:
            return
        if hwnd != self.current_hwnd:
            return

        # Check if actually moved
        new_rect = self._get_rect(hwnd)
        if new_rect == self.current_rect:
            return

        self.current_rect = new_rect
        self.last_move_time = time.time()

        # Cancel settle timer
        if self.settle_timer:
            self.settle_timer.cancel()

        # Remove blur while moving (for performance)
        if self.blur_active:
            remove_accent(hwnd)
            self.blur_active = False

        # Re-arm settle timer
        self._try_settle()

    # ---- Internal ----
    def _get_rect(self, hwnd: int) -> tuple[int, int, int, int]:
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        return (r.left, r.top, r.right, r.bottom)

    def _update_rect(self, hwnd: int):
        self.current_rect = self._get_rect(hwnd)

    def _try_settle(self):
        """Schedule blur application after settle delay."""
        if self.settle_timer:
            self.settle_timer.cancel()
        self.settle_timer = threading.Timer(self.settle_delay, self._on_settled)
        self.settle_timer.daemon = True
        self.settle_timer.start()

    def _on_settled(self):
        """Window has stopped moving — apply blur."""
        hwnd = self.current_hwnd
        if not hwnd:
            return

        # Verify window still exists and hasn't moved
        if not user32.IsWindow(hwnd):
            return
        current = self._get_rect(hwnd)
        if current != self.current_rect:
            return  # Moved during delay, timer will re-arm from callback

        if not self.blur_active:
            success = apply_accent(hwnd, self.accent_state, self.gradient_color)
            if success:
                self.blur_active = True
                title = self._get_title(hwnd)
                print(f"[blur on]  {title} (0x{hwnd:08X})")

    def _get_title(self, hwnd: int) -> str:
        length = user32.GetWindowTextLengthW(hwnd)
        if length == 0:
            return "<untitled>"
        buf = ctypes.create_unicode_buffer(length + 1)
        user32.GetWindowTextW(hwnd, buf, length + 1)
        return buf.value

    # ---- Public ----
    def start(self):
        """Start the service (blocking — runs message loop)."""
        self.running = True
        print(f"Acrylic Service — {self.accent_state=} tint=0x{self.gradient_color:08X}")
        print("Activate a window and stop moving → blur applied")
        print("Press Ctrl+C to stop\n")

        # Register event hooks
        fore_cb = WinEventProc(self._on_foreground)
        loc_cb = WinEventProc(self._on_location_change)

        hook_fore = user32.SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            0, fore_cb, 0, 0, WINEVENT_OUTOFCONTEXT
        )
        hook_loc = user32.SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            0, loc_cb, 0, 0, WINEVENT_OUTOFCONTEXT
        )

        if not hook_fore:
            print(f"[x] SetWinEventHook(FOREGROUND) failed: {ctypes.get_last_error()}")
            return
        if not hook_loc:
            print(f"[x] SetWinEventHook(LOCATIONCHANGE) failed: {ctypes.get_last_error()}")

        print(f"[hooks] foreground={hook_fore} location={hook_loc}")

        # Message loop (required for WinEvent callbacks)
        msg = wintypes.MSG()
        try:
            while self.running:
                # Use PeekMessage + short sleep for responsiveness + CPU efficiency
                while user32.PeekMessageW(ctypes.byref(msg), 0, 0, 0, 1):  # PM_REMOVE
                    user32.TranslateMessage(ctypes.byref(msg))
                    user32.DispatchMessageW(ctypes.byref(msg))

                # Check settle condition (fallback for missed events)
                if self.current_hwnd and not self.blur_active:
                    elapsed = time.time() - self.last_move_time
                    if elapsed > self.settle_delay:
                        self._on_settled()

                time.sleep(0.05)  # 50ms tick
        except KeyboardInterrupt:
            print("\nStopping...")
        finally:
            self.running = False
            if self.settle_timer:
                self.settle_timer.cancel()
            if self.current_hwnd and self.blur_active:
                remove_accent(self.current_hwnd)
            user32.UnhookWinEvent(hook_fore)
            user32.UnhookWinEvent(hook_loc)
            print("Service stopped")


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description="Acrylic Blur Service")
    p.add_argument('--type', choices=['blur', 'acrylic', 'mica'], default='acrylic')
    p.add_argument('--tint', type=lambda x: int(x, 16), default=0x80FFFFFF,
                   help='Gradient color in hex (default: 0x80FFFFFF)')
    p.add_argument('--delay', type=float, default=0.5,
                   help='Settle delay in seconds (default: 0.5)')
    args = p.parse_args()

    svc = AcrylicService(
        accent_type=args.type,
        gradient_color=args.tint,
        settle_delay=args.delay,
    )
    svc.start()
