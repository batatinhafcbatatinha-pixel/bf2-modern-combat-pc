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
    src/game_natives.cpp
    src/input_remap.cpp
    src/menu_hooks.cpp
    src/apt_menu.cpp
    src/pc_settings.cpp
    src/pc_theme.cpp
    src/post_effects.cpp
    src/native_gpu/gpu_hooks.cpp
    src/native_gpu/renderer.cpp
    src/native_gpu/textures.cpp
    src/native_gpu/xenos_hlsl.cpp
    src/native_audio/audio_exports.cpp
    src/native_audio/audio_system.cpp
    src/native_audio/command_queue.cpp
    src/native_audio/xma_engine.cpp
)

if(WIN32)
    add_executable(bf2mc WIN32 ${BF2MC_SOURCES})
else()
    add_executable(bf2mc ${BF2MC_SOURCES})
endif()

rexglue_setup_target(bf2mc)

# Line tables for the hand-written sources (profiling); the generated code stays as configured.
set_source_files_properties(src/native_gpu/renderer.cpp src/native_gpu/textures.cpp src/native_gpu/gpu_hooks.cpp
    src/pc_settings.cpp PROPERTIES COMPILE_OPTIONS "-gcodeview;-gline-tables-only")

# Native D3D11 renderer (src/native_gpu): no GPU emulation plugin.
target_link_libraries(bf2mc PRIVATE rex::xxhash)
if(WIN32)
    target_link_libraries(bf2mc PRIVATE d3d11 dxgi d3dcompiler winmm)
endif()

# Native audio (src/native_audio): XMA via the SDK's FFmpeg build; public headers (LGPL 2.1) in
# third_party/ffmpeg.
target_link_libraries(bf2mc PRIVATE rex::libavcodec rex::libavutil)
target_include_directories(bf2mc PRIVATE third_party/ffmpeg/include)

# Vulkan option of the settings menu: DXVK's d3d11.dll/dxgi.dll in vulkan\ next to the
# executable (pc_settings.cpp loads them by full path when "API grafica" is Vulkan).
set(DXVK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/dxvk-3.1.1" CACHE PATH
    "DXVK release for Windows (x64/d3d11.dll, x64/dxgi.dll, LICENSE)")
if(WIN32 AND EXISTS "${DXVK_DIR}/x64/d3d11.dll")
    add_custom_command(TARGET bf2mc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:bf2mc>/vulkan"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/x64/d3d11.dll" "${DXVK_DIR}/x64/dxgi.dll"
                "$<TARGET_FILE_DIR:bf2mc>/vulkan/")
    if(EXISTS "${DXVK_DIR}/LICENSE")
        add_custom_command(TARGET bf2mc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${DXVK_DIR}/LICENSE"
                    "$<TARGET_FILE_DIR:bf2mc>/vulkan/LICENSE.dxvk.txt")
    endif()
endif()


# Native HLSL compiler for Linux (no Windows compiler DLL).
include(src/platform/native_shader_compiler.cmake)
if(TARGET native_shader_compiler)
    foreach(_native_target IN ITEMS bf2mc)
        if(TARGET ${_native_target})
            target_link_libraries(${_native_target} PRIVATE native_shader_compiler)
        endif()
    endforeach()
endif()
