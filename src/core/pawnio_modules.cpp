// PawnIO module files: lookup and download (see pawnio_modules.h).
#include "core/pawnio_modules.h"
#include "core/integrity.h"
#include "core/pawnio.h"
#include "core/unzip.h"
#include <windows.h>
#include <shlobj.h>
#include <urlmon.h>
#include <stdio.h>
#include <string.h>

constexpr u32 ZIP_MAX = 4 * (u32)MiB;           // the 0.2.11 zip is 68 KB
constexpr u32 MODULE_MIN = 1 * (u32)KiB, MODULE_MAX = 1 * (u32)MiB;     // 0.2.11: 5 to 55 KB
constexpr u32 LICENSE_MIN = 1 * (u32)KiB, LICENSE_MAX = 256 * (u32)KiB;  // LGPL-2.1 text: 27 KB
static const char* const LICENSE_NAME = "COPYING";

void pawnio_modules_url(char* out, u32 size) {
    char under[32];
    snprintf(under, sizeof(under), "%s", PAWNIO_MODULES_VERSION);
    for (char* c = under; *c; ++c)
        if (*c == '.') *c = '_';
    snprintf(out, size, "https://github.com/namazso/PawnIO.Modules/releases/download/%s/release_%s.zip",
             PAWNIO_MODULES_VERSION, under);
}

bool pawnio_modules_dir(wchar_t* out, u32 cap) {
    PWSTR local = nullptr;
    const bool ok = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) &&
                    swprintf(out, cap, L"%s\\ez-pc-bench\\pawnio\\%hs", local, PAWNIO_MODULES_VERSION) > 0;
    CoTaskMemFree(local);
    return ok;
}

static bool file_size_in(const wchar_t* path, u32 lo, u32 hi) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    return !a.nFileSizeHigh && a.nFileSizeLow >= lo && a.nFileSizeLow <= hi;
}

static bool exe_pawnio_path(const char* file, wchar_t* out, u32 cap) {
    const DWORD n = GetModuleFileNameW(nullptr, out, cap);
    if (!n || n >= cap) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    return slash && swprintf(slash + 1, cap - (size_t)(slash + 1 - out), L"pawnio\\%hs", file) > 0;
}

bool pawnio_module_path(const char* name, wchar_t* out, u32 cap) {
    char file[64];
    snprintf(file, sizeof(file), "%s.bin", name);
    wchar_t dir[MAX_PATH];
    if (pawnio_modules_dir(dir, MAX_PATH) && swprintf(out, cap, L"%s\\%hs", dir, file) > 0 &&
        file_size_in(out, MODULE_MIN, MODULE_MAX))
        return true;
    return exe_pawnio_path(file, out, cap) && file_size_in(out, MODULE_MIN, MODULE_MAX);
}

bool pawnio_modules_downloaded() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!pawnio_modules_dir(dir, MAX_PATH)) return false;
    for (u32 i = 0; i < PM_COUNT; ++i) {
        swprintf(path, MAX_PATH, L"%s\\%hs.bin", dir, pawnio_module_name((PawnModuleId)i));
        if (!file_size_in(path, MODULE_MIN, MODULE_MAX)) return false;
    }
    swprintf(path, MAX_PATH, L"%s\\%hs", dir, LICENSE_NAME);
    return file_size_in(path, LICENSE_MIN, LICENSE_MAX);
}

// ---- download ---------------------------------------------------------------------------------------------

// Whole response body into buf (URLMon follows GitHub's redirect to its download host). 0 or an HRESULT.
static HRESULT http_get(const char* url, u8* buf, u32 cap, u32* got) {
    wchar_t wurl[256];
    swprintf(wurl, ARRAY_COUNT(wurl), L"%hs", url);
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IStream* st = nullptr;
    HRESULT hr = URLOpenBlockingStreamW(nullptr, wurl, &st, 0, nullptr);
    *got = 0;
    while (SUCCEEDED(hr)) {
        ULONG n = 0;
        hr = st->Read(buf + *got, cap - *got, &n);
        *got += n;
        if (hr == S_FALSE || (SUCCEEDED(hr) && !n)) {   // end of stream
            hr = S_OK;
            break;
        }
        if (*got == cap) hr = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    if (st) st->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    return hr;
}

// Writes via "<path>.tmp" + rename, so a half-written file never has the real name.
static bool write_file(const wchar_t* path, const void* data, u32 size) {
    wchar_t tmp[MAX_PATH + 8];
    swprintf(tmp, ARRAY_COUNT(tmp), L"%s.tmp", path);
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    const bool ok = WriteFile(f, data, size, &put, nullptr) && put == size;
    CloseHandle(f);
    if (ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    const DWORD err = GetLastError();
    DeleteFileW(tmp);
    SetLastError(err);
    return false;
}

static void utf8(const wchar_t* w, char* out, u32 size) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)size, nullptr, nullptr) && size) out[0] = 0;
}

// Unpacks one member into dir after checking its size range (and, in zip_read, its CRC-32).
static bool unpack(const u8* zip, u32 zip_size, const wchar_t* dir, const char* name, u32 lo, u32 hi, u8* buf,
                   char* msg, u32 size) {
    ZipEntry e;
    if (!zip_find(zip, zip_size, name, &e)) {
        snprintf(msg, size, "%s isn't in the modules zip.", name);
        return false;
    }
    if (e.size < lo || e.size > hi || !zip_read(zip, zip_size, &e, buf)) {
        snprintf(msg, size, "%s in the modules zip is damaged or has an unexpected size (%u bytes).", name, e.size);
        return false;
    }
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\%hs", dir, name);
    if (!write_file(path, buf, e.size)) {
        const DWORD err = GetLastError();
        char p8[MAX_PATH * 3];
        utf8(path, p8, sizeof(p8));
        snprintf(msg, size, "Couldn't write %s (error %lu).", p8, err);
        return false;
    }
    return true;
}

// LGPL-2.1: the object code travels with the license text and a note naming the release and its source.
static void write_readme(const wchar_t* dir, const char* url) {
    char text[768];
    const int n = snprintf(text, sizeof(text),
        "Signed PawnIO modules from the official PawnIO.Modules repository, release %s, unmodified.\r\n"
        "Source: https://github.com/namazso/PawnIO.Modules (tag %s)\r\n"
        "Downloaded by EzPcBench from %s\r\n"
        "SHA-256 of that zip: %s\r\n"
        "License: GNU Lesser General Public License v2.1, see COPYING in this folder.\r\n"
        "Only the modules EzPcBench loads were unpacked. Deleting this folder is safe: the app offers to\r\n"
        "download them again.\r\n",
        PAWNIO_MODULES_VERSION, PAWNIO_MODULES_VERSION, url, PAWNIO_MODULES_SHA256);
    wchar_t path[MAX_PATH];
    swprintf(path, MAX_PATH, L"%s\\README.txt", dir);
    if (n > 0) write_file(path, text, (u32)n);
}

// Checks the zip's hash, then unpacks the modules this app loads and the license text.
static bool unpack_all(const u8* zip, u32 zip_size, const wchar_t* dir, u8* buf, char* msg, u32 size) {
    char hash[65];
    sha256_hex(zip, zip_size, hash);
    if (strcmp(hash, PAWNIO_MODULES_SHA256)) {
        snprintf(msg, size, "The downloaded PawnIO modules zip (%u bytes) doesn't match the expected SHA-256; nothing "
                 "was unpacked.", zip_size);
        return false;
    }
    const int mk = SHCreateDirectoryExW(nullptr, dir, nullptr);
    if (mk != ERROR_SUCCESS && mk != ERROR_ALREADY_EXISTS) {
        char d8[MAX_PATH * 3];
        utf8(dir, d8, sizeof(d8));
        snprintf(msg, size, "Couldn't create %s (error %d).", d8, mk);
        return false;
    }
    for (u32 i = 0; i < PM_COUNT; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "%s.bin", pawnio_module_name((PawnModuleId)i));
        if (!unpack(zip, zip_size, dir, name, MODULE_MIN, MODULE_MAX, buf, msg, size)) return false;
    }
    return unpack(zip, zip_size, dir, LICENSE_NAME, LICENSE_MIN, LICENSE_MAX, buf, msg, size);
}

bool pawnio_modules_download(char* msg, u32 size) {
    wchar_t dir[MAX_PATH];
    if (!pawnio_modules_dir(dir, MAX_PATH)) {
        snprintf(msg, size, "No local application data folder to put the PawnIO modules in.");
        return false;
    }
    char url[192], d8[MAX_PATH * 3];
    pawnio_modules_url(url, sizeof(url));
    utf8(dir, d8, sizeof(d8));
    u8* zip = (u8*)VirtualAlloc(nullptr, ZIP_MAX + MODULE_MAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!zip) {
        snprintf(msg, size, "Out of memory.");
        return false;
    }
    u8* buf = zip + ZIP_MAX;        // one unpacked member at a time (LICENSE_MAX < MODULE_MAX)
    u32 zip_size = 0;
    bool ok = false;
    const HRESULT hr = http_get(url, zip, ZIP_MAX, &zip_size);
    if (FAILED(hr)) {
        snprintf(msg, size, "Downloading the PawnIO modules failed (error 0x%08lX). Check the internet connection; "
                 "the file is %s.", (unsigned long)hr, url);
    } else if (unpack_all(zip, zip_size, dir, buf, msg, size)) {
        write_readme(dir, url);
        ok = pawnio_modules_downloaded();
        if (ok) snprintf(msg, size, "PawnIO modules %s downloaded to %s.", PAWNIO_MODULES_VERSION, d8);
        else snprintf(msg, size, "The PawnIO modules were unpacked but can't be read back from %s.", d8);
    }
    VirtualFree(zip, 0, MEM_RELEASE);
    return ok;
}
