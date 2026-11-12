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
# Stability tester (src/stability/stab_gpu.cpp, D3D12 only; the SPIR-V output is unused)
ezb_shader(../../stability/stab.hlsl cs_stab_fill         cs_6_0)
ezb_shader(../../stability/stab.hlsl cs_stab_check        cs_6_0)
ezb_shader(../../stability/stab.hlsl cs_stab_compute      cs_6_0)
ezb_shader(../../stability/stab.hlsl vs_stab_full         vs_6_0)
ezb_shader(../../stability/stab.hlsl ps_stab_render       ps_6_0)
ezb_shader(../../stability/stab.hlsl cs_stab_render_check cs_6_0)
ezb_shader(../../stability/stab.hlsl cs_stab_power        cs_6_0)
# Extra GPU tests (src/gpu/gpu_ext.h)
ezb_shader(compute_ext.hlsl cs_fp64          cs_6_0)
ezb_shader(compute_ext.hlsl cs_dp4a          cs_6_4)
ezb_shader(compute_ext.hlsl cs_atomic_one    cs_6_0)
ezb_shader(compute_ext.hlsl cs_atomic_unique cs_6_0)
ezb_shader(compute_ext.hlsl cs_atomic_lds    cs_6_0)
ezb_shader(memory.hlsl      cs_chase         cs_6_0)
ezb_shader(memory.hlsl      cs_chase_init    cs_6_0)
ezb_shader(memory.hlsl      cs_sweep         cs_6_0)
ezb_shader(memory.hlsl      cs_mem_fill      cs_6_0)
ezb_shader(texture.hlsl     cs_tex_sample    cs_6_0)
ezb_shader(texture.hlsl     cs_tex_aniso     cs_6_0)
ezb_shader(pso.hlsl         cs_pso           cs_6_0)
ezb_shader(pso.hlsl         vs_pso           vs_6_0)
ezb_shader(pso.hlsl         ps_pso           ps_6_0)
# Vulkan-only GLSL (cooperative matrix): SPIR-V words as a C initializer list in <name>_spv.inc
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH REQUIRED)
function(ezb_glsl file name)
    set(src ${SHADER_SRC}/${file})
    set(inc ${SHADER_OUT}/${name}_spv.inc)
    add_custom_command(OUTPUT ${inc}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=c ${ARGN} -o ${inc} ${src}
        DEPENDS ${src} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${inc} PARENT_SCOPE)
endfunction()
ezb_glsl(coopmat.comp coopmat_f16)
ezb_glsl(coopmat.comp coopmat_i8 -DINT8)
# Feature tests (gpu_features.h). SM 6.5 entries: ray query, task / mesh shaders, sampler feedback.
function(ezb_shader_dxil file entry profile)   # D3D12-only shaders (no SPIR-V equivalent)
    set(src ${SHADER_SRC}/${file})
    set(dxil ${SHADER_OUT}/${entry}_dxil.h)
    add_custom_command(OUTPUT ${dxil}
        COMMAND ${DXC_DXIL} -nologo -Wno-ignored-attributes -T ${profile} -E ${entry} -O3 -Fh ${dxil} -Vn g_${entry}_dxil ${src}
        DEPENDS ${src} ${SHADER_SRC}/common.hlsli VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${dxil} PARENT_SCOPE)
endfunction()
ezb_shader(rt.hlsl        cs_rt_terrain cs_6_0)
ezb_shader(rt.hlsl        cs_rt_trace   cs_6_5)
ezb_shader(mesh.hlsl      vs_mesh       vs_6_0)
ezb_shader(mesh.hlsl      ps_mesh       ps_6_0)
ezb_shader(mesh.hlsl      as_mesh       as_6_5)
ezb_shader(mesh.hlsl      ms_mesh       ms_6_5)
ezb_shader(tess.hlsl      vs_tess       vs_6_0)
ezb_shader(tess.hlsl      hs_tess       hs_6_0)
ezb_shader(tess.hlsl      ds_tess       ds_6_0)
ezb_shader(tess.hlsl      ps_tess       ps_6_0)
ezb_shader(vrs.hlsl       ps_vrs        ps_6_0)
ezb_shader_dxil(sfb.hlsl  ps_sfb_plain    ps_6_5)
ezb_shader_dxil(sfb.hlsl  ps_sfb_feedback ps_6_5)
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
    src/core/sysinfo_cpu.cpp
    src/core/sysinfo_devices.cpp
    src/core/sysinfo_display.cpp
    src/core/sysinfo_gpu.cpp
    src/core/sysreport.cpp
    src/core/physdisk.cpp
    src/core/sensors.cpp
    src/core/monitor.cpp
    src/core/hwread.cpp
    src/core/pawnio.cpp
    src/core/pawnio_modules.cpp
    src/core/unzip.cpp
    src/core/adv_sensors.cpp
    src/core/adv_cpu.cpp
    src/core/adv_smbus.cpp
    src/core/adv_umc.cpp
    src/core/sysinfo_spd.cpp
    src/core/battery.cpp
    src/core/emi.cpp
    src/core/presets.cpp
    src/core/runfile.cpp
    src/core/score.cpp
    src/core/integrity.cpp
    src/core/json.cpp
    src/core/history.cpp
    src/core/report.cpp
    src/core/disk_health.cpp
    src/core/ata.cpp
    src/core/nvme_dev.cpp
    src/core/scsi_pt.cpp
    src/core/csmi.cpp
    src/core/smart_attr.cpp
    src/core/health_store.cpp
    src/core/perf.cpp
    src/core/elevation.cpp
    src/core/smoke_bench.cpp
    src/core/threads.cpp
    src/core/mem_util.cpp
    src/core/throughput.cpp
    src/mem/mem_bench.cpp
    src/mem/mem_kernels.cpp
    src/mem/mem_grid.cpp
    src/mem/mem_load.cpp
    src/cpu/cpu_bench.cpp
    src/cpu/cpu_kernels.cpp
    src/cpu/cpu_crypto.cpp
    src/cpu/cpu_gemm.cpp
    src/cpu/cpu_topo.cpp
    src/cpu/cpu_compress.cpp
    src/cpu/cpu_image.cpp
    src/cpu/cpu_parse.cpp
    src/cpu/cpu_sci.cpp
    src/cpu/cpu_hash.cpp
    src/cpu/cpu_ai.cpp
    src/cpu/cpu_pi.cpp
    src/cpu/cpu_micro.cpp
    src/disk/disk_suite.cpp
    src/disk/disk_io.cpp
    src/disk/disk_cdm.cpp
    src/disk/disk_latency.cpp
    src/disk/disk_nvme.cpp
    src/disk/disk_sweep.cpp
    src/disk/disk_sustained.cpp
    src/disk/disk_recovery.cpp
    src/disk/disk_files.cpp
    src/disk/disk_raw.cpp
    src/gpu/gpu_common.cpp
    src/gpu/gpu_entry.cpp
    src/gpu/gpu_d3d12.cpp
    src/gpu/gpu_d3d12_compute.cpp
    src/gpu/gpu_d3d12_raster.cpp
    src/gpu/gpu_vk.cpp
    src/gpu/gpu_vk_tests.cpp
    src/gpu/gpu_present.cpp
    src/gpu/gpu_present_d3d12.cpp
    src/gpu/gpu_present_vk.cpp
    src/core/whea.cpp
    src/stability/stability.cpp
    src/stability/stab_cpu.cpp
    src/stability/stab_mem.cpp
    src/stability/stab_disk.cpp
    src/stability/stab_gpu.cpp
    src/gpu/gpu_multi.cpp
    src/gpu/gpu_ext.cpp
    src/gpu/gpu_vk_ext.cpp
    src/gpu/gpu_vk_memory.cpp
    src/gpu/gpu_vk_pso.cpp
    src/gpu/gpu_vk_texture.cpp
    src/gpu/gpu_d3d12_ext.cpp
    src/gpu/gpu_d3d12_memory.cpp
    src/gpu/gpu_d3d12_pso.cpp
    src/gpu/gpu_d3d12_texture.cpp
    src/gpu/gpu_features.cpp
    src/gpu/gpu_d3d12_features.cpp
    src/gpu/gpu_vk_features.cpp)
target_include_directories(ezbench_core PUBLIC src PRIVATE ${SHADER_OUT})
add_dependencies(ezbench_core gpu_shaders)
target_link_libraries(ezbench_core PUBLIC ezb_flags PRIVATE ezb_warnings Vulkan::Vulkan
    d3d12 dxgi dxguid powrprof winmm advapi32 shell32 ole32 propsys synchronization pdh gdi32 ntdll tbs setupapi
    cabinet windowscodecs bcrypt oleaut32 cfgmgr32 iphlpapi wevtapi urlmon)
# Benchmark kernels: align loops to 64 B (one Zen 4 op-cache line) so results don't depend on where the
# linker happens to place a hot loop. Unaligned, the prime sieve swung 1850 <-> 2800 Mnum/s between builds.
set_source_files_properties(src/cpu/cpu_bench.cpp src/cpu/cpu_kernels.cpp src/cpu/cpu_crypto.cpp src/cpu/cpu_gemm.cpp
    src/cpu/cpu_topo.cpp src/cpu/cpu_compress.cpp src/cpu/cpu_image.cpp src/cpu/cpu_parse.cpp src/cpu/cpu_sci.cpp
    src/cpu/cpu_hash.cpp src/cpu/cpu_ai.cpp src/cpu/cpu_pi.cpp src/cpu/cpu_micro.cpp
    src/mem/mem_bench.cpp src/mem/mem_kernels.cpp src/mem/mem_grid.cpp src/mem/mem_load.cpp
    PROPERTIES COMPILE_OPTIONS "/clang:-falign-loops=64")

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
    src/app/overlay.cpp
    src/app/health_ui.cpp
    src/app/adv_ui.cpp
    src/app/mem_charts.cpp
    src/app/graph.cpp
    src/app/system_ui.cpp
    src/app/stability_ui.cpp
    src/app/library.cpp
    src/app/score_ui.cpp
    src/app/history_ui.cpp
    src/app/snapshot.cpp
    src/app/settings.cpp
    src/app/theme.cpp
    src/app/updater.cpp
    src/app/gui.rc
    ${CMAKE_BINARY_DIR}/library.rc)
target_link_libraries(ez-pc-bench PRIVATE ezbench_core imgui ezb_flags ezb_warnings d3d11 dxgi dwmapi comdlg32 ole32
    shell32 cabinet windowscodecs urlmon wintrust crypt32 winhttp)
target_link_options(ez-pc-bench PRIVATE /MANIFEST:NO)
# Auto-update from GitHub releases (src/app/updater.h). Only release builds for publishing turn it on (the
# `dist` preset); in dev builds the check never runs (a local build never matches the published exe).
option(EZB_UPDATER "GUI checks GitHub for a newer ez-pc-bench.exe on startup" OFF)
target_compile_definitions(ez-pc-bench PRIVATE EZB_UPDATER=$<BOOL:${EZB_UPDATER}>)

# CLI
add_executable(ez-pc-bench-cli
    src/app/main_cli.cpp
    src/app/stability_cli.cpp
    src/app/app.rc)
target_link_libraries(ez-pc-bench-cli PRIVATE ezbench_core ezb_flags ezb_warnings)
target_link_options(ez-pc-bench-cli PRIVATE /MANIFEST:NO)
