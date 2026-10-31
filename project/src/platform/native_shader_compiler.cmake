if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    find_package(SDL3 REQUIRED CONFIG)
    pkg_check_modules(NATIVE_VKD3D REQUIRED IMPORTED_TARGET libvkd3d-shader>=1.19)
    set(DXVK_NATIVE_DIR "" CACHE PATH "DXVK native prefix containing include/dxvk and lib")
    find_path(NATIVE_DXVK_INCLUDE d3d11.h HINTS "${DXVK_NATIVE_DIR}/include/dxvk" PATH_SUFFIXES dxvk REQUIRED)
    find_library(NATIVE_DXVK_D3D11 NAMES dxvk_d3d11 HINTS "${DXVK_NATIVE_DIR}/lib" "${DXVK_NATIVE_DIR}/lib64" REQUIRED)
    find_library(NATIVE_DXVK_DXGI NAMES dxvk_dxgi HINTS "${DXVK_NATIVE_DIR}/lib" "${DXVK_NATIVE_DIR}/lib64" REQUIRED)
    add_library(native_shader_compiler STATIC "${CMAKE_CURRENT_LIST_DIR}/native_shader_compiler.cpp")
    # The Win32 subset the PC settings screen uses (GDI text through FreeType/fontconfig, ini files, process).
    target_sources(native_shader_compiler PRIVATE "${CMAKE_CURRENT_LIST_DIR}/native_win32_posix.cpp")
    pkg_check_modules(NATIVE_TEXT REQUIRED IMPORTED_TARGET fontconfig freetype2)
    target_link_libraries(native_shader_compiler PRIVATE PkgConfig::NATIVE_TEXT)
    # DT_RPATH, not RUNPATH: DXVK native dlopen()s libSDL3.so.0 and must find it next to the executable too.
    target_link_options(native_shader_compiler INTERFACE -Wl,--disable-new-dtags)
    set_target_properties(native_shader_compiler PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(native_shader_compiler PUBLIC cxx_std_17)
    target_compile_options(native_shader_compiler PUBLIC -fms-extensions)
    target_compile_definitions(native_shader_compiler PUBLIC DXVK_WSI_SDL3)
    target_include_directories(native_shader_compiler PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${NATIVE_DXVK_INCLUDE}")
    target_link_libraries(native_shader_compiler PRIVATE PkgConfig::NATIVE_VKD3D)
    target_link_libraries(native_shader_compiler PUBLIC SDL3::SDL3 "${NATIVE_DXVK_D3D11}" "${NATIVE_DXVK_DXGI}")
endif()

# macOS (x86-64: Intel Macs, or Rosetta 2 on Apple Silicon): the same renderer on DXMT (D3D11 on Metal), shaders
# through vkd3d-shader, window and input through the SDK's SDL3. See MACOS.md and tools/macos/build-deps.sh.
if(APPLE)
    enable_language(OBJC)
    find_package(PkgConfig REQUIRED)
    find_package(SDL3 REQUIRED CONFIG)
    pkg_check_modules(NATIVE_VKD3D REQUIRED IMPORTED_TARGET libvkd3d-shader>=1.19)
    set(DXMT_DIR "" CACHE PATH "DXMT prefix containing include/windows, include/directx and lib/d3d11.dylib")
    if(NOT EXISTS "${DXMT_DIR}/lib/d3d11.dylib")
        message(FATAL_ERROR "DXMT_DIR must contain lib/d3d11.dylib: run tools/macos/build-deps.sh")
    endif()
    add_library(native_shader_compiler STATIC "${CMAKE_CURRENT_LIST_DIR}/native_shader_compiler.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/native_win32_posix.cpp" "${CMAKE_CURRENT_LIST_DIR}/native_metal_bridge.m")
    set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/native_metal_bridge.m" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    pkg_check_modules(NATIVE_TEXT REQUIRED IMPORTED_TARGET fontconfig freetype2)
    target_link_libraries(native_shader_compiler PRIVATE PkgConfig::NATIVE_TEXT PkgConfig::NATIVE_VKD3D)
    target_compile_features(native_shader_compiler PUBLIC cxx_std_17)
    target_compile_options(native_shader_compiler PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fms-extensions>)
    target_include_directories(native_shader_compiler PUBLIC "${CMAKE_CURRENT_LIST_DIR}"
        "${DXMT_DIR}/include/windows" "${DXMT_DIR}/include/directx")
    target_link_libraries(native_shader_compiler PUBLIC SDL3::SDL3 "${DXMT_DIR}/lib/d3d11.dylib"
        "${DXMT_DIR}/lib/dxgi.dylib" "-framework Foundation" "-framework Metal" "-framework QuartzCore")
    # DXMT finds macdrv_functions with dlsym, so the bridge must be linked in and exported.
    target_link_options(native_shader_compiler INTERFACE -Wl,-u,_macdrv_functions -Wl,-export_dynamic
        "-Wl,-rpath,@executable_path")
endif()
