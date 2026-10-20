# bf2mc - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(bf2mc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(BF2MC_SOURCES
    src/main.cpp
    src/hooks.cpp
    src/input_remap.cpp
    src/pc_settings.cpp
    src/native_gpu/gpu_hooks.cpp
    src/native_gpu/renderer.cpp
    src/native_gpu/textures.cpp
    src/native_gpu/xenos_hlsl.cpp
    src/native_audio/audio_exports.cpp
    src/native_audio/audio_system.cpp
    src/native_audio/xma_engine.cpp
)

if(WIN32)
    add_executable(bf2mc WIN32 ${BF2MC_SOURCES})
else()
    add_executable(bf2mc ${BF2MC_SOURCES})
endif()

rexglue_setup_target(bf2mc)

# Native D3D11 renderer (src/native_gpu): no GPU emulation plugin.
target_link_libraries(bf2mc PRIVATE rex::xxhash d3d11 dxgi d3dcompiler winmm)

# Native audio (src/native_audio): XMA via the SDK's FFmpeg build; public headers (LGPL 2.1) in
# third_party/ffmpeg.
target_link_libraries(bf2mc PRIVATE rex::libavcodec rex::libavutil)
target_include_directories(bf2mc PRIVATE third_party/ffmpeg/include)
