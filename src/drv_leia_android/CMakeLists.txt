# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# DisplayXR Leia CNSDK plug-in (Android).
#
# Builds the CNSDK-backed display-processor as a shared library named
# `libdxrp050_leia_cnsdk.so`, matching the runtime's Android plug-in
# discovery convention (libdxrp<NNN>_<id>.so, where NNN is the
# probe-order and <id> matches the iface's `id` field). See
# `docs/specs/runtime/plugin-discovery.md` §3.2 in the runtime tree.
#
# The .so is meant to be dropped into the runtime APK's
# `jniLibs/<ABI>/` so the runtime's target_plugin_loader.c (PR #309)
# discovers it via dladdr-of-self + dirname enumeration at
# `xrCreateInstance` time.

if(NOT ANDROID)
    message(FATAL_ERROR "drv_leia_android is Android-only.")
endif()

# --- CNSDK ----------------------------------------------------------------
#
# CNSDK ships as an AAR + extracted SDK tree (headers + .so + cmake
# config). The expected layout matches what was previously consumed by
# the runtime POC's openxr_android/build.gradle CNSDK_AAR rule:
#
#   ${CNSDK_ROOT}/VERSION.txt
#   ${CNSDK_ROOT}/android/sdk-<ver>.aar
#   ${CNSDK_ROOT}/include/leia/{common,device,headTracking,sdk}/*.h
#   ${CNSDK_ROOT}/share/cmake/CNSDK/CNSDKConfig.cmake
#   ${CNSDK_ROOT}/lib/<ABI>/{libleiaCore-loader.so,libleiaSDK-jni.so}
#
# CI / dev set CNSDK_ROOT to the extracted release tree. Minimum 0.10.54:
# 0.7.28 predates the loader architecture below and fails at runtime with
# "Missing required service: InterlacingService".

if(NOT DEFINED CNSDK_ROOT)
    if(DEFINED ENV{CNSDK_ROOT})
        set(CNSDK_ROOT "$ENV{CNSDK_ROOT}")
    else()
        message(FATAL_ERROR
            "CNSDK_ROOT not set. Extract a CNSDK 0.10.54+ android release tree "
            "and set CNSDK_ROOT (CMake var or env var) to it.\n"
            "Source (private; needs LeiaInc org read access):\n"
            "  gh release download <tag> -R LeiaInc/CNSDK -p 'cnsdk-android-*.zip'\n"
            "The public leiainc.github.io copy is 0.7.28 and no longer works.")
    endif()
endif()
# Android NDK toolchain defaults CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY,
# which restricts find_package to NDK sysroot dirs and ignores
# CMAKE_PREFIX_PATH outside that. Flip it to BOTH so the CNSDK install
# at ${CNSDK_ROOT} is searchable (matches the runtime APK's gradle
# cmake.arguments setup).
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
list(APPEND CMAKE_PREFIX_PATH "${CNSDK_ROOT}")
# Version-gated on purpose. The SDK ships CNSDKConfigVersion.cmake with
# AnyNewerVersion semantics, so this is a real, zero-cost minimum check:
# 0.7.28 is rejected (7 < 10), 0.10.54+ accepted. Cheaper and more accurate
# than parsing VERSION.txt, which is only the fallback below for zips that
# ever ship without it.
find_package(CNSDK 0.10.62 CONFIG REQUIRED)
if(EXISTS "${CNSDK_ROOT}/VERSION.txt")
    file(READ "${CNSDK_ROOT}/VERSION.txt" _cnsdk_ver)
    string(STRIP "${_cnsdk_ver}" _cnsdk_ver)
    if(_cnsdk_ver VERSION_LESS "0.10.62")
        message(FATAL_ERROR
            "CNSDK ${_cnsdk_ver} at ${CNSDK_ROOT} is too old. The loader "
            "architecture needs 0.10.54+, and the window-independent device-orientation "
            "detection (CNSDK #716) needs 0.10.62+ — without it a core hosted in a "
            "non-fullscreen window mis-detects orientation and the DP's weave "
            "framebuffer faults inside vkCreateFramebuffer.")
    endif()
    message(STATUS "Leia CNSDK ${_cnsdk_ver} at ${CNSDK_ROOT}")
else()
    message(STATUS "Leia CNSDK at ${CNSDK_ROOT} (no VERSION.txt)")
endif()

# CNSDK 0.10.x uses a loader architecture: CNSDK::leiaCore (umbrella) links
# the loader .so (libleiaCore-loader.so) which carries the public C-ABI +
# include dirs and dlopens the real core (which talks to the on-device Leia
# service). No more faceTrackingInApp/InService variant split — the runtime
# selects in-service via leia_core_init_configuration_set_face_tracking_runtime.
# (libleiaSDK-jni.so is loaded by the Java/service side at runtime; we bundle
# it into the runtime APK's jniLibs but don't link it here.)

# --- Plug-in target -------------------------------------------------------

set(DRV_LEIA_ANDROID_SOURCES
    leia_cnsdk.cpp
    leia_cnsdk.h
    leia_display_processor_cnsdk.cpp
    leia_display_processor_cnsdk.h
    leia_plugin_android.c
)

# Post-weave alpha-gate (#568) + pre-weave compose-under-bg (runtime#1073 T0)
# shaders compiled to SPIR-V headers. Ported
# verbatim from the desktop driver (src/drv_leia/shaders/). The runtime tree
# owns the spirv_shaders macro; it's available because the runtime is added as
# a subdirectory above (and glslangValidator runs on the host, not the target).
spirv_shaders(
    LEIA_ANDROID_SHADER_HEADERS
    SPIRV_VERSION 1.0
    SOURCES
        shaders/fullscreen_tri.vert
        shaders/alpha_gate.frag
        shaders/compose_under_bg.frag
)
list(APPEND DRV_LEIA_ANDROID_SOURCES ${LEIA_ANDROID_SHADER_HEADERS})

add_library(dxrp050_leia_cnsdk SHARED ${DRV_LEIA_ANDROID_SOURCES})

# Build identity for the iface's .version (#47).
target_compile_definitions(dxrp050_leia_cnsdk PRIVATE DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}")

target_include_directories(dxrp050_leia_cnsdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}    # generated spirv headers (#568 alpha-gate)
)

target_link_libraries(dxrp050_leia_cnsdk
    PRIVATE
        # Runtime aux surface (consumed via FetchContent or local
        # checkout per top-level CMakeLists.txt).
        xrt-interfaces
        aux_util
        aux_os
        aux_math
        aux_vk
        # CNSDK 0.10.x umbrella target — pulls the loader .so + headers.
        # Face-tracking runs in the device's licensed Leia service (selected
        # via set_face_tracking_runtime(IN_SERVICE)); matching the device's
        # CNSDK 0.10.54 is what makes the service connection actually work
        # (0.7.28 mismatched: "Missing required service: InterlacingService").
        CNSDK::leiaCore
        # Android system libs.
        log
        android
        vulkan
)

# Debug-log + ATrace gating: when XRT_DEBUG_ANDROID_VERBOSE is defined,
# DXR_HW_DBG / DXR_ATRACE blocks compile in. Release builds drop them
# all. The runtime APK's Gradle Debug variant sets this; Release does
# not. For standalone CMake builds, pass -DXRT_DEBUG_ANDROID_VERBOSE=1
# on the cmake command line.

set_target_properties(dxrp050_leia_cnsdk PROPERTIES
    OUTPUT_NAME "dxrp050_leia_cnsdk"
    PREFIX "lib"            # libdxrp050_leia_cnsdk.so (matches discovery convention)
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Single-symbol export discipline (plug-in discovery contract §4):
# only xrtPluginNegotiate should be in the dynamic symbol table.
# C_/CXX_VISIBILITY_PRESET=hidden only affects this target's own
# sources, not symbols pulled in from linked static libs (aux_util,
# aux_vk, jni glue, etc.). --exclude-libs,ALL applies a "hidden"
# visibility filter to ALL static libs, so their symbols stay
# private to this .so even if compiled with default visibility.
# XRT_PLUGIN_EXPORT on xrtPluginNegotiate keeps it explicitly visible.
if(ANDROID OR UNIX)
    target_link_options(dxrp050_leia_cnsdk PRIVATE "LINKER:--exclude-libs,ALL")
endif()
