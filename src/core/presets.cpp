#include "core/presets.h"
#include "core/score.h"
#include <string.h>

// Quick: one or two tests per area, short phases, one disk pass on a small file (~1 minute here).
// Extended: every test, median of 3 runs; one disk pass per run so the SSD sees the same writes as Standard.
const RunPreset g_presets[] = {
    { "quick", "Quick", "About a minute: the main CPU, memory, disk and GPU tests with 1 s phases, one disk pass "
                        "on a 256 MiB file. For a fast health check, not for publishing numbers.",
      PRESET_SEL_LIST,
      "cpu.int.alu,cpu.fp.avx2_f32,cpu.fp.sgemm,cpu.crypto.aes,cpu.work.sort,cpu.work.sieve,"
      "mem.bw.read,mem.bw.copy,mem.lat.chase,disk.seq1m_q8t1,disk.rnd4k_q1t1,"
      "gpu_vk.compute.fp32,gpu_vk.compute.vram,gpu_vk.3d.scene,gpu_d3d12.3d.fill,gpu_d3d12.3d.scene",
      1000, 1, 1, 256 },
    { "standard", "Standard", "The default selection with 3 s phases and the best of 3 disk passes (about 10 minutes).",
      PRESET_SEL_DEFAULT, nullptr, 3000, 1, 3, 1024 },
    { "extended", "Extended", "Every test, each run 3 times and reported as the median with its spread; one disk "
                              "pass per run (the median of 3 instead of the best of 3). About 35 minutes.",
      PRESET_SEL_ALL, nullptr, 3000, 3, 1, 1024 },
    { "score", "Score", "Exactly the tests the overall score needs (score.h), at default settings: 3 s phases, best of 3 "
                        "disk passes on a 1 GiB file, both GPU APIs (the score takes the better one). About 5 minutes.",
      PRESET_SEL_LIST, SCORE_V1_TESTS, 3000, 1, 3, 1024 },
    { "sustained", "Sustained", "Thermal / power-limit check, like Cinebench's 10-minute run: all-core SGEMM, the LZ77 "
                                "compressor and the D3D12 scene each loop for 5 minutes (about 15 minutes). Reports "
                                "the first and last pass, min, median and the drop; the Charts tab plots every pass.",
      PRESET_SEL_LIST, "cpu.fp.sgemm,cpu.comp.lz77,gpu_d3d12.3d.scene", 3000, 1, 1, 256, 300 },
};
const u32 g_preset_count = ARRAY_COUNT(g_presets);

const RunPreset* preset_find(const char* id) {
    for (u32 i = 0; i < g_preset_count; ++i)
        if (!_stricmp(g_presets[i].id, id)) return &g_presets[i];
    return nullptr;
}

static bool in_list(const char* list, const char* id) {
    const size_t n = strlen(id);
    for (const char* p = strstr(list, id); p; p = strstr(p + 1, id))
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return true;
    return false;
}

static bool preset_selects(const RunPreset* pr, u32 bench) {
    if (g_benches[bench].suite == SUITE_SMOKE || (g_benches[bench].flags & BENCH_CONFIRM) || bench_unsupported(bench))
        return false;
    switch (pr->select) {
        case PRESET_SEL_LIST: return in_list(pr->ids, g_benches[bench].cli_id);
        case PRESET_SEL_DEFAULT: return !(g_benches[bench].flags & BENCH_DEFAULT_OFF);
        default: return true;
    }
}

void preset_apply(const RunPreset* pr, BenchParams* p, bool* selected) {
    for (u32 i = 0; i < g_bench_count; ++i)     // tests this CPU can't run keep their saved choice (GUI ini)
        if (!bench_unsupported(i)) selected[i] = preset_selects(pr, i);
    p->duration_ms = pr->duration_ms;
    p->repeat = pr->repeat;
    p->disk_passes = pr->disk_passes;
    p->disk_file_bytes = (u64)pr->disk_file_mib * MiB;
    p->sustain_s = pr->sustain_s;
}

const RunPreset* preset_match(const BenchParams* p, const bool* selected) {
    for (u32 k = 0; k < g_preset_count; ++k) {
        const RunPreset* pr = &g_presets[k];
        if (p->duration_ms != pr->duration_ms || p->repeat != pr->repeat || p->disk_passes != pr->disk_passes ||
            p->disk_file_bytes != (u64)pr->disk_file_mib * MiB || p->sustain_s != pr->sustain_s)
            continue;
        bool same = true;
        for (u32 i = 0; i < g_bench_count && same; ++i)
            same = g_benches[i].suite == SUITE_SMOKE || bench_unsupported(i) || selected[i] == preset_selects(pr, i);
        if (same) return pr;
    }
    return nullptr;
}
