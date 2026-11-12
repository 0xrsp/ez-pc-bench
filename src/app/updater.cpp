// Auto-update from GitHub releases (see updater.h). WinHTTP for the API and the download, SHA-256 via
// core/integrity, JSON via core/json.
#include "app/updater.h"
#include "core/elevation.h"
#include "core/integrity.h"
#include "core/json.h"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const wchar_t UPDATE_API_URL[] = L"https://api.github.com/repos/0xrsp/ez-pc-bench/releases/latest";
static const wchar_t API_HEADERS[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28";
static const char UPDATE_ASSET[] = "ez-pc-bench.exe";
constexpr u32 MAX_JSON = 4 * MiB, MAX_EXE = 128 * MiB;

struct HttpBuf {
    u8* data;
    u32 size;
};

struct Updater {
    std::atomic<UpdateState> state;
    std::atomic<u32> got, total;    // download progress, bytes
    UpdateInfo info;
    char sha256[65];                // published hash of the asset (lowercase hex)
    wchar_t url[1024];              // asset download URL
    HttpBuf exe;                    // downloaded asset, kept between the check and the install when the check needed it
    char error[256];
    wchar_t self[MAX_PATH];         // running exe
};
static Updater s_upd;

static void buf_free(HttpBuf* b) {
    if (b->data) VirtualFree(b->data, 0, MEM_RELEASE);
    *b = {};
}

// HTTPS GET (WinHTTP follows the redirect to GitHub's asset host). False on errors, non-200 status or a body
// over `max` bytes; `out` is 0-terminated. Progress goes to s_upd.got / total.
static bool http_get(const wchar_t* url, const wchar_t* headers, u32 max, HttpBuf* out, char* err, u32 err_size) {
    *out = {};
    URL_COMPONENTS uc = { sizeof(uc) };
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || uc.dwHostNameLength >= 256) {
        snprintf(err, err_size, "bad URL");
        return false;
    }
    wchar_t host[256];
    wcsncpy_s(host, uc.lpszHostName, uc.dwHostNameLength);
    const wchar_t* path = uc.lpszUrlPath;       // runs to the end of the URL: path + query

    HINTERNET ses = WinHttpOpen(L"ez-pc-bench-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET con = nullptr, req = nullptr;
    bool ok = false;
    if (ses) {
        WinHttpSetTimeouts(ses, 10000, 10000, 15000, 30000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    DWORD status = 0, length = 0, dw_size = sizeof(DWORD);
    if (!req || !WinHttpSendRequest(req, headers, (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, nullptr)) {
        snprintf(err, err_size, "no connection (error %lu)", GetLastError());
    } else if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &dw_size, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        snprintf(err, err_size, "HTTP status %lu", status);
    } else {
        dw_size = sizeof(DWORD);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &length, &dw_size, WINHTTP_NO_HEADER_INDEX))
            length = 0;     // chunked: unknown
        s_upd.got.store(0, std::memory_order_relaxed);
        s_upd.total.store(length, std::memory_order_relaxed);
        const u32 cap = length ? length : max;      // + 1 byte: reading past `cap` means too large / too long
        out->data = length <= max ? (u8*)VirtualAlloc(nullptr, (SIZE_T)cap + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
                                  : nullptr;
        ok = out->data != nullptr;
        if (!ok) snprintf(err, err_size, length > max ? "too large" : "out of memory");
        while (ok) {
            const u32 room = cap + 1 - out->size;
            DWORD n = 0;
            if (!WinHttpReadData(req, out->data + out->size, room < 256 * 1024 ? room : 256 * 1024, &n)) {
                snprintf(err, err_size, "interrupted (error %lu)", GetLastError());
                ok = false;
            } else if (!n) {
                break;
            } else if ((out->size += n) > cap) {
                snprintf(err, err_size, "too large");
                ok = false;
            }
            s_upd.got.store(out->size, std::memory_order_relaxed);
        }
        if (ok && length && out->size != length) {
            snprintf(err, err_size, "incomplete (%u of %lu bytes)", out->size, length);
            ok = false;
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    if (ok) out->data[out->size] = 0;
    else buf_free(out);
    return ok;
}

// Release fields and the ez-pc-bench.exe asset (URL, size, sha256 digest when GitHub has one).
static bool read_release(const JsonDoc* d) {
    UpdateInfo* in = &s_upd.info;
    json_string(d, json_find(d, 0, "tag_name"), in->tag, sizeof(in->tag));
    json_string(d, json_find(d, 0, "name"), in->name, sizeof(in->name));
    json_string(d, json_find(d, 0, "published_at"), in->published, sizeof(in->published));
    json_string(d, json_find(d, 0, "html_url"), in->html_url, sizeof(in->html_url));
    json_string(d, json_find(d, 0, "body"), in->notes, sizeof(in->notes));    // truncated is fine
    const u32 assets = json_find(d, 0, "assets");
    for (u32 a = json_first(d, assets); a != JSON_NONE; a = json_next(d, assets, a)) {
        char name[128], url[1024], digest[96];
        if (!json_string(d, json_find(d, a, "name"), name, sizeof(name)) || _stricmp(name, UPDATE_ASSET) != 0) continue;
        if (!json_string(d, json_find(d, a, "browser_download_url"), url, sizeof(url)) ||
            !MultiByteToWideChar(CP_UTF8, 0, url, -1, s_upd.url, ARRAY_COUNT(s_upd.url)))
            return false;
        in->size = (u64)json_number(d, json_find(d, a, "size"));
        s_upd.sha256[0] = 0;
        if (json_string(d, json_find(d, a, "digest"), digest, sizeof(digest)) && !strncmp(digest, "sha256:", 7) &&
            strlen(digest + 7) == 64)
            for (u32 i = 0; i < 65; ++i) s_upd.sha256[i] = (char)(digest[7 + i] | (digest[7 + i] >= 'A' ? 0x20 : 0));
        return true;
    }
    return false;
}

static bool download_asset(char* err, u32 err_size) {
    if (!http_get(s_upd.url, L"Accept: application/octet-stream", MAX_EXE, &s_upd.exe, err, err_size)) return false;
    const HttpBuf* b = &s_upd.exe;
    const char* bad = s_upd.info.size && b->size != s_upd.info.size ? "the size doesn't match the release"
                    : b->size < 2 || b->data[0] != 'M' || b->data[1] != 'Z' ? "it isn't an exe" : nullptr;
    if (bad) {
        snprintf(err, err_size, "%s", bad);
        buf_free(&s_upd.exe);
        return false;
    }
    return true;
}

static void check() {
    HttpBuf json;
    char err[128];
    if (!http_get(UPDATE_API_URL, API_HEADERS, MAX_JSON, &json, err, sizeof(err))) return;   // offline etc.: no offer
    JsonNode* nodes = (JsonNode*)VirtualAlloc(nullptr, (SIZE_T)json.size * sizeof(JsonNode), MEM_RESERVE | MEM_COMMIT,
                                              PAGE_READWRITE);
    JsonDoc doc;
    const bool found = nodes && json_parse(&doc, (const char*)json.data, json.size, nodes, json.size) && read_release(&doc);
    if (nodes) VirtualFree(nodes, 0, MEM_RELEASE);
    buf_free(&json);
    const char* mine = integrity_exe_sha256();
    if (!found || !mine[0]) return;
    if (!s_upd.sha256[0]) {         // no published digest (older releases): download the asset and hash it
        if (!download_asset(err, sizeof(err))) return;
        integrity_sha256(s_upd.exe.data, s_upd.exe.size, s_upd.sha256);
    }
    if (!s_upd.sha256[0] || !strcmp(s_upd.sha256, mine)) {
        buf_free(&s_upd.exe);
        return;
    }
    s_upd.state.store(UPD_AVAILABLE, std::memory_order_release);
}

// Leftovers of a previous update: <exe>.old (the old exe, in use until that process exits) and <exe>.new.
static void cleanup() {
    wchar_t path[MAX_PATH + 8];
    swprintf(path, ARRAY_COUNT(path), L"%s.new", s_upd.self);
    DeleteFileW(path);
    swprintf(path, ARRAY_COUNT(path), L"%s.old", s_upd.self);
    for (u32 i = 0; i < 40 && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES && !DeleteFileW(path); ++i) Sleep(250);
}

static DWORD WINAPI check_thread(void* do_check) {
    cleanup();
    if (do_check) check();
    return 0;
}

void updater_init(bool do_check) {
    GetModuleFileNameW(nullptr, s_upd.self, MAX_PATH);
    if (do_check) integrity_exe_sha256();      // computed once, here on the UI thread (it isn't thread-safe)
    if (HANDLE t = CreateThread(nullptr, 0, check_thread, do_check ? (void*)1 : nullptr, 0, nullptr)) CloseHandle(t);
}

UpdateState updater_state() { return s_upd.state.load(std::memory_order_acquire); }
const UpdateInfo* updater_info() { return &s_upd.info; }
const char* updater_error() { return s_upd.error; }

f32 updater_progress() {
    const u32 total = s_upd.total.load(std::memory_order_relaxed);
    const u32 got = s_upd.got.load(std::memory_order_relaxed);
    return total ? (f32)got / (f32)total : 0.0f;
}

static DWORD fail(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_upd.error, sizeof(s_upd.error), fmt, args);
    va_end(args);
    buf_free(&s_upd.exe);
    s_upd.state.store(UPD_FAILED, std::memory_order_release);
    return 0;
}

static DWORD write_file(const wchar_t* path, const void* data, u32 size) {     // 0 or the Win32 error
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD written = 0, e = 0;
    if (!WriteFile(f, data, size, &written, nullptr) || !FlushFileBuffers(f)) e = GetLastError();
    else if (written != size) e = ERROR_WRITE_FAULT;
    CloseHandle(f);
    if (e) DeleteFileW(path);
    return e;
}

static DWORD WINAPI install_thread(void*) {
    char err[128];
    if (!s_upd.exe.data && !download_asset(err, sizeof(err))) return fail("Download failed: %s.", err);
    char hex[65];
    integrity_sha256(s_upd.exe.data, s_upd.exe.size, hex);
    if (strcmp(hex, s_upd.sha256) != 0) return fail("The download doesn't match the release's SHA-256. Nothing was changed.");

    // A running exe can be renamed but not overwritten: move it aside, put the new one in its place.
    wchar_t new_path[MAX_PATH + 8], old_path[MAX_PATH + 8];
    swprintf(new_path, ARRAY_COUNT(new_path), L"%s.new", s_upd.self);
    swprintf(old_path, ARRAY_COUNT(old_path), L"%s.old", s_upd.self);
    if (const DWORD e = write_file(new_path, s_upd.exe.data, s_upd.exe.size))
        return fail("Couldn't write next to the exe (error %lu). Is its folder read-only?", e);
    DeleteFileW(old_path);
    if (!MoveFileExW(s_upd.self, old_path, MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = GetLastError();
        DeleteFileW(new_path);
        return fail("Couldn't rename the running exe (error %lu). Nothing was changed.", e);
    }
    if (!MoveFileExW(new_path, s_upd.self, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        MoveFileExW(old_path, s_upd.self, MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(new_path);
        return fail("Couldn't put the new exe in place (error %lu). The old one was restored.", e);
    }
    buf_free(&s_upd.exe);
    s_upd.state.store(UPD_READY, std::memory_order_release);
    return 0;
}

void updater_install() {
    const UpdateState st = updater_state();
    if (st != UPD_AVAILABLE && st != UPD_FAILED) return;
    s_upd.error[0] = 0;
    s_upd.got.store(0, std::memory_order_relaxed);
    s_upd.total.store((u32)s_upd.info.size, std::memory_order_relaxed);
    s_upd.state.store(UPD_DOWNLOADING, std::memory_order_release);
    if (HANDLE t = CreateThread(nullptr, 0, install_thread, nullptr, 0, nullptr)) CloseHandle(t);
    else fail("Couldn't start the download (error %lu).", GetLastError());
}

void updater_relaunch() {
    if (updater_state() != UPD_READY) return;
    // Same elevation as now: an elevated process starts the new one elevated; otherwise don't ask again.
    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, ARRAY_COUNT(cmd), L"\"%s\"%s", s_upd.self, is_elevated() ? L"" : L" --no-elevate");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(s_upd.self, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}
