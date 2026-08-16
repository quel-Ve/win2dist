/**
 * shm_writer.exe <blurRadius> — create Global shared memory with NULL DACL,
 * write blur radius. Test tool to verify DLL <-> app IPC link.
 * Build: g++ shm_writer.cpp -o shm_writer.exe
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

#define SHM_NAME L"Global\\FrostedDWM_Config"
#define SHM_MAGIC 0x4652444D

int main(int argc, char* argv[]) {
    int radius = (argc > 1) ? atoi(argv[1]) : 30;

    // NULL DACL: allow ALL processes (incl. SYSTEM dwm.exe) to access
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES sa = {sizeof(sa), &sd, TRUE};

    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa,
        PAGE_READWRITE, 0, 8, SHM_NAME);
    if (!h) {
        printf("FAIL: CreateFileMapping err=%lu\n", GetLastError());
        return 1;
    }
    DWORD* data = (DWORD*)MapViewOfFile(h, FILE_MAP_WRITE, 0, 0, 8);
    if (!data) {
        printf("FAIL: MapViewOfFile err=%lu\n", GetLastError());
        return 1;
    }
    data[0] = SHM_MAGIC;
    data[1] = (DWORD)radius;

    printf("SHM created: Global\\FrostedDWM_Config blur=%d\n", radius);
    printf("Keep running... press Ctrl+C to exit\n");
    fflush(stdout);

    // Keep alive so the mapping stays valid
    while (true) {
        // Also allow interactive value change: type a number + Enter
        char line[32];
        if (fgets(line, sizeof(line), stdin)) {
            int v = atoi(line);
            if (v >= 1 && v <= 100) {
                data[1] = (DWORD)v;
                printf("  -> blur set to %d\n", v);
            } else {
                printf("  (enter 1-100)\n");
            }
        }
    }
    return 0;
}
