"""
DWMBlurGlass Pipeline Diagnostic
=================================
Checks every link in the blur injection chain and reports what's broken.
"""
import ctypes
from ctypes import wintypes
import os
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi
dwmapi = ctypes.windll.dwmapi

# Constants
HWND_MESSAGE = -3
HOST_CLASS = "MDWMBlurGlassHostNotify"
EXT_CLASS = "MDWMBlurGlassExtNotify"
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPMODULE = 0x00000008

GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
RESET = "\033[0m"

def ok(msg): print(f"  {GREEN}[OK]{RESET} {msg}")
def fail(msg): print(f"  {RED}[FAIL]{RESET} {msg}")
def warn(msg): print(f"  {YELLOW}[WARN]{RESET} {msg}")
def info(msg): print(f"  [INFO] {msg}")

# ============================================================
# 1. DWMBlurGlass installation check
# ============================================================
print("=" * 60)
print("1. DWMBlurGlass Installation")
print("=" * 60)

BASE = os.path.join(os.path.dirname(__file__), "..", "DWMBlurGlass.2.3.2_Beta3_x64", "Release")
gui_exe = os.path.join(BASE, "DWMBlurGlassGUI.exe")
host_dll = os.path.join(BASE, "DWMBlurGlassHost.dll")
ext_dll = os.path.join(BASE, "DWMBlurGlassExt.dll")
config_ini = os.path.join(BASE, "data", "config.ini")

for name, path in [("GUI", gui_exe), ("Host DLL", host_dll),
                    ("Extension DLL", ext_dll), ("Config", config_ini)]:
    exists = os.path.exists(path)
    (ok if exists else fail)(f"{name}: {path}")

# ============================================================
# 2. DWM process check
# ============================================================
print("\n" + "=" * 60)
print("2. DWM Process (dwm.exe)")
print("=" * 60)

# Find dwm.exe PID
dwm_pid = None
dwm_hwnd = user32.FindWindowW("Dwm", None)
if dwm_hwnd:
    pid = wintypes.DWORD()
    user32.GetWindowThreadProcessId(dwm_hwnd, ctypes.byref(pid))
    dwm_pid = pid.value
    ok(f"dwm.exe PID: {dwm_pid}")
else:
    fail("Cannot find DWM window")
    sys.exit(1)

# ============================================================
# 3. Check if DWMBlurGlassExt.dll is loaded in dwm.exe
# ============================================================
print("\n" + "=" * 60)
print("3. DLL Injection Status (DWMBlurGlassExt.dll in dwm.exe)")
print("=" * 60)

# Open dwm.exe and enumerate modules via Toolhelp32 (more reliable on x64)
try:
    hSnapshot = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, dwm_pid)
    if hSnapshot == -1 or hSnapshot == 0:
        fail(f"CreateToolhelp32Snapshot failed (error {kernel32.GetLastError()}). Run as admin.")
    else:
        class MODULEENTRY32W(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD),
                ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD),
                ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wintypes.DWORD),
                ("hModule", wintypes.HMODULE),
                ("szModule", ctypes.c_wchar * 256),
                ("szExePath", ctypes.c_wchar * 260),
            ]
        me = MODULEENTRY32W()
        me.dwSize = ctypes.sizeof(MODULEENTRY32W)
        found = False
        if kernel32.Module32FirstW(hSnapshot, ctypes.byref(me)):
            while True:
                name = me.szModule
                if "DWMBlurGlass" in name.lower():
                    ok(f"DLL loaded: {name}  base={me.modBaseAddr}")
                    found = True
                if not kernel32.Module32NextW(hSnapshot, ctypes.byref(me)):
                    break
        if not found:
            fail("DWMBlurGlassExt.dll is NOT loaded in dwm.exe!")
            info("This is the ROOT CAUSE — blur won't work without the DLL.")
            info("Fix: Launch DWMBlurGlassGUI.exe AS ADMINISTRATOR to inject the DLL.")
        kernel32.CloseHandle(hSnapshot)
except Exception as e:
    fail(f"Error checking DLL: {e}")

# ============================================================
# 4. IPC notify windows
# ============================================================
print("\n" + "=" * 60)
print("4. IPC Notify Windows")
print("=" * 60)

host_hwnd = user32.FindWindowExW(HWND_MESSAGE, 0, HOST_CLASS, None)
ext_hwnd = user32.FindWindowExW(HWND_MESSAGE, 0, EXT_CLASS, None)

if host_hwnd:
    ok(f"Host notify window found: {host_hwnd:#x} (GUI/Host is running)")
else:
    warn("Host notify window NOT found (DWMBlurGlass GUI is closed)")
    info("DLL may still be active even without the GUI window.")

if ext_hwnd:
    ok(f"DLL notify window found: {ext_hwnd:#x} (DLL notification thread active)")
else:
    warn("DLL notify window NOT found (notification thread may be stopped)")
    info("If DLL is loaded but notify window is gone, hooks may still be active.")

# ============================================================
# 5. DwmEnableBlurBehindWindow test
# ============================================================
print("\n" + "=" * 60)
print("5. DwmEnableBlurBehindWindow API Test")
print("=" * 60)

class DWM_BLURBEHIND(ctypes.Structure):
    _fields_ = [
        ("dwFlags", wintypes.DWORD),
        ("fEnable", wintypes.BOOL),
        ("hRgnBlur", wintypes.HRGN),
        ("fTransitionOnMaximized", wintypes.BOOL),
    ]

# Test on foreground window
hwnd = user32.GetForegroundWindow()
if hwnd:
    length = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(max(length + 1, 64))
    user32.GetWindowTextW(hwnd, buf, length + 1)
    info(f"Test target: {buf.value} ({hwnd:#x})")

    bb = DWM_BLURBEHIND()
    bb.dwFlags = 0x00000001  # DWM_BB_ENABLE
    bb.fEnable = True
    bb.fTransitionOnMaximized = False
    result = dwmapi.DwmEnableBlurBehindWindow(hwnd, ctypes.byref(bb))
    err = kernel32.GetLastError()

    if result == 0:  # S_OK
        ok("DwmEnableBlurBehindWindow returned S_OK")
        info("API call succeeded. If you DON'T see blur:")
        info("  1. DWMBlurGlassExt.dll is not injected in dwm.exe (see section 3)")
        info("  2. DWMBlurGlass blur amount is set to 0 or very low")
        info("  3. blurMethod is set to 'DWMAPI blur' instead of 'Custom blur'")
        info("  4. customAmount is false in config.ini")
    else:
        fail(f"DwmEnableBlurBehindWindow returned 0x{result:08X} (error: {err})")
else:
    fail("No foreground window to test")

# ============================================================
# 6. Config check
# ============================================================
print("\n" + "=" * 60)
print("6. DWMBlurGlass Config (data/config.ini)")
print("=" * 60)

if os.path.exists(config_ini):
    import configparser
    cfg = configparser.ConfigParser()
    cfg.read(config_ini, encoding='utf-8')
    try:
        blur_method = cfg.get('config', 'blurMethod', fallback='?')
        effect_type = cfg.get('config', 'effectType', fallback='?')
        custom_amt = cfg.get('config', 'customAmount', fallback='?')
        blur_amt = cfg.get('config', 'blurAmount', fallback='?')
        apply_global = cfg.get('config', 'applyglobal', fallback='?')

        info(f"blurMethod = {blur_method} (0=CustomBlur, 1=DWMAPI)")
        info(f"effectType = {effect_type} (0=Blur, 1=Acrylic, 2=Mica)")
        info(f"customAmount = {custom_amt}")
        info(f"blurAmount = {blur_amt}")
        info(f"applyglobal = {apply_global}")

        if custom_amt != 'true':
            fail("customAmount is FALSE — blur amount is system default (very subtle)!")
        if blur_method == '1':
            fail("blurMethod is DWMAPI (1) — uses Win10 native weak blur, NOT custom blur!")
            info("Set blurMethod=0 for DWMBlurGlass custom blur.")
        if apply_global != 'true':
            warn("applyglobal is FALSE — blur only applies to windows with DwmEnableBlurBehindWindow called")
    except Exception as e:
        fail(f"Error reading config: {e}")
else:
    fail(f"Config not found: {config_ini}")

# ============================================================
# 7. Scheduled Task check
# ============================================================
print("\n" + "=" * 60)
print("7. DWMBlurGlass Scheduled Task")
print("=" * 60)

try:
    result = subprocess.run(
        ['schtasks', '/query', '/fo', 'csv', '/v'],
        capture_output=True, text=True, timeout=10
    )
    for line in result.stdout.splitlines():
        if 'BlurGlass' in line or 'blurglass' in line.lower():
            info(f"  {line}")
            break
    else:
        warn("No DWMBlurGlass scheduled task found")
        info("Without scheduled task, DLL won't auto-inject after reboot")
        info("Launch DWMBlurGlassGUI.exe AS ADMINISTRATOR once to create the task")
except Exception as e:
    warn(f"Could not query scheduled tasks: {e}")
    info("Run as admin to query scheduled tasks")

# ============================================================
# Summary
# ============================================================
print("\n" + "=" * 60)
print("DIAGNOSIS")
print("=" * 60)
print("""
The blur pipeline is:
  1. DWMBlurGlass GUI (admin) → injects DWMBlurGlassExt.dll into dwm.exe
  2. DLL hooks DwmEnableBlurBehindWindow in dwm.exe
  3. tray_app ALT+DOWN → calls DwmEnableBlurBehindWindow(hwnd)
  4. DWM calls the hooked function → DWMBlurGlass blur shader runs
  5. Window background is replaced with blurred desktop → visible!

If blur doesn't show, the break is at step 1 or 2.
Fix:
  a) Launch DWMBlurGlassGUI.exe AS ADMINISTRATOR
  b) In GUI: enable Blur method = "Custom blur", Effect = "Blur" or "Acrylic"
  c) Verify DLL shows up in section 3 of this diagnostic
  d) Then ALT+DOWN in tray_app should work
""")
