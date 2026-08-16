"""Flash-on-focus-switch investigation: overlay running on window A (fg).
Switch focus to B, then back to A. Sample A's visibility at 2ms resolution
to detect dual-hide capture events, plus A's screen avg at 100ms to see
brightness churn (ease transitions). Reproduces "other window flashes 2x". """
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
        time.sleep(0.002)

def avg_of(hwnd):
    x, y, w, h = win_rect(hwnd)
    img = ImageGrab.grab(bbox=(x, y, x + w, y + h)).convert("RGB")
    px = list(img.getdata())
    return tuple(sum(c[i] for c in px)//len(px) for i in range(3))

A = tk.Tk(); A.configure(bg="#4080C0"); A.geometry("400x300+150+150"); A.update()
Ah = u32.GetParent(ctypes.c_void_p(A.winfo_id())) or A.winfo_id()
B = tk.Tk(); B.configure(bg="#303030"); B.geometry("400x300+900+150"); B.update()
Bh = u32.GetParent(ctypes.c_void_p(B.winfo_id())) or B.winfo_id()


pump(A, 0.4)
print("A fg?", u32.GetForegroundWindow() == Ah)
p = subprocess.Popen([BUILD, "0x%08X" % Ah, "120", "10", "10"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
pump(A, 2.0)
print("steady A avg:", avg_of(Ah))

# ---- switch to B ----

hides = 0; last = True
t_sw = time.time()
while time.time() - t_sw < 1.5:
    vis = u32.IsWindowVisible(Ah)
    if not vis and last: hides += 1; print(f"  +{time.time()-t_sw:.3f}s A HIDDEN (capture)")
    last = vis
    time.sleep(0.002)
print(f"switch to B: {hides} hide events, A avg now: {avg_of(Ah)}")

# ---- switch back to A ----

hides = 0; last = True
t_sw = time.time()
while time.time() - t_sw < 1.5:
    vis = u32.IsWindowVisible(Ah)
    if not vis and last: hides += 1; print(f"  +{time.time()-t_sw:.3f}s A HIDDEN (capture)")
    last = vis
    time.sleep(0.002)
print(f"switch back: {hides} hide events, A avg now: {avg_of(Ah)}")

p.terminate(); time.sleep(0.5)
A.destroy(); B.destroy()
print("done")
