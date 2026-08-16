"""CRITICAL: does the dual-hide capture steal focus from the target?
Monitor GetForegroundWindow() around the overlay's first capture cycle.
If capture (SW_HIDE target) transfers foreground away and it never comes back,
the focus tracker permanently downgrades the window (tint x0.5, wash x0.6)
-> user's "tint 10% no effect on background windows" + "flash on focus switch"."""
import ctypes, subprocess, time
import tkinter as tk

u32 = ctypes.windll.user32
u32.GetParent.restype = ctypes.c_void_p
BUILD = r"D:\Garage\Software\ccproject\12window2clear\native\build5\crisp_overlay.exe"

t = tk.Tk(); t.configure(bg="#4080C0"); t.geometry("400x300+150+150"); t.update(); t.attributes("-topmost", False); t.lower()
th = u32.GetParent(ctypes.c_void_p(t.winfo_id())) or t.winfo_id()

time.sleep(0.5)
print("fg before overlay:", hex(u32.GetForegroundWindow()), "is target?", u32.GetForegroundWindow() == th)

p = subprocess.Popen([BUILD, "0x%08X" % th, "120", "10", "10"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# sample foreground every 100ms for 3s (capture happens ~3 stable frames after start)
for i in range(30):
    fg = u32.GetForegroundWindow()
    vis = u32.IsWindowVisible(th)
    print(f"t={i*0.1:4.1f}s fg={hex(fg)} is_target={fg == th} target_visible={vis}")
    time.sleep(0.1)

p.terminate(); time.sleep(0.5); t.destroy()
print("done")
