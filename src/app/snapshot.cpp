// PNG screenshots of our own windows from the D3D11 back buffer (see snapshot.h).
#include "app/snapshot.h"
#include "app/ui.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>

struct Snapshot {
    u32 window;             // pending request, UINT32_MAX = none
    wchar_t path[MAX_PATH];
    u32 status_window;
    char status[400];
    bool status_new;
};
static Snapshot s_snap = { UINT32_MAX };

void snapshot_request(u32 window, const wchar_t* path) {
    s_snap.window = window;
    wcscpy_s(s_snap.path, MAX_PATH, path);
}

void snapshot_export_path(const char* what, const wchar_t* ext, wchar_t* path) {
    wchar_t dir[MAX_PATH];
    ui_results_dir(dir);
    time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    wchar_t wwhat[64];
    MultiByteToWideChar(CP_UTF8, 0, what, -1, wwhat, 64);
    swprintf(path, MAX_PATH, L"%s\\ezbench_%04d%02d%02d_%02d%02d%02d_%s.%s", dir, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec, wwhat, ext);
}

// RGBA rows (pitch bytes apart) -> 24-bit PNG.
static bool write_png(const wchar_t* path, const u8* pixels, u32 w, u32 h, u32 pitch) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
              SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR);
    if (ok) {
        const u32 row_bytes = (w * 3 + 3) & ~3u;
        u8* row = (u8*)VirtualAlloc(nullptr, (SIZE_T)row_bytes * h, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        ok = row != nullptr;
        for (u32 y = 0; ok && y < h; ++y) {
            const u8* src = pixels + (u64)y * pitch;
            u8* dst = row + (u64)y * row_bytes;
            for (u32 x = 0; x < w; ++x) {
                dst[x * 3 + 0] = src[x * 4 + 2];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 0];
            }
        }
        ok = ok && SUCCEEDED(frame->WritePixels(h, row_bytes, row_bytes * h, row)) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(enc->Commit());
        if (row) VirtualFree(row, 0, MEM_RELEASE);
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) DeleteFileW(path);
    return ok;
}

void snapshot_capture(u32 window, ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc) {
    if (s_snap.window != window) return;
    s_snap.window = UINT32_MAX;
    ID3D11Texture2D* back = nullptr;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back)));
    D3D11_TEXTURE2D_DESC td = {};
    if (ok) {
        back->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        ok = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM || td.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
             SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging));
    }
    if (ok) {
        ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE map = {};
        ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map));
        if (ok) {
            ok = write_png(s_snap.path, (const u8*)map.pData, td.Width, td.Height, map.RowPitch);
            ctx->Unmap(staging, 0);
        }
    }
    if (staging) staging->Release();
    if (back) back->Release();
    char path8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, s_snap.path, -1, path8, sizeof(path8), nullptr, nullptr);
    snprintf(s_snap.status, sizeof(s_snap.status), ok ? "Saved %s" : "Failed to save %s", path8);
    s_snap.status_window = window;
    s_snap.status_new = true;
}

bool snapshot_take_status(u32 window, char* buf, u32 size) {
    if (!s_snap.status_new || s_snap.status_window != window) return false;
    s_snap.status_new = false;
    snprintf(buf, size, "%s", s_snap.status);
    return true;
}
