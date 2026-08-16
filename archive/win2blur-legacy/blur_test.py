"""
Acrylic 效果测试 — 对真实窗口实时切换模糊模式
=============================================
不同于 blur_demo.py（tkinter 看不到效果），本脚本直接操作当前前台窗口。
键盘切换模式，肉眼观察标题栏/框架区变化。

用法:
  python -m window2clear.blur_test              # 操作前台窗口
  python -m window2clear.blur_test --title Obsidian  # 指定窗口

按键:
  1-7  切换模糊类型/色调
  r    移除效果
  q    退出
"""
import ctypes
from ctypes import wintypes
import sys
import time
import threading

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

# ============================================================
# Accent API
# ============================================================
class AccentPolicy(ctypes.Structure):
    _fields_ = [
        ("AccentState", ctypes.c_int),
        ("AccentFlags", ctypes.c_int),
        ("GradientColor", ctypes.c_uint),
        ("AnimationId", ctypes.c_int),
    ]

class WCAData(ctypes.Structure):
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

SetWCA = user32.SetWindowCompositionAttribute
SetWCA.argtypes = [wintypes.HWND, ctypes.POINTER(WCAData)]
SetWCA.restype = wintypes.BOOL


def apply(hwnd: int, state: int, color: int = 0x80FFFFFF) -> bool:
    policy = AccentPolicy()
    policy.AccentState = state
    policy.AccentFlags = 0
    policy.GradientColor = color
    policy.AnimationId = 0
    data = WCAData()
    data.Attribute = WCA_ACCENT_POLICY
    data.SizeOfData = ctypes.sizeof(policy)
    data.Data = ctypes.cast(ctypes.pointer(policy), ctypes.c_void_p)
    return bool(SetWCA(hwnd, ctypes.byref(data)))


# ============================================================
# 预设模式
# ============================================================
PRESETS = [
    # (key, label, state, color)
    ("1", "Blur Behind (Win7 高斯模糊)",     ACCENT_ENABLE_BLURBEHIND,        0x00000000),
    ("2", "Blur + 50% 白色染色",             ACCENT_ENABLE_BLURBEHIND,        0x80FFFFFF),
    ("3", "Blur + 80% 白色染色",             ACCENT_ENABLE_BLURBEHIND,        0xCCFFFFFF),
    ("4", "Acrylic (Win10 亚克力) 无色 ★",   ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x00000000),
    ("5", "Acrylic + 50% 白色 ★ (推荐)",     ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x80FFFFFF),
    ("6", "Acrylic + 60% 暗色",             ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x99000000),
    ("7", "Mica (Win11 云母)",              ACCENT_ENABLE_HOSTBACKDROP,       0x00000000),
]


def get_foreground() -> int | None:
    return user32.GetForegroundWindow()


def get_title(hwnd: int) -> str:
    length = user32.GetWindowTextLengthW(hwnd)
    if length == 0:
        return "<untitled>"
    buf = ctypes.create_unicode_buffer(length + 1)
    user32.GetWindowTextW(hwnd, buf, length + 1)
    return buf.value


def get_class(hwnd: int) -> str:
    buf = ctypes.create_unicode_buffer(128)
    user32.GetClassNameW(hwnd, buf, 128)
    return buf.value


def _kb_thread():
    """Raw keyboard input thread (uses getch for cross-platform)."""
    global _last_key
    try:
        import msvcrt
        while True:
            ch = msvcrt.getch().decode('ascii', errors='ignore').lower()
            _last_key = ch
            if ch == 'q':
                break
    except ImportError:
        pass


# ============================================================
# Main
# ============================================================
def main():
    import argparse
    p = argparse.ArgumentParser(description="Acrylic Blur Test")
    p.add_argument('--title', '-t', help='Window title substring to target')
    args = p.parse_args()

    # Find target window
    if args.title:
        hwnd = None
        def enum_cb(h, _):
            nonlocal hwnd
            if user32.IsWindowVisible(h):
                buf = ctypes.create_unicode_buffer(256)
                user32.GetWindowTextW(h, buf, 256)
                if args.title.lower() in buf.value.lower():
                    hwnd = h
                    return False
            return True
        WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
        if not hwnd:
            print(f"[x] No window matching '{args.title}'")
            return
        print(f"Target: {get_title(hwnd)}  [{get_class(hwnd)}]")
        target_hwnd = hwnd
    else:
        print("Using foreground window (click the window you want to test, then come back here)")
        target_hwnd = None

    print()
    for key, label, _, _ in PRESETS:
        print(f"  [{key}] {label}")
    print(f"  [r] 移除效果")
    print(f"  [q] 退出")
    print()

    # Start keyboard thread
    global _last_key
    _last_key = None
    kb = threading.Thread(target=_kb_thread, daemon=True)
    kb.start()

    current_preset = -1
    last_hwnd = None

    try:
        while True:
            time.sleep(0.1)

            if target_hwnd is None:
                target_hwnd = get_foreground()

            # Detect foreground change
            if target_hwnd != last_hwnd:
                title = get_title(target_hwnd)
                cls = get_class(target_hwnd)
                print(f"\n>>> 前台窗口: {title}  [{cls}]")
                last_hwnd = target_hwnd

            # Handle key press
            if _last_key is not None:
                key = _last_key
                _last_key = None

                if key == 'q':
                    break
                elif key == 'r':
                    apply(target_hwnd, ACCENT_DISABLED)
                    current_preset = -1
                    print(f"  [移除] 效果已清除")
                else:
                    for k, label, state, color in PRESETS:
                        if key == k:
                            apply(target_hwnd, state, color)
                            current_preset = PRESETS.index((k, label, state, color))
                            indicator = "✓" if state != ACCENT_DISABLED else ""
                            print(f"  [{k}] {label} {indicator}")
                            break
                    else:
                        print(f"  [?] 未知按键: {key}")

    except KeyboardInterrupt:
        pass
    finally:
        # Cleanup
        if target_hwnd:
            apply(target_hwnd, ACCENT_DISABLED)
        print("\n效果已清除，退出")


_last_key = None

if __name__ == '__main__':
    main()
