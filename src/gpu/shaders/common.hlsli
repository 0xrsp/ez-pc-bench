// Shared by all GPU benchmark shaders. Binding rules: see src/gpu/gpu_common.h.
#ifndef EZB_COMMON_HLSLI
#define EZB_COMMON_HLSLI

struct Params {
    uint4 u;
    float4 f;
};
// Vulkan: push constants. D3D12: 8 x 32-bit root constants at b0.
[[vk::push_constant]] ConstantBuffer<Params> pc : register(b0);

uint hash_u32(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hash01(uint x) { return (hash_u32(x) & 0xFFFFFFu) / 16777216.0f; }

#endif
