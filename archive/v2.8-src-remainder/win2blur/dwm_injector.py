"""
DWM Injector — WriteProcessMemory + CreateRemoteThread
========================================================
Enables SeDebugPrivilege before injection (required for dwm.exe PPL).
Same approach as DWMBlurGlass's Helper.cpp:Inject().
"""
import ctypes, sys, os, subprocess
from ctypes import wintypes
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

_DLL = os.path.abspath(os.path.join(
    os.path.dirname(__file__), "..", "native", "dwm_inject", "build",
    "libfrosted_dwm.dll"
))

SE_PRIVILEGE_ENABLED = 0x2
TOKEN_ADJUST_PRIVILEGES = 0x20
TOKEN_QUERY = 0x8


def enable_debug_priv():
    """Enable SeDebugPrivilege — required to inject into protected processes."""
    advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    # Get current process token
    token = wintypes.HANDLE()
    k32.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    k32.OpenProcessToken.restype = wintypes.BOOL
    if not k32.OpenProcessToken(k32.GetCurrentProcess(),
                                 TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                 ctypes.byref(token)):
        print(f"  OpenProcessToken failed: {ctypes.get_last_error()}")
        return False

    # Lookup privilege LUID
    class LUID(ctypes.Structure):
        _fields_ = [("LowPart", wintypes.DWORD), ("HighPart", wintypes.LONG)]
    luid = LUID()
    advapi32.LookupPrivilegeValueW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.POINTER(LUID)]
    advapi32.LookupPrivilegeValueW.restype = wintypes.BOOL
    if not advapi32.LookupPrivilegeValueW(None, "SeDebugPrivilege", ctypes.byref(luid)):
        print(f"  LookupPrivilegeValue failed: {ctypes.get_last_error()}")
        k32.CloseHandle(token)
        return False

    # Enable privilege
    class TOKEN_PRIVILEGES(ctypes.Structure):
        _fields_ = [("PrivilegeCount", wintypes.DWORD),
                    ("Privileges", LUID * 1),
                    ("Attributes", wintypes.DWORD * 1)]
    tp = TOKEN_PRIVILEGES()
    tp.PrivilegeCount = 1
    tp.Privileges[0] = luid
    tp.Attributes[0] = SE_PRIVILEGE_ENABLED

    advapi32.AdjustTokenPrivileges.argtypes = [
        wintypes.HANDLE, wintypes.BOOL, ctypes.POINTER(TOKEN_PRIVILEGES),
        wintypes.DWORD, ctypes.POINTER(TOKEN_PRIVILEGES), ctypes.POINTER(wintypes.DWORD)
    ]
    advapi32.AdjustTokenPrivileges.restype = wintypes.BOOL
    if not advapi32.AdjustTokenPrivileges(token, False, ctypes.byref(tp),
                                           ctypes.sizeof(tp), None, None):
        err = ctypes.get_last_error()
        print(f"  AdjustTokenPrivileges failed: {err}")
        k32.CloseHandle(token)
        return False

    err = ctypes.get_last_error()
    k32.CloseHandle(token)
    if err == 0x514:  # ERROR_NOT_ALL_ASSIGNED
        print("  SeDebugPrivilege not assigned (check local security policy)")
        return False
    return True


def find_dwm_pid():
    try:
        out = subprocess.check_output(
            ['tasklist','/FI','IMAGENAME eq dwm.exe','/FO','CSV','/NH'], text=True, timeout=5)
        return int(out.strip().split('\n')[0].split(',')[1].strip('"'))
    except: return None


def is_admin():
    return bool(ctypes.windll.shell32.IsUserAnAdmin())


def inject():
    if not os.path.exists(_DLL):
        print(f"[x] DLL not found: {_DLL}"); return False

    pid = find_dwm_pid()
    if not pid: print("[x] dwm.exe not found"); return False
    print(f"[*] dwm.exe PID: {pid}")

    # Enable SeDebugPrivilege
    print("[*] Enabling SeDebugPrivilege...")
    if not enable_debug_priv():
        print("[!] SeDebugPrivilege failed — injection may still work")

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    # OpenProcess
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.OpenProcess.restype = wintypes.HANDLE
    access = 0x0002|0x0008|0x0020|0x0010|0x0400  # same as DWMBlurGlass
    hProc = k32.OpenProcess(access, False, pid)
    if not hProc:
        print(f"[x] OpenProcess: error {ctypes.get_last_error()}"); return False
    print(f"  OpenProcess: h=0x{hProc:08X}")

    # VirtualAllocEx
    size = ctypes.sizeof(ctypes.c_wchar) * (len(_DLL) + 1)
    pRemote = k32.VirtualAllocEx(hProc, None, size, 0x1000, 0x04)  # MEM_COMMIT, PAGE_READWRITE
    if not pRemote:
        print(f"[x] VirtualAllocEx: error {ctypes.get_last_error()}")
        k32.CloseHandle(hProc); return False
    print(f"  VirtualAllocEx: p=0x{pRemote:08X}")

    # Try WriteProcessMemory first, fall back to NtWriteVirtualMemory
    k32.WriteProcessMemory.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID,
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
    ]
    k32.WriteProcessMemory.restype = wintypes.BOOL
    dll_wide = (ctypes.c_wchar * (len(_DLL) + 1))()
    dll_wide.value = _DLL
    written = ctypes.c_size_t()
    ret = k32.WriteProcessMemory(hProc, pRemote, dll_wide, ctypes.sizeof(dll_wide), ctypes.byref(written))

    if not ret:
        err = ctypes.get_last_error()
        print(f"  WriteProcessMemory: FAIL (error {err})")
        print("[*] Trying NtWriteVirtualMemory (bypass Win32 PPL check)...")

        ntdll = ctypes.WinDLL("ntdll", use_last_error=True)
        # NTSTATUS NtWriteVirtualMemory(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T)
        ntdll.NtWriteVirtualMemory.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.LPCVOID,
            ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)
        ]
        ntdll.NtWriteVirtualMemory.restype = ctypes.c_long  # NTSTATUS
        written2 = ctypes.c_size_t()
        status = ntdll.NtWriteVirtualMemory(hProc, pRemote, dll_wide,
                                             ctypes.sizeof(dll_wide), ctypes.byref(written2))
        if status < 0:  # NTSTATUS error
            print(f"  NtWriteVirtualMemory: FAIL (NTSTATUS 0x{status & 0xFFFFFFFF:08X})")
            print("[x] PPL blocks all user-mode memory writes to dwm.exe")
            k32.VirtualFreeEx(hProc, pRemote, 0, 0x8000)
            k32.CloseHandle(hProc); return False
        print(f"  NtWriteVirtualMemory: {written2.value} bytes OK")
    else:
        print(f"  WriteProcessMemory: {written.value} bytes OK")

    # LoadLibraryW address
    k32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    k32.GetModuleHandleW.restype = wintypes.HMODULE
    ll_addr = k32.GetProcAddress(k32.GetModuleHandleW("kernel32.dll"), b"LoadLibraryW")
    if not ll_addr:
        print("[x] LoadLibraryW not found")
        k32.VirtualFreeEx(hProc, pRemote, 0, 0x8000)
        k32.CloseHandle(hProc); return False

    # CreateRemoteThread
    k32.CreateRemoteThread.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID, ctypes.c_size_t,
        wintypes.LPVOID, wintypes.LPVOID, wintypes.DWORD, wintypes.LPVOID
    ]
    k32.CreateRemoteThread.restype = wintypes.HANDLE
    hThread = k32.CreateRemoteThread(hProc, None, 0, ll_addr, pRemote, 0, None)
    if not hThread:
        err = ctypes.get_last_error()
        print(f"[x] CreateRemoteThread: error {err}")
        k32.VirtualFreeEx(hProc, pRemote, 0, 0x8000)
        k32.CloseHandle(hProc); return False
    print(f"  CreateRemoteThread: h=0x{hThread:08X}")

    # Wait
    k32.WaitForSingleObject(hThread, 5000)
    k32.CloseHandle(hThread)
    k32.VirtualFreeEx(hProc, pRemote, 0, 0x8000)
    k32.CloseHandle(hProc)

    # Verify
    try:
        out = subprocess.check_output(
            ['tasklist','/M','libfrosted_dwm.dll','/FO','CSV'], text=True, timeout=5)
        if 'libfrosted_dwm.dll' in out:
            procs = [l.split(',')[0].strip('"') for l in out.strip().split('\n') if 'frosted' in l]
            print(f"[*] DLL loaded in: {', '.join(procs)}")
            if any('dwm' in p.lower() for p in procs):
                print("[*] SUCCESS — dwm.exe injected!")
            else:
                print("[!] dwm.exe not in loaded list (PPL may still be blocking)")
        else:
            print("[!] DLL not detected in any process")
    except: pass
    return True


def main():
    if not is_admin(): print("[!] Run as Administrator!"); return
    inject()

if __name__ == "__main__":
    main()
