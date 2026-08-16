// affinity.dll — Route I prototype: when injected into the TARGET process,
// set WDA_EXCLUDEFROMCAPTURE on the target window FROM THE OWNER'S context
// (the only context where SetWindowDisplayAffinity is legal — verified:
// cross-process always returns ERROR_ACCESS_DENIED, even from High IL).
//
// The target HWND is passed via a command-line-style argument in the
// injected thread's parameter (a small structure in the remote process).
//
// Lifecycle: sets affinity once, then exits the thread and unloads.
// (Full watchdog/persistent mode is a later stage; this is the viability
// prototype: "can an injected owner-context call set WDA successfully?")

#include <windows.h>
#include <cstdio>

// Structure written into the remote process by the injector
struct InjectedArgs {
    HWND hwnd;          // target window (a window owned by THIS process)
};

// Entry point executed in the remote process (CreateRemoteThread target)
static DWORD WINAPI RemoteEntry(LPVOID lpParam) {
    InjectedArgs* args = (InjectedArgs*)lpParam;
    DWORD result = 0;
    if (args && IsWindow(args->hwnd)) {
        // We are now inside the target process — the window's OWNER.
        // SetWindowDisplayAffinity must succeed here (owner context).
        if (SetWindowDisplayAffinity(args->hwnd, 0x11)) {   // WDA_EXCLUDEFROMCAPTURE
            result = 1;
            // verify it stuck
            DWORD cur = 0;
            GetWindowDisplayAffinity(args->hwnd, &cur);
            if (cur == 0x11) result = 2;
        } else {
            result = (DWORD)GetLastError();
        }
    }
    // Persist result where the injector can read it (a file is simplest for
    // the prototype — no shared memory plumbing yet).
    HANDLE f = CreateFileW(L"C:\\Temp\\affinity_inject_result.txt",
        GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[32];
        int n = wsprintfA(buf, "%lu", result);
        DWORD wr; WriteFile(f, buf, n, &wr, NULL);
        CloseHandle(f);
    }
    return result;
}

// Standard DLL export used with LoadLibrary injection
extern "C" __declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID) {
    if (why == DLL_PROCESS_ATTACH) {
        // The injector uses CreateRemoteThread(LoadLibraryW) then a second
        // CreateRemoteThread(RemoteEntry). The entry itself does the work.
    }
    return TRUE;
}

// Also export the entry so the injector can GetProcAddress it.
extern "C" __declspec(dllexport) DWORD WINAPI AffinityApply(void* p) {
    return RemoteEntry(p);
}
