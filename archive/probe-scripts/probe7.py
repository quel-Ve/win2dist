"""Circle feature: overlay launched with circle_px=60, mouse INSIDE vs OUTSIDE the
target. Quantified center-region diff. Also: circle=0 (no circle arg) as control.
And tint intensity: full-window diff tint 0 vs 10 vs 30 on a bright background."""
import ctypes, subprocess, time, os
import tkinter as tk
from PIL import ImageGrab

u32 = ctypes.windll.user32
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

def region_avg(hwnd, r0, r1):
    x, y, w, h = win_rect(hwnd)
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h)).convert("RGB")
    W, H = img.size
    px = list(img.getdata())
    x0, y0 = int(W*r0[0]), int(H*r0[1]); x1, y1 = int(W*r1[0]), int(H*r1[1])
    sel = [px[y*W+x] for y in range(y0, y1) for x in range(x0, x1)]
    return tuple(sum(c[i] for c in sel)//len(sel) for i in range(3))

def scene(args, mouse_inside, tag):
    t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
    th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
    pump(t, 0.3)
    x, y, w, h = win_rect(th)
    if mouse_inside: u32.SetCursorPos(x + w//2, y + h//2)
    else: u32.SetCursorPos(50, 50)
    p = subprocess.Popen([BUILD, "0x%08X" % th] + [str(a) for a in args],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pump(t, 3.0)
    c = region_avg(th, (0.35, 0.35), (0.65, 0.65))
    e = region_avg(th, (0.05, 0.05), (0.25, 0.25))
    print(f"{tag:34s} center={c} edge={e}")
    p.terminate(); time.sleep(0.6); t.destroy()

# circle control: no circle arg (mouse inside — should be no difference)
scene([120, 0, 10],            True,  "circle_off mouse-inside")
scene([120, 0, 10, 0],         True,  "circle_0   mouse-inside")
# circle on: boost should make center MORE blurred (dimmer on flat color) when mouse inside
scene([120, 0, 10, 60, 200, 30, 60], False, "circle_60  mouse-outside")
scene([120, 0, 10, 60, 200, 30, 60], True,  "circle_60  mouse-inside")
print("done")
