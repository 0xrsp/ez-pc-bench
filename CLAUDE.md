# ez-pc-bench

Lightweight native Windows GUI benchmark (CPU, memory, GPU Vulkan/D3D12, disk). C++ / CMake / clang.
Plan, design and hardware/toolchain knowledge live in [docs/PLAN.md](docs/PLAN.md) — read it first and
keep it updated (decision log, open questions, measured facts).

## Git

- Do NOT add yourself as co-author: no `Co-Authored-By` trailer, no "Generated with Claude" lines.
- Commit messages: short one-line subject. Omit the body/description unless it's genuinely needed; if
  needed, keep it to a few lines.

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

- CMake + Ninja + clang-cl from a VS x64 dev environment (see PLAN.md §3). Ninja must be installed.
