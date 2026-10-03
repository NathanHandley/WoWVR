cmake_minimum_required(VERSION 3.20)

project(WoWVR LANGUAGES CXX)

# ---------------------------------------------------------------------------
# WoWVR builds a proxy d3d9.dll that is loaded by the 32-bit WoW 3.3.5a client.
# The client is x86, so the DLL must be x86 as well. Configure with:
#     cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
# ---------------------------------------------------------------------------
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "WoWVR must be built as 32-bit (x86) to match the WoW client. "
        "Reconfigure with: cmake -S . -B build -G \"Visual Studio 17 2022\" -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT: the client folder only ships msvcr80, and we do not want to add a
# VC redistributable dependency to something the user just drops in a folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WOWVR_CLIENT_DIR "C:/Users/Nathan/.local/bin/Client"
    CACHE PATH "WoW client folder that 'deploy' copies the built files into")

# ---------------------------------------------------------------------------
# OpenVR (vendored, sparse checkout: headers + win32 binaries only)
# ---------------------------------------------------------------------------
set(OPENVR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr")
if(NOT EXISTS "${OPENVR_DIR}/headers/openvr.h")
    message(FATAL_ERROR "OpenVR SDK missing at ${OPENVR_DIR}. See README.md for the clone command.")
endif()

add_library(openvr SHARED IMPORTED)
set_target_properties(openvr PROPERTIES
    IMPORTED_LOCATION "${OPENVR_DIR}/bin/win32/openvr_api.dll"
    IMPORTED_IMPLIB   "${OPENVR_DIR}/lib/win32/openvr_api.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENVR_DIR}/headers")

# ---------------------------------------------------------------------------
# The proxy DLL
# ---------------------------------------------------------------------------
set(WOWVR_SOURCES
    src/core/log.cpp
    src/core/log.h
    src/core/config.cpp
    src/core/config.h
    src/core/hotkeys.cpp
    src/core/hotkeys.h
    src/core/image_dump.cpp
    src/core/image_dump.h
    src/core/math3d.cpp
    src/core/math3d.h
    src/core/paths.cpp
    src/core/paths.h
    src/diag/frame_report.cpp
    src/diag/frame_report.h
    src/game/camera_probe.cpp
    src/game/camera_probe.h
    src/game/cull_frustum.cpp
    src/game/cull_frustum.h
    src/game/sound_listener.cpp
    src/game/sound_listener.h
    src/game/field_watch.cpp
    src/game/field_watch.h
    src/game/game_camera.cpp
    src/game/game_camera.h
    src/game/world_pointer.cpp
    src/game/world_pointer.h
    src/present/d3d12_present.cpp
    src/present/d3d12_present.h
    src/present/gl_interop.cpp
    src/present/gl_interop.h
    src/present/presenter.cpp
    src/present/presenter.h
    src/proxy/d3d9_slots.h
    src/proxy/device_hooks.cpp
    src/proxy/device_hooks.h
    src/proxy/dllmain.cpp
    src/proxy/exports.cpp
    src/proxy/exports.h
    src/proxy/iat_hook.cpp
    src/proxy/iat_hook.h
    src/proxy/real_d3d9.cpp
    src/proxy/real_d3d9.h
    src/proxy/vtable_hook.cpp
    src/proxy/vtable_hook.h
    src/render/eye_targets.cpp
    src/render/eye_targets.h
    src/stereo/projection_patch.cpp
    src/stereo/projection_patch.h
    src/stereo/stereo_targets.cpp
    src/stereo/stereo_targets.h
    src/ui/ui_panel.cpp
    src/ui/ui_panel.h
    src/vr/vr_session.cpp
    src/vr/vr_session.h
)

add_library(wowvr_d3d9 SHARED ${WOWVR_SOURCES})

set_target_properties(wowvr_d3d9 PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
    VS_DEBUGGER_COMMAND "${WOWVR_CLIENT_DIR}/Wow.exe"
    VS_DEBUGGER_WORKING_DIRECTORY "${WOWVR_CLIENT_DIR}")

target_include_directories(wowvr_d3d9 PRIVATE src)
target_link_libraries(wowvr_d3d9 PRIVATE openvr d3d11 dxgi opengl32 delayimp)

target_compile_definitions(wowvr_d3d9 PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE)

# AddressSanitizer, off by default. Configure with -DWOWVR_ASAN=ON to hunt memory
# corruption: the green-model fault is decided at startup and is exactly the kind of
# thing inspection keeps missing. Needs clang_rt.asan_dynamic-i386.dll beside the exe.
option(WOWVR_ASAN "Build the proxy with AddressSanitizer" OFF)

target_compile_options(wowvr_d3d9 PRIVATE /W4 /permissive- /Zi)

if(WOWVR_ASAN)
    target_compile_options(wowvr_d3d9 PRIVATE /fsanitize=address /Oy-)
    target_link_options(wowvr_d3d9 PRIVATE /INCREMENTAL:NO)
    message(STATUS "WoWVR: AddressSanitizer enabled")
endif()
target_link_options(wowvr_d3d9 PRIVATE
    "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/proxy/d3d9.def"
    # Delay-loaded so that a missing or broken SteamVR install degrades to "the game
    # runs flat" instead of "the game will not start".
    /DELAYLOAD:openvr_api.dll
    /DEBUG
    /OPT:REF
    /OPT:ICF)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "src" FILES ${WOWVR_SOURCES})

# ---------------------------------------------------------------------------
# deploy: copy the proxy, OpenVR runtime and a default INI into the client
# ---------------------------------------------------------------------------
add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:wowvr_d3d9>" "${WOWVR_CLIENT_DIR}/d3d9.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PDB_FILE:wowvr_d3d9>" "${WOWVR_CLIENT_DIR}/d3d9.pdb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OPENVR_DIR}/bin/win32/openvr_api.dll" "${WOWVR_CLIENT_DIR}/openvr_api.dll"
    DEPENDS wowvr_d3d9
    COMMENT "Deploying WoWVR to ${WOWVR_CLIENT_DIR}")
