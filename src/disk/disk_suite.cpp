// Disk suite setup: one test file per selected drive (or in the chosen folder), filled once with the test data
// before any timed test. Also the small helpers every disk test file shares.
#include "disk/disk_internal.h"
#include "core/elevation.h"
#include "core/physdisk.h"
#include "core/runner.h"
#include "core/sysinfo.h"
#include "core/timer.h"
#include <winioctl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

DiskSuite g_disk;

constexpr u64 FILL_CHUNK = 1 * MiB;
constexpr u64 FREE_MARGIN = 64 * MiB;    // keep this much free on the volume after creating the file

// ---- small helpers ---------------------------------------------------------------------------------

u64 rng_next(u64* s) {     // xorshift64*
    u64 x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

void fill_pattern(u8* buf, u64 bytes, u32 zero_pct, u64 seed) {
    u64 s = seed | 1;
    const u64 zero = 4096 * (zero_pct > 100 ? 100 : zero_pct) / 100 / 8 * 8;
    for (u64 base = 0; base < bytes; base += 4096) {
        const u64 n = bytes - base < 4096 ? bytes - base : 4096;
        const u64 z = zero < n ? zero : n;
        memset(buf + base, 0, z);
        u64* p = (u64*)(buf + base + z);
        for (u64 i = 0; i < (n - z) / 8; ++i) p[i] = rng_next(&s);
    }
}

void* disk_alloc(u64 bytes) { return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
void disk_free(void* p) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

bool cancelled(const CancelFlag* cancel) { return cancel->load(std::memory_order_relaxed); }

void pause_ms(u32 ms, const CancelFlag* cancel) {
    for (u32 t = 0; t < ms && !cancelled(cancel); t += 20) Sleep(20);
}

// Interned names: a static pool + open-addressing hash, so names repeated by later runs don't grow it.
static char s_pool[256 * 1024];
static u32 s_pool_used;
static const char* s_names[8192];

const char* dname(const char* fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    u32 h = 2166136261u;
    for (const char* c = buf; *c; ++c) h = (h ^ (u8)*c) * 16777619u;
    for (u32 i = 0; i < ARRAY_COUNT(s_names); ++i) {
        const u32 k = (h + i) & (ARRAY_COUNT(s_names) - 1);
        if (s_names[k] && !strcmp(s_names[k], buf)) return s_names[k];
        if (s_names[k]) continue;
        const u32 n = (u32)strlen(buf) + 1;
        if (s_pool_used + n > sizeof(s_pool) || i > ARRAY_COUNT(s_names) / 2) return "?";
        char* s = s_pool + s_pool_used;
        memcpy(s, buf, n);
        s_pool_used += n;
        s_names[k] = s;
        return s;
    }
    return "?";
}

bool disk_begin_test(const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    if (g_disk.count == 0) { res_fail(out, "no test file"); return false; }
    if (g_disk.tests_started++ && p->disk.interval_ms) {
        live_phase(live, "Interval (drive rests)", "");
        pause_ms(p->disk.interval_ms, cancel);
    }
    return !cancelled(cancel);
}

bool disk_temperature(const DiskTarget* t, f32* temp_c) {
    if (t->phys_disk < 0) return false;
    HANDLE h = physdisk_open((u32)t->phys_disk, false);
    if (h == INVALID_HANDLE_VALUE) return false;
    f32 warn = 0.0f;
    const bool ok = physdisk_temperature(h, temp_c, &warn);
    CloseHandle(h);
    return ok;
}

bool disk_work_path(const DiskTarget* t, const wchar_t* name, wchar_t* out) {
    if (!t->work_dir[0]) return false;
    CreateDirectoryW(t->work_dir, nullptr);
    return swprintf(out, MAX_PATH, L"%s\\%s", t->work_dir, name) > 0;
}

// Only ever deletes below a folder whose own name starts with "ezb_" (our work folders).
void disk_delete_tree(const wchar_t* dir) {
    const wchar_t* name = wcsrchr(dir, L'\\');
    if (!name || wcsncmp(name + 1, L"ezb_", 4) != 0) return;
    wchar_t pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pattern, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t path[MAX_PATH];
            if (swprintf(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName) < 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                disk_delete_tree(path);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path);
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir);
}

// ---- targets -----------------------------------------------------------------------------------------
// Test file per drive: <drive>:\ezbench.tmp when elevated (like CrystalDiskMark), else <drive>:\ezbench\ezbench.tmp
// (users may create folders but not files in a drive root), else %TEMP% when it is on the same drive.
// Extra files of other tests go to an ezb_work folder next to it.

static void query_device(DiskTarget* t) {
    t->phys_disk = -1;
    if (!t->letter) return;
    const wchar_t vol[8] = { L'\\', L'\\', L'.', L'\\', t->letter, L':', 0 };
    HANDLE v = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (v == INVALID_HANDLE_VALUE) return;
    STORAGE_DEVICE_NUMBER dn = {};
    DWORD got = 0;
    if (DeviceIoControl(v, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &dn, sizeof(dn), &got, nullptr) &&
        dn.DeviceType == FILE_DEVICE_DISK)
        t->phys_disk = (i32)dn.DeviceNumber;
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR sp = {};
    if (DeviceIoControl(v, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &sp, sizeof(sp), &got, nullptr) && got >= sizeof(sp))
        t->rotational = sp.IncursSeekPenalty != 0;
    CloseHandle(v);
}

// Tries one candidate location: deletes a stale file, checks free space, creates the file.
static bool try_create(DiskTarget* t, const wchar_t* dir, const wchar_t* path, char* error, u32 error_size) {
    DeleteFileW(path);      // stale file from a crashed run
    ULARGE_INTEGER free_user;
    if (GetDiskFreeSpaceExW(dir, &free_user, nullptr, nullptr) && free_user.QuadPart < g_disk.file_bytes + FREE_MARGIN) {
        snprintf(error, error_size, "not enough free space (%llu MiB needed, %llu MiB free)",
                 (g_disk.file_bytes + FREE_MARGIN) / MiB, free_user.QuadPart / MiB);
        return false;
    }
    const DWORD flags = FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_DELETE_ON_CLOSE |
                        (g_disk.buffered ? 0 : FILE_FLAG_NO_BUFFERING);
    // No FILE_SHARE_DELETE: another instance's stale-file cleanup (DeleteFileW, which unlinks open files right away
    // with POSIX delete semantics) then fails instead of pulling the file and its folder out from under this run.
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           CREATE_ALWAYS, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(error, error_size, "cannot create test file (error %lu)", GetLastError());
        return false;
    }
    t->hold = h;
    wcscpy_s(t->path, path);
    return true;
}

// Writes the whole file sequentially with the test data (synchronous).
static bool fill_file(DiskTarget* t, char* error, u32 error_size) {
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG)g_disk.file_bytes;
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(t->hold, size, nullptr, FILE_BEGIN) || !SetEndOfFile(t->hold) ||
        !SetFilePointerEx(t->hold, zero, nullptr, FILE_BEGIN)) {
        snprintf(error, error_size, "cannot size test file (error %lu)", GetLastError());
        return false;
    }
    u8* buf = (u8*)disk_alloc(FILL_CHUNK);
    if (!buf) { snprintf(error, error_size, "out of memory"); return false; }

    live_phase(&g_live, dname("%s preparing test file", t->label), "MB/s");
    const f64 t0 = timer_now();
    f64 last = t0;
    bool ok = true;
    for (u64 off = 0; off < g_disk.file_bytes; off += FILL_CHUNK) {
        fill_pattern(buf, FILL_CHUNK, 0, off * 0x9E3779B97F4A7C15ull + 12345);
        DWORD written = 0;
        if (!WriteFile(t->hold, buf, (DWORD)FILL_CHUNK, &written, nullptr) || written != FILL_CHUNK) {
            snprintf(error, error_size, "writing test file failed (error %lu)", GetLastError());
            ok = false;
            break;
        }
        const f64 now = timer_now();
        if (now - last > 0.1) {
            live_sample(&g_live, (off + FILL_CHUNK) / (now - t0) / 1e6);
            live_progress(&g_live, (f32)((f64)(off + FILL_CHUNK) / (f64)g_disk.file_bytes));
            last = now;
        }
    }
    disk_free(buf);
    if (ok && !FlushFileBuffers(t->hold)) {
        snprintf(error, error_size, "flushing test file failed (error %lu)", GetLastError());
        ok = false;
    }
    return ok;
}

static void close_target(DiskTarget* t) {
    if (t->hold && t->hold != INVALID_HANDLE_VALUE) CloseHandle(t->hold);   // DELETE_ON_CLOSE removes the file
    if (t->path[0]) DeleteFileW(t->path);
    if (t->work_dir[0]) disk_delete_tree(t->work_dir);
    if (t->made_dir[0]) RemoveDirectoryW(t->made_dir);     // only succeeds when empty
    memset(t, 0, sizeof(*t));
}

void disk_shutdown(void*) {
    for (u32 i = 0; i < g_disk.count; ++i) close_target(&g_disk.targets[i]);
    memset(&g_disk, 0, sizeof(g_disk));
}

static void set_work_dir(DiskTarget* t, const wchar_t* base) {
    // base ends with a backslash or not; normalize.
    const size_t n = wcslen(base);
    swprintf(t->work_dir, MAX_PATH, n && base[n - 1] == L'\\' ? L"%sezb_work" : L"%s\\ezb_work", base);
    disk_delete_tree(t->work_dir);      // leftovers of a crashed run
}

static bool open_drive_target(DiskTarget* t, wchar_t drive, char* error, u32 error_size) {
    t->letter = drive;
    snprintf(t->label, sizeof(t->label), "%c:", (char)drive);
    DriveInfo info;
    if (!drive_query(drive, &info)) { snprintf(error, error_size, "drive not found"); return false; }
    t->logical_sector = info.logical_sector ? info.logical_sector : 512;
    t->sector = info.physical_sector > info.logical_sector ? info.physical_sector : info.logical_sector;
    if (t->sector == 0) t->sector = 4096;

    const wchar_t root[4] = { drive, L':', L'\\', 0 };
    wchar_t dir[MAX_PATH], sub_path[MAX_PATH], path[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%sezbench", root);
    swprintf(sub_path, MAX_PATH, L"%s\\ezbench.tmp", dir);
    // Leftovers of a crashed run: the file, the work folder, then the folder (only removed when empty).
    DeleteFileW(sub_path);
    wchar_t old_work[MAX_PATH];
    swprintf(old_work, MAX_PATH, L"%s\\ezb_work", dir);
    disk_delete_tree(old_work);
    RemoveDirectoryW(dir);

    bool created = false;
    const bool have_dir = CreateDirectoryW(dir, nullptr) ? (created = true) : GetLastError() == ERROR_ALREADY_EXISTS;
    if (created) wcscpy_s(t->made_dir, dir);
    if (have_dir) set_work_dir(t, dir);
    if (is_elevated()) {
        swprintf(path, MAX_PATH, L"%sezbench.tmp", root);
        if (try_create(t, root, path, error, error_size)) return fill_file(t, error, error_size);
    }
    if (have_dir && try_create(t, dir, sub_path, error, error_size)) return fill_file(t, error, error_size);
    wchar_t temp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n > 0 && n < MAX_PATH - 16 && towupper(temp[0]) == towupper(drive)) {
        swprintf(path, MAX_PATH, L"%sezbench.tmp", temp);
        if (!have_dir) set_work_dir(t, temp);
        if (try_create(t, temp, path, error, error_size)) return fill_file(t, error, error_size);
    }
    if (!error[0]) snprintf(error, error_size, "no writable location");
    return false;
}

static bool open_folder_target(DiskTarget* t, const wchar_t* folder, char* error, u32 error_size) {
    snprintf(t->label, sizeof(t->label), "Folder");
    if (GetFileAttributesW(folder) == INVALID_FILE_ATTRIBUTES) {
        snprintf(error, error_size, "folder not found (error %lu)", GetLastError());
        return false;
    }
    wchar_t volume[MAX_PATH] = {};
    if (GetVolumePathNameW(folder, volume, MAX_PATH) && volume[1] == L':') t->letter = towupper(volume[0]);
    DWORD spc = 0, bps = 0, nfree = 0, total = 0;
    t->logical_sector = GetDiskFreeSpaceW(volume[0] ? volume : nullptr, &spc, &bps, &nfree, &total) && bps ? bps : 512;
    t->sector = 4096;
    DriveInfo info;
    if (t->letter && drive_query(t->letter, &info) && info.physical_sector > t->sector) t->sector = info.physical_sector;
    if (t->logical_sector > t->sector) t->sector = t->logical_sector;
    set_work_dir(t, folder);
    wchar_t path[MAX_PATH];
    const size_t n = wcslen(folder);
    swprintf(path, MAX_PATH, n && folder[n - 1] == L'\\' ? L"%sezbench.tmp" : L"%s\\ezbench.tmp", folder);
    if (try_create(t, folder, path, error, error_size)) return fill_file(t, error, error_size);
    return false;
}

bool disk_init(const BenchParams* p, void** state, char* error, u32 error_size) {
    memset(&g_disk, 0, sizeof(g_disk));
    *state = &g_disk;
    const bool folder = p->disk.folder[0] != 0;
    if (!folder && (p->disk_drives & ((1u << 26) - 1)) == 0) { snprintf(error, error_size, "no drive selected"); return false; }
    const u64 bytes = p->disk_file_bytes / MiB * MiB;
    g_disk.file_bytes = bytes < 16 * MiB ? 16 * MiB : bytes;
    g_disk.write_through = p->disk_write_through;
    g_disk.buffered = p->disk.buffered;

    for (u32 d = 0; d < DISK_MAX_TARGETS; ++d) {
        if (folder ? d != DISK_FOLDER_SLOT : d == DISK_FOLDER_SLOT || !((p->disk_drives >> d) & 1)) continue;
        DiskTarget* t = &g_disk.targets[g_disk.count++];
        t->slot = d;
        char why[96] = {};
        const bool ok = folder ? open_folder_target(t, p->disk.folder, why, sizeof(why))
                               : open_drive_target(t, (wchar_t)(L'A' + d), why, sizeof(why));
        if (!ok) {
            snprintf(error, error_size, "%s %s", t->label, why);
            disk_shutdown(nullptr);     // the runner only calls shutdown after a successful init
            return false;
        }
        query_device(t);
    }
    return true;
}
