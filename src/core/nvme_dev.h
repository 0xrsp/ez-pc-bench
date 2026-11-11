#pragma once
// NVMe admin queries: Identify, Get Log Page, Get Features and Device Self-test. Through Windows (stornvme:
// IOCTL_STORAGE_QUERY_PROPERTY, which answers on a query-only handle without administrator rights) or through
// the vendor pass-through of USB NVMe bridges (JMicron JMS583, ASMedia ASM2362, Realtek RTL9210; CDB layouts
// as in smartmontools' scsinvme.cpp; read/write handle, untested here).
#include "core/types.h"

typedef void* HANDLE;

enum NvmeTransport : u8 { NVME_TR_NONE, NVME_TR_WINDOWS, NVME_TR_JMICRON, NVME_TR_ASMEDIA, NVME_TR_REALTEK, NVME_TR_COUNT };
extern const char* const NVME_TRANSPORT_NAMES[NVME_TR_COUNT];

struct NvmeDev {
    HANDLE h;               // query-only handle (Windows queries)
    HANDLE rw;              // read/write handle (bridges, self-test); may be INVALID_HANDLE_VALUE / null
    NvmeTransport tr;
};

bool nvme_identify(NvmeDev* d, u32 cns, u32 nsid, u8 out[4096]);
bool nvme_get_log(NvmeDev* d, u8 lid, void* out, u32 size);
// Get Features: `dw0` = completion dword 0; `data` (may be null) for features that return a data buffer.
bool nvme_get_feature(NvmeDev* d, u8 fid, u32* dw0, void* data, u32 size);
// Device Self-test: code 1 = short, 2 = extended, 0xF = abort. Windows needs the read/write handle
// (administrator); of the bridges only JMicron passes non-data commands. `err` gets a reason on failure.
bool nvme_self_test(NvmeDev* d, u8 code, char* err, u32 err_size);

// USB bridge whose vendor pass-through reaches an NVMe drive (by USB vendor / product id).
NvmeTransport nvme_bridge_kind(u16 usb_vid, u16 usb_pid);
// Tries the bridge's pass-through with Identify Controller; fills `id` on success.
bool nvme_attach_bridge(NvmeDev* d, HANDLE rw, NvmeTransport kind, u8 id[4096]);
