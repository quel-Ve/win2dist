"""Reproduce user's real config: wash=39 (alpha 216), tint 12, on a TEXTURED
background (tkinter window with visible content behind target). Tests:
A) tint visibility at user's real wash=39: tint 0 vs 12 vs 30
B) focus switch flash: does switching foreground cause re-render churn
   (sample target visibility at 2ms to catch dual-hide re-captures)"""
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
        time.sleep(0.005)

def region_avg(hwnd, r0, r1):
    x, y, w, h = win_rect(hwnd)
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h)).convert("RGB")
    W, H = img.size
    px = list(img.getdata())
    x0, y0 = int(W*r0[0]), int(H*r0[1]); x1, y1 = int(W*r1[0]), int(H*r1[1])
    sel = [px[y*W+x] for y in range(y0, y1) for x in range(x0, x1)]
    return tuple(sum(c[i] for c in sel)//len(sel) for i in range(3))

def textured_target(bg="#4080C0"):
    """Target window with visible texture (text + shapes) so blur is observable."""
    t = tk.Tk(); t.configure(bg=bg); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
    for i in range(8):
        lbl = tk.Label(t, text="TEXT BLOCK %d" % i, bg=bg, fg="#FFFFFF", font=("Arial", 14))
        lbl.place(x=30 + (i%4)*90, y=40 + (i//4)*90)
    for i in range(6):
        f = tk.Frame(t, bg="#FFA000" if i%2 else "#20D000", width=60, height=20)
        f.place(x=40 + i*55, y=230)
    t.update(); t.attributes("-topmost", False); t.lower()
    return t

def scene(args, tag, bg_tex=True, fg=True, wait=2.5):
    t = textured_target() if bg_tex else tk.Tk()
    if not bg_tex: t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
    th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
    if fg:
        pass
    else:
        other = tk.Tk(); other.geometry("300x200+900+100"); other.update()
        
    pump(t, 0.3)
    p = subprocess.Popen([BUILD, "0x%08X" % th] + [str(a) for a in args],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pump(t, wait)
    c = region_avg(th, (0.35, 0.35), (0.65, 0.65))
    e = region_avg(th, (0.05, 0.05), (0.25, 0.25))
    print(f"{tag:34s} center={c} edge={e}")
    p.terminate(); time.sleep(0.6)
    if not fg: other.destroy()
    t.destroy()

# A) user's real wash: 39 (=255-216), tint 0 vs 12 vs 30
scene([39, 0, 10],  "wash39 tint0  fg")
scene([39, 12, 10], "wash39 tint12 fg")
scene([39, 30, 10], "wash39 tint30 fg")
scene([39, 60, 10], "wash39 tint60 fg")
print("done")
