#pragma once
// Frame capture of other applications (PresentMon / CapFrameX style) from ETW present events.
//
// A real-time ETW session enables two manifest providers, filtered to their present events:
//   Microsoft-Windows-DXGI    {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}  keyword Events (0x2):
//                             42 Present_Start / 43 Present_Stop, 55 / 56 PresentMultiplaneOverlay
//   Microsoft-Windows-DxgKrnl {802EC45A-1E99-4B83-9920-87C98277BA9D}  keyword Present (0x8000000):
//                             168 Flip_Info, 171 PresentHistory_Start, 184 Present_Info,
//                             215 PresentHistoryDetailed_Start (kernel side; covers Vulkan / OpenGL / D3D9)
// Events of the target process(es) are stored raw; capture_analyze() then picks the process with the most
// presents, prefers DXGI (frame time = time between consecutive Present_Start of its busiest swapchain,
// PresentMon's MsBetweenPresents) and falls back to the kernel event it logged most. Display times
// (flip / vsync tracking) are not reconstructed.
//
// Creating the session needs administrator rights or membership in "Performance Log Users".
#include "core/types.h"
#include <atomic>

enum FrameEventKind : u8 {
    FE_DXGI_START, FE_DXGI_STOP,                    // Present_Start / Present_Stop (incl. the MPO variants)
    FE_K_FLIP, FE_K_HISTORY, FE_K_PRESENT, FE_K_HISTORY_DETAILED,
    FE_KIND_COUNT
};

struct FrameEvent {
    u64 time;           // 100 ns units (FILETIME), as ETW delivers them
    u64 swapchain;      // DXGI: IDXGISwapChain pointer; 0 otherwise
    u32 pid, tid;
    u32 flags;          // DXGI Present flags (0x200 = ALLOW_TEARING)
    u16 sync_interval;  // DXGI SyncInterval
    u8 kind;            // FrameEventKind
};

constexpr u32 CAPTURE_MAX_EVENTS = 1u << 20;
constexpr u32 CAPTURE_MAX_PIDS = 32;

// Result of one capture. frame_ms / frame_t point into the capture's own buffers (valid until the next
// capture_start / capture_load_etl).
struct FrameCapture {
    char process[64];               // exe name ("game.exe") or "pid 1234"
    u32 pid;
    char api[24];                   // "DXGI" or "DxgKrnl <event>"
    u64 swapchain;
    f64 seconds;                    // first to last frame
    u32 frames;                     // frame intervals (presents - 1)
    const f32* frame_ms;            // [frames] time since the previous present
    const f32* frame_t;             // [frames] seconds since the first present (end of each interval)
    f64 avg_ms, p50_ms, p95_ms, p99_ms, p999_ms, max_ms;
    f64 avg_fps;                    // frames / seconds
    f64 low1_avg_fps, low01_avg_fps;    // average fps of the slowest 1 % / 0.1 % frames (CapFrameX "1% low average")
    f64 present_call_ms;            // DXGI: average time inside Present() (NaN when unknown)
    i32 sync_interval;              // most common DXGI SyncInterval (-1 = unknown)
    i32 tearing;                    // DXGI_PRESENT_ALLOW_TEARING on most presents: 1 / 0, -1 = unknown
    u32 events, dropped;            // stored / lost (buffer full or ETW lost events)
};

// ---- analysis (pure: also used by the self-test and the .etl reader) ----------------------------------

// Fills `out` (and its frame arrays: `ms` / `t` must hold n entries) from n events in time order. out->process
// may hold the exe name to keep (else "pid N"). `only_pid` = 0 picks the busiest presenting process.
// False (with err) when the chosen process has fewer than 2 presents.
bool capture_analyze(const FrameEvent* ev, u32 n, u32 only_pid, f32* ms, f32* t, FrameCapture* out, char* err, u32 err_size);

// ---- live capture ------------------------------------------------------------------------------------

// True when this process may start an ETW real-time session (elevated, or in Performance Log Users).
bool capture_allowed();
// Resolves "1234" or "game.exe" / "game" (case-insensitive, extension optional) to running process ids.
// Returns the count; `exe` gets the matched exe name.
u32 capture_find_pids(const char* target, u32* pids, u32 max, char* exe, u32 exe_size);

enum CaptureState : u8 { CAP_IDLE, CAP_RUNNING, CAP_DONE, CAP_FAILED };
struct CaptureLive {
    std::atomic<u8> state;          // CaptureState
    std::atomic<u32> events;        // target events so far
    std::atomic<u32> presents;      // target presents so far (DXGI, else kernel; approximate)
    f64 start, seconds;             // timer_now() at start, requested length
    char error[256];
};
extern CaptureLive g_capture;

// Starts a background capture of `target` for `seconds`. False (with err) if it can't start; access denied
// gets a message about administrator rights.
bool capture_start(const char* target, f64 seconds, char* err, u32 err_size);
void capture_stop();                // ends early; the result covers what was recorded
void capture_wait();                // until the capture thread has finished
const FrameCapture* capture_result();   // after CAP_DONE

// Reads present events from an .etl file (e.g. recorded with WPR / PresentMon) instead of a live session.
// `target` may be empty (busiest process), a pid or an exe name of a process that is running now.
bool capture_load_etl(const wchar_t* path, const char* target, char* err, u32 err_size);

// Results file: the usual TOON layout (system, params, tests, metrics: one test "capture.frames") plus
// `kind: frames`, a `capture` block and `frames[N]{t,ms}` with every frame time, so the compare window lists
// a capture like any other run and can draw its frame-time graph.
bool capture_write_toon(const wchar_t* path, const FrameCapture* c);
void capture_default_name(const FrameCapture* c, char* out, u32 size);     // frames_<date>_<time>_<exe>

// Decodes synthetic ETW records (DXGI + DxgKrnl, two processes, two swapchains) and checks the analysis.
bool capture_selftest(char* report, u32 size);
