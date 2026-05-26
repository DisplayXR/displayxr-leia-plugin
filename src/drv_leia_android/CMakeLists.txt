# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: BSL-1.0
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
#   ${CNSDK_ROOT}/include/leia/sdk/*.h
#   ${CNSDK_ROOT}/share/cmake/CNSDK/CNSDKConfig.cmake
#   ${CNSDK_ROOT}/lib/<ABI>/libleiaSDK-faceTrackingInApp.so
#
# CI / dev set CNSDK_ROOT to the extracted release tree. Pinned to
# CNSDK 0.7.28 for the POC.

if(NOT DEFINED CNSDK_ROOT)
    if(DEFINED ENV{CNSDK_ROOT})
        set(CNSDK_ROOT "$ENV{CNSDK_ROOT}")
    else()
        message(FATAL_ERROR
            "CNSDK_ROOT not set. Extract CNSDK 0.7.28 and set "
            "CNSDK_ROOT (CMake var or env var) to the extracted dir. "
            "Source: https://github.com/LeiaInc/leiainc.github.io/tree/master/CNSDK/cnsdk-android-0.7.28.zip")
    endif()
endif()
# Android NDK toolchain defaults CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY,
# which restricts find_package to NDK sysroot dirs and ignores
# CMAKE_PREFIX_PATH outside that. Flip it to BOTH so the CNSDK install
# at ${CNSDK_ROOT} is searchable (matches the runtime APK's gradle
# cmake.arguments setup).
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
list(APPEND CMAKE_PREFIX_PATH "${CNSDK_ROOT}")
find_package(CNSDK CONFIG REQUIRED)
message(STATUS "Leia CNSDK at ${CNSDK_ROOT}")

# CNSDK 0.7.28's CMake config only exports targets for the
# faceTrackingInService variant (out-of-process face tracking via
# a separate Leia system service). For in-process operation on a
# stock device + emulator testing we want the faceTrackingInApp
# variant, which CNSDK ships as a sibling .so but doesn't expose a
# target for. Define our own IMPORTED target around it.
if(NOT TARGET CNSDK::leiaSDK-faceTrackingInApp)
    set(_inapp_lib "${CNSDK_ROOT}/lib/${CMAKE_ANDROID_ARCH_ABI}/libleiaSDK-faceTrackingInApp.so")
    if(NOT EXISTS "${_inapp_lib}")
        message(FATAL_ERROR "CNSDK in-app face-tracking lib not found: ${_inapp_lib}")
    endif()
    # Reuse CNSDK::leiaSDK-faceTrackingInService-shared's include dirs
    # (they're identical between variants — same headers, different
    # backing impl) so the plug-in's #include <leia/sdk/core.h> etc.
    # resolve.
    get_target_property(_cnsdk_includes
        CNSDK::leiaSDK-faceTrackingInService-shared
        INTERFACE_INCLUDE_DIRECTORIES)
    add_library(CNSDK::leiaSDK-faceTrackingInApp-shared SHARED IMPORTED)
    set_target_properties(CNSDK::leiaSDK-faceTrackingInApp-shared PROPERTIES
        IMPORTED_LOCATION "${_inapp_lib}"
        INTERFACE_INCLUDE_DIRECTORIES "${_cnsdk_includes}"
    )
    add_library(CNSDK::leiaSDK-faceTrackingInApp INTERFACE IMPORTED)
    set_target_properties(CNSDK::leiaSDK-faceTrackingInApp PROPERTIES
        INTERFACE_LINK_LIBRARIES "CNSDK::leiaSDK-faceTrackingInApp-shared"
    )
endif()

# --- Plug-in target -------------------------------------------------------

set(DRV_LEIA_ANDROID_SOURCES
    leia_cnsdk.cpp
    leia_cnsdk.h
    leia_display_processor_cnsdk.cpp
    leia_display_processor_cnsdk.h
    leia_plugin_android.c
)

add_library(dxrp050_leia_cnsdk SHARED ${DRV_LEIA_ANDROID_SOURCES})

target_include_directories(dxrp050_leia_cnsdk PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
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
        # CNSDK Vulkan interlacer + in-process face-tracker.
        # (The InApp variant is what we ship; see the IMPORTED-target
        # definition above for why we don't just use CNSDK::leiaSDK.)
        CNSDK::leiaSDK-faceTrackingInApp
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
