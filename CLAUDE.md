# ez-pc-bench

Lightweight native Windows GUI benchmark (CPU, memory, GPU Vulkan/D3D12, disk). C++ / CMake / clang.
Plan, design and hardware/toolchain knowledge live in [docs/PLAN.md](docs/PLAN.md) — read it first and
keep it updated (decision log, open questions, measured facts).

## Git

- Do NOT add yourself as co-author: no `Co-Authored-By` trailer, no "Generated with Claude" lines.
- Commit messages: short one-line subject. Omit the body/description unless it's genuinely needed; if
  needed, keep it to a few lines.

## Releases

- GitHub releases (`gh release create vX.Y.Z`): upload `ez-pc-bench.exe` and `ez-pc-bench-cli.exe` as
  separate assets so each can be downloaded on its own. No zip. Build fresh from the tagged commit first.

## Website

- `site/` is the GitHub Pages site (https://0xrsp.github.io/ez-pc-bench/), plain HTML/CSS in `site/index.html`
  with screenshots in `site/img/`. `.github/workflows/pages.yml` deploys it on every push to main that touches
  `site/` (it copies the Inter fonts in from `third_party/`). Preview locally with
  `python -m http.server 8765 --directory site` after copying the two Inter TTFs to `site/fonts/` (git-ignored).

## Icon

- Master: `assets/icon.svg` (italic E + lightning-bolt Z). After editing it run
  `powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1`: regenerates `assets/icon.ico` (16–256 px,
  embedded in both exes as resource 100 and used for the window icons) and `site/favicon.*` /
  `site/apple-touch-icon.png`. Commit the generated files.

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
- Benchmarks report raw throughput numbers with explicit units; never invented "scores".
- Code should be clean and readable: short files per suite, clear names, comments only where intent isn't obvious.

## Build

- CMake + Ninja + clang-cl + lld (see PLAN.md §3), no vcvars needed:
  `cmake --preset release` then `cmake --build --preset release -- -j 6` (or `debug`). Keep `-j 6` and
  never run two heavy builds/benchmarks at once (the PC froze once; see PLAN.md §13).
- Verify with the CLI: `build/release/ez-pc-bench-cli.exe --run <ids> --out out.toon.txt` (`--list` shows ids,
  `--compare <file>` diffs against a saved result). Results files are TOON text (`*.toon.txt`, see core/toon.h).
- GUI dev flags: `--no-elevate` (skip UAC prompt), `--autorun`, `--duration <ms>`, `--dev` (show the smoke-test
  suite), `--only <ids>` (implies `--dev` when an id starts with `smoke`), `--light` / `--dark`, `--compare [files...]` (opens the compare
  window with those results). Keep the build warning-free.
- Don't rewrite source files through PowerShell `Get-Content`/`Set-Content`: it reads UTF-8 as ANSI and garbles
  non-ASCII characters. Use the Edit tool or `[IO.File]::ReadAllText/WriteAllText`, and prefer `\xC2\xB5`-style
  escapes for non-ASCII string literals.
- New tests: add the function in the suite's file and one row in `src/core/registry.cpp`.
- Reference runs for the GUI library: drop a `*.toon.txt` into `library/` (name it
  `<date>_<cpu-short>_<gpu-short>.toon.txt`) and build; the glob is CONFIGURE_DEPENDS, so the next build
  repacks it into the GUI exe.
