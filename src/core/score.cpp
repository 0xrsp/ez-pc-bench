// Overall score: the version table and the computation (formula in score.h, rationale in
// docs/notes/score-results.md).
#include "core/score.h"
#include "core/monitor.h"
#include "core/results.h"
#include "core/toon.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <wchar.h>

// ---- score v1 ---------------------------------------------------------------------------------------------
// Reference = library/2026-11-11_r7-250_rtx5060-laptop.toon.txt. Only metrics that exist today; v2 adds the
// disk latency tests (QD1 4K read / write + flush, latency under load) and workload-like CPU kernels
// (compression, parsing) once they land, see the notes.

#define US "\xC2\xB5s"
static const ScoreInput s_v1_inputs[] = {
    // CPU single-thread: workload-like tests first, peak FMA / GEMM with low weight.
    { SC_CPU_ST, "cpu.work.sort",     "Single thread",  0, false, 3.0f, 15.9561113, "Melem/s", "Sort 16M u32" },
    { SC_CPU_ST, "cpu.work.sieve",    "Single thread",  0, false, 3.0f, 2877.49156, "Mnum/s",  "Prime sieve" },
    { SC_CPU_ST, "cpu.int.alu",       "Single thread",  0, false, 2.0f, 17.4894827, "Gops/s",  "Integer ALU" },
    { SC_CPU_ST, "cpu.crypto.sha256", "Single thread",  0, false, 1.5f, 2.35902836, "GB/s",    "SHA-256" },
    { SC_CPU_ST, "cpu.crypto.aes",    "Single thread*", 0, false, 1.5f, 28.3299408, "GB/s",    "AES-128-CTR" },
    { SC_CPU_ST, "cpu.int.mul",       "Single thread",  0, false, 1.0f, 4.76377085, "Gops/s",  "Integer multiply" },
    { SC_CPU_ST, "cpu.fp.scalar",     "Single thread",  0, false, 1.0f, 19.0756709, "GFLOPS",  "FP32 FMA scalar" },
    { SC_CPU_ST, "cpu.fp.sgemm",      "Single thread",  0, false, 0.5f, 127.664777, "GFLOPS",  "SGEMM" },
    { SC_CPU_ST, "cpu.fp.avx2_f32",   "Single thread",  0, false, 0.5f, 152.304016, "GFLOPS",  "FP32 FMA AVX2 (peak)" },
    // CPU multi-thread: the same tests on all threads, SGEMM weighted up.
    { SC_CPU_MT, "cpu.work.sort",     "Multi-thread",   0, false, 3.0f, 103.200058, "Melem/s", "Sort 16M u32" },
    { SC_CPU_MT, "cpu.work.sieve",    "Multi-thread",   0, false, 3.0f, 19755.6945, "Mnum/s",  "Prime sieve" },
    { SC_CPU_MT, "cpu.fp.sgemm",      "Multi-thread",   0, false, 2.0f, 856.646232, "GFLOPS",  "SGEMM" },
    { SC_CPU_MT, "cpu.int.alu",       "Multi-thread",   0, false, 1.5f, 136.506345, "Gops/s",  "Integer ALU" },
    { SC_CPU_MT, "cpu.crypto.sha256", "Multi-thread",   0, false, 1.5f, 34.9257045, "GB/s",    "SHA-256" },
    { SC_CPU_MT, "cpu.crypto.aes",    "Multi-thread*",  0, false, 1.5f, 216.628958, "GB/s",    "AES-128-CTR" },
    { SC_CPU_MT, "cpu.int.mul",       "Multi-thread",   0, false, 1.0f, 38.2026507, "Gops/s",  "Integer multiply" },
    { SC_CPU_MT, "cpu.fp.scalar",     "Multi-thread",   0, false, 1.0f, 151.740148, "GFLOPS",  "FP32 FMA scalar" },
    { SC_CPU_MT, "cpu.fp.avx2_f32",   "Multi-thread",   0, false, 0.5f, 1197.17446, "GFLOPS",  "FP32 FMA AVX2 (peak)" },
    // Memory: latency weighted highest.
    { SC_MEM, "mem.lat.chase", "Latency*",     268435456, true,  3.0f, 109.819535, "ns",   "DRAM latency (256 MiB)" },
    { SC_MEM, "mem.lat.chase", "Latency*",     4194304,   true,  1.0f, 11.2558401, "ns",   "L3 latency (4 MiB)" },
    { SC_MEM, "mem.bw.read",   "Multi-thread", 0,         false, 1.5f, 58.8023933, "GB/s", "DRAM read, all threads" },
    { SC_MEM, "mem.bw.copy",   "Multi-thread", 0,         false, 1.5f, 35.0586409, "GB/s", "DRAM copy, all threads" },
    // GPU: best of Vulkan / D3D12 per test, so the API doesn't matter. Frame times include p99.
    { SC_GPU, "gpu_*.3d.scene",      "Avg frame time", 0, true,  3.0f, 0.131394845, "ms",     "Lit scene, avg frame time" },
    { SC_GPU, "gpu_*.3d.scene",      "p99 frame time", 0, true,  2.0f, 0.263300002, "ms",     "Lit scene, p99 frame time" },
    { SC_GPU, "gpu_*.compute.fp32",  "Throughput",     0, false, 1.5f, 14.7627245,  "TFLOPS", "FP32 compute" },
    { SC_GPU, "gpu_*.compute.vram",  "Read",           0, false, 1.5f, 369.010893,  "GB/s",   "VRAM read" },
    { SC_GPU, "gpu_*.3d.fill",       "Pixel fill",     0, false, 1.0f, 108.214445,  "Gpix/s", "Pixel fill" },
    { SC_GPU, "gpu_*.3d.texfill",    "Texel rate",     0, false, 1.0f, 182.045388,  "Gtex/s", "Texture fill" },
    { SC_GPU, "gpu_*.3d.triangles",  "Triangles",      0, false, 1.0f, 2658.36592,  "Mtri/s", "Triangles" },
    // Storage: QD1 latency dominates (what makes a PC feel slow), sequential and high-QD count little.
    { SC_DISK, "disk.rnd4k_q1t1",  "Read latency",  0, true,  4.0f, 57.8683483, US,     "4K random read latency, QD1", SI_DRIVE },
    { SC_DISK, "disk.rnd4k_q1t1",  "Write latency", 0, true,  2.0f, 19.5879136, US,     "4K random write latency, QD1", SI_DRIVE },
    { SC_DISK, "disk.rnd4k_q32t1", "Read",          0, false, 1.0f, 570.6844,   "MB/s", "RND4K Q32T1 read", SI_DRIVE },
    { SC_DISK, "disk.seq1m_q8t1",  "Read",          0, false, 1.0f, 7086.59705, "MB/s", "SEQ1M Q8T1 read", SI_DRIVE },
    { SC_DISK, "disk.seq1m_q1t1",  "Read",          0, false, 0.5f, 3668.93342, "MB/s", "SEQ1M Q1T1 read", SI_DRIVE },
    { SC_DISK, "disk.seq1m_q8t1",  "Write",         0, false, 0.5f, 5983.65512, "MB/s", "SEQ1M Q8T1 write", SI_DRIVE },
};
#undef US
static_assert(ARRAY_COUNT(s_v1_inputs) <= SCORE_MAX_INPUTS, "raise SCORE_MAX_INPUTS");

const ScoreVersion g_score = {
    1, "preliminary", "2026-11-11_r7-250_rtx5060-laptop",
    "Ryzen 7 250 + RTX 5060 Laptop, 32 GB DDR5-5600, Samsung PM9C1a NVMe (AC, Best performance)",
    s_v1_inputs, ARRAY_COUNT(s_v1_inputs),
    { 30, 20, 15, 20, 15 },     // Overall: CPU ST, CPU MT, Memory, GPU, Storage (%)
    "Not calibrated yet: the weights are an initial guess. Calibration fits them so the score ranks reference "
    "machines like their real-task timings (game fps, compile, app launch, photo export, file copy).",
};

const char* const g_score_comp_names[SC_COUNT] = { "CPU single-thread", "CPU multi-thread", "Memory", "GPU", "Storage" };
const char* const g_score_comp_ids[SC_COUNT] = { "cpu_st", "cpu_mt", "memory", "gpu", "storage" };

const char* const g_score_flag_ids[SF_COUNT] = { "incomplete", "settings", "battery", "power-mode", "throttle", "background",
                                                 "memory-integrity", "not-admin", "large-pages", "noisy", "no-conditions" };
const char* const g_score_flag_desc[SF_COUNT] = {
    "Some score tests are missing: component scores only (run the Score preset).",
    "Not the default settings (3 s phases, all threads, 1920x1080, 1 GiB disk file).",
    "Ran on battery (or the power source changed).",
    "Windows power mode is not \"Best performance\".",
    "Throttling: the clock dropped or the CPU / GPU ran hot during a score test.",
    "Background load: the CPU was busy beyond the test during single-thread tests.",
    "Memory integrity (HVCI) is on: it costs some performance.",
    "Not run as administrator.",
    "Memory latency used 2 MiB pages; the reference used 4 KiB pages (lower latency).",
    "A score input varied by 3 % or more between repeats.",
    "Older results file: battery, throttling and background load weren't recorded.",
};

// Single-thread tests whose CPU load shows background activity (anything above the test's own thread).
static const char* const s_load_probes[] = { "mem.lat.chase", "disk.rnd4k_q1t1" };

// ---- computation --------------------------------------------------------------------------------------

static char pick_drive(const RunFile* rf) {
    if (rf->disk_drives & (1u << ('C' - 'A'))) return 'C';
    for (u32 d = 0; d < 26; ++d)
        if (rf->disk_drives & (1u << d)) return (char)('A' + d);
    return 'C';     // files without params: disk metrics are named per drive anyway
}

// Best value of input `in` over the tests matching its pattern. Returns the metric index or -1.
static i32 find_input(const RunFile* rf, const ScoreInput* in, char drive) {
    char name[80];
    if (in->flags & SI_DRIVE) snprintf(name, sizeof(name), "%c: %s", drive, in->metric);
    else snprintf(name, sizeof(name), "%s", in->metric);
    i32 best = -1;
    for (u32 t = 0; t < rf->test_count; ++t) {
        if (!rf_wild(in->test, rf->tests[t].id)) continue;
        const i32 m = rf_find_metric(rf, t, name, in->x);
        if (m < 0 || !(rf->metrics[m].value > 0.0)) continue;
        const f64 v = rf->metrics[m].value, b = best >= 0 ? rf->metrics[best].value : 0.0;
        if (best < 0 || (in->lower_better ? v < b : v > b)) best = m;
    }
    return best;
}

void score_compute(const RunFile* rf, ScoreResult* out) {
    memset(out, 0, sizeof(*out));
    const ScoreVersion* v = &g_score;
    out->version = v->version;
    out->drive = pick_drive(rf);
    f64 log_sum[SC_COUNT] = {}, w_present[SC_COUNT] = {}, w_total[SC_COUNT] = {};
    bool disk_used = false;
    u32 env_flags = 0;
    for (u32 i = 0; i < v->input_count; ++i) {
        const ScoreInput* in = &v->inputs[i];
        ScoreInputValue* iv = &out->in[i];
        w_total[in->comp] += in->weight;
        const i32 m = find_input(rf, in, out->drive);
        if (m < 0) continue;
        const RfMetric* mt = &rf->metrics[m];
        const RfTest* t = &rf->tests[mt->test];
        iv->present = true;
        snprintf(iv->test, sizeof(iv->test), "%s", t->id);
        iv->value = mt->value;
        iv->ratio = in->lower_better ? in->ref / mt->value : mt->value / in->ref;
        log_sum[in->comp] += in->weight * log(iv->ratio);
        w_present[in->comp] += in->weight;
        disk_used |= in->comp == SC_DISK;
        env_flags |= t->env_flags;
        if (mt->cv >= 3.0f) out->flags |= SF_NOISY;
        if (strstr(mt->name, "2 MiB pages")) out->flags |= SF_LARGE_PAGES;
        out->any = true;
    }
    if (!disk_used) out->drive = 0;

    out->complete = out->any;
    for (u32 c = 0; c < SC_COUNT; ++c) {
        out->comp[c] = w_present[c] > 0.0 ? 1000.0 * exp(log_sum[c] / w_present[c]) : NAN;
        out->coverage[c] = w_total[c] > 0.0 ? (f32)(w_present[c] / w_total[c]) : 0.0f;
        out->complete &= w_present[c] >= w_total[c];
    }
    out->overall = NAN;
    if (out->complete) {
        f64 sum = 0.0, wsum = 0.0;
        for (u32 c = 0; c < SC_COUNT; ++c) {
            sum += v->overall_weights[c] * log(out->comp[c] / 1000.0);
            wsum += v->overall_weights[c];
        }
        out->overall = 1000.0 * exp(sum / wsum);
    }
    if (!out->any) return;

    // Validity flags.
    if (!out->complete) out->flags |= SF_INCOMPLETE;
    if (rf->has_params && (rf->duration_ms < 3000.0 || rf->thread_count != 0.0 || rf->gpu_width != 1920.0 ||
                           rf->gpu_height != 1080.0 || rf->disk_file_bytes < (f64)GiB || rf->disk_write_through))
        out->flags |= SF_SETTINGS;
    if (!rf->on_ac || (env_flags & (ENV_BATTERY | ENV_POWER_CHANGED))) out->flags |= SF_BATTERY;
    if (!strcmp(rf->power_mode, "Balanced") || !strcmp(rf->power_mode, "Best power efficiency")) out->flags |= SF_POWER_MODE;
    if (env_flags & (ENV_CPU_DROP | ENV_GPU_DROP | ENV_CPU_HOT | ENV_GPU_HOT)) out->flags |= SF_THROTTLE;
    if (rf->memory_integrity == 1) out->flags |= SF_MEM_INTEGRITY;
    if (!rf->elevated) out->flags |= SF_NOT_ADMIN;
    if (!rf->has_env) out->flags |= SF_NO_CONDITIONS;
    const f64 threads = rf->cpu_threads ? rf->cpu_threads : 1.0;
    for (u32 i = 0; i < ARRAY_COUNT(s_load_probes); ++i) {
        const i32 t = rf_find_test(rf, s_load_probes[i]);
        if (t >= 0 && rf->tests[t].has_env && rf->tests[t].cpu_util > 150.0 / threads + 10.0) out->flags |= SF_BACKGROUND;
    }
}

bool score_from_text(const char* data, u32 size, ScoreResult* out) {
    RunFile* rf = rf_scratch();
    char err[64];
    if (!rf_parse(data, size, rf, err, sizeof(err))) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    score_compute(rf, out);
    return true;
}

void score_current(ScoreResult* out) {
    RunFile* rf = rf_scratch();
    rf_from_current(rf);
    score_compute(rf, out);
}

void score_flag_text(u32 flags, char* buf, u32 size) {
    static const char* const short_names[SF_COUNT] = { "incomplete", "non-default settings", "on battery",
                                                       "power mode not Best performance", "throttling",
                                                       "background load", "memory integrity on", "not admin",
                                                       "large pages", "noisy", "conditions not recorded" };
    buf[0] = 0;
    for (u32 i = 0, n = 0; i < SF_COUNT; ++i) {
        if (!(flags & (1u << i))) continue;
        const size_t len = strlen(buf);
        snprintf(buf + len, size - len, "%s%s", n++ ? ", " : "", short_names[i]);
    }
}

void score_inputs_of(ScoreComp c, u32* present, u32* total, const ScoreResult* s) {
    *present = *total = 0;
    for (u32 i = 0; i < g_score.input_count; ++i) {
        if (g_score.inputs[i].comp != c) continue;
        ++*total;
        *present += s->in[i].present;
    }
}

const char* score_item_name(u32 item) { return item ? g_score_comp_names[item - 1] : "Overall"; }

f64 score_item(const ScoreResult* s, u32 item) { return item ? s->comp[item - 1] : s->overall; }

// ---- output -------------------------------------------------------------------------------------------

static f64 rounded(f64 v) { return isfinite(v) ? floor(v + 0.5) : NAN; }

void score_write_toon(FILE* f, const ScoreResult* s) {
    if (!s->any) return;
    toon_key(f, 0, "score");
    toon_num(f, 1, "version", s->version);
    toon_str(f, 1, "status", g_score.status);
    toon_str(f, 1, "reference", g_score.reference);
    toon_num(f, 1, "overall", rounded(s->overall));
    toon_table(f, 1, "components", SC_COUNT, "id,value,inputs,of");
    for (u32 c = 0; c < SC_COUNT; ++c) {
        u32 present, total;
        score_inputs_of((ScoreComp)c, &present, &total, s);
        ToonRow r = toon_row(f, 2);
        toon_cell_str(&r, g_score_comp_ids[c]);
        toon_cell_num(&r, rounded(s->comp[c]));
        toon_cell_num(&r, present);
        toon_cell_num(&r, total);
        toon_row_end(&r);
    }
    char flags[256] = {};
    for (u32 i = 0; i < SF_COUNT; ++i)
        if (s->flags & (1u << i)) {
            const size_t len = strlen(flags);
            snprintf(flags + len, sizeof(flags) - len, "%s%s", len ? "|" : "", g_score_flag_ids[i]);
        }
    toon_str(f, 1, "flags", flags);
}

bool score_rewrite_file(const wchar_t* path, char* err, u32 err_size) {
    ToonReader r;
    char* buf = toon_read_file(path, &r, err, err_size);
    if (!buf) return false;
    const char* data = r.p;
    const u32 size = (u32)(r.end - r.p);
    ScoreResult s;
    if (!score_from_text(data, size, &s)) {
        toon_free_file(buf);
        snprintf(err, err_size, "not an EzPcBench results file");
        return false;
    }
    wchar_t tmp[MAX_PATH + 8];
    swprintf(tmp, ARRAY_COUNT(tmp), L"%s.tmp", path);
    FILE* f = _wfopen(tmp, L"wb");
    if (!f) { toon_free_file(buf); snprintf(err, err_size, "cannot write a temporary file"); return false; }
    // Copy every line except the old score block; the new one goes before `integrity:` (or at the end).
    bool skip = false, written = false;
    for (const char* p = data; p < data + size;) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(data + size - p));
        const char* end = nl ? nl + 1 : data + size;
        if (*p != ' ' && *p != '\r' && *p != '\n') {
            skip = !strncmp(p, "score:", 6);
            if (!written && !strncmp(p, "integrity:", 10)) { score_write_toon(f, &s); written = true; }
        }
        if (!skip) fwrite(p, 1, (size_t)(end - p), f);
        if (!skip && !nl) fputc('\n', f);
        p = end;
    }
    if (!written) score_write_toon(f, &s);
    const bool ok = ferror(f) == 0;
    fclose(f);
    toon_free_file(buf);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        snprintf(err, err_size, "cannot replace the file");
        return false;
    }
    return true;
}

static void print_num(FILE* f, f64 v) {
    if (isfinite(v)) fprintf(f, "%5.0f", v);
    else fprintf(f, "    -");
}

void score_print(FILE* f, const ScoreResult* s) {
    const ScoreVersion* v = &g_score;
    fprintf(f, "Score v%u (%s). Reference = 1000: %s\n", v->version, v->status, v->reference);
    if (!s->any) {
        fprintf(f, "  No score inputs in this run (run the Score preset: --preset score).\n");
        return;
    }
    fprintf(f, "  %-18s", "Overall");
    print_num(f, s->overall);
    fprintf(f, "%s\n", s->complete ? "" : "  (incomplete run: component scores only)");
    char flags[300];
    score_flag_text(s->flags & ~SF_INCOMPLETE, flags, sizeof(flags));
    if (flags[0]) fprintf(f, "  Flags: %s\n", flags);
    for (u32 c = 0; c < SC_COUNT; ++c) {
        u32 present, total;
        score_inputs_of((ScoreComp)c, &present, &total, s);
        fprintf(f, "  %-18s", g_score_comp_names[c]);
        print_num(f, s->comp[c]);
        if (c == SC_DISK && s->drive) fprintf(f, "  (%u of %u inputs, drive %c:)\n", present, total, s->drive);
        else fprintf(f, "  (%u of %u inputs)\n", present, total);
        for (u32 i = 0; i < v->input_count; ++i) {
            const ScoreInput* in = &v->inputs[i];
            if (in->comp != c) continue;
            const ScoreInputValue* iv = &s->in[i];
            fprintf(f, "      %-30s w%-4.1f", in->label, in->weight);
            if (!iv->present) {
                fprintf(f, " missing (%s)\n", in->test);
                continue;
            }
            char val[32], ref[32];
            format_value(val, sizeof(val), iv->value);
            format_value(ref, sizeof(ref), in->ref);
            fprintf(f, " %10s %-8s ref %10s  %+6.1f%%  %s\n", val, in->unit, ref, (iv->ratio - 1.0) * 100.0, iv->test);
        }
    }
}
