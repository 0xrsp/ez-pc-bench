// Registry entry points for both GPU suites: one thin wrapper per (API, test) forwarding to the backend.
#include "gpu/gpu_common.h"

#define X(id, e, g, gid, name, flags)                                                                     \
    void gpu_vk_##id(void* s, const BenchParams* p, BenchLive* l, BenchResult* o, const CancelFlag* c) {     \
        gpu_vk_run(s, e, p, l, o, c);                                                                      \
    }                                                                                                      \
    void gpu_d3d12_##id(void* s, const BenchParams* p, BenchLive* l, BenchResult* o, const CancelFlag* c) {  \
        gpu_d3d12_run(s, e, p, l, o, c);                                                                   \
    }
GPU_TESTS(X)
#undef X
