"""Baseline control: screenshot target WITHOUT overlay vs WITH overlay (multiple
param sets). Isolates whether the overlay renders at all and whether params move."""
import ctypes, subprocess, sys, time, os
import tkinter as tk
from PIL import ImageGrab

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
u32.GetParent.restype = ctypes.c_void_p
u32.CreateWindowExW.restype = ctypes.c_void_p
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

def center_avg(hwnd, region=(0.35, 0.35, 0.65, 0.65)):
    x, y, w, h = win_rect(hwnd)
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h)).convert("RGB")
    W, H = img.size
    x0, y0 = int(W * region[0]), int(H * region[1])
    x1, y1 = int(W * region[2]), int(H * region[3])
    px = list(img.getdata())
    sel = [px[y * W + x] for y in range(y0, y1) for x in range(x0, x1)]
    return tuple(sum(c[i] for c in sel) // len(sel) for i in range(3))

# 1) baseline: no overlay
t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
pump(t, 1.0)
print("BASELINE no-overlay :", center_avg(th))

# 2) overlay wash=120 tint=0 radius=10
p = subprocess.Popen([BUILD, "0x%08X" % th, "120", "0", "10"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
pump(t, 3.0)
print("OVERLAY 120,0,10   :", center_avg(th))

# 3) change nothing — verify stability (same params, re-screenshot)
pump(t, 2.0)
print("OVERLAY steady      :", center_avg(th))

# 4) overlay wash=255 (full)
p.terminate(); time.sleep(0.5)
p = subprocess.Popen([BUILD, "0x%08X" % th, "255", "0", "10"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
pump(t, 3.0)
print("OVERLAY 255,0,10   :", center_avg(th))

# 5) overlay wash=0 (none — should equal baseline)
p.terminate(); time.sleep(0.5)
p = subprocess.Popen([BUILD, "0x%08X" % th, "0", "0", "10"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
pump(t, 3.0)
print("OVERLAY 0,0,10     :", center_avg(th))

p.terminate(); time.sleep(0.5); t.destroy()
print("done")
