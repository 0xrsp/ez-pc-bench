#pragma once
// PawnIO client: plain DeviceIoControl calls to the signed PawnIO driver (https://pawnio.eu), no PawnIOLib.
// The driver runs small signed modules (PawnIO.Modules, LGPL-2.1) and keeps one module per open handle; a
// module exposes named functions that take and return arrays of 64-bit cells.
//
// Interface from PawnIO's own sources (PawnIO/include/pawnio_um.h, PawnIOLib/PawnIOLib.cpp, PawnIO/src/driver.cpp,
// 2.2.0): device \Device\PawnIO, device type 41394, METHOD_BUFFERED IOCTLs 0x821 (load a module blob) and 0x841
// (execute: 32-byte zero-padded function name, then the input cells; output = the result cells). The device ACL
// is SYSTEM + Administrators. LibreHardwareMonitor talks to it the same way (PawnIo/PawnIo.cs).
//
// UNTESTED against the real driver: PawnIO was not installed on the development machine. Everything here fails
// closed (no device / no module -> "not available") and only reads; see docs/notes/pawnio.md.
#include "core/types.h"

// The signed modules we use. Blobs come from the exe's RCDATA resources "PAWNIO_<NAME>" (CMake embeds
// third_party/pawnio/*.bin when present) or from "<exe folder>\pawnio\<Name>.bin".
enum PawnModuleId : u8 { PM_AMD17, PM_RYZEN_SMU, PM_INTEL_MSR, PM_SMBUS_PIIX4, PM_SMBUS_I801, PM_COUNT };

struct Pawn {
    void* h;            // device handle with the module loaded; null = not open
    void* lock;         // SRWLOCK: calls on one handle are serialized
};

enum PawnDetect : u8 {
    PAWN_ABSENT,        // no driver installed
    PAWN_NOT_RUNNING,   // installed (uninstall entry) but the device isn't there (driver not started / reboot pending)
    PAWN_NO_ACCESS,     // device exists but only opens for administrators
    PAWN_PRESENT,
};

// Uninstall-entry version ("2.2.0", "" if unknown) and a probe open of the device.
PawnDetect pawnio_detect(char* version, u32 size);
const char* pawnio_module_name(PawnModuleId id);
bool pawnio_module_available(PawnModuleId id);      // blob found (resource or file)
// Opens the device and loads the module. 0 on success, else a Win32 error (ERROR_FILE_NOT_FOUND = blob missing,
// ERROR_NOT_SUPPORTED = the module rejected this CPU / chipset, ...).
u32 pawnio_open(Pawn* p, PawnModuleId id);
void pawnio_close(Pawn* p);
// Calls a module function. 0 on success, else a Win32 error (the driver's NTSTATUS mapped by Windows).
u32 pawnio_call(Pawn* p, const char* fn, const u64* in, u32 in_count, u64* out, u32 out_count);

// Named mutexes other hardware tools (LibreHardwareMonitor, HWiNFO, FanControl, OpenRGB, ...) take around shared
// buses, so two programs never interleave an index/data or SMBus transaction.
enum BusLock : u8 { LOCK_PCI, LOCK_SMBUS, LOCK_COUNT };
bool bus_lock(BusLock l, u32 timeout_ms);       // false = busy (skip this sample)
void bus_unlock(BusLock l);
