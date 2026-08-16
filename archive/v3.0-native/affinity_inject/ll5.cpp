#include <windows.h>
#include <cstdio>
int main() {
    HMODULE h = LoadLibraryA("aff_test.dll");
    printf("load from project dir: %p err=%lu\n", h, GetLastError());
    return 0;
}
