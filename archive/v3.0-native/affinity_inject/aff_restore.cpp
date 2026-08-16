// aff_restore.dll — restore WDA_NONE on the target hwnd (read from
// C:\Temp\affinity_args.txt). Injected into the owner process like the setter.
#include <windows.h>
extern "C" __declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        HWND target = nullptr;
        HANDLE f = CreateFileW(L"C:\\Temp\\affinity_args.txt", GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[32] = {0}; DWORD rd = 0;
            ReadFile(f, buf, 31, &rd, nullptr); CloseHandle(f);
            unsigned long long hw = 0;
            for (DWORD i = 0; i < rd; i++) {
                char c = buf[i]; unsigned v;
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
                else continue;
                hw = hw * 16 + v;
            }
            target = (HWND)(ULONG_PTR)hw;
        }
        if (target && IsWindow(target)) {
            SetWindowDisplayAffinity(target, 0);   // restore WDA_NONE
        }
    }
    return TRUE;
}
