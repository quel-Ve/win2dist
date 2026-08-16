#define UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include "../src/profile.h"
static int g_fail = 0;
#define WSTR(x) L##x
#define CHECK(cond) do { if(!(cond)) { wprintf(L"FAIL %s:%d %s\n", L"profile_test.cpp", __LINE__, WSTR(#cond)); g_fail++; } } while(0)
static AutoApp to_app(const ProfileEntry& pe) {
    AutoApp a;
    a.exe = pe.exe; a.cls = pe.cls;
    a.mode = pe.mode; a.alpha = pe.alpha; a.radius = pe.radius;
    a.tint = pe.tint; a.circle = pe.circle;
    return a;
}
// parse -> build_profile_line -> parse must recover every field; -1/false
// fields stay omitted on rebuild (round-trip stability).
static bool rt_stable(const wchar_t* line) {
    ProfileEntry a = parse_profile_line(line);
    wchar_t buf[256];
    build_profile_line(to_app(a), buf);
    ProfileEntry b = parse_profile_line(buf);
    return a.exe == b.exe && a.cls == b.cls && a.mode == b.mode &&
           a.alpha == b.alpha && a.radius == b.radius &&
           a.tint == b.tint && a.circle == b.circle;
}
int wmain() {
    ProfileEntry a = parse_profile_line(L"Notepad.exe = mode=crisp alpha=65 radius=40 tint=20 circle=on");
    CHECK(a.exe == L"notepad.exe"); CHECK(a.mode == 1); CHECK(a.alpha == 65);
    CHECK(a.radius == 40); CHECK(a.tint == 20); CHECK(a.circle == true);
    ProfileEntry b = parse_profile_line(L"Cherry Studio.exe");
    CHECK(b.exe == L"cherry studio.exe"); CHECK(b.mode == -1); CHECK(b.alpha == -1); CHECK(b.circle == false);
    ProfileEntry c = parse_profile_line(L"  = mode=crisp");
    CHECK(c.exe.empty());
    ProfileEntry d = parse_profile_line(L"Taskmgr.exe = radius=25 tint=10 mode=acrylic circle=off");
    CHECK(d.mode == 0); CHECK(d.tint == 10); CHECK(d.circle == false);
    ProfileEntry e = parse_profile_line(L"X.exe = mode=BOGUS alpha=999");
    CHECK(e.mode == -1); CHECK(e.alpha == -1);   // invalid values -> default
    // --- v3.0 writer round-trip (regression: "exe = field" separator) ---
    ProfileEntry f = parse_profile_line(L"Notepad.exe = mode=crisp alpha=65 radius=40 tint=20 circle=on");
    wchar_t wbuf[256];
    build_profile_line(to_app(f), wbuf);
    CHECK(wcscmp(wbuf, L"notepad.exe = mode=crisp alpha=65 radius=40 tint=20 circle=on") == 0);
    CHECK(rt_stable(L"Notepad.exe = mode=crisp alpha=65 radius=40 tint=20 circle=on"));
    CHECK(rt_stable(L"explorer.exe = cls=CabinetWClass"));      // cls-bearing (preset shape)
    CHECK(rt_stable(L"Cherry Studio.exe"));                     // bare exe, no fields
    CHECK(rt_stable(L"Taskmgr.exe = mode=acrylic tint=10"));    // partial fields, acrylic
    CHECK(rt_stable(L"cloudmusic.exe = alpha=80 circle=on"));   // no mode/radius
    wprintf(g_fail ? L"FAILED %d\n" : L"PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
