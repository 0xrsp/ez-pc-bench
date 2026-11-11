#include "core/elevation.h"
#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev = {};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size);
    CloseHandle(token);
    return ok && elev.TokenIsElevated;
}

// Command line without argv[0].
static const wchar_t* skip_program_name(const wchar_t* cmd) {
    if (*cmd == L'"') {
        ++cmd;
        while (*cmd && *cmd != L'"') ++cmd;
        if (*cmd) ++cmd;
    } else {
        while (*cmd && *cmd != L' ' && *cmd != L'\t') ++cmd;
    }
    while (*cmd == L' ' || *cmd == L'\t') ++cmd;
    return cmd;
}

bool relaunch_elevated(const wchar_t* extra_arg) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);

    static wchar_t params[8192];
    swprintf(params, ARRAYSIZE(params), L"%s %s", skip_program_name(GetCommandLineW()), extra_arg ? extra_arg : L"");

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params;
    sei.lpDirectory = cwd;      // runas would otherwise start in System32
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}
