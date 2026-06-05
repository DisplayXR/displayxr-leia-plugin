# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.22)
project(
    DisplayXR-LeiaSR
    VERSION 1.0.6
    LANGUAGES C CXX
    DESCRIPTION "DisplayXR Leia SR display-processor plug-in"
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 AND NOT ANDROID)
    message(FATAL_ERROR
        "The Leia plug-in supports two targets:\n"
        "  - Windows / Leia SR (DisplayXR-LeiaSR.dll, drv_leia)\n"
        "  - Android / Leia CNSDK (libdxrp050_leia_cnsdk.so, drv_leia_android)\n"
        "Configured platform is neither WIN32 nor ANDROID.")
endif()

if(NOT DEFINED BUILD_NUM)
    set(BUILD_NUM "0")
endif()

# --- Runtime dependency -------------------------------------------------
#
# The plug-in DLL consumes the runtime's public ABI (xrt/xrt_plugin.h,
# xrt/xrt_api.h, aux/util/*, aux/math/*, aux/os/*, aux/vk/*) and links
# against DisplayXRClient.lib (the runtime DLL's import lib).
#
# Two modes:
#   - DXR_RUNTIME_SOURCE_DIR set to a local checkout: fast dev iteration
#     and the user's joint-test path (issue #263 plan A6).
#   - Otherwise, FetchContent fetches DXR_RUNTIME_GIT_TAG from GitHub.
#
# Post-#263 the runtime tree no longer carries any drv_leia source, so
# there's no conflict to gate against here — the runtime always builds
# vendor-source-clean.

if(DXR_RUNTIME_SOURCE_DIR)
    message(STATUS "Using local runtime checkout: ${DXR_RUNTIME_SOURCE_DIR}")
    add_subdirectory("${DXR_RUNTIME_SOURCE_DIR}" runtime-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    # Pinned to displayxr-runtime v1.9.0 (ABI major v2 per ADR-020). v1.9.0 is
    # the first runtime tag that ships the optional, append-only
    # xrt_plugin_iface::probe_displays() (#69 / ADR-015) this plug-in
    # implements — earlier tags (≤ v1.8.0) lack the field, so the vtable
    # initializer would not compile against them. The DP vtables carry a
    # struct_size header; the runtime's loader rejects any plug-in whose
    # XRT_PLUGIN_API_VERSION_CURRENT differs from its own. This plug-in MUST be
    # built against headers that ship the matching major, or the runtime will
    # log "ABI major mismatch — plugin_api=N, runtime expects M ... skipping
    # (ADR-020 rule 3)" and fall back to sim_display. The rule-5 CI self-check
    # below asserts this value equals the workflow's runtime checkout ref so
    # the two can't drift.
    set(DXR_RUNTIME_GIT_TAG "82b68ed19158d0d79fb391ac10807ab9cd667b7c" CACHE STRING
        "Runtime git ref to fetch. Must equal the workflow's runtime checkout ref (rule-5 CI check enforces).")
    message(STATUS "Fetching runtime via FetchContent @ ${DXR_RUNTIME_GIT_TAG}")
    FetchContent_Declare(
        displayxr_runtime
        GIT_REPOSITORY https://github.com/DisplayXR/displayxr-runtime.git
        GIT_TAG "${DXR_RUNTIME_GIT_TAG}"
    )
    FetchContent_MakeAvailable(displayxr_runtime)
endif()

if(WIN32)
    # --- SR SDK (Windows / Leia SR plug-in) ----------------------------
    if(NOT DEFINED ENV{LEIASR_SDKROOT})
        message(FATAL_ERROR
            "LEIASR_SDKROOT env var not set. Point at an extracted LeiaSR-SDK-*-win64 directory. "
            "scripts/build-windows.bat fetches it from the sr-sdk-v* release on this repo.")
    endif()
    set(SR_PATH "$ENV{LEIASR_SDKROOT}")
    list(APPEND CMAKE_PREFIX_PATH "${SR_PATH}")
    find_package(simulatedreality CONFIG REQUIRED GLOBAL)
    find_package(srDirectX CONFIG REQUIRED GLOBAL)
    message(STATUS "Leia SR SDK at ${SR_PATH}")

    add_subdirectory(src/drv_leia)
    add_subdirectory(installer)
elseif(ANDROID)
    # --- CNSDK (Android / Leia CNSDK plug-in) --------------------------
    # CNSDK setup (find_package(CNSDK CONFIG)) lives inside
    # src/drv_leia_android/CMakeLists.txt — it consumes CNSDK_ROOT,
    # which the build environment supplies. No NSIS installer on
    # Android; the .so ships in the runtime APK's jniLibs/<ABI>/.
    add_subdirectory(src/drv_leia_android)
endif()
