"""
Debug: test each injection API step-by-step.
Run as Administrator.
"""
import ctypes, sys, os, subprocess
from ctypes import wintypes
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

DLL = os.path.abspath("build/libfrosted_dwm.dll")
print(f"DLL: {DLL}")
print(f"Exists: {os.path.exists(DLL)}")

# Find PID
try:
    out = subprocess.check_output(['tasklist','/FI','IMAGENAME eq dwm.exe','/FO','CSV','/NH'], text=True)
    pid = int(out.strip().split('\n')[0].split(',')[1].strip('"'))
    print(f"dwm.exe PID: {pid}")
except: print("FAIL: find pid"); sys.exit(1)

# Use explicit WinDLL
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

# Step 1: OpenProcess
k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
k32.OpenProcess.restype = wintypes.HANDLE
access = 0x1F0FFF  # PROCESS_ALL_ACCESS
h = k32.OpenProcess(access, False, pid)
err1 = ctypes.get_last_error()
print(f"1. OpenProcess     -> h=0x{h:08X} err={err1}  {'OK' if h else 'FAIL'}")

if not h: sys.exit(1)

# Step 2: VirtualAllocEx
size = ctypes.sizeof(ctypes.c_wchar) * (len(DLL) + 1)
p = k32.VirtualAllocEx(h, None, size, 0x3000, 0x04)  # MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE
err2 = ctypes.get_last_error()
print(f"2. VirtualAllocEx  -> p=0x{p:08X} err={err2} size={size} {'OK' if p else 'FAIL'}")

if not p:
    k32.CloseHandle(h)
    sys.exit(1)

# Step 3: WriteProcessMemory
k32.WriteProcessMemory.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.restype = wintypes.BOOL
dll_wide = (ctypes.c_wchar * (len(DLL) + 1))()
dll_wide.value = DLL
written = ctypes.c_size_t()
ret = k32.WriteProcessMemory(h, p, dll_wide, ctypes.sizeof(dll_wide), ctypes.byref(written))
err3 = ctypes.get_last_error()
print(f"3. WriteProcessMem -> ret={ret} written={written.value} err={err3} {'OK' if ret else 'FAIL'}")
print(f"   byte representation of arg: {dll_wide[:20]}")
print(f"   sizeof(dll_wide)={ctypes.sizeof(dll_wide)}")

if not ret:
    k32.VirtualFreeEx(h, p, 0, 0x8000)
    k32.CloseHandle(h)
    sys.exit(1)

# Step 4: LoadLibraryW address
k32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
k32.GetModuleHandleW.restype = wintypes.HMODULE
kBase = k32.GetModuleHandleW("kernel32.dll")
k32.GetProcAddress.argtypes = [wintypes.HMODULE, wintypes.LPCSTR]
k32.GetProcAddress.restype = ctypes.c_void_p
ll = k32.GetProcAddress(kBase, b"LoadLibraryW")
print(f"4. LoadLibraryW    -> addr=0x{ll:08X} {'OK' if ll else 'FAIL'}")

if not ll:
    # Try wide string
    ll = k32.GetProcAddress(kBase, "LoadLibraryW")
    print(f"4b. LoadLibraryW(wide) -> addr=0x{ll:08X}")

if not ll:
    k32.VirtualFreeEx(h, p, 0, 0x8000)
    k32.CloseHandle(h)
    sys.exit(1)

# Step 5: CreateRemoteThread
k32.CreateRemoteThread.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t, wintypes.LPVOID, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID]
k32.CreateRemoteThread.restype = wintypes.HANDLE
t = k32.CreateRemoteThread(h, None, 0, ll, p, 0, None)
err5 = ctypes.get_last_error()
print(f"5. CreateRemoteThr -> t=0x{t:08X} err={err5} {'OK' if t else 'FAIL'}")

if not t:
    k32.VirtualFreeEx(h, p, 0, 0x8000)
    k32.CloseHandle(h)
    sys.exit(1)

# Step 6: Wait for DLL load
k32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
k32.WaitForSingleObject.restype = wintypes.DWORD
result = k32.WaitForSingleObject(t, 5000)
print(f"6. WaitForSingleObj -> result={result} {'(OK)' if result==0 else '(timeout/error)'}")

# Cleanup
k32.CloseHandle(t)
k32.VirtualFreeEx(h, p, 0, 0x8000)
k32.CloseHandle(h)

# Check if DLL loaded
try:
    out = subprocess.check_output(['tasklist','/M','libfrosted_dwm.dll','/FO','CSV'], text=True, timeout=3)
    if 'libfrosted_dwm.dll' in out:
        print("\n[SUCCESS] DLL is loaded in dwm.exe!")
    else:
        print("\n[DLL not detected in dwm.exe modules]")
except: print("\n[Cannot verify DLL load]")
