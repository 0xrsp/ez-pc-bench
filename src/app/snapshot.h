#pragma once
// Window screenshots as PNG (FUTURE.md section 11, CDM-style sharing): a window asks for one, and after its next
// frame is rendered (before Present) main_gui.cpp hands the back buffer to snapshot_capture, which encodes it
// with WIC (part of Windows) to a file or to the clipboard.
#include "core/types.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;

constexpr u32 SNAP_MAIN = 0xFF;     // the main window; tool windows use their ToolWindow id

// `path` = PNG file to write; null = copy to the clipboard instead (CF_DIB and the registered "PNG" format).
void snapshot_request(u32 window, const wchar_t* path);
// main_gui.cpp: after rendering `window`'s frame. Writes / copies the PNG if one was requested for it.
void snapshot_capture(u32 window, ID3D11Device* dev, ID3D11DeviceContext* ctx, IDXGISwapChain* sc);
// Status of `window`'s last capture ("Saved ...png" / "Copied ..." / "Failed ..."), once; false if nothing new.
bool snapshot_take_status(u32 window, char* buf, u32 size);
// "<exe dir>\results\ezbench_<date>_<time>_<what>.<ext>" for exports.
void snapshot_export_path(const char* what, const wchar_t* ext, wchar_t* path);
