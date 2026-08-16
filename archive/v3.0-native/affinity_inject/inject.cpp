// inject.cpp — Route I prototype injector.
// Usage: inject.exe <target_pid> <hwnd_hex>
// 1. Write affinity.dll path + InjectedArgs into the target process
// 2. CreateRemoteThread(LoadLibraryW) to load the DLL
// 3. CreateRemoteThread(RemoteEntry) to run the WDA call from owner context
// 4. Read result from C:\Temp\affinity_inject_result.txt
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

struct InjectedArgs { HWND hwnd; };

// Read an export's RVA from a local PE file (avoids hardcoding).
static DWORD find_export_rva(const char* dllPath, const char* name) {
    HANDLE f = CreateFileA(dllPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(f, nullptr);
    std::vector<BYTE> buf(sz);
    DWORD rd; ReadFile(f, buf.data(), sz, &rd, nullptr);
    CloseHandle(f);
    // PE parse
    DWORD e_lfanew = *(DWORD*)(buf.data() + 0x3C);
    if (buf[e_lfanew] != 'P' || buf[e_lfanew+1] != 'E') return 0;
    DWORD opt = e_lfanew + 24;
    WORD magic = *(WORD*)(buf.data() + opt);
    bool is64 = (magic == 0x20b);
    DWORD dd = opt + (is64 ? 112 : 96);
    DWORD exp_rva = *(DWORD*)(buf.data() + dd);
    WORD nsec = *(WORD*)(buf.data() + e_lfanew + 6);
    DWORD sec_off = opt + (is64 ? 240 : 224);
    auto rva_to_off = [&](DWORD rva) -> DWORD {
        for (int i = 0; i < nsec; i++) {
            DWORD so = sec_off + i * 40;
            DWORD vsize = *(DWORD*)(buf.data() + so + 8);
            DWORD va = *(DWORD*)(buf.data() + so + 12);
            DWORD rawsz = *(DWORD*)(buf.data() + so + 16);
            DWORD rawp = *(DWORD*)(buf.data() + so + 20);
            if (va <= rva && rva < va + (vsize > rawsz ? vsize : rawsz))
                return rawp + (rva - va);
        }
        return 0;
    };
    DWORD eoff = rva_to_off(exp_rva);
    if (!eoff) return 0;
    DWORD nnames = *(DWORD*)(buf.data() + eoff + 24);
    DWORD names_off = rva_to_off(*(DWORD*)(buf.data() + eoff + 32));
    DWORD ords_off = rva_to_off(*(DWORD*)(buf.data() + eoff + 36));
    DWORD funcs_off = rva_to_off(*(DWORD*)(buf.data() + eoff + 28));
    for (DWORD i = 0; i < nnames; i++) {
        DWORD nrva = *(DWORD*)(buf.data() + names_off + i * 4);
        DWORD noff = rva_to_off(nrva);
        if (!noff) continue;
        if (strcmp((const char*)(buf.data() + noff), name) == 0) {
            WORD ord = *(WORD*)(buf.data() + ords_off + i * 2);
            return *(DWORD*)(buf.data() + funcs_off + ord * 4);
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 3) { printf("usage: inject.exe <pid> <hwnd_hex>\n"); return 1; }
    DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
    HWND hwnd = (HWND)(ULONG_PTR)strtoull(argv[2], nullptr, 16);

    // 1. open target process
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) { printf("OpenProcess failed %lu\n", GetLastError()); return 1; }

    // 2. DLL path (this exe's dir + affinity.dll)
    char dllPath[MAX_PATH];
    GetModuleFileNameA(nullptr, dllPath, MAX_PATH);
    char* slash = strrchr(dllPath, '\\');
    if (slash) strcpy(slash + 1, "affinity.dll");

    // 3. write dll path into target
    void* remPath = VirtualAllocEx(hProc, nullptr, strlen(dllPath) + 1,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remPath) { printf("VirtualAllocEx path failed %lu\n", GetLastError()); return 1; }
    if (!WriteProcessMemory(hProc, remPath, dllPath, strlen(dllPath) + 1, nullptr)) {
        printf("WriteProcessMemory path failed %lu\n", GetLastError()); return 1;
    }

    // 4. LoadLibraryW in target
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadLib, remPath, 0, nullptr);
    if (!hThread) { printf("CreateRemoteThread LoadLibrary failed %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(hThread, 5000);
    DWORD dllBase = 0;
    GetExitCodeThread(hThread, &dllBase);
    CloseHandle(hThread);
    if (!dllBase) { printf("LoadLibrary in target failed\n"); return 1; }
    printf("DLL loaded at 0x%08lX\n", dllBase);

    // 5. write args into target
    InjectedArgs args = { hwnd };
    void* remArgs = VirtualAllocEx(hProc, nullptr, sizeof(args),
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(hProc, remArgs, &args, sizeof(args), nullptr);

    // 6. Remote entry address = dllBase + export RVA (GetProcAddress in the
    //    LOCAL process is wrong — the DLL lives in the REMOTE process).
    DWORD rva = find_export_rva(dllPath, "AffinityApply");
    if (!rva) { printf("cannot find AffinityApply RVA\n"); return 1; }
    void* applyAddr = (void*)((DWORD64)dllBase + rva);
    printf("AffinityApply RVA=0x%lX remote addr = 0x%p\n", rva, applyAddr);

    // 7. run it
    HANDLE hRun = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)applyAddr, remArgs, 0, nullptr);
    if (!hRun) { printf("CreateRemoteThread Apply failed %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(hRun, 5000);
    DWORD result = 0;
    GetExitCodeThread(hRun, &result);
    CloseHandle(hRun);
    printf("AffinityApply returned: %lu (2 = SUCCESS + verified, 1 = set ok, 5 = access denied, 8 = no surface)\n", result);

    // NOTE: do NOT FreeLibrary — the remote thread may still reference the
    // DLL code when it unwinds (caused 0xC0000409 crash in the first test).
    // The full solution keeps the DLL resident with a watchdog anyway.

    VirtualFreeEx(hProc, remPath, 0, MEM_RELEASE);
    VirtualFreeEx(hProc, remArgs, 0, MEM_RELEASE);
    CloseHandle(hProc);
    printf("done\n");
    return 0;
}
