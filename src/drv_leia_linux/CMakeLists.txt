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

# --- Weaver backend seam — the Track B switch point ----------------------
set(DXR_LEIA_LINUX_WEAVER "stub" CACHE STRING
    "Linux weaver backend: 'stub' (no SR SDK, passthrough SBS) or 'sdk' (Track B)")
add_library(leia_weaver_backend INTERFACE)
if(DXR_LEIA_LINUX_WEAVER STREQUAL "stub")
    target_sources(leia_weaver_backend INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/leia_sr_stub.c)
elseif(DXR_LEIA_LINUX_WEAVER STREQUAL "sdk")
    # TODO(Track B): leia_sr_linux_sdk.c + the LeiaSR Linux SDK
    # find_package/link, satisfying the same leia_sr_linux.h signatures.
    message(FATAL_ERROR
        "DXR_LEIA_LINUX_WEAVER=sdk is Track B — the LeiaSR Linux SDK backend "
        "is not implemented yet (contract: docs/leia-linux-sdk-contract.md).")
else()
    message(FATAL_ERROR "Unknown DXR_LEIA_LINUX_WEAVER='${DXR_LEIA_LINUX_WEAVER}' (stub|sdk)")
endif()

# --- Plug-in .so target ---------------------------------------------------

add_library(DisplayXR-LeiaSR SHARED
    leia_plugin_linux.c
    leia_display_processor_linux.c
    leia_display_processor_linux.h
    leia_sr_linux.h
    # Shared, platform-portable device implementation from the Windows arm.
    ../drv_leia/leia_device.c
)

# Surface the plug-in build identity through xrt_plugin_iface::version (#47).
target_compile_definitions(DisplayXR-LeiaSR PRIVATE DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}")

target_include_directories(DisplayXR-LeiaSR PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia # leia_interface.h / leia_types.h
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
)

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
