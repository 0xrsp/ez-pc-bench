# GPU on-screen mode and frame capture (stream notes)

> **Removed 2026-11-11 (trim)**: frame capture of other applications (part B) is gone: `src/frames/`, the Frames tool window
> (`frames_ui.*`, `frames_graph.*`, `TOOL_FRAMES`, `ICON_FRAMES`, `--tool frames`), the CLI `--capture`,
> `--capture-etl`, `--capture-selftest`, and the compare window's frame-time graph / `frames[N]` table / `kind: frames`
> support (compare accepts `kind: benchmark` or no kind only). Part B is kept below as a record. The on-screen mode
> (part A, `--gpu-present`, `src/gpu/gpu_present*`) stays.

FUTURE.md §5: **P2 on-screen mode with a real swapchain** and **P3 frame capture of other applications**.
Measured 2026-11-11 on the target laptop (Ryzen 7 250, RTX 5060 Laptop + Radeon 780M, Optimus, panel on
the 780M), **on battery** (Balanced), so absolute numbers are low; the ratios are what matters.

## A. On-screen mode for the 3D / 2D tests

### What it does
- `BenchParams::gpu_onscreen` (new, default **false** = offscreen as before) + the existing
  `gpu_fullscreen` (was unused; now picks borderless fullscreen vs a window).
- The test renders exactly as offscreen (same target, same work, same preview), then each frame copies the
  shown target into a flip-model swapchain and presents with vsync off:
  - **D3D12** (`gpu_present_d3d12.cpp`): `CreateSwapChainForHwnd` on the direct queue, `FLIP_DISCARD`,
    3 buffers of `gpu_width x gpu_height` RGBA8, `DXGI_SCALING_STRETCH` to the window,
    `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` + `Present(0, DXGI_PRESENT_ALLOW_TEARING)` when
    `DXGI_FEATURE_PRESENT_ALLOW_TEARING` is supported (it is on both GPUs). Copy = `CopyResource`.
  - **Vulkan** (`gpu_present_vk.cpp`): `VK_KHR_win32_surface` swapchain sized to the window
    (`currentExtent`), present mode IMMEDIATE (tearing) > MAILBOX > FIFO, B8G8R8A8 / R8G8B8A8 UNORM,
    copy = `vkCmdBlitImage` (scales if the window differs from the render size). Per-slot acquire
    semaphores, per-image render-done semaphores, sync2 barriers.
- Frame time stays **present-to-present** (QPC after each present call) so the existing frame-time metrics
  and fps columns work unchanged.
- Display statistics (added after the usual metrics, only when the API reports anything):
  `Displayed frames` (%), `Display latency avg / p99` (ms), `Displayed frame time avg / p99` (ms).
  - D3D12: `GetFrameStatistics` after every present; whenever `SyncQPCTime` (vblank of the last screen
    update) changes, that `PresentCount` is logged as displayed at `SyncQPCTime`. So "Displayed frames" =
    screen updates / presents, "Displayed frame time" = interval between screen updates, "Display latency" =
    vblank minus the moment the present call returned.
  - Vulkan: `VK_KHR_present_id` + `VK_KHR_present_wait` when the device has both (RTX 5060 yes, 780M no),
    polled with timeout 0 after every present: the time is when the poll saw completion (frame-time
    granularity). On NVIDIA it completes for every present ~3 ms after the call, i.e. it marks the
    presentation engine taking the image (here the cross-adapter copy), not a scan-out.
  - Not used: `VK_GOOGLE_display_timing` (on neither driver); `VK_EXT_present_timing` (NVIDIA exposes it,
    with present_id2 / wait2; it would give real "first pixel out" times — follow-up).
- Benchmark window (`gpu_present.cpp`): created, pumped and destroyed on the worker thread; fullscreen =
  `WS_POPUP | WS_EX_TOPMOST` over the monitor of the current foreground window, windowed = client area of
  the render size centered in the work area. It takes the foreground while it runs and gives it back to the
  previous window when it closes. Esc / Alt+F4 / close button stop the test (result: failed, "stopped: the
  benchmark window was closed"); a cancelled run closes it normally.

### Settings / flags / file
- Settings window, GPU group: **Present mode** = Offscreen / On-screen, windowed / On-screen, fullscreen.
- ini `[settings]`: `gpu_onscreen=0|1` (new) and `gpu_fullscreen=0|1` (existing key).
- CLI: `--gpu-present offscreen|windowed|fullscreen`. GUI dev flag: `--gpu-present <mode>` (session only,
  not written to the ini).
- TOON `params.gpu_present: offscreen|windowed|fullscreen` (new key; old files simply lack it).

### Measured (battery, 1.5 s phases, `3d.scene`, avg frame time; release build)
The desktop ran at 60 Hz on battery; the fullscreen window got 144 Hz (Windows' dynamic refresh rate),
visible in D3D12's displayed frame time.

| GPU / mode | Vulkan | D3D12 | D3D12 displayed frames / frame time / latency | Vulkan present_wait |
|---|---|---|---|---|
| RTX 5060, offscreen | 0.159 ms (6270 fps) | 0.177 ms (5660 fps) | — | — |
| RTX 5060, windowed | 0.577 ms (1730 fps) | 0.608 ms (1645 fps) | 3.6 % / 16.67 ms / 1.9 ms | 99.8 %, 3.0 ms after the call |
| RTX 5060, fullscreen | 0.612 ms | 0.603 ms | 8.7 % / 6.92 ms / 3.5 ms (p99 7.2) | 99.8 %, 3.2 ms |
| 780M, offscreen | 0.335 ms | 0.353 ms | — | — |
| 780M, windowed | 0.772 ms | 0.679 ms | 4.1 % / 16.67 ms / 1.0 ms | n/a (no present_wait) |
| 780M, fullscreen | 1.489 ms (p99 21 ms) | 0.720 ms | 10.4 % / 6.94 ms / 2.6 ms | n/a |

- Presenting adds ~0.35–0.45 ms per frame on **both** GPUs, so at these frame rates on-screen numbers are
  capped near 1400–1700 fps whatever the test: the 2D blit test does 0.211 ms offscreen (629 GB/s) but
  ~0.64 ms on screen. On the dGPU that includes the cross-adapter copy to the 780M; the 780M (which owns the
  panel) pays a similar cost, so most of it is the present / compose path itself, not the copy.
  Offscreen remains the mode for throughput numbers; on-screen is for presentation behaviour.
- The 780M Vulkan fullscreen run had hitches (p99 21 ms) in the one run made; D3D12 was clean.
- D3D12 frame statistics, first attempt: logging every new `PresentCount` gave "95 % displayed", because
  in composed mode the count advances per present while `SyncQPCTime` only moves per vblank. Logging on
  vblank change gives the plausible numbers above.
- NVIDIA present_wait completes for ~every present (IMMEDIATE) about 3 ms after the call; it is not a
  scan-out time (that would need `VK_EXT_present_timing`).
- Debug build, all 16 3D / 2D tests on screen (fullscreen, RTX 5060): all done, no Vulkan validation
  messages (the D3D12 debug layer isn't installed on this PC). Closing the benchmark window mid-test (WM_CLOSE
  from a script) ends that test as failed "stopped: the benchmark window was closed" and the run continues.

## B. Frame capture of other applications (PresentMon / CapFrameX replacement) — **Removed 2026-11-11 (trim)**

Everything in this part was deleted; `--seconds` remains for the sensor log only. `results_write_system` stays public
(the stability report uses it).

### What it does
- `src/frames/frame_capture.*`: real-time ETW session `EzPcBench-FrameCapture` (QPC clock, 64 KiB buffers,
  1 s flush) with two manifest providers, each limited to its present events with an
  `EVENT_FILTER_TYPE_EVENT_ID` filter:
  - Microsoft-Windows-DXGI `{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}`, keyword `Events` = 0x2:
    42 Present_Start, 43 Present_Stop, 55 / 56 PresentMultiplaneOverlay_Start / _Stop.
  - Microsoft-Windows-DxgKrnl `{802EC45A-1E99-4B83-9920-87C98277BA9D}`, keyword `Present` = 0x8000000:
    168 Flip_Info, 171 PresentHistory_Start, 184 Present_Info, 215 PresentHistoryDetailed_Start.
  Keywords, ids and payload templates were read from the installed manifests (`wevtutil gp ... /ge /gm`,
  `Get-WinEvent -ListProvider`): Present_Start = `pIDXGISwapChain` (pointer), `Flags` (u32),
  `SyncInterval` (u32), `DirtyRects`, `ScrollRects` — decoded by offset (pointer size from the header flags),
  no TDH needed.
- Only events of the target pids are stored (name -> all matching pids via Toolhelp, or a pid, or `*` =
  every process but ours). Afterwards the events are sorted by time and `capture_analyze` picks the process
  with most DXGI presents (else most kernel presents), then its busiest swapchain, and computes frame time =
  delta between consecutive Present_Start (PresentMon's MsBetweenPresents). Apps that don't use DXGI
  (Vulkan / OpenGL on some drivers) fall back to the kernel event logged most for that process.
- Reported: avg fps, frame time avg / median / p95 / p99 / p99.9 / max, 1 % / 0.1 % low (1000 / p99,
  p99.9, the app's existing convention), 1 % / 0.1 % low *average* (mean of the slowest 1 % / 0.1 % frames,
  CapFrameX style), time inside Present(), dominant SyncInterval, whether tearing was allowed, lost events.
- Not reconstructed: displayed / flip / vsync timing (PresentMon's display-tracking state machine), so no
  "MsBetweenDisplayChange" or latency.

### UI / CLI / file
- Tool window **Frame capture** (`--tool frames`, button "Frames" on the System row): application list
  (visible top-level windows) or a typed exe name / pid, duration 5–300 s, live present count, results with
  the frame-time graph, Save (results folder, `frames_<date>_<time>_<exe>.toon.txt`) and Open in compare.
  Without admin rights it says so up front; a failed start shows the reason.
- CLI: `--capture <exe|pid|*> --seconds N --out file.toon.txt`; `--capture-etl file.etl [--capture pid]`
  analyzes a recorded trace (WPR / PresentMon .etl) without a session; `--capture-selftest [--out f]` decodes
  synthetic ETW records (two processes, two swapchains, DXGI + DxgKrnl) and checks the analysis (exit 0 =
  PASS); with `--out` it also writes that synthetic capture (handy for testing the compare window).
- TOON (same layout as a benchmark run, so the compare window loads it as a run):
  ```
  app / format / kind: frames / timestamp / system: (same block as runs)
  params:  duration_ms
  capture: process, pid, api (DXGI | DxgKrnl <event>), seconds, frames, sync_interval, tearing, events, dropped_events
  tests[1]:   capture.frames, suite "Frame capture", group "Capture", name = exe
  metrics[N]: Avg / Median / p95 / p99 / p99.9 / Max frame time (ms), 1% / 0.1% low average (fps), Present call avg (ms)
  frames[N]{t,ms}: end time of each frame (s since the first present), frame time (ms)
  ```
- Compare: captures align on the test id `capture.frames`, so two captures (or a capture and runs) sit side
  by side; `frames[N]` is kept per run (`CmpRun::frame_ms`) and a "Frame times" graph above the table
  overlays every capture in the set (slowest frame per pixel column, hover for values).

### ETW findings
- This session isn't elevated (Administrators is deny-only in the token) and the user isn't in Performance
  Log Users: `StartTraceW` returns `ERROR_ACCESS_DENIED` (5). Both the CLI and the window detect this
  (`capture_allowed`: token elevation or S-1-5-32-559 membership) and show "needs administrator rights".
  A live capture could therefore **not** be tested here; decoding and analysis are covered by
  `--capture-selftest` and the compare round trip by its `--out` file.
- A stale session left by a crash is stopped and restarted (`ERROR_ALREADY_EXISTS`).

## FUTURE.md §5 status
- P2 On-screen mode with a real swapchain: **done** (D3D12 + Vulkan, windowed / fullscreen, tearing,
  present-to-present; displayed-frame data from frame statistics / present_wait). Partial: no
  `VK_EXT_present_timing`, no VRR-specific test.
- P3 Frame capture of other applications: ~~done for present-to-present~~ **Removed 2026-11-11 (trim)** (the whole feature).

## Merge notes
(Frame capture files and edits listed here were removed again on 2026-11-11 (trim); only the `gpu_present*` part stands.)
- New files: `src/gpu/gpu_present.{h,cpp}`, `gpu_present_d3d12.cpp`, `gpu_present_vk.cpp`,
  `src/frames/frame_capture.{h,cpp}`, `src/app/frames_ui.{h,cpp}`, `src/app/frames_graph.{h,cpp}`.
- Edits to existing GPU files (small, additive): `gpu_d3d12_internal.h` (present API), `gpu_d3d12_raster.cpp`
  (open / record / present / close around the frame loop), `gpu_vk_internal.h` (3 bools in `VkCtx`, present
  API), `gpu_vk.cpp` (instance surface extensions + device swapchain / present_id / present_wait and their
  feature structs, via `vkp_instance_extensions` / `vkp_device_extensions`), `gpu_vk_tests.cpp`
  (`FrameSetup::present`, one call in `record_frame`, acquire / submit+present in `test_frames`).
- Shared: `bench.h` (`gpu_onscreen`), `results.{h,cpp}` (`params.gpu_present`; `write_system` made public
  as `results_write_system`), `compare.{h,cpp}` (frames table per run), `compare_ui.cpp` (graph),
  `tool_windows.h` (`TOOL_FRAMES`), `main_gui.cpp` (tool row, `--tool frames`, `--gpu-present`),
  `ui.cpp` (Frames button), `theme.{h,cpp}` (`ICON_FRAMES`), `settings.cpp` / `settings_ui.cpp`,
  `main_cli.cpp`, `CMakeLists.txt` (new sources; no new libraries: ETW is advapi32, already linked).
