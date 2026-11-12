// PNG screenshots of our own windows from the D3D11 back buffer (see snapshot.h).
#include "app/snapshot.h"
#include "app/ui.h"
#include <windows.h>
#include <objbase.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

struct Snapshot {
    u32 window;             // pending request, UINT32_MAX = none
    bool clipboard;         // copy instead of writing `path`
    wchar_t path[MAX_PATH];
    u32 status_window;
    char status[400];
    bool status_new;
};
static Snapshot s_snap = { UINT32_MAX };

void snapshot_request(u32 window, const wchar_t* path) {
    s_snap.window = window;
    s_snap.clipboard = path == nullptr;
    s_snap.path[0] = 0;
    if (path) wcscpy_s(s_snap.path, MAX_PATH, path);
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

// RGBA rows (pitch bytes apart) -> top-down 24-bit BGR rows of `row_bytes` (DWORD-aligned). VirtualFree it.
static u8* to_bgr(const u8* pixels, u32 w, u32 h, u32 pitch, u32 row_bytes) {
    u8* out = (u8*)VirtualAlloc(nullptr, (SIZE_T)row_bytes * h, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (u32 y = 0; out && y < h; ++y) {
        const u8* src = pixels + (u64)y * pitch;
        u8* dst = out + (u64)y * row_bytes;
        for (u32 x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }
    return out;
}

static bool encode_png(IWICImagingFactory* factory, IStream* out, u8* bgr, u32 w, u32 h, u32 row_bytes) {
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    const bool ok = SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(out, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) &&
                    SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) &&
                    SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
                    SUCCEEDED(frame->WritePixels(h, row_bytes, row_bytes * h, bgr)) && SUCCEEDED(frame->Commit()) &&
                    SUCCEEDED(enc->Commit());
    if (frame) frame->Release();
    if (enc) enc->Release();
    return ok;
}

static HGLOBAL global_alloc(SIZE_T size, u8** p) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size);
    *p = g ? (u8*)GlobalLock(g) : nullptr;
    if (g && !*p) { GlobalFree(g); g = nullptr; }
    return g;
}

// PNG bytes held by a memory stream -> an exact-size movable global (clipboard data), or null.
static HGLOBAL png_global(IStream* mem) {
    HGLOBAL src = nullptr;
    STATSTG st = {};
    if (FAILED(GetHGlobalFromStream(mem, &src)) || FAILED(mem->Stat(&st, STATFLAG_NONAME)) || !st.cbSize.QuadPart) return nullptr;
    const SIZE_T size = (SIZE_T)st.cbSize.QuadPart;
    u8* dst;
    HGLOBAL g = global_alloc(size, &dst);
    const void* p = GlobalLock(src);
    if (g && p) memcpy(dst, p, size);
    if (p) GlobalUnlock(src);
    if (g) GlobalUnlock(g);
    if (g && !p) { GlobalFree(g); g = nullptr; }
    return g;
}

// CF_DIB (bottom-up 24-bit; Windows synthesizes CF_DIBV5 / CF_BITMAP from it) plus the registered "PNG" format
// that browsers, Office and chat apps prefer.
static bool copy_to_clipboard(HWND owner, const u8* bgr, u32 w, u32 h, u32 row_bytes, HGLOBAL png) {
    u8* p;
    HGLOBAL dib = global_alloc(sizeof(BITMAPINFOHEADER) + (SIZE_T)row_bytes * h, &p);
    if (dib) {
        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(bi);
        bi.biWidth = (LONG)w;
        bi.biHeight = (LONG)h;
        bi.biPlanes = 1;
        bi.biBitCount = 24;
        bi.biCompression = BI_RGB;
        bi.biSizeImage = row_bytes * h;
        memcpy(p, &bi, sizeof(bi));
        for (u32 y = 0; y < h; ++y) memcpy(p + sizeof(bi) + (u64)(h - 1 - y) * row_bytes, bgr + (u64)y * row_bytes, row_bytes);
        GlobalUnlock(dib);
    }
    bool ok = false;
    bool open = false;
    for (u32 tries = 0; dib && !(open = OpenClipboard(owner) != 0) && tries < 5; ++tries) Sleep(10);   // another app may hold it
    if (open) {
        EmptyClipboard();
        const UINT cf_png = RegisterClipboardFormatW(L"PNG");
        if (png && cf_png && SetClipboardData(cf_png, png)) png = nullptr;     // owned by the clipboard now
        if (SetClipboardData(CF_DIB, dib)) { dib = nullptr; ok = true; }
        CloseClipboard();
    }
    if (dib) GlobalFree(dib);
    if (png) GlobalFree(png);
    return ok;
}

// Back buffer pixels -> PNG file, or clipboard (DIB + PNG).
static bool save_pixels(const u8* pixels, u32 w, u32 h, u32 pitch, HWND owner) {
    const u32 row_bytes = (w * 3 + 3) & ~3u;
    u8* bgr = to_bgr(pixels, w, h, pitch, row_bytes);
    if (!bgr) return false;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IWICImagingFactory* factory = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    if (ok && !s_snap.clipboard) {
        IWICStream* file = nullptr;
        ok = SUCCEEDED(factory->CreateStream(&file)) && SUCCEEDED(file->InitializeFromFilename(s_snap.path, GENERIC_WRITE)) &&
             encode_png(factory, file, bgr, w, h, row_bytes);
        if (file) file->Release();
        if (!ok) DeleteFileW(s_snap.path);
    } else if (ok) {
        IStream* mem = nullptr;
        HGLOBAL png = nullptr;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) && encode_png(factory, mem, bgr, w, h, row_bytes))
            png = png_global(mem);
        if (mem) mem->Release();
        ok = copy_to_clipboard(owner, bgr, w, h, row_bytes, png);   // the DIB alone is still useful without the PNG
    }
    if (factory) factory->Release();
    if (SUCCEEDED(co)) CoUninitialize();
    VirtualFree(bgr, 0, MEM_RELEASE);
    return ok;
}

void snapshot_capture(u32 window, ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc) {
    if (s_snap.window != window) return;
    s_snap.window = UINT32_MAX;
    ID3D11Texture2D* back = nullptr;
    ID3D11Texture2D* staging = nullptr;
    DXGI_SWAP_CHAIN_DESC scd = {};
    bool ok = SUCCEEDED(sc->GetDesc(&scd)) && SUCCEEDED(sc->GetBuffer(0, IID_PPV_ARGS(&back)));
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
            ok = save_pixels((const u8*)map.pData, td.Width, td.Height, map.RowPitch, scd.OutputWindow);
            ctx->Unmap(staging, 0);
        }
    }
    if (staging) staging->Release();
    if (back) back->Release();
    if (s_snap.clipboard) {
        if (ok) snprintf(s_snap.status, sizeof(s_snap.status), "Copied a %u x %u PNG of this window to the clipboard", td.Width, td.Height);
        else snprintf(s_snap.status, sizeof(s_snap.status), "Failed to copy the window to the clipboard");
    } else {
        char path8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, s_snap.path, -1, path8, sizeof(path8), nullptr, nullptr);
        snprintf(s_snap.status, sizeof(s_snap.status), ok ? "Saved %s" : "Failed to save %s", path8);
    }
    s_snap.status_window = window;
    s_snap.status_new = true;
}

bool snapshot_take_status(u32 window, char* buf, u32 size) {
    if (!s_snap.status_new || s_snap.status_window != window) return false;
    s_snap.status_new = false;
    snprintf(buf, size, "%s", s_snap.status);
    return true;
}
