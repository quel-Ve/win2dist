/**
 * DWM DLL injector — injects a DLL into dwm.exe.
 * Auto-unloads any previously injected frosted_dwm DLL before loading.
 * Build: gcc -o injector.exe injector.c -ladvapi32
 * Usage: injector.exe <dll_path>
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

BOOL EnableDebugPriv() {
    HANDLE hToken;
    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return FALSE;

    if (!LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &luid)) {
        CloseHandle(hToken);
        return FALSE;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(hToken);
    return GetLastError() == ERROR_SUCCESS;
}

DWORD FindDwmPid() {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD pid = 0;

    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return pid;
}

// Unload any frosted_dwm DLLs still loaded from previous injections
void UnloadOldDlls(HANDLE hProc) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetProcessId(hProc));
    if (hSnap == INVALID_HANDLE_VALUE) return;

    MODULEENTRY32W me = { sizeof(me) };
    if (Module32FirstW(hSnap, &me)) {
        do {
            if (wcsstr(me.szModule, L"frosted_dwm")) {
                printf("Unloading old: %S (0x%p)\n", me.szModule, me.hModule);
                HANDLE hThread = CreateRemoteThread(hProc, NULL, 0,
                    (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "FreeLibrary"),
                    me.hModule, 0, NULL);
                if (hThread) {
                    WaitForSingleObject(hThread, 3000);
                    CloseHandle(hThread);
                    printf("  -> unloaded\n");
                } else {
                    printf("  -> FAILED (err %lu)\n", GetLastError());
                }
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: injector.exe <dll_path>\n");
        return 1;
    }

    wchar_t dllPath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, dllPath, MAX_PATH);

    printf("DLL: %S\n", dllPath);

    DWORD pid = FindDwmPid();
    if (!pid) { printf("FAIL: dwm.exe not found\n"); return 1; }
    printf("dwm.exe PID: %lu\n", pid);

    if (!EnableDebugPriv()) {
        printf("WARN: SeDebugPrivilege not enabled (error %lu)\n", GetLastError());
    } else {
        printf("SeDebugPrivilege: OK\n");
    }

    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProc) { printf("FAIL: OpenProcess error %lu\n", GetLastError()); return 1; }
    printf("OpenProcess: h=0x%p\n", hProc);

    // Step 0: unload old frosted_dwm DLLs
    UnloadOldDlls(hProc);

    SIZE_T size = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID pRemote = VirtualAllocEx(hProc, NULL, size, MEM_COMMIT, PAGE_READWRITE);
    if (!pRemote) { printf("FAIL: VirtualAllocEx error %lu\n", GetLastError()); CloseHandle(hProc); return 1; }
    printf("VirtualAllocEx: p=0x%p\n", pRemote);

    if (!WriteProcessMemory(hProc, pRemote, dllPath, size, NULL)) {
        printf("FAIL: WriteProcessMemory error %lu\n", GetLastError());
        if (GetLastError() == ERROR_NOACCESS)
            printf("  -> ERROR_NOACCESS: dwm.exe is PPL-protected\n");
        VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return 1;
    }
    printf("WriteProcessMemory: OK (%zu bytes)\n", size);

    LPTHREAD_START_ROUTINE ll = (LPTHREAD_START_ROUTINE)
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (!ll) { printf("FAIL: LoadLibraryW not found\n"); VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE); CloseHandle(hProc); return 1; }

    HANDLE hThread = CreateRemoteThread(hProc, NULL, 0, ll, pRemote, 0, NULL);
    if (!hThread) { printf("FAIL: CreateRemoteThread error %lu\n", GetLastError()); VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE); CloseHandle(hProc); return 1; }
    printf("CreateRemoteThread: h=0x%p\n", hThread);

    WaitForSingleObject(hThread, 5000);
    CloseHandle(hThread);
    VirtualFreeEx(hProc, pRemote, 0, MEM_RELEASE);
    CloseHandle(hProc);

    printf("SUCCESS: DLL injected\n");
    return 0;
}
