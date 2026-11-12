// Disk health window options (see health_store.h).
#include "core/health_store.h"
#include <windows.h>
#include <shlobj.h>
#include <wchar.h>

static wchar_t s_dir[MAX_PATH], s_ini[MAX_PATH];

static const wchar_t* store_dir() {
    if (!s_dir[0]) {
        PWSTR appdata = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) {
            swprintf(s_dir, MAX_PATH, L"%s\\ez-pc-bench\\health", appdata);
            swprintf(s_ini, MAX_PATH, L"%s\\health.ini", s_dir);
        }
        CoTaskMemFree(appdata);
    }
    return s_dir;
}

static bool ensure_dir() {
    if (!store_dir()[0]) return false;
    wchar_t parent[MAX_PATH];
    wcscpy_s(parent, s_dir);
    if (wchar_t* slash = wcsrchr(parent, L'\\')) *slash = 0;
    CreateDirectoryW(parent, nullptr);
    CreateDirectoryW(s_dir, nullptr);
    return GetFileAttributesW(s_dir) != INVALID_FILE_ATTRIBUTES;
}

// Creates health.ini with a UTF-16 BOM so the profile API writes Unicode.
static const wchar_t* ini_path(bool create) {
    if (!store_dir()[0]) return nullptr;
    if (create && ensure_dir()) {
        HANDLE f = CreateFileW(s_ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const u16 bom = 0xFEFF;
            DWORD written;
            WriteFile(f, &bom, sizeof(bom), &written, nullptr);
            CloseHandle(f);
        }
    }
    return s_ini;
}

static void widen(const char* s, wchar_t* out, u32 count) { MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)count); }

bool health_ui_option(const char* name, bool def) {
    const wchar_t* ini = ini_path(false);
    if (!ini) return def;
    wchar_t key[64];
    widen(name, key, ARRAY_COUNT(key));
    return GetPrivateProfileIntW(L"ui", key, def ? 1 : 0, ini) != 0;
}

void health_set_ui_option(const char* name, bool value) {
    const wchar_t* ini = ini_path(true);
    if (!ini) return;
    wchar_t key[64];
    widen(name, key, ARRAY_COUNT(key));
    WritePrivateProfileStringW(L"ui", key, value ? L"1" : L"0", ini);
}
