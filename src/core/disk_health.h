#pragma once
// Drive health for the disk health window and the CLI report (CrystalDiskInfo-style).
//   NVMe: Identify Controller / Namespace, SMART / Health log, error log, firmware slots, self-test log and
//     features through stornvme (no administrator rights needed), or the vendor pass-through of USB NVMe
//     bridges (core/nvme_dev).
//   ATA / SATA: IDENTIFY + SMART attributes / thresholds / self-test log over ATA pass-through, the SMART
//     IOCTLs, SAT or a USB bridge's vendor CDB (core/ata), RAID members via CSMI (core/csmi); all need
//     administrator rights. Without them: IDENTIFY and the Device Statistics log through the storage
//     property query, and IOCTL_STORAGE_PREDICT_FAILURE as the last resort.
// Status Good / Caution / Bad from the drive's own verdicts plus fixed CrystalDiskInfo-style Caution limits.
#include "core/ata.h"
#include "core/nvme_dev.h"
#include "core/physdisk.h"
#include "core/smart_attr.h"

enum HealthStatus : u8 { HEALTH_UNKNOWN, HEALTH_GOOD, HEALTH_CAUTION, HEALTH_BAD };
extern const char* const HEALTH_STATUS_NAMES[4];

enum HealthProto : u8 { HPROTO_NONE, HPROTO_ATA, HPROTO_NVME };

// One row of the SMART table. ATA: a drive attribute. NVMe: a health-log field, numbered like CrystalDiskInfo.
struct SmartRow {
    u8 id;
    bool has_values;            // ATA current / worst / threshold are valid
    u8 current, worst, threshold;
    u16 flags;
    u8 raw[16];                 // little endian
    u8 raw_len;                 // bytes shown as hex
    SmartUnit unit;
    char name[48];
    char decoded[48];
    HealthStatus level;         // row highlight
};

struct SelfTestEntry {
    char kind[24];              // "Short", "Extended", ...
    char result[64];
    u64 hours;                  // power-on hours when it ran
    u64 lba;                    // first failing LBA
    bool lba_valid, failed;
};

struct NvmePowerState {
    f32 max_watts;
    u32 entry_us, exit_us;
    bool non_operational;
};

struct NvmeErrorEntry {
    u64 count, lba;
    u16 sqid, cmdid, status;
    u32 nsid;
};

constexpr u32 SMART_ROWS_MAX = 40;
constexpr u32 SELFTEST_MAX = 21;
constexpr u32 PARTITIONS_MAX = 16;

struct DiskHealth {
    PhysDisk disk;
    DiskLocation loc;
    bool raid_member;                   // found through CSMI, not a \\.\PhysicalDriveN
    u32 csmi_index;

    HealthStatus status;
    char status_text[400];              // reasons, or what's missing
    bool full_access;                   // read/write handle (administrator)
    HealthProto proto;
    char source[96];                    // how the data was read

    // Info panel.
    char model[96], serial[48], firmware[24];
    char interface_name[64], transfer_cur[40], transfer_max[40], standard[64], features[256];
    u32 rotation;                       // 0 unknown, 1 SSD, else rpm
    u64 buffer_bytes;
    u32 logical_sector, physical_sector;
    bool have_power_on_hours, have_power_cycles, have_host_reads, have_host_writes, have_nand_writes;
    u64 power_on_hours, power_cycles, host_reads_bytes, host_writes_bytes, nand_writes_bytes;
    i32 life_left_pct;                  // -1 unknown
    bool have_temp;
    f32 temp_c, warn_c, crit_c;
    DiskPartition parts[PARTITIONS_MAX];
    u32 part_count;
    char part_style[8];

    // SMART table and counters behind the status.
    SmartRow rows[SMART_ROWS_MAX];
    u32 row_count;
    i32 smart_status;                   // ATA SMART RETURN STATUS: 1 = threshold exceeded, 0 = OK, -1 = unknown
    bool have_predict;
    bool predict_failure;
    i64 reallocated, pending, uncorrectable, media_errors;     // -1 = not reported

    // ATA.
    AtaDev ata;
    bool have_ata_id, have_smart, have_thresholds;
    u8 ata_id[512], smart_data[512], smart_thresholds[512];
    AtaIdInfo id;
    AtaSmartCaps caps;
    SmartVendor vendor;
    u32 ata_error_count;

    // NVMe.
    NvmeDev nvme;
    bool have_nvme_id, have_nvme_ns, have_nvme_health, have_nvme_fw;
    u8 nvme_id[4096], nvme_ns[4096], nvme_health[512], nvme_fw[512];
    u32 nvme_version;                   // VER (major << 16 | minor << 8 | tertiary)
    NvmePowerState power_states[32];
    u32 power_state_count;
    NvmeErrorEntry errors[16];
    u32 error_count;
    u32 feature[32];                    // Get Features dword 0, indexed by feature id
    u32 feature_ok;                     // bit per feature id that answered
    u8 apst_table[256];

    // Self-tests (ATA SMART self-test log / NVMe Device Self-test log).
    bool can_self_test, can_short, can_extended, can_conveyance;
    u16 minutes_short, minutes_extended, minutes_conveyance;
    i32 test_progress;                  // 0..100 while a test runs, -1 otherwise
    char test_running[24];
    SelfTestEntry tests[SELFTEST_MAX];
    u32 test_count;
};

// Reads every physical disk (and RAID members when a RAID volume is present). Synchronous: a few IOCTLs per
// drive (milliseconds; longer for USB bridges); don't call during disk tests.
u32 disk_health_scan(DiskHealth* out, u32 max);

enum SelfTestKind : u8 { SELFTEST_SHORT, SELFTEST_EXTENDED, SELFTEST_CONVEYANCE, SELFTEST_ABORT };
// Starts / aborts a self-test. Only from an explicit user action. False with `err` set on failure.
bool disk_selftest_start(DiskHealth* h, SelfTestKind kind, char* err, u32 err_size);
// Refreshes test progress and the self-test log (cheap; poll every few seconds while a test runs).
void disk_selftest_poll(DiskHealth* h);

// Plain-text report of the drives (CrystalDiskInfo "Copy" / "Save as text"). Returns the length.
u32 disk_health_report(const DiskHealth* drives, u32 count, bool hide_serial, char* out, u32 size);
void disk_health_close();   // releases RAID controller handles kept for self-tests
