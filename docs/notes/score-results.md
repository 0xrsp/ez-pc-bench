# Score and results sharing (FUTURE.md §10 and §11)

Stream notes for the overall score (score v1, preliminary) and the results / history / sharing items that M7 left
open. Everything here is built on `dev` as of 2026-11-11; the lead moves the decisions into PLAN.md.

## 1. Score v1

### Formula

```
ratio_i     = value_i / ref_i                 (ref_i / value_i when lower is better: latency, frame time)
component_c = 1000 * exp( Σ w_i ln ratio_i / Σ w_i )          over the inputs of c present in the run
overall     = 1000 * exp( Σ W_c ln(component_c / 1000) / Σ W_c )     only when every input is present
```

- The reference run scores exactly 1000 in every component and Overall (verified: `--score` on the library file).
- 2000 = twice as fast on that component's mix; geometric means, so one huge ratio can't dominate.
- **GPU inputs take the better of Vulkan and D3D12** (test id pattern `gpu_*.…`): the API choice never penalizes.
- **Storage uses C:** when it was tested, else the first tested drive (`SI_DRIVE` inputs drop the `C: ` prefix).
- Missing inputs: the component is computed from the inputs that ran (coverage shown as "4 of 6 inputs"); Overall
  needs every input (`complete`). Partial runs therefore get component scores only.
- The reference values are **frozen in the table** (not read from the library at run time), so adding library runs
  never changes scores. A new version = a new table; scores of different versions are never compared (the
  version is stored with every score).

### Reference

`library/2026-11-11_r7-250_rtx5060-laptop.toon.txt` (PLAN.md §10): Ryzen 7 250, RTX 5060 Laptop, 32 GB DDR5-5600,
Samsung PM9C1a (MZAL81T0HFLB) NVMe, AC + Best performance, elevated, 3 s phases, 5 disk passes.

### Inputs (one static table: `s_v1_inputs` in `src/core/score.cpp`)

| Component | Input | Test / metric | Better | Weight | Reference |
|---|---|---|---|---|---|
| CPU single-thread | Sort 16M u32 | cpu.work.sort / Single thread | higher | 3 | 15.96 Melem/s |
| | Prime sieve | cpu.work.sieve / Single thread | higher | 3 | 2877 Mnum/s |
| | Integer ALU | cpu.int.alu / Single thread | higher | 2 | 17.49 Gops/s |
| | SHA-256 | cpu.crypto.sha256 / Single thread | higher | 1.5 | 2.359 GB/s |
| | AES-128-CTR | cpu.crypto.aes / Single thread* | higher | 1.5 | 28.33 GB/s |
| | Integer multiply | cpu.int.mul / Single thread | higher | 1 | 4.764 Gops/s |
| | FP32 FMA scalar | cpu.fp.scalar / Single thread | higher | 1 | 19.08 GFLOPS |
| | SGEMM | cpu.fp.sgemm / Single thread | higher | 0.5 | 127.7 GFLOPS |
| | FP32 FMA AVX2 (peak) | cpu.fp.avx2_f32 / Single thread | higher | 0.5 | 152.3 GFLOPS |
| CPU multi-thread | same tests, Multi-thread | sort 3, sieve 3, SGEMM 2, ALU 1.5, SHA 1.5, AES 1.5, imul 1, scalar FMA 1, AVX2 FMA 0.5 | higher | | 103.2 / 19756 / 856.6 / 136.5 / 34.93 / 216.6 / 38.20 / 151.7 / 1197 |
| Memory | DRAM latency | mem.lat.chase / Latency* @ 256 MiB | lower | 3 | 109.8 ns |
| | L3 latency | mem.lat.chase / Latency* @ 4 MiB | lower | 1 | 11.26 ns |
| | DRAM read | mem.bw.read / Multi-thread | higher | 1.5 | 58.80 GB/s |
| | DRAM copy | mem.bw.copy / Multi-thread | higher | 1.5 | 35.06 GB/s |
| GPU | Lit scene avg frame time | gpu_*.3d.scene / Avg frame time | lower | 3 | 0.1314 ms |
| | Lit scene p99 frame time | gpu_*.3d.scene / p99 frame time | lower | 2 | 0.2633 ms |
| | FP32 compute | gpu_*.compute.fp32 / Throughput | higher | 1.5 | 14.76 TFLOPS |
| | VRAM read | gpu_*.compute.vram / Read | higher | 1.5 | 369.0 GB/s |
| | Pixel fill | gpu_*.3d.fill / Pixel fill | higher | 1 | 108.2 Gpix/s |
| | Texture fill | gpu_*.3d.texfill / Texel rate | higher | 1 | 182.0 Gtex/s |
| | Triangles | gpu_*.3d.triangles / Triangles | higher | 1 | 2658 Mtri/s |
| Storage | 4K random read latency QD1 | disk.rnd4k_q1t1 / C: Read latency | lower | 4 | 57.87 µs |
| | 4K random write latency QD1 | disk.rnd4k_q1t1 / C: Write latency | lower | 2 | 19.59 µs |
| | RND4K Q32T1 read | disk.rnd4k_q32t1 / C: Read | higher | 1 | 570.7 MB/s |
| | SEQ1M Q8T1 read | disk.seq1m_q8t1 / C: Read | higher | 1 | 7087 MB/s |
| | SEQ1M Q1T1 read | disk.seq1m_q1t1 / C: Read | higher | 0.5 | 3669 MB/s |
| | SEQ1M Q8T1 write | disk.seq1m_q8t1 / C: Write | higher | 0.5 | 5984 MB/s |

Why these: workload-like tests (sort, sieve, hashing, ALU) weigh most for the CPU; peak FMA counts little
(FUTURE §10). Memory latency weighs highest. The GPU uses the lit scene's average *and* p99 frame time plus the
throughput tests. Storage is dominated by QD1 latency; sequential and high-QD numbers barely move how a PC feels.
Left out on purpose: ratios (x, %), "Theoretical" constants, p99.9 frame times (2–8x run-to-run swings, PLAN
compare-summary entry), draw calls (API/driver CPU cost, 2.1x between APIs), clear / blit (L2-resident, fast
clears), PCIe transfer, LDS, core-to-core matrix, thread scaling (its efficiency is ~core count, it double-counts MT).

`SCORE_V1_TESTS` (score.h) lists the 28 tests the table reads; the new **Score preset** (presets.cpp) runs exactly
those at default settings (3 s phases, best of 3 disk passes on 1 GiB, both GPU APIs), about 5 minutes.

### Overall (component weights in %)

| | CPU ST | CPU MT | Memory | GPU | Storage | Meaning |
|---|---|---|---|---|---|---|
| Overall | 30 | 20 | 15 | 20 | 15 | balanced (FUTURE §10's initial guess) |

**Removed 2026-11-11 (trim)**: the Gaming (25/10/15/45/5), Creator (10/40/20/20/10) and Everyday (40/10/15/5/30) profiles. Overall and the
components are unchanged, so score v1 stays v1. `ScoreVersion::overall_weights[SC_COUNT]` replaces `profile_weights`,
`ScoreResult::overall` replaces `profile[]`; `score_item()` / `score_item_name()` / `SCORE_ITEM_COUNT` (0 = Overall,
1 + c = component) serve the pickers and table rows.

### Validity rules (flags, never hidden)

| Flag (file id) | Rule |
|---|---|
| `incomplete` | some inputs missing → component scores only |
| `settings` | not default: phase < 3 s, threads ≠ all, GPU resolution ≠ 1920x1080, disk file < 1 GiB, write-through |
| `battery` | `system.power.on_ac` false, or an input test's env has `battery` / `power-changed` |
| `power-mode` | Windows power mode Balanced or Best power efficiency |
| `throttle` | an input test's env has `cpu-drop`, `gpu-drop`, `cpu-hot` or `gpu-hot` (M7 monitor) |
| `background` | CPU load during single-thread probes (`mem.lat.chase`, `disk.rnd4k_q1t1`) > 150 / threads + 10 % |
| `memory-integrity` | `system.windows.memory_integrity: on` |
| `not-admin` | not elevated |
| `large-pages` | memory latency was measured with 2 MiB pages (reference: 4 KiB) |
| `noisy` | an input's spread over repeats ≥ 3 % (`spread[]`) |
| `no-conditions` | file from before M7: battery / throttling / load weren't recorded |

The background-load rule fired on my own quick runs while ~11 agents were compiling (mem.lat.chase showed 29 %
CPU load on 16 threads; a lone single-thread test is ~6 %), so it does catch the real problem.

### How indicative is it (calibration plan)

Not calibrated: the score is labelled **preliminary** everywhere (UI, CLI, TOON `status`, website). Plan:
1. Collect 8–15 reference machines across the range (thin-and-light iGPU laptop, gaming laptops, desktops with
   old / new CPUs and GPUs, SATA vs NVMe) via the issue form; each runs the Score preset and records real tasks:
   built-in game benchmarks (e.g. Shadow of the Tomb Raider / Cyberpunk 2077 at 1080p medium, avg + 1 % low),
   a Chromium or LLVM compile time, 7-Zip compress / decompress, Lightroom / darktable export of a fixed photo
   set, app cold-start time (VS Code, Office) after a reboot, and a large-folder copy.
2. Define the "truth" as a weighted mix of those timings (game fps, compile + export + compress, app start + copy +
   browser speedometer). (The per-profile truths went with the profiles, removed 2026-11-11 (trim).)
3. Fit the Overall weights (and, if the data allows, input weights) by maximizing Spearman rank correlation
   between the score and the truth, with weights constrained to the published set of components; leave-one-out
   to check it isn't overfitting 10 machines.
4. Publish the fit (data, method, correlation) in PLAN.md and in the About page ("Correlation with
   real-task timings: ρ = …"), bump the version (v2), drop "preliminary" once ρ ≥ 0.9 on the held-out machines.
Published review numbers can seed step 1 (they must be cited per machine); I did not use any for v1.

### Planned v2 inputs (the table accepts missing ids, so they can be added as soon as they exist)

- Storage: QD1 4K random read latency **percentiles** (p50 / p99) from the per-I/O histograms, 4K write + flush
  (fsync) latency, latency under load (QD1 read while sequential writes run), file-system metadata latency;
  sequential numbers further down-weighted (FUTURE §2.1, disk-bench stream).
- CPU: compression / decompression (Windows Compression API XPRESS / LZMS, own LZ77), JSON / text parsing,
  image decode (WIC), hashing (CRC32C / xxHash) — workload-like kernels from the cpu stream (FUTURE §3) replace
  most of the peak-FMA weight; per-core ST pass for hybrid CPUs (score ST on the fastest core class).
- Memory: always measure 4 KiB-page latency for the score (drop the `large-pages` flag), loaded latency.
- GPU: on-screen frame times (§5), ray tracing / mesh-shader feature tests, the heavy scene once §14 lands.
- Thread-scaling efficiency only if calibration shows it adds information.

## 2. Files and formats

- `score:` block at the end of every results file (derived values, recomputed by every reader):
  ```
  score:
    version: 1
    status: preliminary
    reference: 2026-11-11_r7-250_rtx5060-laptop
    overall: 1000                       (null when incomplete)
    (profiles[4]{id,value}: removed 2026-11-11 (trim); readers ignore it in older files)
    components[5]{id,value,inputs,of}: cpu_st / cpu_mt / memory / gpu / storage, inputs present of total
    flags: incomplete|settings|...      ("" when none)
  ```
  `ez-pc-bench-cli --score <files> --score-write` adds / replaces it in older files (done for the library file,
  raw data untouched, so the website can show its score; rewritten again 2026-11-11 to drop `profiles[]`).
- `integrity:` block, last (FUTURE §11 P3): `exe` (file name), `exe_sha256` (SHA-256 of the exe that wrote it),
  `sha256` over every line except the top-level `score` and `integrity` blocks (indentation + text + "\n", so
  CRLF conversion and score rewrites don't break it). Readers report intact / edited / no hash (CLI `--score`,
  compare header tooltip, HTML report). Weak by design: anyone can recompute it.
- Run history: every finished run (GUI and CLI, CLI `--no-history` to skip) is saved to
  `<exe dir>\results\history\<yyyymmdd_hhmmss>_<cpu>_<gpu>.toon.txt`. The History window skips files whose
  top-level `kind` is set and isn't `benchmark` (stability reports; frame-capture files are gone, removed 2026-11-11 (trim)).
- Exports go to `<exe dir>\results\`: `ezbench_<date>_<time>_score.png` (window back buffer via WIC),
  `ezbench_<date>_<time>_report.html` (self-contained: inline CSS, ~1 KB of JS for the filter, light / dark).
  The compare window's `_compare.png` file export is **Removed 2026-11-11 (trim)**: **Copy PNG** puts the same back-buffer image on the
  clipboard instead (CF_DIB, from which Windows synthesizes CF_DIBV5 / CF_BITMAP, plus the registered "PNG" format),
  status "Copied a W x H PNG of this window to the clipboard". `snapshot_request(window, nullptr)` = clipboard; the
  Score tab's Save PNG still writes a file.

## 3. UI

- Main window output panel tabs: Results | Charts | **Score** | **Ranking** (`--tab score|ranking` dev flag).
  - Score: big Overall (or "Partial"), version + "preliminary", flags in the warning color (hover: one line each),
    component bars 0–2x with a marker at the reference (1000), "n/m inputs";
    hover a component for its inputs (value, reference, change, weight, test that won), click to expand them inline.
    Buttons: About the score, Save PNG, Select the Score preset.
  - Ranking (FUTURE §11 P1): rank by Overall, any score component or any plain metric of this run (search box);
    bars of this run vs every library run, best first, lower-is-better metrics inverted; hover for the machine.
- About the score (modal, from the Score tab, the compare window and History): formula, reference, calibration
  state, Overall's component weights, every input with weight / reference / direction, all validity flags.
- Compare window: **Score** group (toggle "Score") with Overall and the components per run (flagged runs in
  the warning color, hover = breakdown), integrity in the run header tooltip, Copy PNG, Save HTML, About the score.
- History tool window (button on the System row, `--tool history`): trend chart of the score or any metric across
  the saved runs (points of flagged runs in the warning color, hover = run, value, flags), runs list newest first
  with Overall, the selected value and flags; tick runs → "Compare n" adds them to the compare window.
- CLI: `--preset score`, `--score [files...]` (breakdown with every input, value, reference, change, test; of files
  without running, or of this run), `--score-write`, `--html <file>`, `--no-history`.

## 4. Website (community results without a server)

- `.github/ISSUE_TEMPLATE/submit-result.yml`: issue form (machine, preset, condition checkboxes, the `.toon.txt`
  dropped into a textarea, notes, publishing consent). Maintainers copy accepted files into `library/`.
- `site/results.html`: searchable, sortable table (score + components + DRAM latency, scene fps, 4K QD1 latency),
  links to each raw file; mobile: the table scrolls horizontally, no page overflow at 375 px.
- `tools/site_library.py` (stdlib Python, no network) turns `library/*.toon.txt` into `data/library.json`. The Pages
  workflow runs it and copies the library files to `_site/library/`; it also triggers on `library/**` now.
  `tools/serve_site.ps1` does the same into the git-ignored `site/data/`, `site/library/` for previews.
- The app itself never talks to the network.

## 5. FUTURE.md items

| Item | State |
|---|---|
| §10 P1 normalization, frozen reference per version | done |
| §10 P1 components, weighted geomeans | done (v1 inputs = metrics on `dev`) |
| §10 P1 overall + profiles | overall done (weights = initial guess); profiles removed 2026-11-11 (trim) |
| §10 P1 how indicative / calibration | method + plumbing done (preliminary label, About page with correlation slot); calibration itself needs other machines |
| §10 P1 validity rules | done (11 flags) |
| §10 P1 UI (bars, inputs on hover/expand, vs reference, vs library, TOON) | done (profiles removed 2026-11-11 (trim)) |
| §10 P2 CLI `--score`, compare score row | done |
| §10 P2 §14 workloads in a new version | later (v2 list above) |
| §11 P1 ranking view | done (score and per metric; the library has one run so far) |
| §11 P1 Score preset | done |
| §11 P2 automatic history + trend chart | done |
| §11 P2 PNG + HTML | done (PNG = window screenshot: compare window to the clipboard, Score tab to a file; HTML from the compare set, also CLI `--html`) |
| §11 P2 community results | done (issue form + static results page); review of submissions is manual |
| §11 P3 integrity hash | done |

## 6. Proposed rule wording (for the lead to apply)

CLAUDE.md, Code style, replace "Benchmarks report raw throughput numbers with explicit units; never invented
'scores'." with:

> Benchmarks report raw numbers with explicit units; those are the primary output everywhere. The only
> aggregate is the overall score (`core/score.*`): derived from the raw results by a published, versioned
> formula (one data table per version), always shown with its breakdown and version, never replacing the raw
> numbers. No other made-up "points".

PLAN.md §1, replace 'No synthetic "scores".' with:

> Raw numbers are the primary output. A derived, versioned overall score (FUTURE.md §10) summarizes them; its
> formula, weights and reference are published in the app and docs, and it is labelled preliminary until
> calibrated against real-task timings.

The website's meta description / hero still say "Raw numbers, no scores"; reword together with the rule.

## 7. Verification (2026-11-11)

- `ez-pc-bench-cli --score library/2026-11-11_r7-250_rtx5060-laptop.toon.txt`: every input +0.0 %, all
  components and Overall 1000 (profiles too, before they were removed); flag `no-conditions` (pre-M7 file). Integrity: "no hash (older file)".
- M7-era quick run (`--preset quick`, 1 s phases, not elevated, ~11 agents building in parallel): CPU ST 832
  (6 of 9 inputs), CPU MT 905, Memory 954, GPU 756 (5 of 7; p99 frame time −65 % under load), Storage 885
  (4 of 6); no Overall (incomplete); flags settings, background (mem.lat.chase at 29 % CPU), not-admin.
  The same numbers come back from the saved file (`--score file`), in the compare window and in History.
- Integrity: the written file reads "intact"; changing one digit makes it "edited after it was written";
  `--score-write` on it leaves it byte-identical and still intact.
- History window: a fake `kind: stability` file is skipped ("other files skipped"); the trend picks a score item
  that exists when no run has an Overall.
- Website: results page with the library run (score 1000 from its score block), search + sort, no horizontal
  page overflow at 375 px (the table scrolls inside its card).
- Trim (2026-11-11): `--score` on the old library file (still with `profiles[]`) reads it fine (Overall 1000);
  `--score-write` only dropped the five `profiles[]` lines; a new run's `score:` block has no `profiles[]`; the HTML
  report, compare window (old + new file side by side), Score tab, Ranking ("Score: Overall") and History show Overall
  + components only; `tools/site_library.py` output has no `profiles` key. Copy PNG: the clipboard held "PNG" and
  CF_DIB (+ synthesized formats), both decoding to the 1682 x 871 compare window, upright.

## 8. Merge notes

- New core files (CMake `ezbench_core`): `runfile.cpp` (flat results reader shared by score / history / ranking),
  `score.cpp`, `integrity.cpp`, `history.cpp`, `report.cpp`; core links `bcrypt` (SHA-256).
- New app files (GUI target): `score_ui.cpp`, `history_ui.cpp`, `snapshot.cpp`; GUI links `windowscodecs` (PNG).
- Shared-file edits (all small, additive): `results.cpp` (score + integrity blocks at the end of the file),
  `compare.h/.cpp` (`CmpRun::score`, `CmpRun::integrity`, filled on add / peek / current), `compare_ui.cpp` (score
  group, Save PNG (now Copy PNG) / HTML, About), `presets.cpp/.h` (Score preset), `ui.cpp/.h` (Score / Ranking tabs, history
  autosave, History tool button, `ui_selection`, `ui_set_tab`), `main_gui.cpp` (History tool window row,
  `snapshot_capture` after rendering each window, `--tab`), `tool_windows.h` (`TOOL_HISTORY`), `theme.cpp/.h`
  (`ICON_HISTORY`), `main_cli.cpp` (`--score`, `--score-write`, `--html`, `--no-history`, preset help).
  Other streams adding tool windows will conflict on the `ToolWindow` enum / `s_tool_desc` / `tool_names` lines
  only (append order).
- `library/2026-11-11_…toon.txt` gained a `score:` block (raw data unchanged); keep it when adding runs, or run
  `--score-write` on new ones (the app writes it automatically for new runs).
- New result files carry `score:` and `integrity:` blocks; readers that don't know them skip them (compare.cpp
  already does). Stability files: no score inputs → no score (score `any` false), and History
  ignores files with another `kind`.
- `score_current` / `rf_scratch` use one ~400 KB static scratch: UI thread / CLI main thread only.
