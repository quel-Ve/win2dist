"""High-precision: with target NOT foreground (another window focused), does the
overlay keep re-capturing (dual-hide cycles = visible flashing) or is it stable?
Sample target visibility + fg at 5ms for 4s. Also count capture cycles."""
import ctypes, subprocess, time
import tkinter as tk

u32 = ctypes.windll.user32
u32.GetParent.restype = ctypes.c_void_p
BUILD = r"D:\Garage\Software\ccproject\12window2clear\native\build5\crisp_overlay.exe"

t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()
other = tk.Tk(); other.geometry("200x150+900+150"); other.update()

time.sleep(0.3)
print("fg:", hex(u32.GetForegroundWindow()), "target fg?", u32.GetForegroundWindow() == th)

p = subprocess.Popen([BUILD, "0x%08X" % th, "120", "10", "10"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

hides = 0
last_vis = True
start = time.time()
t0 = start
while time.time() - start < 4.0:
    vis = u32.IsWindowVisible(th)
    if not vis and last_vis:
        hides += 1
        print(f"t={time.time()-t0:5.2f}s TARGET HIDDEN (capture #? hide #{hides})")
    last_vis = vis
    time.sleep(0.005)
print("total hide events:", hides)

p.terminate(); time.sleep(0.5); t.destroy(); other.destroy()
print("done")
