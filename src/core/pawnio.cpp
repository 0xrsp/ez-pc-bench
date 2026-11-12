// PawnIO client (see pawnio.h). Read-only use is up to the callers: this file only moves bytes.
#include "core/pawnio.h"
#include "core/pawnio_modules.h"
#include <windows.h>
#include <winioctl.h>
#include <sddl.h>
#include <stdio.h>
#include <string.h>

// pawnio_um.h (PawnIO 2.x): k_device_type = 41394, all IOCTLs METHOD_BUFFERED / FILE_ANY_ACCESS.
constexpr DWORD PIO_DEVICE_TYPE = 41394;
constexpr DWORD IOCTL_PIO_LOAD_BINARY = CTL_CODE(PIO_DEVICE_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD IOCTL_PIO_EXECUTE_FN = CTL_CODE(PIO_DEVICE_TYPE, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr u32 PIO_FN_NAME = 32;            // function name field, zero padded
constexpr u32 PIO_MAX_IN = 16;             // input cells we ever send
static const wchar_t* const PIO_DEVICE = L"\\\\?\\GLOBALROOT\\Device\\PawnIO";

static const char* const s_module_names[PM_COUNT] = { "AMDFamily17", "RyzenSMU", "IntelMSR", "SmbusPIIX4", "SmbusI801" };

const char* pawnio_module_name(PawnModuleId id) { return id < PM_COUNT ? s_module_names[id] : "?"; }

PawnDetect pawnio_detect(char* version, u32 size) {
    if (size) version[0] = 0;
    wchar_t v[32] = {};
    DWORD bytes = sizeof(v);
    const bool installed = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
                                        L"DisplayVersion", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, v, &bytes) == ERROR_SUCCESS;
    if (installed && size) WideCharToMultiByte(CP_UTF8, 0, v, -1, version, (int)size, nullptr, nullptr);
    HANDLE h = CreateFileW(PIO_DEVICE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return PAWN_PRESENT;
    }
    if (GetLastError() == ERROR_ACCESS_DENIED) return PAWN_NO_ACCESS;
    return installed ? PAWN_NOT_RUNNING : PAWN_ABSENT;
}

// ---- module blobs ---------------------------------------------------------------------------------------

struct Blob {
    const void* data;
    u32 size;
    void* heap;         // owned copy (file), freed by blob_free
};

static bool blob_from_file(PawnModuleId id, Blob* b) {
    wchar_t path[MAX_PATH];
    if (!pawnio_module_path(s_module_names[id], path, MAX_PATH)) return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz = {};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < 4 * (i64)MiB;
    void* mem = ok ? HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sz.QuadPart) : nullptr;
    DWORD got = 0;
    ok = mem && ReadFile(f, mem, (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart;
    CloseHandle(f);
    if (!ok) {
        if (mem) HeapFree(GetProcessHeap(), 0, mem);
        return false;
    }
    *b = { mem, got, mem };
    return true;
}

static void blob_free(Blob* b) {
    if (b->heap) HeapFree(GetProcessHeap(), 0, b->heap);
    b->heap = nullptr;
}

bool pawnio_module_available(PawnModuleId id) {
    wchar_t path[MAX_PATH];
    return id < PM_COUNT && pawnio_module_path(s_module_names[id], path, MAX_PATH);
}

// ---- device -----------------------------------------------------------------------------------------------

u32 pawnio_open(Pawn* p, PawnModuleId id) {
    pawnio_close(p);
    if (id >= PM_COUNT) return ERROR_INVALID_PARAMETER;
    Blob b = {};
    if (!blob_from_file(id, &b)) return ERROR_FILE_NOT_FOUND;
    HANDLE h = CreateFileW(PIO_DEVICE, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const u32 err = GetLastError();
        blob_free(&b);
        return err ? err : ERROR_DEVICE_NOT_AVAILABLE;
    }
    DWORD ret = 0;
    // The driver checks the module's signature (official build) and runs its main(), which refuses hardware it
    // doesn't support (STATUS_NOT_SUPPORTED -> ERROR_NOT_SUPPORTED).
    const BOOL ok = DeviceIoControl(h, IOCTL_PIO_LOAD_BINARY, (void*)b.data, b.size, nullptr, 0, &ret, nullptr);
    const u32 err = ok ? 0 : GetLastError();
    blob_free(&b);
    if (!ok) {
        CloseHandle(h);
        return err ? err : ERROR_GEN_FAILURE;
    }
    p->h = h;
    p->lock = nullptr;      // SRWLOCK_INIT
    return 0;
}

void pawnio_close(Pawn* p) {
    if (p->h) CloseHandle((HANDLE)p->h);   // the driver unloads the module when the handle closes
    p->h = nullptr;
}

u32 pawnio_call(Pawn* p, const char* fn, const u64* in, u32 in_count, u64* out, u32 out_count) {
    if (!p->h) return ERROR_INVALID_HANDLE;
    if (strlen(fn) >= PIO_FN_NAME || in_count > PIO_MAX_IN) return ERROR_INVALID_PARAMETER;
    alignas(8) u8 buf[PIO_FN_NAME + PIO_MAX_IN * sizeof(u64)] = {};
    memcpy(buf, fn, strlen(fn));
    if (in_count) memcpy(buf + PIO_FN_NAME, in, in_count * sizeof(u64));
    DWORD ret = 0;
    AcquireSRWLockExclusive((PSRWLOCK)&p->lock);
    const BOOL ok = DeviceIoControl((HANDLE)p->h, IOCTL_PIO_EXECUTE_FN, buf, PIO_FN_NAME + in_count * (u32)sizeof(u64), out,
                                    out_count * (u32)sizeof(u64), &ret, nullptr);
    const u32 err = ok ? 0 : GetLastError();
    ReleaseSRWLockExclusive((PSRWLOCK)&p->lock);
    return ok ? 0 : (err ? err : ERROR_GEN_FAILURE);
}

// ---- bus locks ----------------------------------------------------------------------------------------------

static HANDLE s_locks[LOCK_COUNT];
static bool s_lock_tried[LOCK_COUNT];
static const wchar_t* const s_lock_names[LOCK_COUNT] = { L"Global\\Access_PCI", L"Global\\Access_SMBUS.HTP.Method" };

static HANDLE lock_handle(BusLock l) {
    if (s_lock_tried[l]) return s_locks[l];
    s_lock_tried[l] = true;
    // Same as LibreHardwareMonitor (Mutexes.cs): create with access for everyone, or open the existing one.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;
    HANDLE h = CreateMutexW(sd ? &sa : nullptr, FALSE, s_lock_names[l]);
    if (!h) h = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, s_lock_names[l]);
    if (sd) LocalFree(sd);
    s_locks[l] = h;
    return h;
}

// Called from one sampling thread (plus init before it starts), so the lazy creation needs no lock of its own.
bool bus_lock(BusLock l, u32 timeout_ms) {
    HANDLE h = lock_handle(l);
    if (!h) return true;        // no mutex object at all: nothing to coordinate with
    const DWORD w = WaitForSingleObject(h, timeout_ms);
    return w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
}

void bus_unlock(BusLock l) {
    if (s_locks[l]) ReleaseMutex(s_locks[l]);
}
