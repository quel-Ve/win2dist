// profile.cpp
#include "profile.h"
#include <vector>
static std::wstring lower(const std::wstring& s) {
    std::wstring r = s; for (auto& c : r) c = (wchar_t)towlower(c); return r;
}
static int parse_int_field(const std::wstring& v, int lo, int hi) {
    wchar_t* end = nullptr; long n = wcstol(v.c_str(), &end, 10);
    if (!end || *end != 0 || end == v.c_str()) return -1;
    return (n >= lo && n <= hi) ? (int)n : -1;
}
ProfileEntry parse_profile_line(const wchar_t* line) {
    ProfileEntry e; if (!line || !*line) return e;
    std::wstring s(line);
    size_t eq = s.find(L'=');
    std::wstring exeTok = eq == std::wstring::npos ? s : s.substr(0, eq);
    while (!exeTok.empty() && exeTok.back() == L' ') exeTok.pop_back();
    if (exeTok.empty()) return e;
    e.exe = lower(exeTok);
    if (eq == std::wstring::npos) return e;
    std::wstring rest = s.substr(eq + 1);
    size_t i = 0;
    while (i < rest.size()) {
        while (i < rest.size() && rest[i] == L' ') i++;
        size_t j = rest.find(L' ', i);
        if (j == std::wstring::npos) j = rest.size();
        std::wstring tok = rest.substr(i, j - i); i = j + 1;
        size_t keq = tok.find(L'=');
        if (keq == std::wstring::npos) continue;
        std::wstring k = lower(tok.substr(0, keq));
        std::wstring v = lower(tok.substr(keq + 1));
        if (k == L"mode") e.mode = (v == L"crisp") ? 1 : (v == L"acrylic") ? 0 : -1;
        else if (k == L"alpha") e.alpha = parse_int_field(v, 0, 255);
        else if (k == L"radius") e.radius = parse_int_field(v, 1, 120);
        else if (k == L"tint") e.tint = parse_int_field(v, 0, 100);
        else if (k == L"circle") e.circle = (v == L"on");
        else if (k == L"cls") e.cls = v;
    }
    return e;
}
void build_profile_line(const AutoApp& a, wchar_t* out) {
    std::wstring s = a.exe;
    wchar_t b[32];
    bool first = true;
    auto emit = [&](const wchar_t* tok) {
        s += first ? L" = " : L" ";   // "exe = field1 field2 ..." — exe token
        s += tok;                     // stops at the first '=' (parser contract)
        first = false;
    };
    if (!a.cls.empty()) { wsprintfW(b, L"cls=%s", a.cls.c_str()); emit(b); }
    if (a.mode >= 0) emit(a.mode ? L"mode=crisp" : L"mode=acrylic");
    if (a.alpha >= 0) { wsprintfW(b, L"alpha=%d", a.alpha); emit(b); }
    if (a.radius >= 0) { wsprintfW(b, L"radius=%d", a.radius); emit(b); }
    if (a.tint >= 0) { wsprintfW(b, L"tint=%d", a.tint); emit(b); }
    if (a.circle) emit(L"circle=on");
    wcscpy(out, s.c_str());
}
