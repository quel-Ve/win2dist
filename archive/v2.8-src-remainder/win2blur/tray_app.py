"""
Window2Clear Tray App - system tray transparency + acrylic overlay.

Usage:
  python -m window2clear.tray_app
"""
import ctypes
from ctypes import wintypes
import sys
import os
import subprocess
import threading
import tkinter as tk
from tkinter import ttk

from window2clear.config import load, save, CONFIG_PATH

if sys.stdout is not None:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# ============================================================
# Windows API
# ============================================================
user32 = ctypes.windll.user32
shell32 = ctypes.windll.shell32
kernel32 = ctypes.windll.kernel32
dwmapi = ctypes.windll.dwmapi
gdi32 = ctypes.windll.gdi32

# --- Fix argtypes to prevent OverflowError on x64 ---
user32.DefWindowProcW.argtypes = [wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM]
user32.DefWindowProcW.restype = wintypes.LPARAM

# --- Window messages ---
WM_CREATE = 0x0001
WM_DESTROY = 0x0002
WM_HOTKEY = 0x0312
WM_TRAYICON = 0x8001

# --- Hotkey IDs ---
ID_UP = 1
ID_DOWN = 2
ID_TOGGLE = 3
ID_ACRYLIC = 4

# --- Tray icon ---
NIM_ADD = 0
NIM_DELETE = 2
NIF_ICON = 0x00000002
NIF_MESSAGE = 0x00000001
NIF_TIP = 0x00000004

# --- Menu ---
MF_STRING = 0x00000000
MF_SEPARATOR = 0x00000800
WM_COMMAND = 0x0111
ID_SETTINGS = 1001
ID_EXIT_RESTORE = 1002
ID_EXIT_KEEP = 1003

# --- Window styles ---
WS_EX_LAYERED = 0x00080000
WS_EX_NOREDIRECTIONBITMAP = 0x00200000
WS_OVERLAPPEDWINDOW = 0x00CF0000
GWL_EXSTYLE = -20
LWA_ALPHA = 0x00000002


class NOTIFYICONDATAW(ctypes.Structure):
    _fields_ = [
        ("cbSize", wintypes.DWORD),
        ("hWnd", wintypes.HWND),
        ("uID", ctypes.c_uint),
        ("uFlags", ctypes.c_uint),
        ("uCallbackMessage", ctypes.c_uint),
        ("hIcon", wintypes.HICON),
        ("szTip", ctypes.c_wchar * 128),
        ("dwState", wintypes.DWORD),
        ("dwStateMask", wintypes.DWORD),
        ("szInfo", ctypes.c_wchar * 256),
        ("uTimeoutOrVersion", ctypes.c_uint),
        ("szInfoTitle", ctypes.c_wchar * 64),
        ("dwInfoFlags", wintypes.DWORD),
    ]



# --- Acrylic Overlay (zero-flicker, DWM native, no injection needed) ---
# Launches acrylic_overlay.exe as a subprocess per window.
# Overlay creates a transparent acrylic window behind target → DWM blurs background.

# Resolve acrylic_overlay.exe for both dev and PyInstaller builds
if getattr(sys, 'frozen', False):
    # PyInstaller onefile: bundled data extracted to _MEIPASS
    _OVERLAY_EXE = os.path.join(sys._MEIPASS, "acrylic_overlay.exe")
else:
    _OVERLAY_EXE = os.path.join(os.path.dirname(__file__),
        "..", "native", "build4", "acrylic_overlay.exe")
    if not os.path.exists(_OVERLAY_EXE):
        _OVERLAY_EXE = os.path.join(os.path.dirname(__file__),
            "..", "acrylic_overlay.exe")  # project root copy

# Tint cycle: 2% black → OFF
TINT_CYCLE = [
    0x05000000,   # 2% black
]
DEFAULT_TINT = TINT_CYCLE[0]

# Resolve welcome_demo.exe path (dev + PyInstaller)
if getattr(sys, 'frozen', False):
    _WELCOME_EXE = os.path.join(sys._MEIPASS, "welcome_demo.exe")
else:
    _WELCOME_EXE = os.path.join(os.path.dirname(__file__), "..", "native", "build4", "welcome_demo.exe")
    if not os.path.exists(_WELCOME_EXE):
        _WELCOME_EXE = os.path.join(os.path.dirname(__file__), "..", "welcome_demo.exe")

def _start_overlay(hwnd: int, tint: int = DEFAULT_TINT) -> subprocess.Popen | None:
    """Launch acrylic overlay behind a window. Returns Popen or None."""
    if not os.path.exists(_OVERLAY_EXE):
        print(f"[!] acrylic_overlay.exe not found: {_OVERLAY_EXE}")
        return None
    proc = subprocess.Popen(
        [_OVERLAY_EXE, f"0x{hwnd:08X}", f"0x{tint:08X}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        creationflags=subprocess.CREATE_NO_WINDOW,
    )
    return proc

def _stop_overlay(proc: subprocess.Popen | None):
    """Terminate acrylic overlay subprocess."""
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()


# ============================================================
# Welcome Demo Window (pure Win32, in-process)
# ============================================================
def _welcome_run(app):
    """Create Win32 window cycling native → transparent → acrylic every 2s."""
    W, H = 420, 420
    CN = "win2blurWelcome"
    BTN_ID = 1001; TIMER_ID = 1

    class PAINTSTRUCT(ctypes.Structure):
        _fields_ = [("hdc", wintypes.HDC), ("fErase", wintypes.BOOL),
                    ("rcPaint", wintypes.RECT), ("fRestore", wintypes.BOOL),
                    ("fIncUpdate", wintypes.BOOL), ("rgbReserved", ctypes.c_ubyte * 32)]

    # Fonts / brushes (module-level to survive wndproc callbacks)
    hfont_title = gdi32.CreateFontW(28, 0, 0, 0, 700, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI")
    hfont_body = gdi32.CreateFontW(17, 0, 0, 0, 400, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI")
    hfont_btn = gdi32.CreateFontW(17, 0, 0, 0, 600, 0, 0, 0, 0, 0, 0, 0, 0, "Segoe UI")

    effects = [("native", "No effect  (100% opaque)", 255, False),
               ("transparent", "Transparent  (~78%)", 200, False),
               ("acrylic", "Acrylic Blur  (2% black)", 200, True)]
    state = {"idx": 1, "hwnd": None}  # start at transparent

    def apply(hwnd, name, alpha, use_acrylic):
        policy = AccentPolicy()
        policy.AccentState = 4 if use_acrylic else 0  # 4=ACCENT_ENABLE_ACRYLIC, 0=DISABLED
        policy.AccentFlags = 0
        policy.GradientColor = 0x1A000000 if use_acrylic else 0
        data = WindowCompositionAttrData()
        data.Attribute = 19; data.SizeOfData = ctypes.sizeof(policy)
        data.Data = ctypes.cast(ctypes.pointer(policy), ctypes.c_void_p)
        user32.SetWindowCompositionAttribute(hwnd, ctypes.byref(data))
        user32.SetLayeredWindowAttributes(hwnd, 0, alpha, 2)  # LWA_ALPHA

    class AccentPolicy(ctypes.Structure):
        _fields_ = [("AccentState", ctypes.c_int), ("AccentFlags", ctypes.c_int),
                    ("GradientColor", ctypes.c_uint), ("AnimationId", ctypes.c_int)]
    class WindowCompositionAttrData(ctypes.Structure):
        _fields_ = [("Attribute", ctypes.c_int), ("Data", ctypes.c_void_p),
                    ("SizeOfData", ctypes.c_int)]

    # Window proc (all constants numeric — avoids closure NameError)
    @ctypes.WINFUNCTYPE(wintypes.LPARAM, wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM)
    def _wp(hwnd, msg, wparam, lparam):
        if msg == 0x0001:  # WM_CREATE
            user32.SetTimer(hwnd, TIMER_ID, 2000, None)
            return 0
        elif msg == 0x0113:  # WM_TIMER
            state["idx"] = (state["idx"] + 1) % 3
            _, alpha_val, use_acr = effects[state["idx"]][1], effects[state["idx"]][2], effects[state["idx"]][3]
            apply(hwnd, "", alpha_val, use_acr)
            user32.InvalidateRect(hwnd, None, True)
            return 0
        elif msg == 0x0111:  # WM_COMMAND
            if wparam == BTN_ID:
                apply(hwnd, "", 255, False)
                user32.DestroyWindow(hwnd)
            return 0
        elif msg == 0x000F:  # WM_PAINT
            ps = PAINTSTRUCT()
            hdc = user32.BeginPaint(hwnd, ctypes.byref(ps))
            rect = wintypes.RECT(); user32.GetClientRect(hwnd, ctypes.byref(rect))
            w, h_r = rect.right, rect.bottom
            hbr = gdi32.CreateSolidBrush(0x1E1E1E)
            user32.FillRect(hdc, ctypes.byref(rect), hbr)
            gdi32.DeleteObject(hbr)
            gdi32.SetBkMode(hdc, 1)

            gdi32.SetTextColor(hdc, 0x4FC3F7)
            gdi32.SelectObject(hdc, hfont_title)
            tr = wintypes.RECT(); tr.top=18; tr.bottom=52; tr.left=0; tr.right=w
            user32.DrawTextW(hdc, "win2blur", -1, ctypes.byref(tr), 0x25)

            gdi32.SetTextColor(hdc, 0xA0A0A0)
            gdi32.SelectObject(hdc, hfont_body)
            sr = wintypes.RECT(); sr.top=54; sr.bottom=74; sr.left=0; sr.right=w
            user32.DrawTextW(hdc, "Transparency + Acrylic Frosted Glass", -1, ctypes.byref(sr), 0x25)

            name, label = effects[state["idx"]][0], effects[state["idx"]][1]
            gdi32.SetTextColor(hdc, 0x4FC3F7)
            er = wintypes.RECT(); er.top=90; er.bottom=114; er.left=0; er.right=w
            user32.DrawTextW(hdc, label, -1, ctypes.byref(er), 0x25)

            gdi32.SetTextColor(hdc, 0xA0A0A0)
            lines = ["ALT+LEFT / RIGHT   Transparency", "ALT+UP   Toggle on/off", "ALT+DOWN   Acrylic blur cycle",
                     "", "Tray  right-click   Settings | Restore&Exit | Keep&Exit"]
            y = 130
            for line in lines:
                lr = wintypes.RECT(); lr.top=y; lr.bottom=y+22; lr.left=0; lr.right=w
                user32.DrawTextW(hdc, line, -1, ctypes.byref(lr), 0x25)
                y += 22

            gdi32.SelectObject(hdc, hfont_body)
            user32.EndPaint(hwnd, ctypes.byref(ps))
            return 0
        elif msg == 0x0002:  # WM_DESTROY
            user32.KillTimer(hwnd, TIMER_ID)
            user32.PostQuitMessage(0)
            return 0
        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    hinst = kernel32.GetModuleHandleW(None)
    class WNDCLASSW(ctypes.Structure):
        _fields_ = [("style", ctypes.c_uint), ("lpfnWndProc", type(_wp)),
                    ("cbClsExtra", ctypes.c_int), ("cbWndExtra", ctypes.c_int),
                    ("hInstance", wintypes.HINSTANCE), ("hIcon", wintypes.HICON),
                    ("hCursor", wintypes.HANDLE), ("hbrBackground", wintypes.HBRUSH),
                    ("lpszMenuName", wintypes.LPCWSTR), ("lpszClassName", wintypes.LPCWSTR)]
    wc = WNDCLASSW()
    wc.lpfnWndProc = _wp; wc.hInstance = hinst; wc.lpszClassName = CN
    wc.hbrBackground = gdi32.GetStockObject(5)  # NULL_BRUSH
    wc.hCursor = user32.LoadCursorW(0, 32512)
    if not user32.RegisterClassW(ctypes.byref(wc)): return

    sw = user32.GetSystemMetrics(0); sh = user32.GetSystemMetrics(1)
    x = (sw - W) // 2; y = (sh - H) // 2

    hwnd = user32.CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP,
        CN, "win2blur — Welcome",
        WS_OVERLAPPEDWINDOW & ~(0x00020000 | 0x00040000),
        x, y, W, H, 0, 0, hinst, 0)
    if not hwnd: return
    state["hwnd"] = hwnd

    # DWM frame extension
    val = ctypes.c_int(2); dwmapi.DwmSetWindowAttribute(hwnd, 2, ctypes.byref(val), ctypes.sizeof(val))

    # "Got it" button
    user32.CreateWindowExW(0, "BUTTON", "Got it", 0x50000000 | 0x10000000 | 0x8000,
        (W - 120) // 2, H - 70, 120, 36, hwnd, BTN_ID, hinst, 0)

    # Start at transparent
    apply(hwnd, "", 200, False)
    user32.ShowWindow(hwnd, 1)

    # Run welcome message loop (blocks until "Got it" clicked)
    msg = wintypes.MSG()
    while user32.GetMessageW(ctypes.byref(msg), 0, 0, 0):
        user32.TranslateMessage(ctypes.byref(msg))
        user32.DispatchMessageW(ctypes.byref(msg))

    gdi32.DeleteObject(hfont_title)
    gdi32.DeleteObject(hfont_body)
    gdi32.DeleteObject(hfont_btn)

class Window2Clear:
    """Main application: tray icon + global hotkeys + transparency control."""

    def __init__(self):
        self.cfg = load()
        self.step = int(self.cfg["Settings"]["TransparencyStep"])
        self.hwnd = None
        self.running = False
        self._wnd_proc_cb = None

        self.up_mod = int(self.cfg["Hotkeys"]["TransparencyUpModifiers"])
        self.up_key = int(self.cfg["Hotkeys"]["TransparencyUpKey"])
        self.down_mod = int(self.cfg["Hotkeys"]["TransparencyDownModifiers"])
        self.down_key = int(self.cfg["Hotkeys"]["TransparencyDownKey"])
        self.toggle_mod = int(self.cfg["Hotkeys"]["TransparencyToggleModifiers"])
        self.toggle_key = int(self.cfg["Hotkeys"]["TransparencyToggleKey"])

        self._last_alpha: dict[int, int] = {}   # hwnd -> last transparent alpha
        self._modified: set[int] = set()        # hwnds we made layered/transparent
        self._overlays: dict[int, tuple[subprocess.Popen, int]] = {}  # hwnd -> (proc, tint_index)

        self.acrylic_mod = int(self.cfg["Hotkeys"]["AcrylicToggleModifiers"])
        self.acrylic_key = int(self.cfg["Hotkeys"]["AcrylicToggleKey"])

    # ---- Window procedure ----
    def _wnd_proc(self, hwnd, msg, wparam, lparam):
        if msg == WM_CREATE:
            self._add_tray_icon(hwnd)
            self._register_hotkeys(hwnd)
            return 0

        elif msg == WM_HOTKEY:
            self._on_hotkey(wparam)
            return 0

        elif msg == WM_TRAYICON:
            if lparam == 0x0205:  # WM_RBUTTONUP
                self._show_menu(hwnd)
            return 0

        elif msg == WM_COMMAND:
            if wparam == ID_SETTINGS:
                self._open_settings()
            elif wparam == ID_EXIT_RESTORE:
                self._shutdown(restore=True)
            elif wparam == ID_EXIT_KEEP:
                self._shutdown(restore=False)
            return 0

        elif msg == WM_DESTROY:
            user32.PostQuitMessage(0)
            return 0

        return user32.DefWindowProcW(hwnd, msg, wparam, lparam)

    # ---- Tray icon ----
    def _add_tray_icon(self, hwnd):
        nid = NOTIFYICONDATAW()
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        nid.hWnd = hwnd
        nid.uID = 1
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP
        nid.uCallbackMessage = WM_TRAYICON
        nid.hIcon = user32.LoadIconW(0, 32512)  # IDI_APPLICATION
        nid.szTip = "win2blur"
        shell32.Shell_NotifyIconW(NIM_ADD, ctypes.byref(nid))

    def _remove_tray_icon(self, hwnd):
        nid = NOTIFYICONDATAW()
        nid.cbSize = ctypes.sizeof(NOTIFYICONDATAW)
        nid.hWnd = hwnd
        nid.uID = 1
        shell32.Shell_NotifyIconW(NIM_DELETE, ctypes.byref(nid))

    # ---- Hotkeys ----
    def _register_hotkeys(self, hwnd):
        if self.cfg["Switches"].getboolean("EnableTransparencyUp", True):
            ok = user32.RegisterHotKey(hwnd, ID_UP, self.up_mod, self.up_key)
            if not ok:
                print(f"[!] RegisterHotKey(UP) failed: mod={self.up_mod} key={self.up_key}")

        if self.cfg["Switches"].getboolean("EnableTransparencyDown", True):
            ok = user32.RegisterHotKey(hwnd, ID_DOWN, self.down_mod, self.down_key)
            if not ok:
                print(f"[!] RegisterHotKey(DOWN) failed: mod={self.down_mod} key={self.down_key}")

        if self.cfg["Switches"].getboolean("EnableTransparencyToggle", True):
            ok = user32.RegisterHotKey(hwnd, ID_TOGGLE, self.toggle_mod, self.toggle_key)
            if not ok:
                print(f"[!] RegisterHotKey(TOGGLE) failed: mod={self.toggle_mod} key={self.toggle_key}")

        if self.cfg["Switches"].getboolean("EnableAcrylicToggle", True):
            ok = user32.RegisterHotKey(hwnd, ID_ACRYLIC, self.acrylic_mod, self.acrylic_key)
            if not ok:
                print(f"[!] RegisterHotKey(ACRYLIC) failed: mod={self.acrylic_mod} key={self.acrylic_key}")

    def _on_hotkey(self, hotkey_id):
        hwnd = user32.GetForegroundWindow()
        if not hwnd:
            return

        style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
        if not (style & WS_EX_LAYERED):
            user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED)

        alpha_byte = ctypes.c_ubyte(255)
        flags = wintypes.DWORD()
        user32.GetLayeredWindowAttributes(hwnd, ctypes.byref(wintypes.COLORREF()),
                                          ctypes.byref(alpha_byte), ctypes.byref(flags))

        alpha = alpha_byte.value
        if alpha == 0:
            alpha = 255

        step = self.step * 255 // 100
        if hotkey_id == ID_UP:
            alpha = max(10, alpha - step)
        elif hotkey_id == ID_DOWN:
            alpha = min(255, alpha + step)
        elif hotkey_id == ID_TOGGLE:
            if alpha < 255:
                # Currently transparent -> save and make opaque
                self._last_alpha[hwnd] = alpha
                alpha = 255
            else:
                # Currently opaque -> restore last alpha
                alpha = self._last_alpha.get(hwnd, 255 - step * 255 // 100)
                if alpha >= 255:
                    alpha = 255 - step * 255 // 100

        elif hotkey_id == ID_ACRYLIC:
            if hwnd in self._overlays:
                # Cycle to next tint, or OFF if at the end
                old_proc, old_idx = self._overlays[hwnd]
                new_idx = old_idx + 1
                _stop_overlay(old_proc)
                del self._overlays[hwnd]
                if new_idx < len(TINT_CYCLE):
                    # Next tint level
                    proc = _start_overlay(hwnd, TINT_CYCLE[new_idx])
                    if proc:
                        self._overlays[hwnd] = (proc, new_idx)
                # else: cycled past the end → OFF
            else:
                # Start at first tint level
                if TINT_CYCLE:
                    proc = _start_overlay(hwnd, TINT_CYCLE[0])
                    if proc:
                        self._overlays[hwnd] = (proc, 0)

            length = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(max(length + 1, 32))
            user32.GetWindowTextW(hwnd, buf, length + 1)
            if hwnd in self._overlays:
                idx = self._overlays[hwnd][1]
                pct = (idx + 1) * 20  # rough opacity for label
                print(f"  [{buf.value[:40]}] blur={pct}%")
            else:
                print(f"  [{buf.value[:40]}] blur OFF")
            return

        user32.SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA)
        self._modified.add(hwnd)

        pct = alpha * 100 // 255
        length = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(max(length + 1, 32))
        user32.GetWindowTextW(hwnd, buf, length + 1)
        label = "opaque" if alpha == 255 else f"opacity={pct}%"
        print(f"  [{buf.value[:40]}] {label}")

    # ---- Tray menu ----
    def _show_menu(self, hwnd):
        menu = user32.CreatePopupMenu()
        user32.AppendMenuW(menu, MF_STRING, ID_SETTINGS, "Settings")
        user32.AppendMenuW(menu, MF_SEPARATOR, 0, None)
        user32.AppendMenuW(menu, MF_STRING, ID_EXIT_RESTORE, "Restore && Exit")
        user32.AppendMenuW(menu, MF_STRING, ID_EXIT_KEEP, "Keep && Exit")

        pt = wintypes.POINT()
        user32.GetCursorPos(ctypes.byref(pt))
        user32.SetForegroundWindow(hwnd)
        user32.TrackPopupMenu(menu, 0, pt.x, pt.y, 0, hwnd, None)
        user32.DestroyMenu(menu)

    # ---- Settings dialog ----
    def _open_settings(self):
        def _run_dialog():
            root = tk.Tk()
            root.title("Window2Clear Settings")
            root.geometry("340x260")
            root.resizable(False, False)
            root.configure(bg="#f0f0f0")

            frame = ttk.Frame(root, padding=16)
            frame.pack(fill=tk.BOTH, expand=True)

            ttk.Label(frame, text="Transparency step per hotkey press (%)").pack(anchor=tk.W)
            step_var = tk.IntVar(value=self.step)
            step_scale = ttk.Scale(frame, from_=1, to=20, variable=step_var,
                                   orient=tk.HORIZONTAL, command=lambda v: step_var.set(int(float(v))))
            step_scale.pack(fill=tk.X, pady=(4, 4))
            step_lbl = ttk.Label(frame, text=f"{self.step}%")
            step_lbl.pack(anchor=tk.E)

            def _update_step_label(*_):
                step_lbl.config(text=f"{step_var.get()}%")
            step_var.trace_add("write", _update_step_label)

            ttk.Separator(frame).pack(fill=tk.X, pady=8)

            mod_names = {1: "ALT", 2: "CTRL", 4: "SHIFT", 8: "WIN"}
            key_names = {37: "⬅", 38: "⬆", 39: "➡", 40: "⬇"}
            up_name = f"{mod_names.get(self.up_mod, str(self.up_mod))}+{key_names.get(self.up_key, str(self.up_key))}"
            down_name = f"{mod_names.get(self.down_mod, str(self.down_mod))}+{key_names.get(self.down_key, str(self.down_key))}"
            toggle_name = f"{mod_names.get(self.toggle_mod, str(self.toggle_mod))}+{key_names.get(self.toggle_key, str(self.toggle_key))}"
            acrylic_name = f"{mod_names.get(self.acrylic_mod, str(self.acrylic_mod))}+{key_names.get(self.acrylic_key, str(self.acrylic_key))}"
            ttk.Label(frame, text=f"More transparent:  {up_name}").pack(anchor=tk.W, pady=(0, 2))
            ttk.Label(frame, text=f"Less transparent: {down_name}").pack(anchor=tk.W, pady=(0, 2))
            ttk.Label(frame, text=f"Toggle on/off:       {toggle_name}").pack(anchor=tk.W, pady=(0, 2))
            ttk.Label(frame, text=f"Acrylic blur:         {acrylic_name}").pack(anchor=tk.W)

            btn_frame = ttk.Frame(frame)
            btn_frame.pack(side=tk.BOTTOM, fill=tk.X, pady=(12, 0))

            def _on_ok():
                self.step = step_var.get()
                self.cfg["Settings"]["TransparencyStep"] = str(self.step)
                save(self.cfg)
                root.destroy()

            ttk.Button(btn_frame, text="Save", command=_on_ok).pack(side=tk.RIGHT, padx=4)
            ttk.Button(btn_frame, text="Cancel", command=root.destroy).pack(side=tk.RIGHT, padx=4)

            root.mainloop()

        threading.Thread(target=_run_dialog, daemon=True).start()

    # ---- Lifecycle ----
    def _shutdown(self, restore: bool = True):
        self.running = False
        if restore:
            for hwnd in self._modified:
                try:
                    style = user32.GetWindowLongW(hwnd, GWL_EXSTYLE)
                    if style & WS_EX_LAYERED:
                        user32.SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA)
                        user32.SetWindowLongW(hwnd, GWL_EXSTYLE, style & ~WS_EX_LAYERED)
                except Exception:
                    pass
            if self._modified:
                print(f"Restored {len(self._modified)} window(s)")
            self._modified.clear()
            for hwnd, (proc, idx) in list(self._overlays.items()):
                _stop_overlay(proc)
            self._overlays.clear()
            self._save_session({})
        else:
            self._save_session_state()
            print(f"Keeping {len(self._modified)} window(s), {len(self._overlays)} overlay(s)")
        if self.hwnd:
            user32.UnregisterHotKey(self.hwnd, ID_UP)
            user32.UnregisterHotKey(self.hwnd, ID_DOWN)
            user32.UnregisterHotKey(self.hwnd, ID_TOGGLE)
            user32.UnregisterHotKey(self.hwnd, ID_ACRYLIC)
            self._remove_tray_icon(self.hwnd)
            user32.DestroyWindow(self.hwnd)

    def _save_session_state(self):
        """Save current window states for next-launch restore."""
        items = []
        for hwnd in self._modified:
            try:
                length = user32.GetWindowTextLengthW(hwnd)
                buf = ctypes.create_unicode_buffer(max(length + 1, 64))
                user32.GetWindowTextW(hwnd, buf, length + 1)
                title = buf.value
                cls_buf = ctypes.create_unicode_buffer(64)
                user32.GetClassNameW(hwnd, cls_buf, 64)
                cls = cls_buf.value
                alpha_byte = ctypes.c_ubyte(255)
                user32.GetLayeredWindowAttributes(hwnd, ctypes.byref(wintypes.COLORREF()),
                                                  ctypes.byref(alpha_byte), ctypes.byref(wintypes.DWORD()))
                alpha = alpha_byte.value
                tint_idx = self._overlays[hwnd][1] if hwnd in self._overlays else -1
                items.append(f"{title}|{cls}|{alpha}|{tint_idx}")
            except Exception:
                pass
        self._save_session(items)

    def _save_session(self, items: list[str]):
        cfg = load()
        if cfg.has_section('Session'):
            cfg.remove_section('Session')
        cfg.add_section('Session')
        for i, item in enumerate(items):
            cfg['Session'][f'window_{i}'] = item
        save(cfg)

    def _restore_session(self):
        """Restore window states from previous session. Called at startup."""
        cfg = load()
        if not cfg.has_section('Session'):
            return
        items = [cfg['Session'][k] for k in cfg['Session']]
        if not items:
            return
        restored = 0
        for item in items:
            try:
                parts = item.split('|')
                if len(parts) < 3: continue
                title, cls, alpha_str = parts[0], parts[1], parts[2]
                tint_idx = int(parts[3]) if len(parts) > 3 else -1
                alpha = int(alpha_str)
                found_hwnd = None
                def enum_cb(hwnd, _):
                    nonlocal found_hwnd
                    if user32.IsWindowVisible(hwnd):
                        t_len = user32.GetWindowTextLengthW(hwnd)
                        if t_len > 0:
                            t_buf = ctypes.create_unicode_buffer(t_len + 1)
                            user32.GetWindowTextW(hwnd, t_buf, t_len + 1)
                            if t_buf.value == title:
                                c_buf = ctypes.create_unicode_buffer(64)
                                user32.GetClassNameW(hwnd, c_buf, 64)
                                if c_buf.value == cls:
                                    found_hwnd = hwnd; return False
                    return True
                WNDENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
                user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
                if found_hwnd:
                    style = user32.GetWindowLongW(found_hwnd, GWL_EXSTYLE)
                    if not (style & WS_EX_LAYERED):
                        user32.SetWindowLongW(found_hwnd, GWL_EXSTYLE, style | WS_EX_LAYERED)
                    user32.SetLayeredWindowAttributes(found_hwnd, 0, alpha, LWA_ALPHA)
                    self._modified.add(found_hwnd)
                    if tint_idx >= 0 and tint_idx < len(TINT_CYCLE):
                        proc = _start_overlay(found_hwnd, TINT_CYCLE[tint_idx])
                        if proc:
                            self._overlays[found_hwnd] = (proc, tint_idx)
                    restored += 1
            except Exception:
                pass
        if restored:
            print(f"Restored {restored} window(s) from session")
        self._save_session({})

    def _show_welcome(self):
        """Show startup welcome dialog — live demo with acrylic blur + transparency."""
        def _dialog():
            import tkinter as tk
            root = tk.Tk()
            root.title("win2blur")
            root.tk.call('tk', 'scaling', 1.5)
            W, H = 500, 560
            sw = root.winfo_screenwidth()
            sh = root.winfo_screenheight()
            x = (sw - W) // 2
            y = (sh - H) // 2
            root.geometry(f"{W}x{H}+{x}+{y}")
            root.resizable(False, False)
            bg_def = "#1e1e1e"; fg_def = "#e0e0e0"; fg2 = "#a0a0a0"; acc = "#4fc3f7"; bg2 = "#2d2d2d"
            root.configure(bg=bg_def)

            tk.Label(root, text="win2blur", font=("Segoe UI", 16, "bold"),
                     fg=acc, bg=bg_def).pack(pady=(18, 0))
            tk.Label(root, text="Transparency + Acrylic Frosted Glass",
                     font=("Segoe UI", 10), fg=fg2, bg=bg_def).pack(pady=(0, 14))

            def section(title):
                tk.Label(root, text=title, font=("Segoe UI", 11, "bold"),
                         fg=fg_def, bg=bg_def, anchor=tk.W).pack(fill=tk.X, padx=28, pady=(8, 4))

            def row(key, desc):
                r = tk.Frame(root, bg=bg_def)
                r.pack(fill=tk.X, padx=28, pady=2)
                tk.Label(r, text=key, font=("Consolas", 10, "bold"), fg=acc, bg=bg_def2,
                         width=13, anchor=tk.CENTER).pack(side=tk.LEFT, padx=(0, 10))
                tk.Label(r, text=desc, font=("Segoe UI", 10), fg=fg2, bg=bg_def).pack(side=tk.LEFT)

            section("Transparency")
            row("ALT+←", "More transparent")
            row("ALT+→", "Less transparent")
            row("ALT+↑", "Toggle on/off")

            section("Acrylic Blur")
            row("ALT+↓", "Cycle: 2% black → OFF")

            section("Tray Menu")
            for d in ["Settings — change step size",
                      "Restore && Exit — restore windows",
                      "Keep && Exit — resume on restart"]:
                tk.Label(root, text=f"  •  {d}", font=("Segoe UI", 10), fg=fg2, bg=bg_def,
                         anchor=tk.W).pack(fill=tk.X, padx=34)

            tk.Label(root, text="\nRunning in system tray", font=("Segoe UI", 10),
                     fg=fg2, bg=bg_def).pack()

            btn = tk.Button(root, text="Got it", font=("Segoe UI", 11),
                            bg=acc, fg="#1e1e1e", activebackground="#29b6f6",
                            relief=tk.FLAT, width=8, pady=6,
                            command=root.destroy)
            btn.pack(pady=(14, 18))

            root.mainloop()
        threading.Thread(target=_dialog, daemon=True).start()

    def _run_welcome(self):
        """Launch C++ welcome demo (effect cycling) and wait for it to close."""
        if os.path.exists(_WELCOME_EXE):
            subprocess.run([_WELCOME_EXE],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW)
        else:
            user32.MessageBoxW(0,
                "win2blur\n\nALT+LEFT/RIGHT  Transparency\nALT+UP  Toggle\n"
                "ALT+DOWN  Acrylic blur\n\nTray right-click for menu",
                "win2blur", 0x40)

    def run(self):
        """Start the app (blocking)."""
        self.running = True

        WNDPROC = ctypes.WINFUNCTYPE(
            wintypes.LPARAM, wintypes.HWND, ctypes.c_uint, wintypes.WPARAM, wintypes.LPARAM
        )

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

        self._wnd_proc_cb = WNDPROC(self._wnd_proc)
        hinst = kernel32.GetModuleHandleW(None)

        wc = WNDCLASSW()
        wc.style = 0
        wc.lpfnWndProc = self._wnd_proc_cb
        wc.hInstance = hinst
        wc.lpszClassName = "Window2ClearClass"

        if not user32.RegisterClassW(ctypes.byref(wc)):
            print(f"[x] RegisterClass failed: {ctypes.get_last_error()}")
            return

        self.hwnd = user32.CreateWindowExW(
            0, "Window2ClearClass", "Window2Clear",
            0, 0, 0, 0, 0, 0, 0, hinst, None
        )

        if not self.hwnd:
            print(f"[x] CreateWindowEx failed: {ctypes.get_last_error()}")
            return

        print(f"Window2Clear - config: {CONFIG_PATH}")
        print(f"  ALT+LEFT   more transparent")
        print(f"  ALT+RIGHT  less transparent")
        print(f"  ALT+UP     toggle on/off")
        print(f"  ALT+DOWN   acrylic blur cycle")
        print(f"  Tray right-click -> Settings / Restore && Exit / Keep && Exit")
        print(f"  Step: {self.step}%\n")

        # Run welcome demo in-process (pure Win32, no tkinter needed)
        self._run_welcome()
        self._restore_session()

        msg = wintypes.MSG()
        while self.running:
            result = user32.GetMessageW(ctypes.byref(msg), 0, 0, 0)
            if result <= 0:
                break
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))

        print("Window2Clear stopped")


if __name__ == '__main__':
    app = Window2Clear()
    app.run()
