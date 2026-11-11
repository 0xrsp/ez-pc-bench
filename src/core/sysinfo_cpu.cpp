// CPU details (sysinfo_ext.h): codename, microcode, clocks, ISA flags, cache geometry, core map. No admin.
#include "core/sysinfo.h"
#include "core/sysinfo_ext.h"
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

CpuDetails g_cpux;

// ---- codename -----------------------------------------------------------------------------------------------
// Family / model -> codename. AMD: family 17h-1Ah (Zen 1-5); Intel: family 6 client and server parts since
// Skylake. Unknown models show the microarchitecture from the family (AMD) or nothing.

struct CodeName { u16 family; u8 model_lo, model_hi; const char* name; const char* arch; };
static const CodeName AMD_NAMES[] = {
    { 0x17, 0x00, 0x07, "Summit Ridge / Naples", "Zen" },        { 0x17, 0x08, 0x0F, "Pinnacle Ridge", "Zen+" },
    { 0x17, 0x10, 0x17, "Raven Ridge", "Zen" },                  { 0x17, 0x18, 0x1F, "Picasso", "Zen+" },
    { 0x17, 0x20, 0x2F, "Dali", "Zen" },                         { 0x17, 0x30, 0x3F, "Rome / Castle Peak", "Zen 2" },
    { 0x17, 0x60, 0x67, "Renoir", "Zen 2" },                     { 0x17, 0x68, 0x6F, "Lucienne", "Zen 2" },
    { 0x17, 0x70, 0x7F, "Matisse", "Zen 2" },                    { 0x17, 0x90, 0x9F, "Van Gogh", "Zen 2" },
    { 0x17, 0xA0, 0xAF, "Mendocino", "Zen 2" },
    { 0x19, 0x00, 0x0F, "Milan / Chagall", "Zen 3" },            { 0x19, 0x10, 0x1F, "Genoa / Storm Peak", "Zen 4" },
    { 0x19, 0x20, 0x2F, "Vermeer", "Zen 3" },                    { 0x19, 0x40, 0x4F, "Rembrandt", "Zen 3+" },
    { 0x19, 0x50, 0x5F, "Cezanne / Barcelo", "Zen 3" },          { 0x19, 0x60, 0x6F, "Raphael", "Zen 4" },
    { 0x19, 0x70, 0x74, "Phoenix", "Zen 4" },                    { 0x19, 0x75, 0x77, "Hawk Point", "Zen 4" },
    { 0x19, 0x78, 0x7F, "Phoenix 2", "Zen 4 + Zen 4c" },         { 0x19, 0xA0, 0xAF, "Bergamo / Siena", "Zen 4c" },
    { 0x1A, 0x00, 0x1F, "Turin", "Zen 5" },                      { 0x1A, 0x20, 0x2F, "Strix Point", "Zen 5 + Zen 5c" },
    { 0x1A, 0x40, 0x4F, "Granite Ridge", "Zen 5" },              { 0x1A, 0x60, 0x6F, "Krackan Point", "Zen 5 + Zen 5c" },
    { 0x1A, 0x70, 0x7F, "Strix Halo", "Zen 5" },
};
static const CodeName INTEL_NAMES[] = {
    { 6, 0x4E, 0x4E, "Skylake", "Skylake" },                     { 6, 0x5E, 0x5E, "Skylake", "Skylake" },
    { 6, 0x55, 0x55, "Skylake-SP / Cascade Lake", "Skylake" },   { 6, 0x8E, 0x8E, "Kaby / Whiskey / Comet Lake", "Skylake" },
    { 6, 0x9E, 0x9E, "Kaby / Coffee Lake", "Skylake" },          { 6, 0xA5, 0xA6, "Comet Lake", "Skylake" },
    { 6, 0x7D, 0x7E, "Ice Lake", "Sunny Cove" },                 { 6, 0x6A, 0x6C, "Ice Lake-SP", "Sunny Cove" },
    { 6, 0xA7, 0xA7, "Rocket Lake", "Cypress Cove" },            { 6, 0x8C, 0x8D, "Tiger Lake", "Willow Cove" },
    { 6, 0x97, 0x9A, "Alder Lake", "Golden Cove + Gracemont" },  { 6, 0xBE, 0xBE, "Alder Lake-N", "Gracemont" },
    { 6, 0xB7, 0xB7, "Raptor Lake", "Raptor Cove + Gracemont" }, { 6, 0xBA, 0xBA, "Raptor Lake", "Raptor Cove + Gracemont" },
    { 6, 0xBF, 0xBF, "Raptor Lake", "Raptor Cove + Gracemont" }, { 6, 0xAA, 0xAC, "Meteor Lake", "Redwood Cove + Crestmont" },
    { 6, 0xBD, 0xBD, "Lunar Lake", "Lion Cove + Skymont" },      { 6, 0xC5, 0xC6, "Arrow Lake", "Lion Cove + Skymont" },
    { 6, 0xCC, 0xCC, "Panther Lake", "Cougar Cove + Darkmont" }, { 6, 0x8F, 0x8F, "Sapphire Rapids", "Golden Cove" },
    { 6, 0xCF, 0xCF, "Emerald Rapids", "Raptor Cove" },          { 6, 0xAD, 0xAE, "Granite Rapids", "Redwood Cove" },
};

static void codename_query(CpuDetails* d, const CpuInfo* c) {
    const bool amd = !strcmp(c->vendor, "AuthenticAMD"), intel = !strcmp(c->vendor, "GenuineIntel");
    const CodeName* t = amd ? AMD_NAMES : intel ? INTEL_NAMES : nullptr;
    const u32 n = amd ? ARRAY_COUNT(AMD_NAMES) : intel ? ARRAY_COUNT(INTEL_NAMES) : 0;
    for (u32 i = 0; i < n; ++i)
        if (t[i].family == c->family && c->model >= t[i].model_lo && c->model <= t[i].model_hi) {
            snprintf(d->codename, sizeof(d->codename), "%s", t[i].name);
            snprintf(d->microarch, sizeof(d->microarch), "%s", t[i].arch);
            return;
        }
    if (amd) {
        const char* arch = c->family == 0x17 ? "Zen / Zen 2" : c->family == 0x19 ? "Zen 3 / Zen 4" : c->family == 0x1A ? "Zen 5" : "";
        snprintf(d->microarch, sizeof(d->microarch), "%s", arch);
    }
}

// ---- ISA flags --------------------------------------------------------------------------------------------

enum Reg : u8 { EAX, EBX, ECX, EDX };
struct IsaFlag { u32 leaf, sub; Reg reg; u8 bit; const char* name; };
static const IsaFlag ISA_FLAGS[] = {
    { 1, 0, EDX, 0, "fpu" },       { 1, 0, EDX, 4, "tsc" },          { 1, 0, EDX, 8, "cx8" },       { 1, 0, EDX, 15, "cmov" },
    { 1, 0, EDX, 19, "clflush" },  { 1, 0, EDX, 23, "mmx" },         { 1, 0, EDX, 24, "fxsr" },     { 1, 0, EDX, 25, "sse" },
    { 1, 0, EDX, 26, "sse2" },     { 1, 0, EDX, 28, "ht" },
    { 1, 0, ECX, 0, "sse3" },      { 1, 0, ECX, 1, "pclmulqdq" },    { 1, 0, ECX, 3, "monitor" },   { 1, 0, ECX, 5, "vmx" },
    { 1, 0, ECX, 9, "ssse3" },     { 1, 0, ECX, 12, "fma" },         { 1, 0, ECX, 13, "cx16" },     { 1, 0, ECX, 19, "sse4_1" },
    { 1, 0, ECX, 20, "sse4_2" },   { 1, 0, ECX, 21, "x2apic" },      { 1, 0, ECX, 22, "movbe" },    { 1, 0, ECX, 23, "popcnt" },
    { 1, 0, ECX, 25, "aes" },      { 1, 0, ECX, 26, "xsave" },       { 1, 0, ECX, 28, "avx" },      { 1, 0, ECX, 29, "f16c" },
    { 1, 0, ECX, 30, "rdrand" },   { 1, 0, ECX, 31, "hypervisor" },
    { 7, 0, EBX, 0, "fsgsbase" },  { 7, 0, EBX, 2, "sgx" },          { 7, 0, EBX, 3, "bmi1" },      { 7, 0, EBX, 4, "hle" },
    { 7, 0, EBX, 5, "avx2" },      { 7, 0, EBX, 7, "smep" },         { 7, 0, EBX, 8, "bmi2" },      { 7, 0, EBX, 9, "erms" },
    { 7, 0, EBX, 10, "invpcid" },  { 7, 0, EBX, 11, "rtm" },         { 7, 0, EBX, 16, "avx512f" },  { 7, 0, EBX, 17, "avx512dq" },
    { 7, 0, EBX, 18, "rdseed" },   { 7, 0, EBX, 19, "adx" },         { 7, 0, EBX, 20, "smap" },     { 7, 0, EBX, 21, "avx512ifma" },
    { 7, 0, EBX, 23, "clflushopt" }, { 7, 0, EBX, 24, "clwb" },      { 7, 0, EBX, 28, "avx512cd" }, { 7, 0, EBX, 29, "sha_ni" },
    { 7, 0, EBX, 30, "avx512bw" }, { 7, 0, EBX, 31, "avx512vl" },
    { 7, 0, ECX, 1, "avx512vbmi" }, { 7, 0, ECX, 2, "umip" },        { 7, 0, ECX, 3, "pku" },       { 7, 0, ECX, 5, "waitpkg" },
    { 7, 0, ECX, 6, "avx512_vbmi2" }, { 7, 0, ECX, 7, "shstk" },     { 7, 0, ECX, 8, "gfni" },      { 7, 0, ECX, 9, "vaes" },
    { 7, 0, ECX, 10, "vpclmulqdq" }, { 7, 0, ECX, 11, "avx512_vnni" }, { 7, 0, ECX, 12, "avx512_bitalg" },
    { 7, 0, ECX, 14, "avx512_vpopcntdq" }, { 7, 0, ECX, 16, "la57" }, { 7, 0, ECX, 22, "rdpid" }, { 7, 0, ECX, 27, "movdiri" },
    { 7, 0, ECX, 28, "movdir64b" },
    { 7, 0, EDX, 4, "fsrm" },      { 7, 0, EDX, 8, "avx512_vp2intersect" }, { 7, 0, EDX, 14, "serialize" },
    { 7, 0, EDX, 15, "hybrid" },   { 7, 0, EDX, 20, "ibt" },         { 7, 0, EDX, 22, "amx_bf16" }, { 7, 0, EDX, 23, "avx512_fp16" },
    { 7, 0, EDX, 24, "amx_tile" }, { 7, 0, EDX, 25, "amx_int8" },
    { 7, 1, EAX, 0, "sha512" },    { 7, 1, EAX, 4, "avx_vnni" },     { 7, 1, EAX, 5, "avx512_bf16" }, { 7, 1, EAX, 7, "cmpccxadd" },
    { 7, 1, EAX, 23, "avx_ifma" },
    { 7, 1, EDX, 4, "avx_vnni_int8" }, { 7, 1, EDX, 5, "avx_ne_convert" }, { 7, 1, EDX, 19, "avx10" },
    { 0x80000001, 0, ECX, 0, "lahf_lm" }, { 0x80000001, 0, ECX, 2, "svm" }, { 0x80000001, 0, ECX, 5, "abm" },
    { 0x80000001, 0, ECX, 6, "sse4a" },   { 0x80000001, 0, ECX, 8, "3dnowprefetch" }, { 0x80000001, 0, ECX, 11, "xop" },
    { 0x80000001, 0, ECX, 16, "fma4" },   { 0x80000001, 0, ECX, 21, "tbm" },
    { 0x80000001, 0, EDX, 11, "syscall" }, { 0x80000001, 0, EDX, 20, "nx" }, { 0x80000001, 0, EDX, 26, "pdpe1gb" },
    { 0x80000001, 0, EDX, 27, "rdtscp" }, { 0x80000001, 0, EDX, 29, "lm" },
    { 0x80000008, 0, EBX, 0, "clzero" },  { 0x80000008, 0, EBX, 9, "wbnoinvd" },
};

static void isa_query(CpuDetails* d) {
    int r[4];
    __cpuid(r, 0);
    const u32 max_std = (u32)r[0];
    __cpuid(r, 0x80000000);
    const u32 max_ext = (u32)r[0];
    for (u32 i = 0; i < ARRAY_COUNT(ISA_FLAGS) && d->isa_count < ARRAY_COUNT(d->isa); ++i) {
        const IsaFlag* f = &ISA_FLAGS[i];
        if (f->leaf >= 0x80000000 ? f->leaf > max_ext : f->leaf > max_std) continue;
        if (f->leaf == 7 && f->sub == 1) {
            __cpuidex(r, 7, 0);
            if ((u32)r[0] < 1) continue;    // leaf 7 sub-leaf count
        }
        __cpuidex(r, (int)f->leaf, (int)f->sub);
        if (((u32)r[f->reg] >> f->bit) & 1) d->isa[d->isa_count++] = f->name;
    }
}

// ---- registry: microcode, nominal clock -------------------------------------------------------------------------

static void registry_query(CpuDetails* d) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    u8 rev[8] = {};
    DWORD size = sizeof(rev), type = 0;
    if (RegQueryValueExW(k, L"Update Revision", nullptr, &type, rev, &size) == ERROR_SUCCESS && type == REG_BINARY) {
        u32 lo, hi = 0;
        memcpy(&lo, rev, 4);
        if (size >= 8) memcpy(&hi, rev + 4, 4);
        d->microcode = hi ? hi : lo;    // Intel: revision in the high dword; AMD: one dword
    }
    DWORD mhz = 0;
    size = sizeof(mhz);
    if (RegQueryValueExW(k, L"~MHz", nullptr, &type, (BYTE*)&mhz, &size) == ERROR_SUCCESS && type == REG_DWORD)
        d->base_mhz = mhz;
    RegCloseKey(k);
}

// ---- SMBIOS type 4 (processor) ------------------------------------------------------------------------------

static void smbios_cpu_query(CpuDetails* d) {
    const DWORD sig = 'RSMB';
    const UINT size = GetSystemFirmwareTable(sig, 0, nullptr, 0);
    if (!size) return;
    u8* buf = (u8*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    if (GetSystemFirmwareTable(sig, 0, buf, size) == size) {
        const u32 length = *(const u32*)(buf + 4);
        const u8* p = buf + 8;
        const u8* end = p + length;
        while (p + 4 <= end) {
            const u8 type = p[0], len = p[1];
            if (len < 4) break;
            const u8* next = p + len;
            while (next + 1 < end && !(next[0] == 0 && next[1] == 0)) ++next;
            next += 2;
            if (type == 4 && len >= 0x18 && !d->max_mhz) {
                const char* s = (const char*)p + len;      // string 1 is the socket designation
                for (u8 i = 1; i < p[0x04] && (const u8*)s < end; ++i) s += strlen(s) + 1;
                if (p[0x04] && (const u8*)s < end) snprintf(d->socket, sizeof(d->socket), "%s", s);
                d->bus_mhz = *(const u16*)(p + 0x12);
                d->max_mhz = *(const u16*)(p + 0x14);
            }
            if (type == 127) break;
            p = next;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

// ---- caches and cores -----------------------------------------------------------------------------------------

static void cache_query(CpuDetails* d) {
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &size);
    u8* buf = (u8*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    if (GetLogicalProcessorInformationEx(RelationCache, (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)buf, &size)) {
        for (DWORD off = 0; off < size;) {
            const auto* e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buf + off);
            const CACHE_RELATIONSHIP* cr = &e->Cache;
            const char type = cr->Type == CacheData ? 'D' : cr->Type == CacheInstruction ? 'I' : cr->Type == CacheUnified ? 'U' : 'T';
            CpuCache* c = nullptr;
            for (u32 i = 0; i < d->cache_count && !c; ++i)
                if (d->caches[i].level == cr->Level && d->caches[i].type == type && d->caches[i].size_kib == cr->CacheSize / 1024)
                    c = &d->caches[i];
            if (!c && d->cache_count < ARRAY_COUNT(d->caches)) {
                c = &d->caches[d->cache_count++];
                c->level = cr->Level;
                c->type = type;
                c->ways = cr->Associativity;
                c->line = cr->LineSize;
                c->size_kib = cr->CacheSize / 1024;
            }
            if (c) c->count++;
            off += e->Size;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    // Sort by level, data before instruction (insertion sort, at most 8 entries).
    for (u32 i = 1; i < d->cache_count; ++i)
        for (u32 j = i; j > 0 && (d->caches[j].level * 4 + (d->caches[j].type == 'I')) <
                                     (d->caches[j - 1].level * 4 + (d->caches[j - 1].type == 'I')); --j) {
            const CpuCache t = d->caches[j]; d->caches[j] = d->caches[j - 1]; d->caches[j - 1] = t;
        }
}

// Core map from CPU sets: core index, efficiency class (hybrid), scheduling class (preferred cores) and the
// last-level cache each core shares.
static void core_query(CpuDetails* d) {
    ULONG size = 0;
    GetSystemCpuSetInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
    u8* buf = (u8*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) return;
    u32 core_ids[64];
    u8 l3_ids[64];
    if (GetSystemCpuSetInformation((SYSTEM_CPU_SET_INFORMATION*)buf, size, &size, GetCurrentProcess(), 0)) {
        for (ULONG off = 0; off < size;) {
            const auto* e = (const SYSTEM_CPU_SET_INFORMATION*)(buf + off);
            off += e->Size;
            if (e->Type != CpuSetInformation || e->CpuSet.Group != 0) continue;
            const u32 id = e->CpuSet.CoreIndex;
            CpuCore* c = nullptr;
            for (u32 i = 0; i < d->core_count && !c; ++i)
                if (core_ids[i] == id) c = &d->cores[i];
            if (!c) {
                if (d->core_count >= ARRAY_COUNT(d->cores)) continue;
                core_ids[d->core_count] = id;
                c = &d->cores[d->core_count++];
                c->lp_first = e->CpuSet.LogicalProcessorIndex;
                c->efficiency = e->CpuSet.EfficiencyClass;
                c->sched = e->CpuSet.SchedulingClass;
                c->l3 = e->CpuSet.LastLevelCacheIndex;
            }
            if (e->CpuSet.LogicalProcessorIndex < c->lp_first) c->lp_first = e->CpuSet.LogicalProcessorIndex;
            c->lp_count++;
        }
    }
    HeapFree(GetProcessHeap(), 0, buf);
    u8 top = 0;
    for (u32 i = 0; i < d->core_count; ++i) top = d->cores[i].efficiency > top ? d->cores[i].efficiency : top;
    for (u32 i = 0; i < d->core_count; ++i) {
        if (d->cores[i].efficiency == top) d->perf_cores++;
        else d->eff_cores++;
        bool seen = false;
        for (u32 k = 0; k < d->l3_count && !seen; ++k) seen = l3_ids[k] == d->cores[i].l3;
        if (!seen && d->l3_count < ARRAY_COUNT(l3_ids)) l3_ids[d->l3_count++] = d->cores[i].l3;
    }
    d->hybrid = d->eff_cores > 0;
}

void cpu_details_query(CpuDetails* d) {
    memset(d, 0, sizeof(*d));
    codename_query(d, &g_sys.cpu);
    isa_query(d);
    registry_query(d);
    smbios_cpu_query(d);
    cache_query(d);
    core_query(d);
}
