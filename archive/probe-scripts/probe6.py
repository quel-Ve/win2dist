"""Capture crisp_overlay's own stdout (it echoes parsed params) + screenshot pixel
distribution, for target FOREGROUND vs BACKGROUND, same args. Determines whether
the overlay received/uses the right params and what the screen actually shows."""
import ctypes, subprocess, time, os
import tkinter as tk
from PIL import ImageGrab

u32 = ctypes.windll.user32
u32.GetParent.restype = ctypes.c_void_p
BUILD = r"D:\Garage\Software\ccproject\12window2clear\native\build5\crisp_overlay.exe"
OUT = r"D:\Garage\Software\ccproject\12window2clear\probe_shots"
os.makedirs(OUT, exist_ok=True)

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

def analyze(hwnd, tag):
    x, y, w, h = win_rect(hwnd)
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h)).convert("RGB")
    img.save(os.path.join(OUT, tag + ".png"))
    W, H = img.size
    px = list(img.getdata())
    # full-window stats
    avg = tuple(sum(c[i] for c in px)//len(px) for i in range(3))
    dark = sum(1 for c in px if sum(c) < 120)
    blue = sum(1 for c in px if c[2] > c[0] + 30 and c[2] > c[1] + 30)
    print(f"{tag:22s} avg={avg} dark%={dark*100//len(px)} blue%={blue*100//len(px)}")

def run_scene(fg, tag):
    t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
    th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
    if fg:
        
        pump(t, 0.3)
    else:
        other = tk.Tk(); other.geometry("200x150+900+150"); other.update()
        
        pump(t, 0.3)
    p = subprocess.Popen([BUILD, "0x%08X" % th, "120", "10", "10"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    pump(t, 3.0)
    analyze(th, tag)
    p.terminate(); time.sleep(0.4)
    out = p.stdout.read() if p.stdout else ""
    print(f"--- overlay stdout ({tag}) ---")
    print(out.strip()[:500] if out.strip() else "(empty)")
    p.terminate(); time.sleep(0.6)
    if not fg: other.destroy()
    t.destroy()

run_scene(True,  "fg_True_w120_t10")
run_scene(False, "fg_False_w120_t10")
print("done")
