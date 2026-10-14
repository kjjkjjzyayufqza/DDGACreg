cmake_minimum_required(VERSION 3.24)

add_library(ddgac SHARED)

file(GLOB_RECURSE DDGAC_CPP CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
target_sources(ddgac PRIVATE ${DDGAC_CPP})

target_include_directories(ddgac PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/minhook/include")

target_link_libraries(ddgac PRIVATE minhook)
target_compile_definitions(ddgac PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(ddgac PRIVATE /W4 /EHsc /utf-8)
    set_target_properties(ddgac PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# version.dll proxy exports, MASM thunks, and HLSL bytecode are added by the
# display port. Keep this output name so Windows loads the DLL beside the EXE.
set_target_properties(ddgac PROPERTIES OUTPUT_NAME "version" SUFFIX ".dll")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/display/version.def")
    target_sources(ddgac PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/display/version.def")
    if(MSVC)
        set_property(TARGET ddgac APPEND PROPERTY LINK_FLAGS
            "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/display/version.def\"")
    endif()
endif()

file(GLOB DDGAC_ASM CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/display/*.asm")
if(DDGAC_ASM)
    enable_language(ASM_MASM)
    target_sources(ddgac PRIVATE ${DDGAC_ASM})
endif()

target_link_libraries(ddgac PRIVATE
    d3d11 dxgi d3dcompiler windowscodecs ole32 uuid ws2_32 winhttp)

# Display port: embed HLSL bytecode with fxc, the same entry points and flags as
# src/display/shaders/compile.cmd. MASM thunks and version.def stay above.
# Upstream builds with _CRT_SECURE_NO_WARNINGS (log.cpp uses _wfopen).
target_compile_definitions(ddgac PRIVATE _CRT_SECURE_NO_WARNINGS)
set(DDGAC_SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/display/shaders")
set(DDGAC_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/display_shaders")

set(_ddgac_fxc_globs)
if(DEFINED ENV{WindowsSdkDir} AND NOT "$ENV{WindowsSdkDir}" STREQUAL "")
    list(APPEND _ddgac_fxc_globs "$ENV{WindowsSdkDir}/bin/*/x64/fxc.exe")
endif()
list(APPEND _ddgac_fxc_globs
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe"
    "C:/Program Files/Windows Kits/10/bin/*/x64/fxc.exe")
set(_ddgac_fxc_hits)
foreach(_glob IN LISTS _ddgac_fxc_globs)
    file(GLOB _hits ${_glob})
    list(APPEND _ddgac_fxc_hits ${_hits})
endforeach()
if(_ddgac_fxc_hits)
    list(SORT _ddgac_fxc_hits COMPARE NATURAL ORDER DESCENDING)
    list(GET _ddgac_fxc_hits 0 DDGAC_FXC)
endif()
if(NOT DDGAC_FXC)
    message(FATAL_ERROR "fxc.exe was not found. Install the Windows SDK so display shaders can be embedded.")
endif()

set(DDGAC_SHADER_HEADERS
    cabinetlight_VSFull.h
    cabinetlight_VSShape.h
    cabinetlight_PSShape.h
    cabinetlight_PSCells.h
    cabinetlight_PSPatches.h
    cabinetlight_PSBounce.h
    cabinetlight_PSCube.h
    cabinetlight_PSGloss.h
    cabinetlight_PSShade.h
    cabinetlight_PSBoothMap.h
    cabinetlight_PSAcrylic.h
    cabinetlight_PSHood.h
    cabinetlight_PSChrome.h
    cabinetlight_PSAO.h
    cabinetlight_PSScrew.h
    compositor_VSMain.h
    compositor_PSMain.h
    compositor_PSWarp.h
    compositor_PSPanel.h)
set(DDGAC_SHADER_HEADER_PATHS)
foreach(_header IN LISTS DDGAC_SHADER_HEADERS)
    list(APPEND DDGAC_SHADER_HEADER_PATHS "${DDGAC_SHADER_OUT}/${_header}")
endforeach()

add_custom_command(
    OUTPUT ${DDGAC_SHADER_HEADER_PATHS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DDGAC_SHADER_OUT}"
    COMMAND cmd /C "${DDGAC_SHADER_SRC}/compile.cmd" "${DDGAC_FXC}" "${DDGAC_SHADER_OUT}"
    DEPENDS
        "${DDGAC_SHADER_SRC}/compile.cmd"
        "${DDGAC_SHADER_SRC}/cabinetlight.hlsl"
        "${DDGAC_SHADER_SRC}/compositor.hlsl"
    COMMENT "Compiling display shaders with fxc"
    VERBATIM)
add_custom_target(ddgac_shaders DEPENDS ${DDGAC_SHADER_HEADER_PATHS})
add_dependencies(ddgac ddgac_shaders)

target_include_directories(ddgac PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/display"
    "${DDGAC_SHADER_OUT}")
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/display/compositor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/display/cabinetlight.cpp"
    PROPERTIES OBJECT_DEPENDS "${DDGAC_SHADER_HEADER_PATHS}")
target_sources(ddgac PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/display/version.rc")
