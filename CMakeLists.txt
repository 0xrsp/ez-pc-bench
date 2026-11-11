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

# Core: benchmarks, runner, sysinfo, results (shared by GUI and CLI)
add_library(ezbench_core STATIC
    src/core/timer.cpp
    src/core/registry.cpp
    src/core/runner.cpp
    src/core/results.cpp
    src/core/baseline.cpp
    src/core/sysinfo.cpp
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
    src/disk/disk_bench.cpp)
target_include_directories(ezbench_core PUBLIC src)
target_link_libraries(ezbench_core PUBLIC ezb_flags PRIVATE ezb_warnings
    dxgi powrprof winmm advapi32 shell32 ole32 propsys synchronization)

# GUI
add_executable(ez-pc-bench WIN32
    src/app/main_gui.cpp
    src/app/ui.cpp
    src/app/settings.cpp
    src/app/theme.cpp
    src/app/gui.rc)
target_link_libraries(ez-pc-bench PRIVATE ezbench_core imgui ezb_flags ezb_warnings d3d11 dxgi dwmapi comdlg32 ole32 shell32)
target_link_options(ez-pc-bench PRIVATE /MANIFEST:NO)

# CLI
add_executable(ez-pc-bench-cli
    src/app/main_cli.cpp
    src/app/app.rc)
target_link_libraries(ez-pc-bench-cli PRIVATE ezbench_core ezb_flags ezb_warnings)
target_link_options(ez-pc-bench-cli PRIVATE /MANIFEST:NO)
