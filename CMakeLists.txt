# Fable2 - ReXGlue Recompiled Project
# SDK boilerplate lives in generated/rexglue.cmake, which 'rexglue codegen'
# (re)writes on every run.

cmake_minimum_required(VERSION 3.25)
project(Fable2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR
        "generated/rexglue.cmake not found. Run codegen first from the repo root:\n"
        "  rexglue codegen Fable2_manifest.toml")
endif()
include(generated/rexglue.cmake)

# Third-party libraries
add_library(dlmalloc STATIC thirdparty/dlmalloc/dlmalloc.c)
target_include_directories(dlmalloc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dlmalloc)
target_compile_definitions(dlmalloc PUBLIC
    MSPACES=1
    USE_LOCKS=1
    ONLY_MSPACES=1
    HAVE_MORECORE=0
    ABORT_ON_ASSERT_FAILURE=0
    USE_DL_PREFIX=1
    FOOTERS=0
)

# Sources
set(FABLE2_SOURCES
    src/main.cpp
    src/heap.cpp
    src/hooks.cpp
    src/critical_sections.cpp
    src/cpu_idle.cpp
    src/scripted_input.cpp
    src/render_mode.cpp
    src/gpu_capture.cpp
    src/shader_capture.cpp
)

add_executable(Fable2 ${FABLE2_SOURCES})

target_link_libraries(Fable2 PRIVATE dlmalloc)
if(APPLE)
    target_link_libraries(Fable2 PRIVATE "-framework Metal" "-framework Foundation" objc)
endif()

# Generated sources, rex::runtime, platform entry point, runtime library
# staging, and the xenos GPU emulation plugin (Vulkan; MoltenVK on macOS).
rexglue_setup_target(Fable2 GPU_PLUGINS xenos)

# Native renderer (work in progress, docs/NATIVE_RENDERER.md): builds the
# renderer from src/gpu into the game; selected at run time.
# Profile-guided optimization of the whole executable (the recompiled game code
# is most of the CPU time): "gen" builds an instrumented binary (run it with
# LLVM_PROFILE_FILE=<dir>/fable2-%c%p.profraw; %c writes the counters
# continuously, the game hard-exits), "use" builds with FABLE2_PGO_PROFILE
# (merged with llvm-profdata).
set(FABLE2_PGO "off" CACHE STRING "Profile-guided optimization: off, gen, use")
set(FABLE2_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for FABLE2_PGO=use")
foreach(_pgo_target Fable2 Fable2_recomp)
    if(NOT TARGET ${_pgo_target})
        continue()
    endif()
    if(FABLE2_PGO STREQUAL "gen")
        target_compile_options(${_pgo_target} PRIVATE -fprofile-instr-generate)
    elseif(FABLE2_PGO STREQUAL "use" AND FABLE2_PGO_PROFILE)
        target_compile_options(${_pgo_target} PRIVATE "-fprofile-instr-use=${FABLE2_PGO_PROFILE}"
                               -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
    endif()
endforeach()
if(FABLE2_PGO STREQUAL "gen")
    target_link_options(Fable2 PRIVATE -fprofile-instr-generate)
endif()

option(FABLE2_NATIVE_RENDERER "Build the native renderer (Metal through MoltenVK) and the launcher" ON)
if(FABLE2_NATIVE_RENDERER)
    include(cmake/native_renderer.cmake)
    if(APPLE)
        include(cmake/launcher.cmake)
    endif()
endif()


# The registration table is one huge function of SetFunction calls that runs
# once at startup; optimizing it takes the compiler 10+ minutes for no gain.
set_property(SOURCE generated/Fable2_register.cpp APPEND PROPERTY COMPILE_OPTIONS -O0 -fno-global-isel)
# The shared PCH is built at the target's -O level and clang rejects it here.
set_property(SOURCE generated/Fable2_register.cpp PROPERTY SKIP_PRECOMPILE_HEADERS ON)
