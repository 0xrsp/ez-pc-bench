// Disk verify test (STAB_DISK): write a temporary file with known data (unbuffered + write-through, so it goes to
// the drive and not just the cache), read it back unbuffered and compare every word. Each pass uses new data.
// Errors are counted per mismatching 4 KiB page. The file is opened with FILE_FLAG_DELETE_ON_CLOSE, so even a
// killed process leaves nothing behind.
#include "stability/stab_internal.h"
#include "core/timer.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

constexpr u64 BLOCK = 1 * MiB;
constexpr u64 PAGE_WORDS = 4096 / 8;

static void fill_block(u64* w, u64 pass, u64 block) {
    u64 x = stab_mix(pass * 0x100000 + block) | 1;
    for (u64 i = 0; i < BLOCK / 8; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; w[i] = x; }
    w[0] = 0x455A42535441424Bull ^ block;      // "EZBSTABK" ^ block: a misplaced block shows up as a header error
    w[1] = pass;
}

// Picks the file location: %TEMP% when it is on the target drive, else <drive>:\ezbench\ (created; removed after).
static bool pick_path(char drive, wchar_t* path, wchar_t* dir, bool* made_dir) {
    *made_dir = false;
    dir[0] = 0;
    wchar_t temp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n && towupper(temp[0]) == (wchar_t)drive) {
        swprintf(path, MAX_PATH, L"%sezbench_stability.tmp", temp);
        return true;
    }
    swprintf(dir, MAX_PATH, L"%c:\\ezbench", drive);
    if (CreateDirectoryW(dir, nullptr)) *made_dir = true;
    else if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    swprintf(path, MAX_PATH, L"%s\\stability.tmp", dir);
    return true;
}

static bool io_at(HANDLE h, bool write, u64 offset, void* buf, u64 bytes) {
    OVERLAPPED ov = {};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    DWORD done = 0;
    const BOOL ok = write ? WriteFile(h, buf, (DWORD)bytes, &done, &ov) : ReadFile(h, buf, (DWORD)bytes, &done, &ov);
    return ok && done == bytes;
}

void stab_disk_run(f64 deadline) {
    StabTestState* ts = &g_stab.tests[STAB_DISK];
    const char drive = (char)towupper((wchar_t)g_stab.cfg.disk_drive);
    u64 bytes = g_stab.cfg.disk_bytes ? g_stab.cfg.disk_bytes : 256 * MiB;
    bytes = (bytes + BLOCK - 1) / BLOCK * BLOCK;
    wchar_t root[4] = { (wchar_t)drive, L':', L'\\', 0 };
    ULARGE_INTEGER free_bytes = {};
    if (!GetDiskFreeSpaceExW(root, &free_bytes, nullptr, nullptr)) {
        stab_fail_setup(STAB_DISK, "drive %c: not found", drive);
        return;
    }
    if (free_bytes.QuadPart < bytes + 1 * GiB) {
        stab_fail_setup(STAB_DISK, "not enough free space on %c: (needs %llu MiB + 1 GiB)", drive, bytes / MiB);
        return;
    }
    wchar_t path[MAX_PATH], dir[MAX_PATH];
    bool made_dir = false;
    if (!pick_path(drive, path, dir, &made_dir)) {
        stab_fail_setup(STAB_DISK, "cannot create a folder on %c: (error %lu)", drive, GetLastError());
        return;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        stab_fail_setup(STAB_DISK, "cannot create the test file on %c: (error %lu)", drive, GetLastError());
        if (made_dir) RemoveDirectoryW(dir);
        return;
    }
    u64* wbuf = (u64*)VirtualAlloc(nullptr, BLOCK * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    u64* rbuf = wbuf ? wbuf + BLOCK / 8 : nullptr;
    const u64 blocks = bytes / BLOCK;
    ts->size_bytes = bytes;
    ts->chunk_count = blocks < STAB_MAX_CHUNKS ? (u32)blocks : STAB_MAX_CHUNKS;
    ts->chunk_bytes = (blocks + ts->chunk_count - 1) / ts->chunk_count * BLOCK;
    snprintf(ts->note, sizeof(ts->note), "%llu MiB file on %c:, unbuffered, write-through; errors counted per 4 KiB page",
             bytes / MiB, drive);
    bool injected = false;

    for (u64 pass = 0; wbuf && !stab_done(deadline); ++pass) {
        stab_phase(STAB_DISK, "Writing");
        for (u64 b = 0; b < blocks && !stab_done(deadline); ++b) {
            fill_block(wbuf, pass, b);
            if (!io_at(h, true, b * BLOCK, wbuf, BLOCK)) {
                stab_error(STAB_DISK, -1, (i32)(b * BLOCK / ts->chunk_bytes), b * BLOCK, "write failed", 0, GetLastError());
                break;
            }
        }
        if (g_stab.cfg.inject && !injected && blocks > 1) {     // dev self-test: corrupt one bit of block 1 on disk
            injected = true;
            fill_block(wbuf, pass, 1);
            wbuf[1000] ^= 1ull << 5;
            io_at(h, true, BLOCK, wbuf, BLOCK);
        }
        FlushFileBuffers(h);
        if (stab_done(deadline)) break;
        stab_phase(STAB_DISK, "Reading back");
        for (u64 b = 0; b < blocks && !stab_done(deadline); ++b) {
            const i32 chunk = (i32)(b * BLOCK / ts->chunk_bytes);
            if (!io_at(h, false, b * BLOCK, rbuf, BLOCK)) {
                stab_error(STAB_DISK, -1, chunk, b * BLOCK, "read failed", 0, GetLastError());
                continue;
            }
            fill_block(wbuf, pass, b);
            if (memcmp(wbuf, rbuf, BLOCK) != 0)
                for (u64 p = 0; p < BLOCK / 8; p += PAGE_WORDS)
                    for (u64 i = p; i < p + PAGE_WORDS; ++i)
                        if (rbuf[i] != wbuf[i]) {
                            stab_error(STAB_DISK, -1, chunk, b * BLOCK + i * 8, "read-back mismatch", wbuf[i], rbuf[i]);
                            break;      // one error per page
                        }
            stab_work(STAB_DISK, (f64)BLOCK / 1e9);
        }
    }
    if (wbuf) VirtualFree(wbuf, 0, MEM_RELEASE);
    CloseHandle(h);     // deletes the file
    if (made_dir) RemoveDirectoryW(dir);
}
