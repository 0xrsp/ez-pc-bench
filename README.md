# ez-pc-bench

A small native Windows benchmark for CPU, memory, GPU (Vulkan and Direct3D 12) and disks. It reports raw
numbers (GFLOPS, GB/s, ns, IOPS, FPS), never synthetic "scores". It comes as a GUI and a command-line exe
that run the same tests.

![ez-pc-bench](docs/screenshot.png)

## What it measures

- **CPU:** integer and FP32/FP64 FMA throughput (scalar, AVX2, AVX-512), SGEMM, SHA-256, AES, sort,
  prime sieve, core-to-core latency and thread scaling. Each runs single-threaded and on all threads.
- **Memory:** DRAM read, write, copy and triad bandwidth, latency from 4 KiB to 1 GiB, and cache
  bandwidth sweeps.
- **GPU (Vulkan + D3D12):**
  - compute: FP32, FP16 and INT32 throughput, VRAM and shared-memory bandwidth, PCIe transfers
  - 3D: triangles, fill rate, draw calls, a lit scene, with avg / p99 / p99.9 frame times (the GUI also
    shows them as fps)
  - 2D: sprites, blit and clear
  - both APIs run the same shaders, so their numbers compare directly
- **Disk:** CrystalDiskMark-style SEQ1M and RND4K at several queue depths, reads and writes, on one or
  more drives.

## Download

Get `ez-pc-bench.exe` (GUI) and/or `ez-pc-bench-cli.exe` from
[Releases](https://github.com/0xrsp/ez-pc-bench/releases). You need Windows
10/11 x64, a CPU with AVX2, and a GPU driver with Vulkan support (it installs `vulkan-1.dll`). The GPU
tests need D3D12 and Vulkan 1.3.

## Build

Requirements:
- LLVM (clang-cl + lld)
- CMake ≥ 3.29 and Ninja
- MSVC build tools with a Windows SDK
- Vulkan SDK

```
cmake --preset release
cmake --build --preset release -- -j 6
```

This builds `build\release\ez-pc-bench.exe` (GUI) and `ez-pc-bench-cli.exe`. No developer prompt or
vcvars is needed.

## Usage

**GUI:**
1. Pick the GPU and disk(s) at the top.
2. Tick the tests you want and press **Run**.
3. **Save results...** asks for an optional run name (Enter keeps the default, e.g.
   `ezbench_20261111_113811_r7-250_rtx5060-laptop`) and writes `results\<name>.toon.txt` next to the exe.
4. **Compare...** opens the compare window. Add saved results (or drop files onto the window), the
   current run, or reference runs from the built-in **Library...** to see them side by side: an
   overall / per-suite summary, system info, raw values and the change vs a reference run.
   **Copy table** puts it on the clipboard for spreadsheets.

The app asks for admin rights at start. They are optional; without them a few tweaks are skipped.

**CLI:**

```
ez-pc-bench-cli --list                            # test ids
ez-pc-bench-cli --run cpu,mem --out run.toon.txt  # run suites / ids / patterns
ez-pc-bench-cli --compare run.toon.txt            # run and diff against a saved run
```

Results are plain [TOON](https://github.com/toon-format/toon) text files.

## Tips

- For stable numbers: use AC power with Windows power mode *Best performance*, and close other apps
  (games especially).
- The disk tests use one temporary file per drive (`<drive>:\ezbench.tmp`). It is deleted automatically,
  even after a crash. A full default disk run (best of 3 passes) writes about 80 GB, so use
  `--passes 1` for frequent runs.

## Reference runs

Runs I made on the PCs I've tested live in [library/](library/) and are built (compressed) into the GUI,
so you can add them from **Library...** in the compare window. The main reference is a Ryzen 7 250 /
RTX 5060 Laptop / Samsung NVMe laptop on AC, Best performance, as admin:
[library/2026-11-11_r7-250_rtx5060-laptop.toon.txt](library/2026-11-11_r7-250_rtx5060-laptop.toon.txt).

Design notes and measurements: [docs/PLAN.md](docs/PLAN.md).

MIT licensed, see [LICENSE](LICENSE). Third-party: Dear ImGui (MIT), Inter font (SIL OFL 1.1).
