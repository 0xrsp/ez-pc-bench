cmake_minimum_required(VERSION 3.29)
project(ez-pc-bench LANGUAGES C CXX RC)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "Build with clang-cl (see CMakePresets.json)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Common flags for all our targets
add_library(ezb_flags INTERFACE)
target_compile_options(ezb_flags INTERFACE /EHs-c- /GR- /arch:AVX2 /utf-8 /Zc:__cplusplus)
target_compile_definitions(ezb_flags INTERFACE
    _HAS_EXCEPTIONS=0 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

add_library(ezb_warnings INTERFACE)
target_compile_options(ezb_warnings INTERFACE /W4 -Wno-missing-field-initializers -Wno-unused-parameter)

# Dear ImGui (vendored, warnings off)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(imgui PRIVATE /W0)
target_link_libraries(imgui PRIVATE ezb_flags)

# GPU: Vulkan SDK + shaders. One HLSL source -> DXIL (Windows SDK dxc, signed via its dxil.dll) and
# SPIR-V (Vulkan SDK dxc). Binding rules in src/gpu/gpu_common.h. Output: build/<preset>/shaders/<entry>_{dxil,spv}.h
if(NOT DEFINED ENV{VULKAN_SDK})
    file(GLOB _vk_sdks "C:/VulkanSDK/*")
    list(SORT _vk_sdks)
    list(POP_BACK _vk_sdks _vk_sdk)
    set(ENV{VULKAN_SDK} "${_vk_sdk}")
endif()
find_package(Vulkan REQUIRED)
file(GLOB _winsdk_dxc "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxc.exe")
list(SORT _winsdk_dxc)
list(POP_BACK _winsdk_dxc DXC_DXIL)
find_program(DXC_SPIRV dxc HINTS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH)
if(NOT DXC_DXIL OR NOT DXC_SPIRV)
    message(FATAL_ERROR "dxc not found (Windows SDK: ${DXC_DXIL}, Vulkan SDK: ${DXC_SPIRV})")
endif()

set(SHADER_SRC ${CMAKE_SOURCE_DIR}/src/gpu/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_HEADERS "")
function(ezb_shader file entry profile)
    set(src ${SHADER_SRC}/${file})
    set(dxil ${SHADER_OUT}/${entry}_dxil.h)
    set(spv ${SHADER_OUT}/${entry}_spv.h)
    add_custom_command(OUTPUT ${dxil}
        COMMAND ${DXC_DXIL} -nologo -Wno-ignored-attributes -T ${profile} -E ${entry} -O3 ${ARGN} -Fh ${dxil} -Vn g_${entry}_dxil ${src}
        DEPENDS ${src} ${SHADER_SRC}/common.hlsli VERBATIM)
    add_custom_command(OUTPUT ${spv}
        COMMAND ${DXC_SPIRV} -nologo -spirv -fspv-target-env=vulkan1.3 -fvk-t-shift 8 0 -fvk-s-shift 16 0
                -T ${profile} -E ${entry} -O3 ${ARGN} -Fh ${spv} -Vn g_${entry}_spv ${src}
        DEPENDS ${src} ${SHADER_SRC}/common.hlsli VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${dxil} ${spv} PARENT_SCOPE)
endfunction()

ezb_shader(alu.hlsl       cs_fp32      cs_6_0)
ezb_shader(alu.hlsl       cs_int32     cs_6_0)
ezb_shader(alu_fp16.hlsl  cs_fp16      cs_6_2 -enable-16bit-types)
ezb_shader(bandwidth.hlsl cs_bw_read   cs_6_0)
ezb_shader(bandwidth.hlsl cs_bw_write  cs_6_0)
ezb_shader(bandwidth.hlsl cs_bw_copy   cs_6_0)
ezb_shader(lds.hlsl       cs_lds       cs_6_0)
ezb_shader(downscale.hlsl cs_downscale cs_6_0)
ezb_shader(raster.hlsl    vs_triangles  vs_6_0)
ezb_shader(raster.hlsl    vs_drawcall   vs_6_0)
ezb_shader(raster.hlsl    ps_color      ps_6_0)
ezb_shader(raster.hlsl    vs_fullscreen vs_6_0)
ezb_shader(raster.hlsl    ps_fill       ps_6_0)
ezb_shader(raster.hlsl    ps_texfill    ps_6_0)
ezb_shader(raster.hlsl    vs_scene      vs_6_0)
ezb_shader(raster.hlsl    ps_scene      ps_6_0)
ezb_shader(raster.hlsl    vs_sprite     vs_6_0)
ezb_shader(raster.hlsl    ps_sprite     ps_6_0)
add_custom_target(gpu_shaders DEPENDS ${SHADER_HEADERS})

# Core: benchmarks, runner, sysinfo, results (shared by GUI and CLI)
add_library(ezbench_core STATIC
    src/core/timer.cpp
    src/core/registry.cpp
    src/core/runner.cpp
    src/core/results.cpp
    src/core/baseline.cpp
    src/core/compare.cpp
    src/core/toon.cpp
    src/core/sysinfo.cpp
    src/core/sysinfo_win.cpp
    src/core/physdisk.cpp
    src/core/sensors.cpp
    src/core/monitor.cpp
    src/core/hwread.cpp
    src/core/battery.cpp
    src/core/emi.cpp
    src/core/presets.cpp
    src/core/disk_health.cpp
    src/core/perf.cpp
    src/core/elevation.cpp
    src/core/smoke_bench.cpp
    src/core/threads.cpp
    src/core/mem_util.cpp
    src/core/throughput.cpp
    src/mem/mem_bench.cpp
    src/mem/mem_kernels.cpp
    src/cpu/cpu_bench.cpp
    src/cpu/cpu_kernels.cpp
    src/cpu/cpu_crypto.cpp
    src/cpu/cpu_gemm.cpp
    src/disk/disk_bench.cpp
    src/gpu/gpu_common.cpp
    src/gpu/gpu_entry.cpp
    src/gpu/gpu_d3d12.cpp
    src/gpu/gpu_d3d12_compute.cpp
    src/gpu/gpu_d3d12_raster.cpp
    src/gpu/gpu_vk.cpp
    src/gpu/gpu_vk_tests.cpp)
target_include_directories(ezbench_core PUBLIC src PRIVATE ${SHADER_OUT})
add_dependencies(ezbench_core gpu_shaders)
target_link_libraries(ezbench_core PUBLIC ezb_flags PRIVATE ezb_warnings Vulkan::Vulkan
    d3d12 dxgi dxguid powrprof winmm advapi32 shell32 ole32 propsys synchronization pdh gdi32 ntdll tbs setupapi)
# Benchmark kernels: align loops to 64 B (one Zen 4 op-cache line) so results don't depend on where the
# linker happens to place a hot loop. Unaligned, the prime sieve swung 1850 <-> 2800 Mnum/s between builds.
set_source_files_properties(src/cpu/cpu_bench.cpp src/cpu/cpu_kernels.cpp src/cpu/cpu_crypto.cpp src/cpu/cpu_gemm.cpp
    src/mem/mem_bench.cpp src/mem/mem_kernels.cpp PROPERTIES COMPILE_OPTIONS "/clang:-falign-loops=64")

# Run library: reference runs in library/*.toon.txt, packed + LZMS-compressed by a small host tool and
# embedded in the GUI exe as RCDATA 103 (read by src/app/library.cpp).
add_executable(ezb_pack tools/pack_library.cpp)
target_include_directories(ezb_pack PRIVATE src)
target_link_libraries(ezb_pack PRIVATE ezb_flags ezb_warnings cabinet)
file(GLOB LIBRARY_RUNS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/library/*.toon.txt)
set(LIBRARY_BIN ${CMAKE_BINARY_DIR}/library.bin)
add_custom_command(OUTPUT ${LIBRARY_BIN}
    COMMAND ezb_pack ${LIBRARY_BIN} ${LIBRARY_RUNS}
    DEPENDS ezb_pack ${LIBRARY_RUNS} VERBATIM)
file(WRITE ${CMAKE_BINARY_DIR}/library.rc "103 10 \"${LIBRARY_BIN}\"\n")     # 10 = RT_RCDATA
set_source_files_properties(${CMAKE_BINARY_DIR}/library.rc PROPERTIES OBJECT_DEPENDS ${LIBRARY_BIN})
# .rc files aren't dependency-scanned: rebuild them when the icon (tools/make_icon.ps1) or fonts change.
set_source_files_properties(src/app/gui.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/icon.ico;${CMAKE_SOURCE_DIR}/third_party/inter/Inter-Regular.ttf;${CMAKE_SOURCE_DIR}/third_party/inter/Inter-Medium.ttf")
set_source_files_properties(src/app/app.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/assets/icon.ico)

# GUI
add_executable(ez-pc-bench WIN32
    src/app/main_gui.cpp
    src/app/ui.cpp
    src/app/compare_ui.cpp
    src/app/settings_ui.cpp
    src/app/temps_ui.cpp
    src/app/health_ui.cpp
    src/app/library.cpp
    src/app/settings.cpp
    src/app/theme.cpp
    src/app/gui.rc
    ${CMAKE_BINARY_DIR}/library.rc)
target_link_libraries(ez-pc-bench PRIVATE ezbench_core imgui ezb_flags ezb_warnings d3d11 dxgi dwmapi comdlg32 ole32
    shell32 cabinet)
target_link_options(ez-pc-bench PRIVATE /MANIFEST:NO)

# CLI
add_executable(ez-pc-bench-cli
    src/app/main_cli.cpp
    src/app/app.rc)
target_link_libraries(ez-pc-bench-cli PRIVATE ezbench_core ezb_flags ezb_warnings)
target_link_options(ez-pc-bench-cli PRIVATE /MANIFEST:NO)
