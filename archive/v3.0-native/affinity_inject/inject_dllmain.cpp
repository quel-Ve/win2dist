// inject_dllmain.exe — load affinity DLL into a target process; the DLL's
// DllMain sets WDA_EXCLUDEFROMCAPTURE on the hwnd read from
// C:\Temp\affinity_args.txt (owner context), writes result to
// C:\Temp\affinity_result.txt.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
int main(int argc, char* argv[]) {
    if (argc < 2) { printf("usage: inject_dllmain.exe <pid>\n"); return 1; }
    DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) { printf("OpenProcess failed %lu\n", GetLastError()); return 1; }
    // absolute path — remote thread's CWD is the TARGET's, not ours
    const char* dllPath =
        "D:\\AcademicM\\2ccproject\\12window2clear\\native\\affinity_inject\\aff_restore.dll";
    void* remPath = VirtualAllocEx(hProc, nullptr, strlen(dllPath) + 1,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remPath) { printf("VirtualAllocEx failed %lu\n", GetLastError()); return 1; }
    WriteProcessMemory(hProc, remPath, dllPath, strlen(dllPath) + 1, nullptr);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    HANDLE hT = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadLib, remPath, 0, nullptr);
    if (!hT) { printf("CreateRemoteThread failed %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(hT, 5000);
    DWORD base = 0; GetExitCodeThread(hT, &base); CloseHandle(hT);
    printf("LoadLibrary returned 0x%08lX (%s)\n", base, base ? "OK" : "FAILED");
    // keep DLL resident (DllMain did the work)
    VirtualFreeEx(hProc, remPath, 0, MEM_RELEASE);
    CloseHandle(hProc);
    printf("done\n");
    return 0;
}
