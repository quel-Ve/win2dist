"""Precise reproduction: tint visibility at multiple levels + wash=0 anomaly.
Saves screenshots + prints center-region RGB for each param set."""
import ctypes, subprocess, time, os
import tkinter as tk
from PIL import ImageGrab, Image

u32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32
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

def shot(hwnd, tag):
    x, y, w, h = win_rect(hwnd)
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h)).convert("RGB")
    path = os.path.join(OUT, tag + ".png")
    img.save(path)
    W, H = img.size
    px = list(img.getdata())
    cx0, cy0, cx1, cy1 = int(W*0.35), int(H*0.35), int(W*0.65), int(H*0.65)
    sel = [px[y*W+x] for y in range(cy0, cy1) for x in range(cx0, cx1)]
    avg = tuple(sum(c[i] for c in sel)//len(sel) for i in range(3))
    # corner region (target window edge — shows whether overlay covers the whole area)
    corn = [px[y*W+x] for y in range(0, H//6) for x in range(0, W//6)]
    cavg = tuple(sum(c[i] for c in corn)//len(corn) for i in range(3))
    print(f"{tag:24s} center={avg}  corner={cavg}")
    return path

# ONE persistent target window; sequential param sets on the SAME window
t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
pump(t, 1.0)
shot(th, "b0_baseline")

def run(args, tag, wait=3.0):
    p = subprocess.Popen([BUILD, "0x%08X" % th] + [str(a) for a in args],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pump(t, wait)
    shot(th, tag)
    p.terminate()
    time.sleep(0.8)

run([120, 0, 10],    "w120_t0")
run([120, 10, 10],   "w120_t10")
run([120, 30, 10],   "w120_t30")
run([120, 60, 10],   "w120_t60")
run([255, 0, 10],    "w255_t0")
run([0, 0, 10],      "w0_t0_ANOMALY")
t.destroy()
print("done")
