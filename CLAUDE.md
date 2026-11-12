# ez-pc-bench

Lightweight native Windows GUI benchmark (CPU, memory, GPU Vulkan/D3D12, disk). C++ / CMake / clang.
Plan, design and hardware/toolchain knowledge live in [docs/PLAN.md](docs/PLAN.md) — read it first and
keep it updated (decision log, open questions, measured facts).

## Git

- Branches: day-to-day work happens on `dev` (commit and push there). `main` only receives finished work:
  merge `dev` into `main` with `git merge --no-ff dev` once a milestone (or a self-contained feature) is
  complete, builds warning-free and passes a CLI run. Never commit directly to `main`. Releases are tagged
  on `main`; the Pages site deploys from `main` only.
- Do NOT add yourself as co-author: no `Co-Authored-By` trailer, no "Generated with Claude" lines.
- Commit messages: short one-line subject. Omit the body/description unless it's genuinely needed; if
  needed, keep it to a few lines.

## Releases

- GitHub releases (`gh release create vX.Y.Z`): upload `ez-pc-bench.exe` and `ez-pc-bench-cli.exe` as
  separate assets so each can be downloaded on its own. No zip. Build fresh from the tagged commit first,
  with the `dist` preset (`cmake --preset dist`, `cmake --build --preset dist -- -j 6`): release + the GUI's
  auto-update check (`EZB_UPDATER`, src/app/updater.h). The GUI compares its own SHA-256 with the
  `ez-pc-bench.exe` asset of `releases/latest`, so that asset must be exactly the exe from `build/dist`.

## Website

- `site/` is the GitHub Pages site (https://0xrsp.github.io/ez-pc-bench/), plain HTML/CSS in `site/index.html`
  with screenshots in `site/img/`. `.github/workflows/pages.yml` deploys it on every push to main that touches
  `site/` (it copies the Inter fonts in from `third_party/`). Preview locally with
  `powershell -ExecutionPolicy Bypass -File tools\serve_site.ps1` (copies the fonts to the git-ignored
  `site/fonts/`, serves http://localhost:8765 with Python).

## Icon

- Master: `assets/icon.svg` (italic E + lightning-bolt Z). After editing it run
  `powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1`: regenerates `assets/icon.ico` (16–256 px,
  embedded in both exes as resource 100 and used for the window icons) and `site/favicon.*` /
  `site/apple-touch-icon.png`. Commit the generated files.

## PawnIO modules

- The app downloads the PawnIO.Modules release named by `PAWNIO_MODULES_VERSION` and checks it against
  `PAWNIO_MODULES_SHA256` (both in `src/core/pawnio_modules.h`). When a new release is out
  (https://github.com/namazso/PawnIO.Modules/releases), bump both together: download the new zip, take its
  hash with `Get-FileHash` (lowercase it), update the two constants and test `--pawnio-modules` +
  `--adv-report` (steps in docs/notes/pawnio.md §10).

## Code style

- C-style C++: procedural, data-oriented. Plain structs + free functions, flat arrays indexed by id,
  static tables instead of class hierarchies.
- Light C++ only: `std::atomic`, `constexpr`, `enum class`/plain enums, default member initializers,
  occasional small templates/lambdas where they clearly simplify. Fixed-width types (`u8`..`u64`, `f32`).
- Avoid: inheritance/virtuals, exceptions (`/EHs-c-`), RTTI, iostreams, smart pointers, heavy STL
  containers in hot paths, hidden allocations. Prefer fixed arrays, arenas, `VirtualAlloc`.
- Use Win32 APIs directly (threads, files, timers). No extra dependencies beyond Dear ImGui (vendored),
  Vulkan SDK and Windows SDK without discussing first.
- Keep it fast and small: no work in the UI thread while benchmarks run beyond drawing at a throttled rate.
- Benchmarks report raw numbers with explicit units; those are the primary output everywhere. The only
  aggregate is the overall score (`core/score.*`): derived from the raw results by a published, versioned
  formula (one data table per version), always shown with its breakdown and version, never replacing the raw
  numbers. No other made-up "points".
- Code should be clean and readable: short files per suite, clear names, comments only where intent isn't obvious.

## Build

- CMake + Ninja + clang-cl + lld (see PLAN.md §3), no vcvars needed:
  `cmake --preset release` then `cmake --build --preset release -- -j 6` (or `debug`). Keep `-j 6` and
  never run two heavy builds/benchmarks at once (the PC froze once; see PLAN.md §13).
- Verify with the CLI: `build/release/ez-pc-bench-cli.exe --run <ids> --out out.toon.txt` (`--list` shows ids,
  `--compare <file>` diffs against a saved result). Results files are TOON text (`*.toon.txt`, see core/toon.h).
- CLI modes (besides `--run`): `--health [--hide-serial]` (drive health report), `--sysinfo [base]` (system
  report), `--stress <tests>` (stability tester), `--log-sensors <file>` (CSV) / `--list-sensors`,
  `--score [files...]` (`--score-write`), `--html <file>`, `--adv-report` (PawnIO sensors), `--pawnio-modules` (download them); run options `--preset quick|standard|extended|sustained|score`, `--repeat N`,
  `--sustain <minutes>`, `--yes` (allow `BENCH_CONFIRM` tests), `--cpu-baseline` (dev: act as if the CPU had no
  AVX / FMA / AVX-512). `-h` prints the full usage.
- GUI dev flags: `--no-elevate` (skip UAC prompt), `--autorun`, `--duration <ms>`, `--dev` (show the smoke-test
  suite), `--only <ids>` (implies `--dev` when an id starts with `smoke`), `--light` / `--dark` / `--theme <id>` (e.g. `solarized-dark`, `monokai`; ids in theme.cpp), `--compare [files...]` (opens the compare
  window with those results), `--tool compare|settings|bench|sensors|health|system|stability|history` (`temps` =
  `sensors`; `bench` = Benchmark settings) (opens that tool window), `--tab results|charts|score|ranking`, `--charts`
  (= `--tab charts`), `--sustain <minutes>`, `--gpu-present offscreen|windowed|fullscreen`, `--overlay` (sensor
  overlay), `--sensor-filter <text>` (Sensors search box), `--sensor-view overview|sensors|log|<device>` (Sensors tab or
  Overview device, e.g. `cpu`, `gpu0`), `--open-gpu-combo` / `--open-theme-combo` (GPU combo / Settings theme dropdown open, for
  screenshots; the latter with `--tool settings`),
  `--stability-run <ids> [seconds]` (`--dev --inject` adds the error self-test), `--check-update` (run the update
  check in a non-`dist` build: always offers the latest release), `--no-update` (skip it in a `dist` build;
  `--dev` skips it too), `--cpu-baseline` (act as if the CPU had no AVX / FMA / AVX-512: those tests greyed out).
  Session only, never saved to the ini.
  Keep the build warning-free.
- Don't rewrite source files through PowerShell `Get-Content`/`Set-Content`: it reads UTF-8 as ANSI and garbles
  non-ASCII characters. Use the Edit tool or `[IO.File]::ReadAllText/WriteAllText`, and prefer `\xC2\xB5`-style
  escapes for non-ASCII string literals.
- New tests: add the function in the suite's file and one row in `src/core/registry.cpp`.
- The build targets x86-64-v2 (SSE4.2 + POPCNT), not AVX2. Code using AVX2 / FMA / AVX-512 goes in functions
  marked `TARGET_AVX2` / `TARGET_FMA` / `__attribute__((target(...)))` and runs only after a CPU check; a test
  that needs them sets `isa` in its registry row (the GUI greys it out, the runner skips it). Vectorized plain C++
  hot loops get an AVX2 copy too (`FORCE_INLINE` body, see `cpu_sci.cpp`), or AVX2 PCs lose up to half.
- Reference runs for the GUI library: drop a `*.toon.txt` into `library/` (name it
  `<date>_<cpu-short>_<gpu-short>.toon.txt`) and build; the glob is CONFIGURE_DEPENDS, so the next build
  repacks it into the GUI exe.
