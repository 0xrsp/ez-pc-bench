#pragma once
// On-screen mode for the 3D / 2D GPU tests (BenchParams::gpu_onscreen). The test still renders into its
// offscreen target exactly as in offscreen mode; each frame then copies that target into a flip-model
// swapchain on a benchmark window and presents it with vsync off (D3D12: DXGI_PRESENT_ALLOW_TEARING,
// Vulkan: IMMEDIATE, else MAILBOX). Frame time = QPC between consecutive presents (present-to-present).
// Where the API reports it, frames confirmed on screen go into a display log and add "Displayed ..." /
// "Display latency" metrics:
//   D3D12: IDXGISwapChain::GetFrameStatistics after every present; each time its SyncQPCTime (the vblank of
//          the last screen update) changes, its PresentCount is logged as shown at that time. Frames
//          replaced before a screen update never appear: "Displayed frames" = screen updates / presents.
//   Vulkan: VK_KHR_present_id + VK_KHR_present_wait (NVIDIA; not on the 780M), polled with timeout 0 after
//          every present, so the time is when the poll saw it (frame-time granularity).
//
// The window lives on the benchmark worker thread (created, pumped and destroyed there), never touches
// the UI window, gives the foreground back to the previous window when it closes, and stops the test
// early when the user presses Esc or closes it.
#include "gpu/gpu_common.h"
#include <windows.h>

struct PresentWindow {
    HWND hwnd;
    HWND prev_foreground;       // re-activated when the window closes
    u32 w, h;                   // client size in pixels
    bool fullscreen;            // borderless, covering the monitor
    bool closed;                // Esc / Alt+F4 / close button
};

// Creates and shows the window for `test_name` on the calling thread. Fullscreen covers the monitor of the
// current foreground window; windowed has a gpu_width x gpu_height client area (clamped to the work area).
bool present_window_open(PresentWindow* w, const BenchParams* p, const char* api, const char* test_name,
                         char* err, u32 err_size);
bool present_window_pump(PresentWindow* w);     // dispatches pending messages; false once the user closed it
void present_window_close(PresentWindow* w);

// Display log of the measured frames. Ids are the backend's present ids (increasing by one per present).
void present_log_begin(u64 first_id);           // at the start of the measured phase
void present_log_presented(u64 id, f64 t);      // QPC seconds right after the present call returned
void present_log_displayed(u64 id, f64 t);      // QPC seconds when that present reached the screen
// Adds "Displayed frames" (% of presents seen on screen), "Display latency avg / p99" (present call to
// on screen) and "Displayed frame time avg / p99" (interval between distinct screen updates). Adds nothing
// when no present was confirmed (the API doesn't report it).
void present_log_report(BenchResult* out);

const char* present_mode_name(const BenchParams* p);    // "offscreen", "windowed", "fullscreen"
const char* gpu_test_name(GpuTestId test);              // UI name from GPU_TESTS
