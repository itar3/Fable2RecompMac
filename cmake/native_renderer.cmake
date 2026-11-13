# Native renderer (src/gpu, adapted from EdgeOfTimeRecomp; docs/NATIVE_RENDERER.md).
# Included from CMakeLists.txt when FABLE2_NATIVE_RENDERER is on; replaces the
# ReXGlue xenos GPU emulation plugin at run time with fable2_renderer=native.

set(FABLE2_THIRDPARTY "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty")

# reeot-XenosRecomp with the Fable 2 patches (tools/xenosrecomp-patches),
# applied once to the submodule's working tree.
file(GLOB _fable2_xr_patches "${CMAKE_CURRENT_SOURCE_DIR}/tools/xenosrecomp-patches/*.patch")
foreach(_patch IN LISTS _fable2_xr_patches)
    execute_process(
        COMMAND git -C "${FABLE2_THIRDPARTY}/XenosRecomp" apply --reverse --check "${_patch}"
        RESULT_VARIABLE _already_applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already_applied EQUAL 0)
        execute_process(
            COMMAND git -C "${FABLE2_THIRDPARTY}/XenosRecomp" apply "${_patch}"
            RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "Could not apply ${_patch} to thirdparty/XenosRecomp")
        endif()
        message(STATUS "fable2: applied ${_patch}")
    endif()
endforeach()

# plume with the Fable 2 patches (tools/plume-patches).
file(GLOB _fable2_plume_patches "${CMAKE_CURRENT_SOURCE_DIR}/tools/plume-patches/*.patch")
foreach(_patch IN LISTS _fable2_plume_patches)
    execute_process(
        COMMAND git -C "${FABLE2_THIRDPARTY}/plume" apply --reverse --check "${_patch}"
        RESULT_VARIABLE _already_applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT _already_applied EQUAL 0)
        execute_process(
            COMMAND git -C "${FABLE2_THIRDPARTY}/plume" apply "${_patch}"
            RESULT_VARIABLE _applied)
        if(NOT _applied EQUAL 0)
            message(FATAL_ERROR "Could not apply ${_patch} to thirdparty/plume")
        endif()
        message(STATUS "fable2: applied ${_patch}")
    endif()
endforeach()

set(PLUME_D3D12_AGILITY_SDK_ENABLED OFF CACHE BOOL "" FORCE)
add_subdirectory("${FABLE2_THIRDPARTY}/plume" "${CMAKE_BINARY_DIR}/plume" EXCLUDE_FROM_ALL)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Guest shader constants: uniform buffers bound per draw (the GPU's constant
# path; Metal's constant address space) instead of loads through buffer
# addresses. The translator and the renderer must agree.
option(FABLE2_CONSTANT_BUFFERS "Guest shaders read their constants from bound uniform buffers" OFF)
set(XENOS_RECOMP_CONSTANT_BUFFERS ${FABLE2_CONSTANT_BUFFERS} CACHE BOOL "" FORCE)
add_subdirectory("${FABLE2_THIRDPARTY}/XenosRecomp" "${CMAKE_BINARY_DIR}/XenosRecomp" EXCLUDE_FROM_ALL)

# Host shaders (copy, resolve, TAA, ImGui): compiled with the bundled dxc.
set(REEOT_D3D12 OFF)
set(REEOT_GEN_DIR "${CMAKE_BINARY_DIR}/native_generated")
include("${CMAKE_CURRENT_LIST_DIR}/host_shaders.cmake")

# The game's shaders, captured with fable2_shader_dump_dir, translated ahead of
# time by XenosRecomp into a shader cache.
set(FABLE2_SHADER_INPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shader_dump" CACHE PATH
    "Captured shader containers (fable2_shader_dump_dir) the shader cache is built from")
set(FABLE2_SHADER_COMMON_H "${FABLE2_THIRDPARTY}/XenosRecomp/XenosRecomp/shader_common.h")
set(FABLE2_SHADER_CACHE_CPP "${REEOT_GEN_DIR}/shader_cache.cpp")
file(GLOB _fable2_captured_shaders CONFIGURE_DEPENDS "${FABLE2_SHADER_INPUT_DIR}/*.bin")
if(_fable2_captured_shaders)
    add_custom_command(
        OUTPUT "${FABLE2_SHADER_CACHE_CPP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REEOT_GEN_DIR}"
        COMMAND $<TARGET_FILE:XenosRecomp>
                "${FABLE2_SHADER_INPUT_DIR}" "${FABLE2_SHADER_CACHE_CPP}" "${FABLE2_SHADER_COMMON_H}"
        DEPENDS XenosRecomp "${FABLE2_SHADER_COMMON_H}" ${_fable2_captured_shaders}
        COMMENT "Translating the captured Fable 2 shaders into the shader cache"
        VERBATIM)
    add_library(fable2_shader_cache OBJECT "${FABLE2_SHADER_CACHE_CPP}")
else()
    message(STATUS "fable2: no captured shaders in ${FABLE2_SHADER_INPUT_DIR}: the game's shaders "
                   "are translated while it runs (the first start stutters, later starts load them "
                   "from shader_runtime/)")
    add_library(fable2_shader_cache OBJECT src/gpu/shaders/shader_cache_empty.cpp)
endif()
target_include_directories(fable2_shader_cache PRIVATE src/gpu/shaders)
target_compile_options(fable2_shader_cache PRIVATE -O0 -w)

target_sources(Fable2 PRIVATE
    src/platform/display.cpp
    src/platform/moltenvk.cpp
    src/platform/process.cpp
    src/gpu/patches/aspect_ratio.cpp
    src/gpu/fable2_d3d_names.cpp
    src/gpu/fable2_vblank.cpp
    src/gpu/fable2_constants.cpp
    src/gpu/fable2_watchdog.cpp
    src/gpu/queries.cpp
    src/gpu/writeback.cpp
    src/gpu/settings.cpp
    src/gpu/device.cpp
    src/gpu/frame_ring.cpp
    src/gpu/present.cpp
    src/gpu/imgui_overlay.cpp
    src/gpu/format.cpp
    src/gpu/textures.cpp
    src/gpu/surfaces.cpp
    src/gpu/vertex_layout.cpp
    src/gpu/draw.cpp
    src/gpu/memexport.cpp
    src/gpu/render_thread.cpp
    src/gpu/resolve.cpp
    src/gpu/trace.cpp
    src/gpu/pipeline/pipeline_cache.cpp
    src/gpu/gpu_timing.cpp
    src/gpu/memory_report.cpp
    src/gpu/pipeline/pso_records.cpp
    src/gpu/pipeline/pso_precache.cpp
    src/gpu/pipeline/pso_predictor.cpp
    src/gpu/taa.cpp
    src/gpu/shaders/guest_shaders.cpp
    src/gpu/shaders/runtime_shaders.cpp
    src/gpu/hooks/stubs.cpp
    src/gpu/hooks/state.cpp
    src/gpu/hooks/resource.cpp
    "${FABLE2_THIRDPARTY}/XenosRecomp/thirdparty/smol-v/source/smolv.cpp")

foreach(shader IN ITEMS copy_vs imgui_vs)
    reeot_host_shader(Fable2 ${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS blit_ps copy_depth_ps imgui_ps taa_ps
        velocity_resolve_2x_ps velocity_resolve_4x_ps velocity_resolve_8x_ps
        resolve_msaa_color_2x_ps resolve_msaa_color_4x_ps resolve_msaa_color_8x_ps
        resolve_msaa_depth_2x_ps resolve_msaa_depth_4x_ps resolve_msaa_depth_8x_ps
        derive_depth_stencil_2x_ps derive_depth_stencil_4x_ps derive_depth_stencil_8x_ps)
    reeot_host_shader(Fable2 ${shader} ps_6_0)
endforeach()

target_include_directories(Fable2 PRIVATE
    src
    "${REEOT_GEN_DIR}"
    "${FABLE2_THIRDPARTY}/renderdoc"
    "${FABLE2_THIRDPARTY}/XenosRecomp/XenosRecomp"
    "${FABLE2_THIRDPARTY}/XenosRecomp/thirdparty/smol-v/source")
target_compile_definitions(Fable2 PRIVATE FABLE2_NATIVE_RENDERER EOT_MVK NOMINMAX)
if(FABLE2_CONSTANT_BUFFERS)
    target_compile_definitions(Fable2 PRIVATE FABLE2_CONSTANT_BUFFERS)
endif()
# Runtime shader translation (src/gpu/shaders/runtime_shaders.cpp): the
# translator and shader header this build used.
target_compile_definitions(Fable2 PRIVATE
    FABLE2_XENOSRECOMP_EXE="$<TARGET_FILE:XenosRecomp>"
    FABLE2_SHADER_COMMON_H="${FABLE2_SHADER_COMMON_H}")
add_dependencies(Fable2 XenosRecomp)
# This hooks the same XDK functions as the renderer (emulated-GPU debugging).
set_source_files_properties(src/render_mode.cpp PROPERTIES HEADER_FILE_ONLY ON)
target_link_libraries(Fable2 PRIVATE
    plume
    fable2_shader_cache
    xxHash::xxhash
    libzstd_static
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework Metal"
    "-framework Foundation"
    "-framework CoreFoundation"
    "-framework IOKit"
    "-framework IOSurface"
    "-framework CoreGraphics")
