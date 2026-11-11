// Image workloads through WIC (Windows Imaging Component, built into Windows): JPEG and PNG decode, and a
// high-quality cubic downscale. The test image is generated procedurally and encoded in memory once (no
// binary assets). Every pool thread uses its own WIC factory in the multithreaded apartment.
#include "cpu/cpu_internal.h"
#include "core/mem_util.h"
#include "core/results.h"
#include "core/threads.h"
#include "core/throughput.h"
#include <windows.h>
#include <wincodec.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static constexpr u32 IMG_W = 1920, IMG_H = 1080, IMG_STRIDE = IMG_W * 3;
static constexpr u64 IMG_BYTES = (u64)IMG_STRIDE * IMG_H;
static constexpr u32 OUT_W = 1280, OUT_H = 720;     // resize target

struct ImageData {
    u8* pixels;             // 24bpp BGR source
    u8* jpeg; u64 jpeg_size;
    u8* png; u64 png_size;
    bool ready, failed;
};
static ImageData s_img;

struct ImgCtx {
    IWICImagingFactory* fac[MAX_LP];
    IWICBitmap* src[MAX_LP];        // resize: per-thread copy of the source pixels
    u8* out;                        // per thread IMG_BYTES
    bool png;                       // decode: which file
    bool failed;
};

static IWICImagingFactory* thread_factory(ImgCtx* c, u32 index) {
    if (!c->fac[index]) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);     // pool threads live forever; never uninitialized
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&c->fac[index]))))
            c->fac[index] = nullptr;
    }
    return c->fac[index];
}

// Smooth gradients, a few discs and some noise: compresses roughly like a photo (JPEG q90 ~0.5 MB).
static void make_pixels(u8* px) {
    u64 s = 0x2545F4914F6CDD1Dull;
    for (u32 y = 0; y < IMG_H; ++y) {
        for (u32 x = 0; x < IMG_W; ++x) {
            f32 r = 40.0f + 180.0f * x / IMG_W, g = 60.0f + 150.0f * y / IMG_H, b = 120.0f + 80.0f * sinf(x * 0.01f + y * 0.007f);
            for (u32 k = 0; k < 6; ++k) {
                const f32 cx = 200.0f + 300.0f * k, cy = 300.0f + 90.0f * k, rad = 120.0f + 25.0f * k;
                const f32 dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy < rad * rad) { r = 255.0f - 30.0f * k; g = 40.0f * k; b = 200.0f - 20.0f * k; }
            }
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            const f32 n = (f32)(s % 17) - 8.0f;
            u8* p = px + (u64)y * IMG_STRIDE + x * 3;
            const f32 v[3] = { b + n, g + n, r + n };
            for (u32 c = 0; c < 3; ++c) p[c] = (u8)(v[c] < 0.0f ? 0.0f : v[c] > 255.0f ? 255.0f : v[c]);
        }
    }
}

static bool encode(IWICImagingFactory* fac, const GUID& container, f32 quality, u8** out, u64* out_size) {
    IStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) &&
              SUCCEEDED(fac->CreateEncoder(container, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, &props));
    if (ok && quality > 0.0f) {
        PROPBAG2 opt = {};
        opt.pstrName = (LPOLESTR)L"ImageQuality";
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = quality;
        ok = SUCCEEDED(props->Write(1, &opt, &v));
    }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(frame->Initialize(props)) && SUCCEEDED(frame->SetSize(IMG_W, IMG_H)) &&
         SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
         SUCCEEDED(frame->WritePixels(IMG_H, IMG_STRIDE, (UINT)IMG_BYTES, s_img.pixels)) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    HGLOBAL hg = nullptr;
    if (ok && SUCCEEDED(GetHGlobalFromStream(stream, &hg))) {
        STATSTG st = {};
        stream->Stat(&st, STATFLAG_NONAME);
        *out_size = st.cbSize.QuadPart;
        *out = (u8*)big_alloc(*out_size);
        const void* src = GlobalLock(hg);
        ok = *out && src;
        if (ok) memcpy(*out, src, *out_size);
        GlobalUnlock(hg);
    } else {
        ok = false;
    }
    if (props) props->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    return ok;
}

static void prepare_task(u32, u32, void* user) {
    ImgCtx* c = (ImgCtx*)user;
    IWICImagingFactory* fac = thread_factory(c, 0);
    s_img.pixels = (u8*)big_alloc(IMG_BYTES);
    if (!fac || !s_img.pixels) { s_img.failed = true; return; }
    make_pixels(s_img.pixels);
    s_img.failed = !encode(fac, GUID_ContainerFormatJpeg, 0.9f, &s_img.jpeg, &s_img.jpeg_size) ||
                   !encode(fac, GUID_ContainerFormatPng, 0.0f, &s_img.png, &s_img.png_size);
}

// Decodes the JPEG or PNG into `out` as 24bpp BGR.
static bool decode(IWICImagingFactory* fac, bool png, u8* out) {
    IWICStream* ws = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    WICPixelFormatGUID fmt = {};
    bool ok = SUCCEEDED(fac->CreateStream(&ws)) &&
              SUCCEEDED(ws->InitializeFromMemory(png ? s_img.png : s_img.jpeg, (DWORD)(png ? s_img.png_size : s_img.jpeg_size))) &&
              SUCCEEDED(fac->CreateDecoderFromStream(ws, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(frame->GetPixelFormat(&fmt));
    if (ok && IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR)) {
        ok = SUCCEEDED(frame->CopyPixels(nullptr, IMG_STRIDE, (UINT)IMG_BYTES, out));
    } else if (ok) {
        ok = SUCCEEDED(fac->CreateFormatConverter(&conv)) &&
             SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeCustom)) &&
             SUCCEEDED(conv->CopyPixels(nullptr, IMG_STRIDE, (UINT)IMG_BYTES, out));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (ws) ws->Release();
    return ok;
}

static f64 kern_decode(void* ctx, u32 index, u32) {
    ImgCtx* c = (ImgCtx*)ctx;
    IWICImagingFactory* fac = thread_factory(c, index);
    if (!fac || !decode(fac, c->png, c->out + (u64)index * IMG_BYTES)) c->failed = true;
    return (f64)IMG_W * IMG_H;
}

static f64 kern_resize(void* ctx, u32 index, u32) {
    ImgCtx* c = (ImgCtx*)ctx;
    IWICImagingFactory* fac = thread_factory(c, index);
    if (!fac) { c->failed = true; return 0.0; }
    if (!c->src[index] && FAILED(fac->CreateBitmapFromMemory(IMG_W, IMG_H, GUID_WICPixelFormat24bppBGR, IMG_STRIDE,
                                                             (UINT)IMG_BYTES, s_img.pixels, &c->src[index]))) {
        c->failed = true;
        return 0.0;
    }
    IWICBitmapScaler* sc = nullptr;
    const bool ok = SUCCEEDED(fac->CreateBitmapScaler(&sc)) &&
                    SUCCEEDED(sc->Initialize(c->src[index], OUT_W, OUT_H, WICBitmapInterpolationModeHighQualityCubic)) &&
                    SUCCEEDED(sc->CopyPixels(nullptr, OUT_W * 3, OUT_W * 3 * OUT_H, c->out + (u64)index * IMG_BYTES));
    if (sc) sc->Release();
    if (!ok) c->failed = true;
    return (f64)IMG_W * IMG_H;
}

// Mean absolute difference per channel between two BGR images of the same size.
static f64 mean_abs_diff(const u8* a, const u8* b, u64 bytes) {
    u64 sum = 0;
    for (u64 i = 0; i < bytes; ++i) sum += (u64)(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
    return (f64)sum / bytes;
}

static f64 mean(const u8* a, u64 bytes) {
    u64 sum = 0;
    for (u64 i = 0; i < bytes; ++i) sum += a[i];
    return (f64)sum / bytes;
}

enum ImgKind { IMG_JPEG, IMG_PNG, IMG_RESIZE };

static void img_test(ImgKind kind, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    static ImgCtx c;
    for (u32 i = 0; i < MAX_LP; ++i) if (c.src[i]) { c.src[i]->Release(); c.src[i] = nullptr; }
    c.png = kind == IMG_PNG;
    c.failed = false;
    if (!s_img.ready) {
        live_phase(live, "Encoding test image", "");
        pool_run(1, prepare_task, &c);
        s_img.ready = true;
    }
    if (s_img.failed) { res_fail(out, "WIC: could not create the test image"); return; }
    c.out = (u8*)big_alloc((u64)thread_count_for(p->thread_count) * IMG_BYTES);
    if (!c.out) { res_fail(out, "out of memory"); return; }

    st_mt_test(kind == IMG_RESIZE ? kern_resize : kern_decode, &c, "MPix/s", 1e6, p, live, out, cancel);
    // Check thread 0's last output against the source.
    if (c.failed) {
        res_fail(out, "WIC call failed");
    } else if (kind == IMG_PNG) {
        if (memcmp(c.out, s_img.pixels, IMG_BYTES) != 0) res_fail(out, "PNG decode differs from the source");
    } else if (kind == IMG_JPEG) {
        const f64 err = mean_abs_diff(c.out, s_img.pixels, IMG_BYTES);
        if (err > 8.0) res_fail(out, "JPEG decode too far from the source (mean error %.2f levels)", err);
    } else {
        const f64 a = mean(s_img.pixels, IMG_BYTES), b = mean(c.out, (u64)OUT_W * OUT_H * 3);
        if (fabs(a - b) > 0.02 * a) res_fail(out, "resized image brightness %.1f vs source %.1f", b, a);
    }
    big_free(c.out);
    c.out = nullptr;
}

void cpu_img_jpeg(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    img_test(IMG_JPEG, p, live, out, cancel);
}
void cpu_img_png(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    img_test(IMG_PNG, p, live, out, cancel);
}
void cpu_img_resize(void*, const BenchParams* p, BenchLive* live, BenchResult* out, const CancelFlag* cancel) {
    img_test(IMG_RESIZE, p, live, out, cancel);
}
