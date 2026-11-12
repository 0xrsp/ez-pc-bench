// GPU suites over several adapters: one backend device per adapter selected in BenchParams::gpu_adapters,
// created at suite init. Each test runs on every adapter in turn into the same result; with more than one
// adapter the metric names get the adapter's short name as prefix ("radeon-780m: Read"). Also routes the
// extra tests (gpu_ext.h) before the base backends.
#include "gpu/gpu_ext.h"
#include "core/results.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum GpuApi : u8 { API_VK, API_D3D12 };
constexpr u32 GPU_MAX_ADAPTERS = 8;

struct GpuMulti {
    GpuApi api;
    u32 count;
    u32 adapter[GPU_MAX_ADAPTERS];          // index into g_sys.gpus
    void* backend[GPU_MAX_ADAPTERS];
    char tag[GPU_MAX_ADAPTERS][48];         // metric name prefix
};

u32 gpu_adapter_mask(const BenchParams* p) {
    const u32 valid = g_sys.gpu_count >= 32 ? ~0u : (1u << g_sys.gpu_count) - 1;
    const u32 m = p->gpu_adapters & valid;
    return m ? m : 1u << (p->gpu_adapter < g_sys.gpu_count ? p->gpu_adapter : 0);
}

// ---- metric names --------------------------------------------------------------------------------
// Metric names must outlive the run (static strings); prefixed names are interned here. Only touched by
// the runner's worker thread.

static char s_pool[128 * 1024];
static u32 s_pool_used;
static const char* s_names[4096];
static u32 s_name_count;

static const char* intern_prefixed(const char* tag, const char* name) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s: %s", tag, name);
    for (u32 i = 0; i < s_name_count; ++i)
        if (!strcmp(s_names[i], buf)) return s_names[i];
    const u32 len = (u32)strlen(buf) + 1;
    if (s_name_count == ARRAY_COUNT(s_names) || s_pool_used + len > sizeof(s_pool)) return name;
    char* s = s_pool + s_pool_used;
    memcpy(s, buf, len);
    s_pool_used += len;
    s_names[s_name_count++] = s;
    return s;
}

// ---- suite --------------------------------------------------------------------------------------------

void gpu_suite_shutdown(void* state) {
    GpuMulti* m = (GpuMulti*)state;
    if (!m) return;
    for (u32 i = 0; i < m->count; ++i) {
        if (m->api == API_VK) gpu_vk_shutdown(m->backend[i]);
        else gpu_d3d12_shutdown(m->backend[i]);
    }
    free(m);
}

static bool suite_init(GpuApi api, const BenchParams* p, void** state, char* error, u32 error_size) {
    *state = nullptr;
    if (g_sys.gpu_count == 0) {
        snprintf(error, error_size, "no GPU adapter found");
        return false;
    }
    GpuMulti* m = (GpuMulti*)calloc(1, sizeof(GpuMulti));
    if (!m) { snprintf(error, error_size, "out of memory"); return false; }
    m->api = api;
    const u32 mask = gpu_adapter_mask(p);
    const bool several = (mask & (mask - 1)) != 0;
    for (u32 i = 0; i < g_sys.gpu_count && m->count < GPU_MAX_ADAPTERS; ++i) {
        if (!((mask >> i) & 1)) continue;
        BenchParams q = *p;
        q.gpu_adapter = i;
        char err[128] = {};
        void* b = nullptr;
        const bool ok = api == API_VK ? gpu_vk_init(&q, &b, err, sizeof(err)) : gpu_d3d12_init(&q, &b, err, sizeof(err));
        if (!ok) {
            if (several) snprintf(error, error_size, "%s: %s", g_sys.gpus[i].name, err);
            else snprintf(error, error_size, "%s", err);
            gpu_suite_shutdown(m);
            return false;
        }
        const u32 k = m->count++;
        m->adapter[k] = i;
        m->backend[k] = b;
        gpu_short_name(g_sys.gpus[i].name, m->tag[k], sizeof(m->tag[k]));
        for (u32 j = 0; j < k; ++j)     // two identical adapters: tell them apart by index
            if (!strcmp(m->tag[j], m->tag[k])) {
                const size_t len = strlen(m->tag[k]);
                snprintf(m->tag[k] + len, sizeof(m->tag[k]) - len, "#%u", i);
                break;
            }
    }
    *state = m;
    return true;
}

bool gpu_vk_suite_init(const BenchParams* p, void** state, char* error, u32 error_size) {
    return suite_init(API_VK, p, state, error, error_size);
}

bool gpu_d3d12_suite_init(const BenchParams* p, void** state, char* error, u32 error_size) {
    return suite_init(API_D3D12, p, state, error, error_size);
}

static void run_one(GpuMulti* m, u32 k, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                    const CancelFlag* cancel) {
    BenchParams q = *p;
    q.gpu_adapter = m->adapter[k];
    if (m->api == API_VK) {
        if (!gpu_vk_run_ext(m->backend[k], test, &q, live, out, cancel)) gpu_vk_run(m->backend[k], test, &q, live, out, cancel);
    } else {
        if (!gpu_d3d12_run_ext(m->backend[k], test, &q, live, out, cancel)) gpu_d3d12_run(m->backend[k], test, &q, live, out, cancel);
    }
}

void gpu_suite_run(void* state, GpuTestId test, const BenchParams* p, BenchLive* live, BenchResult* out,
                   const CancelFlag* cancel) {
    GpuMulti* m = (GpuMulti*)state;
    if (m->count == 1) { run_one(m, 0, test, p, live, out, cancel); return; }

    // Several adapters: each one's metrics are appended to one contiguous slice, renamed with its tag.
    res_begin(out, RESULT_VALUES);
    char failed[128] = {}, skipped[128] = {};
    bool any = false;
    for (u32 k = 0; k < m->count && !cancel->load(std::memory_order_relaxed); ++k) {
        const u32 mark = res_mark();
        BenchResult r = {};
        run_one(m, k, test, p, live, &r, cancel);
        if (r.error[0] || r.skipped || cancel->load(std::memory_order_relaxed)) {
            res_rewind(mark);
            char* dst = r.skipped ? skipped : failed;
            if (!dst[0] && r.error[0]) snprintf(dst, sizeof(failed), "%s: %s", m->tag[k], r.error);
            continue;
        }
        if (r.first != mark) memmove(&g_metrics[mark], &g_metrics[r.first], r.count * sizeof(Metric));
        res_rewind(mark + r.count);
        for (u32 i = 0; i < r.count; ++i) g_metrics[mark + i].name = intern_prefixed(m->tag[k], g_metrics[mark + i].name);
        out->kind = r.kind;
        if (r.x_name) res_series_axis(out, r.x_name, r.x_unit);
        any = true;
    }
    out->count = res_mark() - out->first;
    // A failure on any adapter fails the test (its metrics stay); a test skipped everywhere is skipped. A skip on
    // only some adapters (feature missing there) just leaves out their metrics.
    if (failed[0]) res_fail(out, "%s", failed);
    else if (!any && skipped[0]) res_skip(out, "%s", skipped);
}
