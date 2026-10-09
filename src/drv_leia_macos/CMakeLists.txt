# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# DisplayXR-LeiaSR plug-in — macOS arm (srSDK Metal weaver).
#
# Same self-contained build model as the Linux arm (and the runtime's own
# ELF plug-ins): STATIC-link the aux libraries, do NOT link the runtime dylib,
# do NOT define XRT_USING_RUNTIME_DLL, and export exactly one symbol,
# xrtPluginNegotiate (ADR-019). Mach-O two-level namespace already keeps the
# static aux copies from interposing the runtime's; the export filter keeps the
# dylib's surface to the one ABI entry point anyway.
#
# SR SDK: the v2 C API (sr/sr.h + sr/sr_metal.h) through the STATIC loader
# libsrSDK_loader.a, which dlopens libLeiaSR_runtime.dylib at run time (its
# manifest, or SR_RUNTIME_PATH for dev trees). Point the build at an SDK with
# either
#   SRSDK_ROOT=<tree with include/sr + lib/libsrSDK_loader.a>   (env or -D), or
#   -DSRSDK_INCLUDE_DIR=<dir containing sr/> -DSRSDK_LOADER_LIBRARY=<.a>
# (the second form is what scripts/build-macos.sh uses against a LeiaSR-macos
# build tree, whose packaged install/ can lag the build).

enable_language(OBJC)

if(NOT SRSDK_ROOT AND DEFINED ENV{SRSDK_ROOT})
    set(SRSDK_ROOT "$ENV{SRSDK_ROOT}")
endif()
if(NOT SRSDK_INCLUDE_DIR AND SRSDK_ROOT)
    set(SRSDK_INCLUDE_DIR "${SRSDK_ROOT}/include")
endif()
if(NOT SRSDK_LOADER_LIBRARY AND SRSDK_ROOT)
    set(SRSDK_LOADER_LIBRARY "${SRSDK_ROOT}/lib/libsrSDK_loader.a")
endif()
set(SRSDK_INCLUDE_DIR "${SRSDK_INCLUDE_DIR}" CACHE PATH "Directory containing sr/sr.h and sr/sr_metal.h")
set(SRSDK_LOADER_LIBRARY "${SRSDK_LOADER_LIBRARY}" CACHE FILEPATH "libsrSDK_loader.a (static srSDK loader)")

if(NOT EXISTS "${SRSDK_INCLUDE_DIR}/sr/sr_metal.h" OR NOT EXISTS "${SRSDK_LOADER_LIBRARY}")
    # Hard error, as on the Linux Track B arm: a silent fallback would let
    # someone hardware-validate a plug-in that cannot weave.
    message(FATAL_ERROR
        "DisplayXR-LeiaSR (macOS): the LeiaSR macOS SDK was not found.\n"
        "  SRSDK_INCLUDE_DIR='${SRSDK_INCLUDE_DIR}' (needs sr/sr_metal.h)\n"
        "  SRSDK_LOADER_LIBRARY='${SRSDK_LOADER_LIBRARY}'\n"
        "Set SRSDK_ROOT to a LeiaSR macOS install tree, or pass both variables "
        "(scripts/build-macos.sh does this for a LeiaSR-macos build tree).")
endif()
message(STATUS "DisplayXR-LeiaSR (macOS): srSDK headers ${SRSDK_INCLUDE_DIR}, loader ${SRSDK_LOADER_LIBRARY}")

add_library(DisplayXR-LeiaSR SHARED
    leia_plugin_macos.m
    leia_sr_macos.m
    leia_sr_macos.h
    leia_display_processor_macos.m
    leia_display_processor_macos.h
    # Shared, platform-portable head device from the Windows arm.
    ../drv_leia/leia_device.c
)

target_compile_definitions(DisplayXR-LeiaSR PRIVATE DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}")

target_include_directories(DisplayXR-LeiaSR PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia       # leia_interface.h / leia_edid_table.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia_linux # leia_lens_owner_linux.h (pure-C lens ownership rules)
    ${SRSDK_INCLUDE_DIR}
)

# The SR C99 create-info macros (SrInstanceCreateInfo(...) etc.) pre-fill every
# field and let the caller override some — by design, so clang's
# initializer-overrides note is noise here. Manual retain/release (no ARC), as
# the runtime's own Metal code.
target_compile_options(DisplayXR-LeiaSR PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC>:-Wno-initializer-overrides -fno-objc-arc>
)

target_link_libraries(DisplayXR-LeiaSR PRIVATE
    xrt-interfaces
    aux_util
    aux_os
    aux_math
    drv_includes
    "${SRSDK_LOADER_LIBRARY}"
    "-framework Foundation"
    "-framework AppKit"
    "-framework CoreGraphics"
    "-framework QuartzCore"
    "-framework Metal"
    "-framework IOKit"
    "-framework ImageIO"
)

set_target_properties(DisplayXR-LeiaSR PROPERTIES
    OUTPUT_NAME "DisplayXR-LeiaSR"
    PREFIX ""
    SUFFIX ".dylib"
    C_VISIBILITY_PRESET hidden
    OBJC_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
# Single-symbol export (ADR-019), the Mach-O twin of the Linux version script.
target_link_options(DisplayXR-LeiaSR PRIVATE "LINKER:-exported_symbol,_xrtPluginNegotiate")

# Discovery manifest next to the dylib. binary_path must be ABSOLUTE (the
# runtime's loader does no relative resolution), so it is generated here, at
# generate time, from the target's real output path.
set(DXR_LEIA_MACOS_PROBE_ORDER 50)
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:DisplayXR-LeiaSR>/050-leia-sr.json"
    CONTENT "{
    \"file_format_version\": \"1.0\",
    \"plugin\": {
        \"id\":           \"leia-sr\",
        \"display_name\": \"DisplayXR Leia SR (macOS)\",
        \"vendor\":       \"Leia Inc.\",
        \"version\":      \"${DXR_PLUGIN_GIT_DESC}\",
        \"binary_path\":  \"$<TARGET_FILE:DisplayXR-LeiaSR>\",
        \"probe_order\":  ${DXR_LEIA_MACOS_PROBE_ORDER}
    }
}
")

install(TARGETS DisplayXR-LeiaSR
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/displayxr/plugins COMPONENT Runtime
)
