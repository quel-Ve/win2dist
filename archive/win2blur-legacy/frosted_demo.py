"""
Frosted Glass Demo — 纯 Win32 毛玻璃演示
=========================================
绕过 tkinter 的完全不透明背景限制，创建一个带透明度的
Win32 原生窗口，让 Acrylic 模糊效果真正可见。

alpha ~90 (约 35% 不透明度，高度透明)，叠加 Acrylic 模糊。
"""
import ctypes
from ctypes import wintypes
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# ============================================================
# Windows API
# ============================================================
user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32
dwmapi = ctypes.windll.dwmapi

# --- 常量 ---
CW_USEDEFAULT = 0x80000000
WM_PAINT = 0x000F
WM_DESTROY = 0x0002
LWA_ALPHA = 0x00000002
TRANSPARENT = 0x00000001
WS_OVERLAPPEDWINDOW = 0x00CF0000
WS_EX_LAYERED = 0x00080000
WS_EX_NOREDIRECTIONBITMAP = 0x00200000
DWMWA_NCRENDERING_POLICY = 2
DWMNCRP_ENABLED = 2
WCA_ACCENT_POLICY = 19
ACCENT_ENABLE_ACRYLICBLURBEHIND = 4

# --- 结构体 ---
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

class PAINTSTRUCT(ctypes.Structure):
    _fields_ = [
        ("hdc", wintypes.HDC),
        ("fErase", wintypes.BOOL),
        ("rcPaint", wintypes.RECT),
        ("fRestore", wintypes.BOOL),
        ("fIncUpdate", wintypes.BOOL),
        ("rgbReserved", ctypes.c_ubyte * 32),
    ]

# --- 窗口过程类型 ---
WNDPROC = ctypes.WINFUNCTYPE(wintypes.LPARAM, wintypes.HWND, ctypes.c_uint,
                              wintypes.WPARAM, wintypes.LPARAM)

class WNDCLASSW(ctypes.Structure):
    _fields_ = [
        ("style", ctypes.c_uint),
        ("lpfnWndProc", WNDPROC),
        ("cbClsExtra", ctypes.c_int),
        ("cbWndExtra", ctypes.c_int),
        ("hInstance", wintypes.HINSTANCE),
        ("hIcon", wintypes.HICON),
        ("hCursor", wintypes.HANDLE),
        ("hbrBackground", wintypes.HBRUSH),
        ("lpszMenuName", wintypes.LPCWSTR),
        ("lpszClassName", wintypes.LPCWSTR),
    ]

# Fix argtypes for DefWindowProcW to prevent OverflowError
user32.DefWindowProcW.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = wintypes.LPARAM


def apply_acrylic(hwnd: int, gradient_color: int = 0x80FFFFFF) -> bool:
    """Apply Acrylic blur via SetWindowCompositionAttribute."""
    policy = AccentPolicy()
    policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND
    policy.AccentFlags = 0
    policy.GradientColor = gradient_color
    policy.AnimationId = 0

    data = WindowCompositionAttributeData()
    data.Attribute = WCA_ACCENT_POLICY
    data.SizeOfData = ctypes.sizeof(policy)
    data.Data = ctypes.cast(ctypes.pointer(policy), ctypes.c_void_p)

    SetWindowCompositionAttribute = user32.SetWindowCompositionAttribute
    SetWindowCompositionAttribute.argtypes = [wintypes.HWND, ctypes.POINTER(WindowCompositionAttributeData)]
    SetWindowCompositionAttribute.restype = wintypes.BOOL

    return bool(SetWindowCompositionAttribute(hwnd, ctypes.byref(data)))


@WNDPROC
def wnd_proc(hwnd, msg, wparam, lparam):
    """Window procedure — handle paint and destroy."""
    if msg == WM_PAINT:
        ps = PAINTSTRUCT()
        hdc = user32.BeginPaint(hwnd, ctypes.byref(ps))
        rect = wintypes.RECT()
        user32.GetClientRect(hwnd, ctypes.byref(rect))

        # NULL_BRUSH: don't fill background → Acrylic blur shows through
        null_brush = gdi32.GetStockObject(5)
        old_brush = gdi32.SelectObject(hdc, null_brush)

        # Transparent text background (SetBkMode is in gdi32)
        gdi32.SetBkMode(hdc, TRANSPARENT)
        gdi32.SetTextColor(hdc, 0x00FFFFFF)

        # Big bold font
        font = gdi32.CreateFontW(
            48, 0, 0, 0, 700, 0, 0, 0, 0, 0, 0, 0, 0,
            "Microsoft YaHei UI"
        )
        old_font = gdi32.SelectObject(hdc, font)

        text = "毛玻璃 Frosted Glass"
        # DT_CENTER | DT_VCENTER | DT_SINGLELINE
        user32.DrawTextW(hdc, text, -1, ctypes.byref(rect), 0x00000025)

        gdi32.SelectObject(hdc, old_font)
        gdi32.DeleteObject(font)
        gdi32.SelectObject(hdc, old_brush)
        user32.EndPaint(hwnd, ctypes.byref(ps))
        return 0

    elif msg == WM_DESTROY:
        user32.PostQuitMessage(0)
        return 0

    return user32.DefWindowProcW(hwnd, msg, wparam, lparam)


# ============================================================
# Main
# ============================================================
def main():
    ALPHA = 90          # ~35% opacity (highly transparent)
    TINT = 0x80FFFFFF   # 50% white acrylic
    WINDOW_CLASS = "FrostedGlassDemo"

    hinst = kernel32.GetModuleHandleW(None)

    wc = WNDCLASSW()
    wc.style = 0
    wc.lpfnWndProc = wnd_proc
    wc.hInstance = hinst
    wc.hbrBackground = gdi32.GetStockObject(5)  # NULL_BRUSH
    wc.lpszClassName = WINDOW_CLASS
    wc.hCursor = user32.LoadCursorW(0, 32512)   # IDC_ARROW

    if not user32.RegisterClassW(ctypes.byref(wc)):
        print(f"[x] RegisterClass failed: {ctypes.get_last_error()}")
        return

    hwnd = user32.CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
        WINDOW_CLASS,
        f"Frosted Glass — alpha={ALPHA} acrylic",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 500,
        0, 0, hinst, 0
    )

    if not hwnd:
        print(f"[x] CreateWindowEx failed: {ctypes.get_last_error()}")
        return

    # 1. Acrylic blur
    if apply_acrylic(hwnd, TINT):
        print(f"[OK] Acrylic blur  (tint=0x{TINT:08X})")
    else:
        print("[x] Acrylic blur failed")

    # 2. Extend DWM frame into client area
    val = ctypes.c_int(DWMNCRP_ENABLED)
    dwmapi.DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY,
                                  ctypes.byref(val), ctypes.sizeof(val))

    # 3. Layered transparency (alpha=90)
    user32.SetLayeredWindowAttributes(hwnd, 0, ALPHA, LWA_ALPHA)
    print(f"[OK] Transparency  (alpha={ALPHA}/255 ≈ {ALPHA*100//255}%)")

    user32.ShowWindow(hwnd, 1)
    user32.UpdateWindow(hwnd)

    print(f"\n{'='*60}")
    print(f"  毛玻璃窗口已创建 — 关窗退出")
    print(f"  看看窗口后面的内容是否被模糊了？")
    print(f"{'='*60}\n")

    msg = wintypes.MSG()
    while user32.GetMessageW(ctypes.byref(msg), 0, 0, 0):
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))

    print("窗口已关闭")


if __name__ == '__main__':
    main()
