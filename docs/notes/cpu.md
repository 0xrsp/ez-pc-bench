# CPU stream notes (FUTURE.md §3 + Sustained preset)

Written by the CPU agent for the lead; fold the relevant parts into PLAN.md / FUTURE.md when merging.

## What was built

### Sustained mode (runner level, FUTURE §3 P1)

- `BenchParams::sustain_s` (seconds per test, 0 = off, max `MAX_SUSTAIN_S` = 3600). In sustained mode the
  runner loops each selected test until `sustain_s` has passed (always at least 2 passes, at most
  `MAX_SUSTAIN_PASSES` = 500; huge results get as many passes as fit in the 4 MiB value buffer). It reuses
  the repeat machinery: the reported values are the **median over the passes**, `g_spread` gets min / max /
  CV (so the existing "median of N" hover and noisy flag work). Sustained mode overrides `repeat`.
- Per-pass series: `g_sustain[bench]` (runner.h) keeps every pass's value of up to 16 tracked metrics
  (values results: the first 16 metrics; series / matrix results: one tracked value per pass = the average
  over all points, matrix off-diagonal) plus the pass end times, in a 4 MiB f32 pool. `sustain_stats()`
  gives first / last / min / max / median and `drop_pct` = how much worse the last pass is than the first
  (positive = slower at the end; inverted for lower-is-better units via `unit_lower_is_better`).
- Settings: ini key `sustain_s` ([settings]); Settings window row "Sustained mode" (slider 0-60 min, "off").
  Reset to defaults clears it.
- CLI: `--sustain <minutes>` (decimals allowed, e.g. `--sustain 0.5`); live line shows
  `pass N, t/T s`; after each test a `sustained: N passes in T s` block with first / last / min / max /
  median / drop per tracked metric.
- GUI: live panel shows "sustained pass N, t of T s"; overall progress is time-based; the Charts tab shows
  one per-pass plot per tracked metric (hover: pass, end time, value) for every sustained test, including
  values-only tests. Dev flags (session-only, not saved): `--sustain <minutes>`, `--charts` (opens the
  Charts tab). Add them to the CLAUDE.md flag list.
- TOON: `params.sustain_s`; tables
  `sustained[N]{id,name,unit,passes,seconds,first,last,min,max,median,drop_pct}` (one row per tracked metric,
  name gets " (average)" for series / matrix tests) and `sustain_passes[M]{id,name,pass,t,value}` (t = end of
  the pass, seconds from the test start). Readers ignore unknown tables, so compare / baseline / library are
  unaffected (they see the medians in `metrics`).

### Sustained preset (FUTURE §11)

`sustained` / "Sustained": `cpu.fp.sgemm`, `cpu.comp.lz77`, `gpu_d3d12.3d.scene`, 3 s phases, 5 minutes per
test (~15 minutes). `RunPreset` got a trailing `sustain_s` field (0 for the other presets); `preset_apply`
sets it and `preset_match` compares it. CLI `--preset sustained`.

### New CPU tests

| Id | Group | What | Metrics | Default |
|---|---|---|---|---|
| `cpu.topo.percore` | Topology | int ALU kernel + dependent-add clock on the first LP of each core in turn | series x = core: `Int ALU` Gops/s, `Clock (add chain)` MHz | on |
| `cpu.topo.hybrid` | Topology | LZ77 compressor, one thread + all threads per CPU-set EfficiencyClass | per class ST / MT MB/s, `E / P, one thread` | on (skips on non-hybrid) |
| `cpu.comp.xpress` / `xpress_huff` / `mszip` / `lzms` | Compression | Windows Compression API (cabinet.dll), 256 KiB chunks of a 4 MiB text-like corpus | compress ST / MT, decompress ST / MT MB/s (of uncompressed data), ratio | on |
| `cpu.comp.lz77` | Compression | own LZ4-format greedy LZ77 (pure integer) | same | on |
| `cpu.img.jpeg` / `png` | Image | WIC decode of a generated 1920x1080 image (JPEG q90 / PNG) to 24bpp BGR | MPix/s ST / MT | on |
| `cpu.img.resize` | Image | WIC high-quality cubic scaler 1920x1080 -> 1280x720 | MPix/s of source pixels | on |
| `cpu.parse.json` | Parsing | validating recursive-descent JSON parser on a generated 1 MiB document | MB/s | on |
| `cpu.parse.text` | Parsing | tokenize 1 MiB of prose, lowercase, FNV hash, count in an open-addressing table | MB/s | on |
| `cpu.sci.fft` | Science | 64K-point complex FP64 FFT, radix-4 Stockham, forward + inverse per call | GFLOPS (5 n log2 n per transform) | on |
| `cpu.sci.nbody` | Science | 2048-body all-pairs gravity, FP32 SoA + leapfrog, one system per thread | Ginteractions/s | on |
| `cpu.sci.raybox` / `raytri` | Science | slab test vs 4096 boxes / Moller-Trumbore vs 1024 triangles, 1024 rays | Mtests/s | on |
| `cpu.crypto.aes_gcm` | Crypto | AES-128-GCM through Windows CNG (BCrypt), 64 KiB per call | GB/s | on |
| `cpu.crypto.chacha20` | Crypto | own ChaCha20, AVX2, 8 blocks per iteration | GB/s | on |
| `cpu.crypto.crc32c` | Crypto | SSE4.2 `crc32` instruction, one stream (latency bound) | GB/s | on |
| `cpu.crypto.xxhash` | Crypto | xxHash64, scalar | GB/s | on |
| `cpu.ai.int8` | AI inference | 192x256x256 u8 x s8 -> s32 matmul: AVX-512 VNNI, else AVX-VNNI, else AVX2 vpmaddubsw | GOPS (2 per MAC); metric name says the path | on |
| `cpu.ai.bf16` | AI inference | same shape, AVX-512 BF16 `vdpbf16ps` -> FP32 (skips without it) | GFLOPS | on |
| `cpu.work.pi` | Workloads | 1M digits of pi: Chudnovsky binary splitting, FFT multiply, Newton 1/x and 1/sqrt | seconds ST / MT, MT / ST | **off** |
| `cpu.micro.instr` | Microarchitecture | latency + reciprocal throughput of 19 instructions | clk | **off** |
| `cpu.micro.branch` | Microarchitecture | one conditional branch on a random pattern of length 2 ... 65536 | series: clk per loop iteration | **off** |
| `cpu.micro.code` | Microarchitecture | JIT-generated 4-byte NOPs, 1 KiB ... 2 MiB footprint | series: IPC | **off** |

Self-checks (a test fails rather than reporting a number if any of these is off):
- compression: every thread's chunk round-trips before and after the timed phases;
- JPEG mean error vs the source < 8 levels, PNG bit exact, resize mean brightness within 2 %;
- JSON: object / array / string / number / literal counts, decoded string bytes and their sum, and the sum
  of all numbers must equal what the generator wrote (on every timed call); tokenizer: token and distinct
  word counts (every call);
- FFT: single tone at bin 5 and a random round trip (1e-12); n-body: pairwise forces cancel (|sum m a| <
  1e-4 sum |m a|); rays: hit counts of 64 rays within 0.1 % of a double-precision reference;
- AES-GCM: NIST test case 2; ChaCha20: RFC 8439 2.3.2 block, AVX2 == scalar, scalar == CNG
  ChaCha20-Poly1305 ciphertext (when CNG has it); CRC32C("123456789") = E3069283; xxHash64 of "", "abc"
  and the 39-byte python-xxhash example;
- int8 / BF16: bit-exact vs a scalar reference (inputs chosen so FP32 sums are exact);
- pi: every FFT product checked modulo 2^61-1 and rounding error < 0.2; first 50 digits and the Feynman
  point (decimals 762-767 = 999999); single- and multi-thread digits identical;
- instruction table: the add latency must come out at 1 clk (it is the clock reference).

All new kernel files get `/clang:-falign-loops=64` like the existing ones.

## Shared-file edits (all additive)

- `src/core/bench.h`: `BenchParams::sustain_s`, `MAX_SUSTAIN_S`.
- `src/core/runner.h/.cpp`: sustained loop in `run_test` (now takes the bench id), `g_sustain`,
  `g_sustain_pool`, `sustain_stats`, `sustain_metric`, `RunState::sustain_s` / `test_start`; repeat values
  buffer became a flat 4 MiB f64 array (median code unchanged in behavior).
- `src/core/results.cpp`: `params.sustain_s`, `write_sustain` (tables above).
- `src/core/presets.h/.cpp`: `sustain_s` field, Sustained preset.
- `src/core/threads.h/.cpp`: `CpuTopology::eff_class[]` (EfficiencyClass per LP).
- `src/core/registry.cpp`: 26 CPU rows (+ declarations).
- `src/app/settings.cpp`, `settings_ui.cpp`: `sustain_s` key, row, session-only when set by `--sustain`.
- `src/app/main_cli.cpp`: `--sustain`, live line, `print_sustain`; preset list in help.
- `src/app/main_gui.cpp`, `ui.h/.cpp`: `--sustain` / `--charts` dev flags, live line, `draw_sustain` in the
  Charts tab, `draw_series` now draws one plot per run of same-named points (needed for the two-metric
  per-core series), preset combo width fits "Sustained".
- `CMakeLists.txt`: 9 new sources in `ezbench_core`, loop alignment for them, link libs
  `cabinet windowscodecs bcrypt oleaut32` (all Windows SDK).

## Measured (2026-11-11, AC + "Best performance", not elevated, short phases)

**Caveat:** ~11 agents were building in parallel during these runs. The lock serializes benchmarks, not
compiles, so single-thread phases shared their core with compiler processes (lower clock, busy SMT sibling)
while multi-thread phases displaced them. ST numbers below are therefore 2-5x low and several MT / ST ratios
exceed 16, which is impossible on a quiet 8C/16T machine. They show the tests work and self-verify; re-measure
ST on an idle machine before using them as references. Phases 0.5-1 s (`--duration 500` / `1000`).

| Test | ST -> MT | Notes |
|---|---|---|
| `cpu.comp.xpress` | compress 102 -> 3193, decompress 290 -> 8820 MB/s | ratio 2.13 |
| `cpu.comp.xpress_huff` | 35 -> 924, 176 -> 7038 MB/s | ratio 2.55 |
| `cpu.comp.mszip` | 2.4 -> 64, 93 -> 1149 MB/s | ratio 3.41; Windows compresses at max effort |
| `cpu.comp.lzms` | 1.3 -> 21, 44 -> 1265 MB/s | ratio 3.25 |
| `cpu.comp.lz77` | 859 -> 8069, 1612 -> 14140 MB/s (quieter run) | ratio 1.93; under the build load ST swung 10-930 MB/s pass to pass |
| `cpu.img.jpeg` / `png` / `resize` | 139 -> 2714 / 60 -> 1224 / 51 -> 932 MPix/s | |
| `cpu.parse.json` / `text` | 195 -> 5132 / 256 -> 3776 MB/s | |
| `cpu.sci.fft` (16K) | 4.4 -> 59.6 GFLOPS | 64K points was DRAM-bound: 6.7 -> 18.4 (2.7x) |
| `cpu.sci.nbody` (AVX2) | 3.95 -> 26.4 Ginteractions/s (quieter run) | scalar version (compiler won't vectorize FP sums under precise FP): 0.35 -> 3.75 |
| `cpu.sci.raybox` / `raytri` | 2131 -> 17282 / 764 -> 8410 Mtests/s | |
| `cpu.crypto.aes_gcm` (CNG) | 3.2 -> 42.6 GB/s | |
| `cpu.crypto.chacha20` | 1.4 -> 19.4 GB/s | |
| `cpu.crypto.crc32c` / `xxhash` | 5.6 -> 91 / 3.8 -> 84 GB/s | |
| `cpu.ai.int8` (AVX-512 VNNI) | 488 -> 3780 GOPS | ~108 int8 ops/clk ST = 85 % of the double-pumped vpdpbusd zmm peak |
| `cpu.ai.bf16` (AVX-512 BF16) | 200 -> 1898 GFLOPS | |
| `cpu.work.pi` 1M digits | 1.66 s ST, 0.645 s MT (2.57x) | all checks pass (modular check of every FFT product, known digits, ST == MT); 4.2 s ST under the build load |
| `cpu.topo.percore` | 5.4-9.4 Gops/s, 1.9-3.9 GHz | spread is the build load moving between cores, not the silicon |
| `cpu.topo.hybrid` | skipped | one efficiency class |

Instruction table (`cpu.micro.instr`, clk, latency / reciprocal throughput) — matches published Zen 4
figures: add 1.03 / 0.31 (0.25 + the loop's fused dec/jnz), complex lea 1.94 / 0.68, imul 3.0 / 1.1,
popcnt 1.0 / 0.36, crc32 3.0 / 1.1, L1 load 4.0 / 0.40, vaddps / vmulps ymm 3.0 / 0.5, FMA ymm 4.0 / 0.53,
vdivps ymm 10.7 / 3.0, vsqrtps ymm 15.1 / 5.0, vpaddd 1.0 / 0.25, vpmulld 3.0 / 0.5, vpshufb 2.0 / 0.5,
vpermps 4.0 / 0.5, aesenc 4.0 / 0.5, pclmulqdq 4.0 / 2.0, vaddps zmm 3.0 / 0.99, vfmadd231pd zmm 4.0 / 1.0
(512-bit ops at half the 256-bit rate: double-pumped).

Code footprint (`cpu.micro.code`, 4-byte NOPs): ~12 per clock up to 32 KiB, ~3.9 at 64-512 KiB, 3.2 at 1 MiB,
2.0 at 2 MiB. 12 / clk is above Zen 4's 6-wide dispatch, so NOPs appear to be fused in pairs out of the op
cache; beyond the op cache / L1i the 4-wide decoders set the rate.

Sustained mode (`--run cpu.int.alu,cpu.comp.lz77 --sustain 0.5 --duration 1000`): 13 and 12 passes in
~31 s each; `sustained` / `sustain_passes` tables and the CLI summary correct (e.g. int ALU MT first 127.9,
last 128.6, min 75.2, median 127.4 Gops/s, drop -0.5 %). GUI (`--autorun --only cpu.int.alu,cpu.topo.percore
--sustain 0.3 --charts`): 16 passes, the per-pass plot shows the ST clock ramp (9.5 -> 15.2 Gops/s).
Sustained preset (`--preset sustained --sustain 0.2 --duration 500`): SGEMM 10 passes, MT first 867 / last
810 GFLOPS (drop 6.6 %); D3D12 scene 18 passes, avg frame time first 0.161 / last 0.141 ms (drop -12.4 %, i.e.
faster at the end: the sign is inverted correctly for lower-is-better units).

Branch predictor (`cpu.micro.branch`): ~3.0-3.3 clk per loop iteration for random patterns of 4 ... 8192
branches (learned), 3.7 at 16K, 4.2 at 32K, 10.4 at 64K (mostly mispredicted); 4.65 at length 2.
Fixed a bug found by its own count check: as a separate `"r"(0)` input, clang put the wrap value in the same
register as the loop index (both 0), so the index never wrapped. Watch for this in inline asm with
constant inputs.

## FUTURE.md §3 status

- [x] P1 Sustained mode — done (runner level, any test, CLI / GUI / ini / TOON / chart).
- [x] P1 Per-core single-thread pass — done (`cpu.topo.percore`). Uses one LP per core in Windows core
  order; the x axis is our core index (`g_topo.core`), not an APIC id.
- [~] P1 Hybrid P / E split — implemented (`cpu.topo.hybrid`, `CpuTopology::eff_class`), **untested**: this
  Zen 4 has one efficiency class, so the test skips. Needs a run on an Intel hybrid (or Zen 5c) machine.
  Classes are ranked highest first and named P-cores / E-cores / LP E-cores.
- [x] P2 Compression (4 Windows algorithms + own LZ77), WIC JPEG / PNG decode + resize, JSON parse, text
  tokenizing, FFT, n-body, ray-box / ray-triangle, CRC32C, xxHash64, AES-GCM, ChaCha20, int8 (VNNI / AVX-VNNI /
  AVX2) and BF16 — done. Deviations: AES-GCM is Windows CNG's implementation (SymCrypt), not our own kernel —
  it is what Windows TLS / BitLocker-style code uses, and our own AES-CTR test already covers the raw AES-NI
  / VAES rate; the AVX-VNNI int8 path is untested (Zen 4 has AVX-512 VNNI, so that path runs instead);
  ChaCha20 is the cipher only (no Poly1305).
- [x] P2 Pi / big-number workload — done (`cpu.work.pi`, default off). MT parallelizes only the series
  (binary-splitting ranges + pairwise merges); the last merges and the Newton steps are single-threaded,
  so MT / ST stays well below the core count, like y-cruncher's small sizes.
- [x] P3 Instruction latency / throughput table — done (`cpu.micro.instr`, 19 instructions).
- [x] P3 Branch predictor and op-cache / code-footprint microbenchmarks — done (`cpu.micro.branch`,
  `cpu.micro.code`). Only the pattern-length sweep for one branch (no branch-count x history grid) and
  4-byte NOPs only.

## Suggested score inputs (§10)

Workload-like tests that run in 3-7 s at default settings and self-verify (ST and MT unless noted):
- CPU single-thread / multi-thread: `cpu.comp.lz77` (compress + decompress), `cpu.comp.xpress`
  (decompress weighted higher: it's what loading compressed game / OS data does), `cpu.parse.json`,
  `cpu.parse.text`, `cpu.img.jpeg`, `cpu.img.resize`, `cpu.crypto.aes_gcm`, `cpu.sci.fft`,
  `cpu.sci.raytri`, plus the existing `cpu.work.sort` / `cpu.work.sieve`.
- MT only, lower weight: `cpu.ai.int8` (inference-like), `cpu.sci.nbody`.
- Not score inputs: `cpu.comp.mszip` / `cpu.comp.lzms` compress (Windows' max-ratio settings, 1-4 MB/s per
  thread, a call takes ~0.1-0.2 s so short phases are coarse), `cpu.topo.*`, `cpu.micro.*` (diagnostics),
  `cpu.work.pi` (long, default off), `cpu.ai.bf16` (skips on most CPUs).
- Sustained / thermal: `drop_pct` of `cpu.fp.sgemm` MT from the Sustained preset is a candidate
  "sustained performance" indicator (flag, not a score input).
