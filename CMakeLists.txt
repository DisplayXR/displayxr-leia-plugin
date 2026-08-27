# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0

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

# Three targets: Windows / Leia SR (drv_leia), Android / Leia CNSDK
# (drv_leia_android), and Linux desktop / stub weaver (drv_leia_linux,
# Track A of the Linux port — SDK backend lands with Track B, see
# docs/leia-linux-sdk-contract.md). The Android arm was accidentally
# dropped by the v1.17.0 re-pin (29fabc7) — restored here.
set(DXR_LINUX_DESKTOP FALSE)
if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT WIN32)
    set(DXR_LINUX_DESKTOP TRUE) # CMake-side twin of the XRT_OS_LINUX && !XRT_OS_ANDROID code gate
endif()
if(NOT WIN32 AND NOT ANDROID AND NOT DXR_LINUX_DESKTOP)
    message(FATAL_ERROR
        "The Leia plug-in supports three targets:\n"
        "  - Windows / Leia SR (DisplayXR-LeiaSR.dll, drv_leia)\n"
        "  - Android / Leia CNSDK (libdxrp050_leia_cnsdk.so, drv_leia_android)\n"
        "  - Linux desktop / stub weaver (DisplayXR-LeiaSR.so, drv_leia_linux)\n"
        "Configured platform is none of these (macOS is not supported).")
endif()

if(NOT DEFINED BUILD_NUM)
    set(BUILD_NUM "0")
endif()

# --- Plug-in build identity (#47) ----------------------------------------
#
# git-describe baked into the iface's `.version` so every consumer — the
# runtime's loader log, `displayxr-cli info`, the Android diagnostics
# dashboard (runtime#558) — can tell WHICH plug-in build is loaded. The
# Android .so is hand-dropped into the runtime's untracked jniLibs, so a
# stale binary is otherwise invisible (it cost a debugging session when a
# pre-#522 .so silently reported MANAGED-only eye tracking).
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE DXR_PLUGIN_GIT_DESC
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT DXR_PLUGIN_GIT_DESC)
    set(DXR_PLUGIN_GIT_DESC "v${PROJECT_VERSION}")
endif()
message(STATUS "Plug-in version (iface .version): ${DXR_PLUGIN_GIT_DESC}")

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

if(DXR_LINUX_DESKTOP)
    # Headless-Linux runtime defaults — the exact set the runtime's own
    # scripts/build_linux.sh passes, as cache defaults so any invocation
    # (CI container, dev box, bare cmake) configures the runtime subdir
    # identically. CLI stays ON: displayxr-cli selftest is the Track A
    # validation vehicle.
    set(XRT_MODULE_CLI ON CACHE BOOL "displayxr-cli selftest vehicle (leia-plugin Linux default)")
    foreach(_dxr_off
            XRT_FEATURE_SERVICE
            XRT_BUILD_DRIVER_QWERTY
            XRT_FEATURE_DEBUG_GUI
            XRT_FEATURE_WINDOW_PEEK
            XRT_HAVE_SDL2
            XRT_HAVE_OPENCV
            XRT_HAVE_LIBUSB
            XRT_BUILD_DRIVER_EUROC)
        set(${_dxr_off} OFF CACHE BOOL "headless Linux default (leia-plugin)")
    endforeach()
endif()

# Runtime ABI pins. Declared OUTSIDE the fetch/local-checkout branch below
# because they are the plug-in's ABI pairing statement, not merely a
# FetchContent argument: installer/CMakeLists.txt derives the installer's
# minimum-runtime floor from DXR_RUNTIME_GIT_TAG, and that floor must exist
# in both build shapes (fetched pin AND local DXR_RUNTIME_SOURCE_DIR).
# Keep both as literal quoted strings — the rule-5 CI self-check greps for
# `DXR_RUNTIME_GIT_TAG\s+"([^"]+)"` and a computed value would defeat it.
#
# Pinned to displayxr-runtime v2.8.0. That tag is the first runtime that ships the
# append-only D3D11 DP vtable slot 21 `get_backend_state` (runtime#1062/#1064)
# whose implementation is guarded on XRT_DP_D3D11_HAS_BACKEND_STATE: built
# against an older runtime the guard compiles the slot OUT, and the SR-restart
# self-heal still works (the ~1 Hz poll in process_atlas arms it) but the
# runtime cannot SEE the backend state — no `dp=` in the service [HEALTH] line
# and no forced DP recreate for a plug-in that reports STALE. v2.8.0 also
# carries slot 20 `set_window` (runtime#1008, XRT_DP_D3D11_HAS_SET_WINDOW),
# which the previous v2.7.0 pin existed for and which keeps the in-place weaver
# re-bind on focus changes. Both slots are appended, and v2.8.0 reports the same
# plug-in ABI major (v5) as v2.7.0, so this re-pin adds capability without
# moving a single existing slot. The ABI floor it encodes is plugin ABI
# v5 (vk_bundle ABI-parity layout + the Linux dma-buf device-extension
# guarantee, runtime#802) which this plug-in's bg-capture producer needs.
# The DP vtables carry a struct_size header; the runtime's loader rejects
# any plug-in whose XRT_PLUGIN_API_VERSION_CURRENT differs from its own.
# This plug-in MUST be built against headers that ship the matching major,
# or the runtime will log "ABI major mismatch — plugin_api=N, runtime
# expects M ... skipping (ADR-020 rule 3)" and fall back to sim_display.
# The rule-5 CI self-check below asserts this value equals the workflow's
# runtime checkout ref so the two can't drift.
set(DXR_RUNTIME_GIT_TAG "v2.14.6" CACHE STRING
    "Runtime git ref to fetch. Must equal the workflow's runtime checkout ref (rule-5 CI check enforces).")
# Linux carries its own pin: the Windows/Android tag predates runtime
# Linux support entirely (scripts/build_linux.sh first shipped in
# v1.26.2; the VK-compositor + CI dev-lib fixes #708/#709 first ship
# in v1.28.0). build-linux.yml's rule-5 self-check asserts this value
# equals its RUNTIME_REF so the two can't drift.
set(DXR_RUNTIME_GIT_TAG_LINUX "v2.2.0" CACHE STRING
    "Runtime git ref for Linux builds. Must equal build-linux.yml's RUNTIME_REF (rule-5 CI check enforces).")

# Android carries its own pin too. It currently holds the SAME value as the
# Windows tag -- the point is not that it differs today, it is that the Android
# arm can move independently without dragging the Windows installer's derived
# MIN_RUNTIME_VERSION with it.
#
# FLOOR IS v2.7.2. That is the first tag carrying BOTH
# XRT_PLUGIN_HOST_HAS_CLASS_HOST_CONTEXT (runtime#1037 / ADR-036 D2) and
# XRT_DP_VK_HAS_WINDOW_SCREEN_RECT (runtime#1033), which src/drv_leia_android
# compiles against. Both are #ifdef-guarded, so an older pin still COMPILES --
# it just silently ships a plug-in whose CNSDK DexClassLoader cannot reach the
# runtime APK's com.leia.* glue. That failure is a black screen on device, not
# a build error, which is why build-android.yml also asserts the guard actually
# compiled IN by reading a witness string back out of the .so.
#
# The _ANDROID suffix is invisible to the runtime's scripts/check_plugin_abi.py
# regex (DXR_RUNTIME_GIT_TAG\s+") because \s+ cannot match an underscore --
# the same reason _LINUX above is already safe.
set(DXR_RUNTIME_GIT_TAG_ANDROID "v2.14.6" CACHE STRING
    "Runtime git ref for Android builds (floor v2.7.2). Must equal build-android.yml's RUNTIME_REF_ANDROID (rule-5 CI check enforces).")

if(DXR_RUNTIME_SOURCE_DIR)
    message(STATUS "Using local runtime checkout: ${DXR_RUNTIME_SOURCE_DIR}")
    add_subdirectory("${DXR_RUNTIME_SOURCE_DIR}" runtime-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    if(DXR_LINUX_DESKTOP)
        set(_dxr_runtime_ref "${DXR_RUNTIME_GIT_TAG_LINUX}")
        if(_dxr_runtime_ref MATCHES "^v1\\.([0-9]|1[0-9]|2[0-7])\\.")
            message(FATAL_ERROR
                "Runtime ${_dxr_runtime_ref} predates usable Linux support (first complete in v1.28.0). "
                "Set DXR_RUNTIME_GIT_TAG_LINUX to a Linux-capable ref, or point DXR_RUNTIME_SOURCE_DIR "
                "at a local runtime checkout.")
        endif()
    elseif(ANDROID)
        set(_dxr_runtime_ref "${DXR_RUNTIME_GIT_TAG_ANDROID}")
        string(REGEX REPLACE "^v" "" _dxr_android_ver "${_dxr_runtime_ref}")
        if(_dxr_android_ver MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$"
           AND _dxr_android_ver VERSION_LESS "2.7.2")
            message(FATAL_ERROR
                "Runtime ${_dxr_runtime_ref} predates the Android class-host Context slot "
                "(runtime#1037, first tagged v2.7.2). Set DXR_RUNTIME_GIT_TAG_ANDROID to v2.7.2 "
                "or newer, or point DXR_RUNTIME_SOURCE_DIR at a local runtime checkout.")
        endif()
    else()
        set(_dxr_runtime_ref "${DXR_RUNTIME_GIT_TAG}")
    endif()
    message(STATUS "Fetching runtime via FetchContent @ ${_dxr_runtime_ref}")
    FetchContent_Declare(
        displayxr_runtime
        GIT_REPOSITORY https://github.com/DisplayXR/displayxr-runtime.git
        GIT_TAG "${_dxr_runtime_ref}"
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
elseif(DXR_LINUX_DESKTOP)
    # --- Linux desktop (Track A: stub weaver, no SR SDK) ----------------
    # Interface = docs/leia-linux-sdk-contract.md (PROPOSED, #81);
    # implementation = stub passthrough until the SR Linux SDK ships
    # (Track B swaps the backend via DXR_LEIA_LINUX_WEAVER=sdk). No
    # find_package, no installer — the .so is discovered via a JSON
    # manifest (XRT_PLUGIN_SEARCH_PATH / XDG DisplayProcessors roots).
    add_subdirectory(src/drv_leia_linux)
endif()
