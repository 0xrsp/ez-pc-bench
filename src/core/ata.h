#pragma once
// ATA commands for SATA / PATA drives and SATA drives behind USB bridges or RAID controllers: IDENTIFY DEVICE,
// SMART (READ DATA / THRESHOLDS / LOG, RETURN STATUS, EXECUTE OFF-LINE for self-tests) and SET FEATURES (AAM / APM).
// Every transport except the query-property ones needs a read/write handle, i.e. administrator rights.
#include "core/types.h"

typedef void* HANDLE;

enum AtaTransport : u8 {
    ATA_TR_NONE,
    ATA_TR_ATA_PT,          // IOCTL_ATA_PASS_THROUGH (AHCI / storahci, IDE)
    ATA_TR_SMART_IOCTL,     // SMART_RCV_DRIVE_DATA / SMART_SEND_DRIVE_COMMAND (legacy; IDENTIFY and SMART only)
    ATA_TR_SAT16,           // SCSI / ATA Translation, ATA PASS-THROUGH (16): most USB bridges (ASMedia, Realtek,
                            //   JMicron JMS56x/57x, VIA, Genesys), SAS HBAs, UAS
    ATA_TR_SAT12,           // ATA PASS-THROUGH (12): older bridges that reject the 16-byte CDB
    ATA_TR_JMICRON,         // JMicron JM20329 / JM20336 / JM20337 / JM20339 vendor CDB 0xDF
    ATA_TR_PROLIFIC,        // Prolific PL2571 / PL2773 / PL3507 (JMicron-style CDB with a 2-byte suffix)
    ATA_TR_SUNPLUS,         // Sunplus SPIF215 / SPIF225 vendor CDB 0xF8
    ATA_TR_CYPRESS,         // Cypress CY7C68300 ATACB vendor CDB 0x24
    ATA_TR_CSMI,            // RAID controller (Intel RST, AMD) via CSMI STP pass-through (core/csmi.h)
    ATA_TR_COUNT
};
extern const char* const ATA_TRANSPORT_NAMES[ATA_TR_COUNT];

struct AtaDev {
    HANDLE h;               // the disk (or \\.\ScsiN: for CSMI)
    AtaTransport tr;
    u8 port;                // JMicron bridge port (0 / 1); CSMI phy identifier
    u8 csmi_port;           // CSMI port identifier
    u8 sas_address[8];      // CSMI attached device SAS address
};

// Task file in (features, count, lba_low/mid/high, device, command) and out (error in `features`, status in `command`).
struct AtaRegs {
    u8 features, count, lba_low, lba_mid, lba_high, device, command;
};
enum AtaDir : u8 { ATA_NO_DATA, ATA_DATA_IN, ATA_DATA_OUT };

// One command. `data` holds `sectors` x 512 bytes. `out` (may be null) gets the returned registers when the
// transport provides them (false in *have_out otherwise).
bool ata_cmd(AtaDev* d, const AtaRegs* in, AtaRegs* out, bool* have_out, void* data, u32 sectors, AtaDir dir);

bool ata_identify(AtaDev* d, u8 out[512]);
bool ata_smart_read_data(AtaDev* d, u8 out[512]);
bool ata_smart_read_thresholds(AtaDev* d, u8 out[512]);
bool ata_smart_read_log(AtaDev* d, u8 address, u8 out[512]);     // SMART READ LOG (1 sector)
i32 ata_smart_status(AtaDev* d);                                // 1 = threshold exceeded, 0 = OK, -1 = unknown
bool ata_smart_execute(AtaDev* d, u8 subcommand);              // EXECUTE OFF-LINE IMMEDIATE (self-tests)
bool ata_set_features(AtaDev* d, u8 feature, u8 count);         // AAM / APM

// Finds a transport that answers IDENTIFY DEVICE. `rw`: read/write handle; `bus_type`: STORAGE_BUS_TYPE;
// `usb_vid` / `usb_pid`: the enclosure bridge (0 if not USB). Fills `id` on success.
bool ata_attach(AtaDev* d, HANDLE rw, u32 bus_type, u16 usb_vid, u16 usb_pid, u8 id[512]);

// Without administrator rights: IDENTIFY DEVICE and READ LOG EXT pages through IOCTL_STORAGE_QUERY_PROPERTY
// (storahci answers these on a query-only handle; untested here, this machine has no SATA drive).
bool ata_query_identify(HANDLE h, u8 out[512]);
bool ata_query_log(HANDLE h, u8 log, u8 page, u8 out[512]);
// SMART_RCV_DRIVE_DATA_EX (FILE_ANY_ACCESS, Windows 10 21H2+, undocumented): SMART READ DATA / THRESHOLDS
// on a query-only handle when the driver supports it. `features` = 0xD0 (data) or 0xD1 (thresholds).
bool ata_query_smart(HANDLE h, u8 features, u8 out[512]);

// Decoded IDENTIFY DEVICE.
struct AtaIdInfo {
    char model[41], serial[21], firmware[9];
    u64 sectors;
    u32 logical_sector, physical_sector;    // bytes
    u16 rotation;                           // 0 = not reported, 1 = SSD, else rpm
    u32 buffer_bytes;                       // word 21 (obsolete, often 0)
    u8 sata_gen_max, sata_gen_cur;          // 1 = 1.5 Gb/s, 2 = 3 Gb/s, 3 = 6 Gb/s; 0 = unknown / not SATA
    u8 queue_depth;
    char standard[64];                      // "ACS-3 | SATA 3.2"
    bool smart, smart_enabled, lba48, ncq, trim, write_cache, write_cache_enabled, devsleep, apm, apm_enabled,
         aam, aam_enabled, security, gpl, sct, sanitize;
    u8 apm_value, aam_value, aam_recommended;
};
void ata_parse_identify(const u8 id[512], AtaIdInfo* out);

// SMART READ DATA sector fields.
struct AtaSmartCaps {
    u8 offline_status, selftest_status;     // byte 362 / 363 (high nibble 0xF = test in progress, low = 10 % units left)
    bool can_selftest, can_conveyance, can_selective, can_offline, error_log;
    u16 poll_short_min, poll_extended_min, poll_conveyance_min;
};
void ata_parse_smart_caps(const u8 smart[512], AtaSmartCaps* out);
