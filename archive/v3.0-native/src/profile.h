// profile.h
#pragma once
#define UNICODE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
struct ProfileEntry {
    std::wstring exe, cls;
    int mode = -1, alpha = -1, radius = -1, tint = -1;
    bool circle = false;
};
ProfileEntry parse_profile_line(const wchar_t* line);

// v3.0: per-app profile as the tray stores it — same fields as ProfileEntry.
// Lives here (beside its serializer) so the write path is unit-testable.
// -1 / false = unset -> inherit globals at apply time.
struct AutoApp {
    std::wstring exe, cls;
    int mode = -1, alpha = -1, radius = -1, tint = -1;
    bool circle = false;
};
// Serialize one profile to an [Apps] line: "exe = mode=.. alpha=.. ...".
// Explicit fields only; -1 / false fields omitted (reload as "inherit global").
// The FIRST field must be preceded by " = " — parse_profile_line defines the
// exe token as everything before the first '='; without the separator the
// first field's '=' would be absorbed into the exe token and all fields lost.
void build_profile_line(const AutoApp& a, wchar_t* out);
