# Stability tester — implementation notes (FUTURE.md §6)

Stream notes for the lead; fold the decisions and measured facts into PLAN.md when merging.

## What was built

A separate tool (FUTURE.md §0 decision): its own controller, live state and report, no shared code path with
the benchmark runner / results table.

| Piece | Files |
|---|---|
| Controller, monitor, limits, error log, verdict, TOON report | `src/stability/stability.{h,cpp}`, `stab_internal.h` |
| CPU kernels (`cpu`), per-core cycling (`cores`), cache test (`cache`) | `src/stability/stab_cpu.cpp` |
| Memory error test (`mem`) | `src/stability/stab_mem.cpp` |
| Disk verify (`disk`) | `src/stability/stab_disk.cpp` |
| Own D3D12 path: VRAM test (`vram`), known-answer compute + render (`gpu`), max power (`power`) | `src/stability/stab_gpu.cpp`, `src/stability/stab.hlsl` |
| WHEA-Logger counter (API for the runner too) | `src/core/whea.{h,cpp}` |
| CLI mode `--stress` | `src/app/stability_cli.{h,cpp}` (+ 3 lines in `main_cli.cpp`) |
| Tool window `--tool stability` | `src/app/stability_ui.{h,cpp}` |

### Tests

| id | What | Errors attributed to |
|---|---|---|
| `cpu` | Every LP runs self-checking kernels: integer mixing (rotl / xor / 64-bit multiply), SSE mul + add, AVX2 FMA, AVX-512 FMA on per-thread data that stays in L1 (8 KiB + mirror), and AVX2 FMA streaming a shared buffer (≤ 256 MiB, ¼–⅛ of free RAM) from RAM. Mixed mode rotates the kernels every 4 s on all threads at once (load steps). | logical processor |
| `cores` | One thread re-pins itself to each physical core in turn for `core_seconds` (CoreCycler-style), running the CPU kernels. Seconds per core are recorded. | logical processor (= core) |
| `cache` | Per LP: LCG stream fill, check + write complement, check, in buffers sized to ¾ of L1 / L2 (÷ SMT threads per core) and ¾ of L3 ÷ LPs; levels rotate every 4 s. | logical processor |
| `mem` | One region per LP of a locked, pre-faulted buffer: address-in-address (+ inverted), walking 1s, walking 0s, xorshift random, moving inversions (P / ~P, ascending then descending block by block). Checks OR-reduce 4 KiB blocks and rescan a block only on a mismatch. | LP + region (chunk) + virtual address |
| `vram` | Up to 64 committed buffers (256 MiB chunks, larger chunks for big cards), patterns filled and checked on the GPU (address, walking 1/0, random, moving inversions). Mismatches counted per chunk on the GPU, the first 16 logged with index / expected / actual. | chunk + byte offset |
| `gpu` | Compute: 65536 threads × 512 iterations of an integer + float chain (4 lanes), checked in the shader against references computed on the CPU (4 seeds, uploaded once). Render: 4 full-screen layers into an R32_UINT 1024² target (last layer must win), every pixel re-derived and compared by a compute shader, plus 2048 pixels per batch compared on the CPU. | compute / render |
| `power` | Maximum-power load (FMA × 256 per element over a 256 MiB buffer, streamed), unchecked except for device loss. Labelled as such in the UI / CLI; stops at the GPU temperature limit. | — |
| `disk` | Temp file (`%TEMP%` if on the target drive, else `<drive>:\ezbench\`), `NO_BUFFERING | WRITE_THROUGH | DELETE_ON_CLOSE`, 1 MiB blocks of xorshift data with a header (block id, pass); write all, flush, read all back unbuffered, compare; new data every pass. One error per mismatching 4 KiB page. | 1/64 of the file (chunk) + byte offset |

Known answers: CPU float kernels are compared with references computed once by scalar code, one `_ss` operation
at a time (`_mm_fmadd_ss` / `_mm_mul_ss` + `_mm_add_ss`, loop vectorization / interleave / unroll disabled; checked
in the disassembly: `vfmadd213ss`, `vmulss` + `vaddss`); FMA / mul / add are correctly rounded in every encoding,
so SIMD and scalar must agree bit for bit. Data in [0.5, 1): no denormals or overflow. GPU float chains use
`precise` HLSL (no mad contraction) of multiply, add, floor, subtract, all correctly rounded / exact in D3D, with
values in [0, 4): the CPU replica with `_mm_*_ss` intrinsics matches exactly (verified on the RTX 5060: zero
mismatches besides the injected ones). The integer kernel's reference is the same arithmetic in a separate,
non-inlined, non-vectorized function.

Run modes: in sequence (each test `seconds`), together (all for `seconds`: combined CPU + GPU load = `combined`),
or until stopped (`seconds = 0`). Workers run at normal priority with power throttling opted out; the 1 Hz monitor
runs at HIGHEST so the limits keep working under full load; the GUI draws at 15 FPS during a run. The run keeps the
system awake.

Safety: temperature limits (CPU = hottest ACPI thermal zone, best effort; selected GPU via D3DKMT; hottest
physical drive), stop on battery ≤ 10 %, optional stop at the first error. State is always restored: threads
joined, memory unlocked / freed and the working-set limits restored, GPU device released, disk file deleted (also
on a crash: DELETE_ON_CLOSE), `SetThreadExecutionState` reset. The GUI stops and waits for a run at exit; a
benchmark run can't start while a stability run is active (and vice versa in the stability window).

Self-test (`--inject`, GUI checkbox only with `--dev`): each test corrupts exactly one value once (a result bit,
a memory / cache word after the fill, a VRAM word in the fill shader, a compute result and a rendered pixel in
the shaders, one bit of block 1 of the disk file) to prove detection, attribution and reporting end to end.

### CLI

```
ez-pc-bench-cli --stress <cpu,cache,cores,mem,vram,gpu,power,disk | all | combined>
    [--minutes N | --seconds N] [--together] [--cpu-kernel mixed|int|sse|avx2|avx512|large]
    [--core-seconds N] [--mem-mib N] [--vram-mib N] [--gpu N] [--disk-drive C] [--disk-mib N]
    [--max-cpu-temp C] [--max-gpu-temp C] [--max-drive-temp C] [--stop-on-error] [--inject] [--out file.toon.txt]
ez-pc-bench-cli --whea-history      (dev: counts WHEA-Logger events already in the System log)
```

A status line every second, then the report (per-test status / errors / time to first error / work, errors by LP
and chunk, sensor avg / max, WHEA, the first 16 errors). Exit code 0 pass, 1 errors or WHEA events, 2 stopped
early / a test couldn't run, 3 usage. `all` = everything except `power`.

### TOON report (`kind: stability`)

```
app: ez-pc-bench
format: 1
kind: stability
timestamp: ...
system: ...                      (same block as benchmark results: results_write_system)
stability:
  result: pass|fail|stopped|incomplete
  why: "..."
  seconds, mode (sequence|together), duration_s, stop_reason, self_test, cpu_kernel, core_seconds, gpu, disk_drive
  limits: { cpu_temp_c, gpu_temp_c, drive_temp_c, stop_on_error }
  whea: { monitored, corrected, uncorrected }
sensors[8]{name,unit,avg,max}:   cpu_clock, cpu_load, cpu_temp, gpu_clock, gpu_load, gpu_temp, gpu_power, drive_temp
tests[N]{id,name,status,seconds,errors,first_error_s,work,work_unit,size_bytes,note}:
lp_errors[M]{test,lp,core,errors,seconds}:        per-LP tests (seconds: per-core cycling only)
chunk_errors[K]{test,chunk,offset,bytes,errors}:  non-zero only
errors[E]{test,t,lp,chunk,address,what,expected,actual,bits}:   first 64, hex strings
```

The compare window rejects files with a `kind` other than `benchmark` ("a stability report, not benchmark
results"); the CLI `--compare` baseline loader already rejects files without a metrics table.

## For the lead (merging)

- Shared-file edits (all small, additive): `CMakeLists.txt` (sources, 7 `ezb_shader` lines pointing at
  `../../stability/stab.hlsl`, `wevtapi` link), `main_cli.cpp` (include, usage line, `--stress` / `--whea-history`
  dispatch before `runner_init`), `main_gui.cpp` (tool row, `--tool stability`, UI throttled to 15 FPS while a
  stability run is active, stop + wait at exit), `tool_windows.h` (`TOOL_STABILITY`), `ui.cpp` (tool button,
  benchmark Run refuses while a stability run is active), `theme.{h,cpp}` (`ICON_PULSE`), `results.{h,cpp}`
  (`write_system` exposed as `results_write_system`), `compare.cpp` (reject non-benchmark `kind`).
- New link library: `wevtapi` (Windows SDK). New shaders: `cs_stab_fill`, `cs_stab_check`, `cs_stab_compute`,
  `vs_stab_full`, `ps_stab_render`, `cs_stab_render_check`, `cs_stab_power` (DXIL used; SPIR-V emitted by the
  shared step, unused).
- **WHEA API to wire into the runner** (`core/whea.h`): call `whea_start()` once (e.g. `runner_init` or at run
  start, `whea_stop()` at exit), `whea_snapshot()` at run start and after each test; store the deltas (e.g. a
  `whea` count in `TestEnv` / `env[]` and a run-level line), and flag tests during which a WHEA event arrived. The
  callback runs on a wevtapi thread; the counters are atomics, so snapshots are cheap from any thread.
- Settings in the stability window are session-only (not in settings.ini).

## Decisions

- D3D12 only for the GPU tests (no Vulkan path): one API is enough to find errors; it avoids touching the GPU
  agents' files. Device on the GPU selected in the main window (by LUID).
- Root descriptors (8 constants + 3 root UAVs), no descriptor heaps except one RTV.
- Worker priority normal (not the benchmarks' HIGHEST, which starves the OS sensor threads). Below-normal was
  tried first: with other processes busy (parallel builds) the workers got so little CPU that a 20 s test overran
  its deadline by 5 s and the setup took 8-15 s.
- References are computed by optimized scalar code: `optnone` versions were ~2 us per operation, which made the
  CPU test's setup take 8-15 s (64 M reference FMAs for the large-data slices).
- Memory test keeps ≥ 2 GiB of RAM available, default 70 % of available; VRAM default 85 % of the free local
  budget. Locking failure falls back to a pre-faulted unlocked buffer with a note.
- Errors: CPU / cache / memory / VRAM count every mismatching word; disk counts mismatching 4 KiB pages; GPU
  compute / render count mismatching values. Verdict `fail` also when Windows logged any WHEA event during the
  run (corrected ones included: under stress they are a sign of instability).

- `--whea-history` and the GUI's `--stability-run <ids> [seconds]` (dev: starts a run with 1 GiB memory / VRAM,
  64 MiB disk file, 2 s per core; `--dev --inject` adds the self-test) exist for verification and screenshots.

## Verification (2026-11-11, R7 250 + RTX 5060 Laptop, on AC)

Every run through the shared lock wrapper, one at a time; memory and VRAM ≤ 1 GiB, each run ≤ 30 s, CPU + GPU
combined 15 s. Build warning-free.

| Run | Result |
|---|---|
| `--stress cpu --seconds 20 --inject` | fail, exactly 1 error: AVX2 FMA, LP 2 (core 1), bit 22 of lane 17 (the injected one); 25 s before the fixes below |
| `--stress cpu --seconds 20` | pass, 20.0 s, 2203 G ops checked, kernels rotate int → SSE → AVX2 → AVX-512 → large; CPU 3.9 GHz avg / 4.7 max, ACPI zone 64-68 °C |
| `--stress cache --seconds 13 --inject` | 1 error, L1 level, LP 2; 348 GB checked |
| `--stress cores --seconds 16 --core-seconds 2` | pass; 2.0 s on each of the 8 cores, 339 G ops; with `--inject` 1 error on core 0 (integer) |
| `--stress mem --seconds 20 --mem-mib 1024 --inject` | 1 error: "Address in address", LP 2, region 0, virtual address and bit 17 reported; 1 GiB locked; 288 GB checked |
| `--stress disk --seconds 15 --disk-mib 64 --inject` | 1 error at byte 0x101F40 (block 1, the corrupted bit 5), 12.5 GB read back; no file left in `%TEMP%` (also after a killed GUI run) |
| `--stress vram --seconds 15 --vram-mib 1024 --inject` | 1 error, chunk 0, offset 0x5555554 (word n/3), bit 8; 4 × 256 MiB of 7119 MiB free; 2173 GB checked (~145 GB/s incl. fills) |
| `--stress gpu --seconds 15 --inject` | 2 errors: 1 compute (bit 4), 1 rendered pixel (7,7) (bit 18); no other mismatch in 55 k dispatches / frames, so the CPU references match the GPU bit for bit |
| `--stress power --seconds 10` | pass, GPU up to 63.8 °C, avg 1840 MHz |
| `--stress combined --seconds 15` | pass: CPU 15.0 s (2023 G ops) + GPU 15.1 s (1245 dispatches / frames), CPU avg 3.8 GHz |
| `--stress all --seconds 4 ...` | pass, all 7 tests (power excluded), 28 s total, each 4.0-4.1 s |
| `--stress power --seconds 20 --max-gpu-temp 58` | stopped at 2.6 s: "GPU temperature 61 °C reached the 58 °C limit", exit 2 |
| `--stress cpu --seconds 20 --max-cpu-temp 50` | stopped at 0.4 s (ACPI zone 52 °C), exit 2 |
| `--stress cache,mem --inject --stop-on-error` | stopped at 1.0 s after the cache error; memory not run; exit 1 |
| GUI `--tool stability --stability-run combined 15` | live table, LP grid, sensor graphs (screenshot while running) |
| GUI `--dev --inject --stability-run cpu,mem,disk 5` | "Fail - 3 errors in CPU, Memory, Disk verify", LP 2 red, report auto-saved |
| GUI `--compare <stability report>` | rejected: "a stability report, not benchmark results" |
| WHEA | 0 WHEA-Logger events in this PC's System log (Get-WinEvent agrees). The query + subscription code was verified with a copy pointed at Microsoft-Windows-PowerShell/Operational: history count matched Get-WinEvent (2389 / 2390), live events counted with level and id |

Measured facts:
- The ACPI thermal zone did follow load here this time (52 °C idle → 64-74 °C under CPU load), refreshed every
  sample with the workers at normal priority; it still isn't a die temperature.
- D3DKMT reports no power figure for the RTX 5060 Laptop (GPU power graph stays "—").
- VRAM free local budget with nothing else on the dGPU: 7119 MiB of 8 GiB.

## FUTURE.md §6 status

| Item | Status |
|---|---|
| Own tool window, `--tool stability`, CLI `--stress ... --minutes N` (+ `--seconds`), TOON `kind: stability`, compare ignores it | done |
| P1 CPU self-checking kernels (int, SSE, AVX2, AVX-512, cache-resident + streaming), per-LP attribution | done |
| P1 Memory error test (locked, pre-faulted, moving inversions, walking 1s / 0s, random, address-in-address, all threads, address / pattern / expected / actual), partial-coverage statement | done. Physical addresses need a driver (virtual only). No TestMem5-style configurable mixes. |
| P1 VRAM test | done for the selected adapter (not "every adapter" in one run) |
| P1 GPU error detection (compute + render, known answers) | done, D3D12 only |
| P1 Combined CPU + GPU load | done (`combined` / "All at once"; any tests can be combined) |
| P1 WHEA monitoring | done for the stability tester; runner wiring left to the lead (API above) |
| P1 Live sensor graphs, temperature limits, stop on first error, restore state | done. CPU limit = ACPI zone (best effort until PawnIO, FUTURE.md §8). Limits are session-only settings. |
| P2 Per-core cycling | done (one thread per core, kernel choice shared with the CPU test) |
| P2 Cache test | done |
| P2 Disk verify | done (default 256 MiB file, choice 64 MiB-4 GiB) |
| P2 Report (pass / fail, time to first error, errors per core / chunk, WHEA, max temps, avg clocks and power) | done (TOON + CLI + window). GPU power is "% of limit" where the driver reports it. |
| P3 Max-power GPU load, labelled, temperature cut-off | done |
| Not done | GPU tests on Vulkan; stability settings persistence; saving the GUI report when the window was closed before the run ended (the CLI always writes with `--out`); WHEA fault injection (needs WHEA test tooling / admin) |
