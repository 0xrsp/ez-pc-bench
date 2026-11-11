// D3D12 backend for the GPU suite. Placeholder until implemented (see gpu_common.h for the contract).
#include "gpu/gpu_common.h"
#include "core/results.h"
#include <stdio.h>

bool gpu_d3d12_init(const BenchParams*, void** state, char* error, u32 error_size) {
    *state = nullptr;
    snprintf(error, error_size, "D3D12 backend not implemented yet");
    return false;
}

void gpu_d3d12_shutdown(void*) {}

void gpu_d3d12_run(void*, GpuTestId, const BenchParams*, BenchLive*, BenchResult* out, const CancelFlag*) {
    res_skip(out, "not implemented");
}
