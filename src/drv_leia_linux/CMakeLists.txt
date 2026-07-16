# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# DisplayXR-LeiaSR plug-in — Linux desktop arm (Track A).
#
# Self-contained ELF plug-in following the runtime's PROVEN
# drv_sim_display_plugin Linux model (runtime src/xrt/drivers/CMakeLists.txt,
# Linux branch): STATIC-link the aux libraries, do NOT link the runtime .so,
# do NOT define XRT_USING_RUNTIME_DLL, and hide every symbol except
# xrtPluginNegotiate via the version script (issue #496 / ADR-019 — exported
# aux symbols would interpose the runtime's copies at dlopen time).
#
# The weaver backend sits behind the leia_sr_linux.h seam (interface shaped
# by docs/leia-linux-sdk-contract.md, PROPOSED); Track A ships the stub.

find_package(Vulkan REQUIRED) # loader lib for the stub's raw vkCmd* calls

# Compose-under-bg transparency shaders (runtime#757) → SPIR-V byte-array
# headers. The runtime tree owns the spirv_shaders macro (available because the
# runtime is added as a subdirectory above). Shaders are per-backend copies
# (same pattern as drv_leia_android/shaders), sourced from ../drv_leia/shaders.
spirv_shaders(
    LEIA_LINUX_SHADER_HEADERS
    SPIRV_VERSION 1.0
    SOURCES
        shaders/fullscreen_tri.vert
        shaders/compose_under_bg.frag
        shaders/alpha_gate.frag
)

# xcb + xcb-randr for the transient panel-position query (#91 / runtime#715):
# leia_edid_probe_linux.c matches the frozen EDID table against RandR outputs
# to place the panel in the desktop. Already an apt dep of the CI image (the
# runtime's own build needs libxcb-randr0-dev).
find_package(PkgConfig REQUIRED)
pkg_check_modules(PLUGIN_XCB REQUIRED IMPORTED_TARGET xcb xcb-randr)

# Desktop background capture for transparency (runtime#757 WS1): xdg-desktop-
# portal ScreenCast (D-Bus) → PipeWire → dma-buf. OPTIONAL — not REQUIRED — so a
# CI image without these -dev packages still builds the graceful-decline path and
# stays green (portal/PipeWire cannot run in CI anyway). When present, the real
# producer compiles behind DXR_LEIA_HAVE_PIPEWIRE.
pkg_check_modules(PLUGIN_PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PLUGIN_DBUS IMPORTED_TARGET dbus-1)
if(PLUGIN_PIPEWIRE_FOUND AND PLUGIN_DBUS_FOUND)
    message(STATUS "DisplayXR-LeiaSR: desktop capture enabled (libpipewire-0.3 + dbus-1)")
else()
    message(STATUS "DisplayXR-LeiaSR: desktop capture unavailable (no libpipewire-0.3/dbus-1) "
                   "— transparency falls back to the 2D-under backdrop")
endif()

# --- Weaver backend seam — the Track B switch point ----------------------
set(DXR_LEIA_LINUX_WEAVER "stub" CACHE STRING
    "Linux weaver backend: 'stub' (no SR SDK, passthrough SBS) or 'sdk' (Track B)")
add_library(leia_weaver_backend INTERFACE)
if(DXR_LEIA_LINUX_WEAVER STREQUAL "stub")
    target_sources(leia_weaver_backend INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/leia_sr_stub.c)
elseif(DXR_LEIA_LINUX_WEAVER STREQUAL "sdk")
    # Track B: the real LeiaSR srSDK (SPIKE — pinned to the prototype package,
    # srSDK API 1.0.0). The SDK is commercial-licensed and never vendored:
    # point SRSDK_ROOT (or CMAKE_PREFIX_PATH) at a local unpack. Absent SDK is
    # a hard error by design — a silent stub fallback would let someone
    # hardware-validate the wrong backend, the exact confusion this seam
    # exists to prevent. CI never enters this branch (default = stub).
    if(DEFINED ENV{SRSDK_ROOT})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{SRSDK_ROOT}")
    endif()
    find_package(srSDK 1.0 CONFIG REQUIRED) # exports srSDK::loader (static; dlopens libLeiaSR_runtime.so)
    target_sources(leia_weaver_backend INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/leia_sr_linux_sdk.c)
    target_link_libraries(leia_weaver_backend INTERFACE srSDK::loader)

    # DEV-ONLY convenience: bake the SDK lib dir as DT_RUNPATH so the static
    # loader's dlopen("libLeiaSR_runtime.so") resolves with zero environment
    # setup on a spike/bring-up box (glibc honors the CALLING object's
    # runpath — that's this plug-in .so).
    #
    # RELEASE builds MUST turn this OFF (-DDXR_LEIA_SDK_DEV_RPATH=OFF): the
    # deployment model is Windows parity — Leia's own Linux runtime installer
    # lays down the SR stack and registers it at
    # /etc/leia/sr/1/active_runtime.json (the loader's first search path,
    # with the SR_RUNTIME_PATH env as the manual override). A shipped plug-in
    # must resolve the runtime through that registration, never through a
    # build-machine path baked into the ELF.
    option(DXR_LEIA_SDK_DEV_RPATH
        "Bake the srSDK lib dir as DT_RUNPATH (dev/bring-up only; OFF for release builds)" ON)
    if(DXR_LEIA_SDK_DEV_RPATH)
        get_target_property(_srsdk_inc srSDK::loader_dynamic INTERFACE_INCLUDE_DIRECTORIES)
        get_filename_component(_srsdk_lib "${_srsdk_inc}/../lib" ABSOLUTE)
        target_link_options(leia_weaver_backend INTERFACE "LINKER:-rpath,${_srsdk_lib}")
        message(STATUS "DisplayXR-LeiaSR: Track B srSDK weaver backend (DEV rpath -> ${_srsdk_lib})")
    else()
        message(STATUS "DisplayXR-LeiaSR: Track B srSDK weaver backend (release: runtime via "
                       "/etc/leia/sr/1/active_runtime.json, no baked rpath)")
    endif()
else()
    message(FATAL_ERROR "Unknown DXR_LEIA_LINUX_WEAVER='${DXR_LEIA_LINUX_WEAVER}' (stub|sdk)")
endif()

# --- Plug-in .so target ---------------------------------------------------

add_library(DisplayXR-LeiaSR SHARED
    leia_plugin_linux.c
    leia_display_processor_linux.c
    leia_display_processor_linux.h
    leia_edid_probe_linux.c
    leia_edid_probe_linux.h
    leia_bg_capture_linux.c
    leia_bg_capture_linux.h
    leia_sr_linux.h
    # Shared, platform-portable device implementation from the Windows arm.
    ../drv_leia/leia_device.c
    # Generated SPIR-V headers for the compose-under-bg pass (runtime#757).
    ${LEIA_LINUX_SHADER_HEADERS}
)

# Surface the plug-in build identity through xrt_plugin_iface::version (#47).
target_compile_definitions(DisplayXR-LeiaSR PRIVATE DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}")

target_include_directories(DisplayXR-LeiaSR PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia # leia_interface.h / leia_types.h
    ${CMAKE_CURRENT_BINARY_DIR}             # generated shaders/*.h (runtime#757)
)

target_link_libraries(DisplayXR-LeiaSR PRIVATE
    leia_weaver_backend
    xrt-interfaces
    aux_util
    aux_os
    aux_math
    aux_vk
    drv_includes
    Vulkan::Vulkan
    PkgConfig::PLUGIN_XCB
)

# Desktop-capture producer (runtime#757 WS1): link + enable the real path only
# when both libs are present; otherwise the graceful-decline path is compiled.
if(PLUGIN_PIPEWIRE_FOUND AND PLUGIN_DBUS_FOUND)
    target_compile_definitions(DisplayXR-LeiaSR PRIVATE DXR_LEIA_HAVE_PIPEWIRE)
    target_link_libraries(DisplayXR-LeiaSR PRIVATE PkgConfig::PLUGIN_PIPEWIRE PkgConfig::PLUGIN_DBUS)
endif()

# One-symbol export discipline (#496): hidden visibility + version script
# keeping only xrtPluginNegotiate global; --exclude-libs,ALL is belt-and-
# braces against the statically linked aux archives.
set_target_properties(DisplayXR-LeiaSR PROPERTIES
    OUTPUT_NAME "DisplayXR-LeiaSR"
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_link_options(DisplayXR-LeiaSR PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/plugin_exports.version"
    "LINKER:--exclude-libs,ALL"
)

install(TARGETS DisplayXR-LeiaSR
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/displayxr/plugins COMPONENT Runtime
)
