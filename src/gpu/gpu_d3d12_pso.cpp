// D3D12 pipeline creation (gpu_ext.h): compute and graphics PSOs per second, cold vs cached.
// Cold: every PSO gets shaders the driver has never seen. D3D12 has no specialization constants, so each
// pipeline uses a copy of the DXIL container with a new SEED literal in the bitcode (see "unique shaders");
// the container checksum the runtime verifies is then recomputed.
// Cached: the same PSOs loaded from an ID3D12PipelineLibrary that the cold pass stored them in, serialized and
// reopened like on the next game start.
#include "gpu/gpu_d3d12_ext.h"
#include "core/results.h"
#include "core/timer.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cs_pso_dxil.h"
#include "vs_pso_dxil.h"
#include "ps_pso_dxil.h"

// ---- DXIL container checksum -------------------------------------------------------------------------
// The DXBC / DXIL container digest: MD5 rounds with a custom final block layout (bit count in the first word,
// 2 * size + 1 in the last), over everything after the 20-byte magic + digest.

static u32 rotl(u32 x, u32 c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(u32 st[4], const u32 x[16]) {
    static const u32 R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                               5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                               4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                               6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    static u32 K[64];
    if (!K[0])
        for (u32 i = 0; i < 64; ++i) K[i] = (u32)(u64)(fabs(sin(i + 1.0)) * 4294967296.0);
    u32 a = st[0], b = st[1], c = st[2], d = st[3];
    for (u32 i = 0; i < 64; ++i) {
        u32 f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        const u32 t = d;
        d = c;
        c = b;
        b = b + rotl(a + f + K[i] + x[g], R[i]);
        a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

static void container_digest(const u8* data, u32 size, u32 out[4]) {
    u32 st[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    const u32 full = size & ~63u, left = size - full;
    u32 x[16];
    for (u32 off = 0; off < full; off += 64) {
        memcpy(x, data + off, 64);
        md5_block(st, x);
    }
    memset(x, 0, sizeof(x));
    if (left < 56) {
        x[0] = size << 3;
        memcpy((u8*)x + 4, data + full, left);
        ((u8*)x)[4 + left] = 0x80;
    } else {
        memcpy(x, data + full, left);
        if (left < 64) ((u8*)x)[left] = 0x80;
        md5_block(st, x);
        memset(x, 0, sizeof(x));
        x[0] = size << 3;
    }
    x[15] = (size << 1) | 1;
    md5_block(st, x);
    memcpy(out, st, 16);
}

// Header: 'DXBC', digest[16], version (4), container size (4), part count (4), part offsets; part = fourcc, size, data.
static bool container_ok(const u8* c, u32 size) {
    u32 total;
    memcpy(&total, c + 24, 4);
    if (size < 32 || memcmp(c, "DXBC", 4) || total != size) return false;
    u32 digest[4];
    container_digest(c + 20, size - 20, digest);
    return memcmp(digest, c + 4, 16) == 0;
}

// ---- unique shaders ------------------------------------------------------------------------------------
// pso.hlsl uses SEED = PSO_SEED only as an xor operand, so the DXIL bitcode holds it as one integer constant
// record: LLVM writes it sign-rotated (v << 1) as VBR8 (7 data bits + continue bit per 8-bit field, fields LSB
// first). Any replacement in [2^30, 2^31) has the same 5-field length, so the bitstream stays valid. Changing the
// HASH part (shader identity) and the container digest alone was not enough: NVIDIA's driver still served those
// PSOs from its cache (~1 ms instead of ~35 ms).

constexpr u32 PSO_SEED = 0x5EED1234u;
constexpr u32 VBR8_BITS = 40;

static void vbr8_bits(u32 v, u8 bits[VBR8_BITS]) {
    u64 x = (u64)v << 1;
    for (u32 f = 0; f < 5; ++f, x >>= 7) {
        const u32 field = (u32)(x & 0x7F) | (f < 4 ? 0x80u : 0u);
        for (u32 b = 0; b < 8; ++b) bits[f * 8 + b] = (field >> b) & 1;
    }
}

static bool bit_get(const u8* d, u64 i) { return (d[i >> 3] >> (i & 7)) & 1; }
static void bit_put(u8* d, u64 i, bool v) { d[i >> 3] = (u8)((d[i >> 3] & ~(1u << (i & 7))) | ((u32)v << (i & 7))); }

// Bit offset (from the container start) of the SEED literal in the DXIL part, or 0 when not found exactly once.
static u64 find_seed(const u8* c, u32 size) {
    u32 parts;
    memcpy(&parts, c + 28, 4);
    u8 pat[VBR8_BITS];
    vbr8_bits(PSO_SEED, pat);
    u64 found = 0;
    u32 hits = 0;
    for (u32 i = 0; i < parts && 32 + i * 4 + 4 <= size; ++i) {
        u32 off, len;
        memcpy(&off, c + 32 + i * 4, 4);
        if (off + 8 > size || memcmp(c + off, "DXIL", 4)) continue;
        memcpy(&len, c + off + 4, 4);
        const u64 begin = (u64)(off + 8) * 8, end = (u64)(off + 8 + (len < size - off - 8 ? len : size - off - 8)) * 8;
        for (u64 s = begin; s + VBR8_BITS <= end; ++s) {
            u32 k = 0;
            while (k < VBR8_BITS && bit_get(c, s + k) == pat[k]) ++k;
            if (k == VBR8_BITS) { found = s; ++hits; }
        }
    }
    return hits == 1 ? found : 0;
}

// Copies the container with a new SEED literal (from `seed`), marks its HASH part and recomputes the checksum.
static void make_unique(const u8* src, u32 size, u64 seed_bit, u8* dst, u64 seed) {
    memcpy(dst, src, size);
    u8 bits[VBR8_BITS];
    vbr8_bits(0x40000000u | (u32)(seed & 0x3FFFFFFFu), bits);
    for (u32 k = 0; k < VBR8_BITS; ++k) bit_put(dst, seed_bit + k, bits[k]);
    u32 parts;
    memcpy(&parts, dst + 28, 4);
    for (u32 i = 0; i < parts && 32 + i * 4 + 4 <= size; ++i) {
        u32 off;
        memcpy(&off, dst + 32 + i * 4, 4);
        if (off + 8 + 20 <= size && !memcmp(dst + off, "HASH", 4)) memcpy(dst + off + 8 + 4, &seed, 8);    // after the flags
    }
    u32 digest[4];
    container_digest(dst + 20, size - 20, digest);
    memcpy(dst + 4, digest, 16);
}

// ---- test ------------------------------------------------------------------------------------------------

struct PsoCtx {
    ID3D12RootSignature* root;
    u8* blobs;                      // GE_PSO_MAX x (cs, vs, ps) unique containers
    u32 stride;
    f64 ms[GE_PSO_MAX];
};

static const u8* blob(PsoCtx* c, u32 i, u32 which) { return c->blobs + ((u64)i * 3 + which) * c->stride; }

static void fill_compute(PsoCtx* c, u32 i, D3D12_COMPUTE_PIPELINE_STATE_DESC* pd) {
    *pd = {};
    pd->pRootSignature = c->root;
    pd->CS = { blob(c, i, 0), sizeof(g_cs_pso_dxil) };
}

// A typical blended mesh pass: RGBA8 + D32, depth test, alpha blend, back-face culling.
static void fill_graphics(PsoCtx* c, u32 i, D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd) {
    *pd = {};
    pd->pRootSignature = c->root;
    pd->VS = { blob(c, i, 1), sizeof(g_vs_pso_dxil) };
    pd->PS = { blob(c, i, 2), sizeof(g_ps_pso_dxil) };
    D3D12_RENDER_TARGET_BLEND_DESC* b = &pd->BlendState.RenderTarget[0];
    b->BlendEnable = TRUE;
    b->SrcBlend = D3D12_BLEND_SRC_ALPHA;
    b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOp = D3D12_BLEND_OP_ADD;
    b->SrcBlendAlpha = D3D12_BLEND_ONE;
    b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b->LogicOp = D3D12_LOGIC_OP_NOOP;
    b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd->SampleMask = 0xFFFFFFFFu;
    pd->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd->RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pd->RasterizerState.DepthClipEnable = TRUE;
    pd->DepthStencilState.DepthEnable = TRUE;
    pd->DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd->DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd->DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd->PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd->NumRenderTargets = 1;
    pd->RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd->SampleDesc.Count = 1;
}

struct PassResult {
    u32 count;
    f64 rate;           // pipelines per second
    f64 max_ms;
    bool ok;
};

// Creates PSOs 0..max (cold: CreateXxxPipelineState + StorePipeline into lib; cached: LoadXxxPipeline from lib)
// until the budget runs out; the first one always counts.
static PassResult run_pass(D3d* d, PsoCtx* c, bool graphics, ID3D12PipelineLibrary* lib, bool cached, u32 max, f64 budget_s,
                           const char* phase, BenchLive* live, const CancelFlag* cancel, f32 p0, f32 p1) {
    PassResult r = { 0, 0.0, 0.0, true };
    live_phase(live, phase, "PSO/s");
    f64 total = 0.0;
    const f64 t_start = timer_now();
    for (u32 i = 0; i < max && !cancel->load(std::memory_order_relaxed); ++i) {
        if (i && timer_now() - t_start >= budget_s) break;
        wchar_t name[16];
        swprintf(name, ARRAY_COUNT(name), L"%c%u", graphics ? L'g' : L'c', i);
        ID3D12PipelineState* pso = nullptr;
        HRESULT hr;
        const f64 t0 = timer_now();
        if (graphics) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
            fill_graphics(c, i, &pd);
            hr = cached ? lib->LoadGraphicsPipeline(name, &pd, IID_PPV_ARGS(&pso)) : d->device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso));
        } else {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd;
            fill_compute(c, i, &pd);
            hr = cached ? lib->LoadComputePipeline(name, &pd, IID_PPV_ARGS(&pso)) : d->device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
        }
        const f64 ms = (timer_now() - t0) * 1000.0;
        if (FAILED(hr) || !pso) { r.ok = false; break; }
        if (!cached && lib) lib->StorePipeline(name, pso);      // outside the timed part
        pso->Release();
        c->ms[i] = ms;
        total += ms;
        r.max_ms = ms > r.max_ms ? ms : r.max_ms;
        r.count = i + 1;
        live_sample(live, 1000.0 / ms);
        gpu_ext_progress(live, timer_now() - t_start, budget_s, p0, p1);
    }
    r.rate = total > 0.0 ? r.count / (total / 1000.0) : 0.0;
    return r;
}

void d3x_pso_test(D3d* d, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    const u64 seed_bit[3] = { find_seed(g_cs_pso_dxil, sizeof(g_cs_pso_dxil)), find_seed(g_vs_pso_dxil, sizeof(g_vs_pso_dxil)),
                              find_seed(g_ps_pso_dxil, sizeof(g_ps_pso_dxil)) };
    if (!container_ok(g_cs_pso_dxil, sizeof(g_cs_pso_dxil)) || !seed_bit[0] || !seed_bit[1] || !seed_bit[2]) {
        res_fail(out, "unexpected DXIL layout (checksum or SEED literal), can't make unique shaders");
        return;
    }
    ID3D12Device1* dev1 = nullptr;
    if (FAILED(d->device->QueryInterface(IID_PPV_ARGS(&dev1)))) {
        res_skip(out, "ID3D12Device1 (pipeline libraries) not available");
        return;
    }
    PsoCtx* c = (PsoCtx*)calloc(1, sizeof(PsoCtx));
    u32 stride = (u32)sizeof(g_cs_pso_dxil);
    stride = sizeof(g_vs_pso_dxil) > stride ? (u32)sizeof(g_vs_pso_dxil) : stride;
    stride = sizeof(g_ps_pso_dxil) > stride ? (u32)sizeof(g_ps_pso_dxil) : stride;
    if (c) {
        c->stride = (stride + 15) & ~15u;
        c->blobs = (u8*)malloc((u64)GE_PSO_MAX * 3 * c->stride);
        c->root = d3x_root(d);
    }
    bool ok = c && c->blobs && c->root;

    const f64 budget = p->duration_ms / 1000.0 * 0.4;
    PassResult res[2][2] = {};     // [compute, graphics][cold, cached]
    bool no_library = false;
    for (u32 g = 0; g < 2 && ok && !no_library && !cancel->load(std::memory_order_relaxed); ++g) {
        for (u32 i = 0; i < GE_PSO_MAX; ++i) {
            const u64 seed = gpu_ext_pso_seed();
            make_unique(g_cs_pso_dxil, sizeof(g_cs_pso_dxil), seed_bit[0], (u8*)blob(c, i, 0), seed);
            make_unique(g_vs_pso_dxil, sizeof(g_vs_pso_dxil), seed_bit[1], (u8*)blob(c, i, 1), seed);
            make_unique(g_ps_pso_dxil, sizeof(g_ps_pso_dxil), seed_bit[2], (u8*)blob(c, i, 2), seed);
        }
        ID3D12PipelineLibrary* lib = nullptr;
        if (!ok) break;
        const HRESULT hr = dev1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&lib));
        if (hr == DXGI_ERROR_UNSUPPORTED) { no_library = true; break; }
        if (FAILED(hr)) { ok = false; break; }
        const f32 base = g * 0.5f;
        res[g][0] = run_pass(d, c, g == 1, lib, false, GE_PSO_MAX, budget, g ? "Graphics, cold" : "Compute, cold", live, cancel,
                             base, base + 0.4f);
        // Serialize and reopen the library (a game's pipeline cache file on the next start).
        const SIZE_T size = lib->GetSerializedSize();
        void* data = size ? malloc(size) : nullptr;
        ok = res[g][0].ok && data && SUCCEEDED(lib->Serialize(data, size));
        lib->Release();
        lib = nullptr;
        if (ok) ok = SUCCEEDED(dev1->CreatePipelineLibrary(data, size, IID_PPV_ARGS(&lib)));
        if (ok) {
            res[g][1] = run_pass(d, c, g == 1, lib, true, res[g][0].count, 1e9, g ? "Graphics, cached" : "Compute, cached", live,
                                 cancel, base + 0.4f, base + 0.5f);
            ok = res[g][1].ok;
        }
        d3d_release(lib);
        free(data);         // the library references it until released
    }

    dev1->Release();
    if (c) {
        d3d_release(c->root);
        free(c->blobs);
        free(c);
    }
    if (no_library) { res_skip(out, "the driver doesn't support pipeline libraries"); return; }
    if (!ok) { res_fail(out, "pipeline creation failed"); return; }
    if (cancel->load(std::memory_order_relaxed)) return;
    res_begin(out, RESULT_VALUES);
    res_add(out, "Compute, cold", "PSO/s", res[0][0].rate);
    res_add(out, "Compute, cached", "PSO/s", res[0][1].rate);
    res_add(out, "Compute, cold, slowest", "ms", res[0][0].max_ms);
    res_add(out, "Graphics, cold", "PSO/s", res[1][0].rate);
    res_add(out, "Graphics, cached", "PSO/s", res[1][1].rate);
    res_add(out, "Graphics, cold, slowest", "ms", res[1][0].max_ms);
}
