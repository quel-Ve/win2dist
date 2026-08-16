"""Circle test WITHOUT idle-fade interference: screenshot at 0.6s after launch
(< 1.5s idle threshold) with mouse inside vs outside. Also nudge mouse to keep
it fresh. Plus tint comparison on same-window with short wait."""
import ctypes, subprocess, time
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

def scene(args, mouse_inside, tag, wait=0.6, nudge=False):
    t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
    th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
    pump(t, 0.3)
    x, y, w, h = win_rect(th)
    cx, cy = x + w//2, y + h//2
    if mouse_inside: u32.SetCursorPos(cx, cy)
    else: u32.SetCursorPos(50, 50)
    p = subprocess.Popen([BUILD, "0x%08X" % th] + [str(a) for a in args],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    end = time.time() + wait
    while time.time() < end:
        if nudge and mouse_inside:
            # keep the circle fresh: small circular mouse moves every 0.4s
            a = (time.time() * 5) % 6.283
            u32.SetCursorPos(cx + int(8*ctypes.math if False else 0), cy)  # placeholder
        pump(t, 0.05)
    c = region_avg(th, (0.35, 0.35), (0.65, 0.65))
    e = region_avg(th, (0.05, 0.05), (0.25, 0.25))
    print(f"{tag:34s} center={c} edge={e}")
    p.terminate(); time.sleep(0.6); t.destroy()

import math
def scene_nudge(args, tag):
    t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
    th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
    pump(t, 0.3)
    x, y, w, h = win_rect(th)
    cx, cy = x + w//2, y + h//2
    u32.SetCursorPos(cx, cy)
    p = subprocess.Popen([BUILD, "0x%08X" % th] + [str(a) for a in args],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    # keep mouse moving in a small circle so idle fade never trips
    end = time.time() + 3.0
    i = 0
    while time.time() < end:
        a = i * 0.35
        u32.SetCursorPos(cx + int(12*math.cos(a)), cy + int(12*math.sin(a)))
        i += 1
        pump(t, 0.08)
    c = region_avg(th, (0.35, 0.35), (0.65, 0.65))
    e = region_avg(th, (0.05, 0.05), (0.25, 0.25))
    print(f"{tag:34s} center={c} edge={e}")
    p.terminate(); time.sleep(0.6); t.destroy()

# quick circle check (0.6s, before idle fade)
scene([120, 0, 10, 60, 200, 30, 60], False, "circle60 mouse-outside q")
scene([120, 0, 10, 60, 200, 30, 60], True,  "circle60 mouse-inside  q")
# sustained-motion circle check (3s, idle fade never trips)
scene_nudge([120, 0, 10, 60, 200, 30, 60], "circle60 mouse-moving  3s")
print("done")
