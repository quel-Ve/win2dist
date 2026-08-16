"""Verify focus-factor hypothesis: same tint value, window FOREGROUND vs BACKGROUND.
If g_tintF halves tint on unfocused windows, the same tint will look much weaker
when another window has focus (user's "tint 10% no effect" on background windows)."""
import ctypes, subprocess, time, os
import tkinter as tk
from PIL import ImageGrab

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
u32.GetParent.restype = ctypes.c_void_p
BUILD = r"D:\Garage\Software\ccproject\12window2clear\native\build5\crisp_overlay.exe"

class RECT(ctypes.Structure):
    _fields_ = [("left", ctypes.c_long), ("top", ctypes.c_long),
                ("right", ctypes.c_long), ("bottom", ctypes.c_long)]

def win_rect(hwnd):
    r = RECT()
    u32.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(r))
    return r.left, r.top, r.right - r.left, r.bottom - r.top

def pump(tkwin, secs):
    end = time.time() + secs
    while time.time() < end:
        tkwin.update()
        time.sleep(0.02)

def center_avg(hwnd):
    x, y, w, h = win_rect(hwnd)
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h)).convert("RGB")
    W, H = img.size
    px = list(img.getdata())
    cx0, cy0, cx1, cy1 = int(W*0.35), int(H*0.35), int(W*0.65), int(H*0.65)
    sel = [px[y*W+x] for y in range(cy0, cy1) for x in range(cx0, cx1)]
    return tuple(sum(c[i] for c in sel)//len(sel) for i in range(3))

t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()

pump(t, 0.8)
print("target foreground?", u32.GetForegroundWindow() == th)

def run(args, tag, fg, wait=3.5):
    if fg:
        pass
    else:
        other = tk.Tk(); other.geometry("200x150+900+150"); other.update()
        
    pump(t, 0.5)
    p = subprocess.Popen([BUILD, "0x%08X" % th] + [str(a) for a in args],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pump(t, wait)
    avg = center_avg(th)
    print(f"{tag:20s} fg={fg} center={avg}")
    p.terminate(); time.sleep(0.8)
    if not fg: other.destroy()

run([120, 10, 10], "w120_t10", fg=True)
run([120, 10, 10], "w120_t10", fg=False)
run([120, 40, 10], "w120_t40", fg=True)
run([120, 40, 10], "w120_t40", fg=False)
t.destroy()
print("done")
