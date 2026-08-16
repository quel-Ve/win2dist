#include <windows.h>
#include <cstdio>
#include <cstdlib>
int main() {
    DWORD pid = GetCurrentProcessId();
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    const char* dllPath = "affinity_probe.dll";  // relative — loads from cwd
    void* remPath = VirtualAllocEx(hProc, nullptr, strlen(dllPath)+1, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    WriteProcessMemory(hProc, remPath, dllPath, strlen(dllPath)+1, nullptr);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLib = GetProcAddress(k32, "LoadLibraryA");
    HANDLE hT = CreateRemoteThread(hProc, nullptr, 0, (LPTHREAD_START_ROUTINE)loadLib, remPath, 0, nullptr);
    WaitForSingleObject(hT, 5000);
    DWORD base = 0; GetExitCodeThread(hT, &base); CloseHandle(hT);
    printf("remote LoadLibrary: 0x%08lX (%s)\n", base, base ? "OK" : "FAILED");
    CloseHandle(hProc);
    return 0;
}
