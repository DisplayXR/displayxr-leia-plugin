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

if(DXR_RUNTIME_SOURCE_DIR)
    message(STATUS "Using local runtime checkout: ${DXR_RUNTIME_SOURCE_DIR}")
    add_subdirectory("${DXR_RUNTIME_SOURCE_DIR}" runtime-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    # Pinned to displayxr-runtime v2.0.0 (a8f6e74f) — the release that ships
    # the XR_EXT_* → XR_DXR_* extension rename (#734), which this plug-in's
    # own rename commit builds against. Replaces the TEMPORARY pre-squash
    # rename-branch pin (its displayxr_mcp branch SHA is no longer fetchable).
    # The DP vtables carry a struct_size header; the runtime's loader rejects
    # any plug-in whose XRT_PLUGIN_API_VERSION_CURRENT differs from its own.
    # This plug-in MUST be built against headers that ship the matching major,
    # or the runtime will log "ABI major mismatch — plugin_api=N, runtime
    # expects M ... skipping (ADR-020 rule 3)" and fall back to sim_display.
    # The rule-5 CI self-check below asserts this value equals the workflow's
    # runtime checkout ref so the two can't drift.
    set(DXR_RUNTIME_GIT_TAG "a8f6e74fdfbd26d54bcce859ca5f60bb66963828" CACHE STRING
        "Runtime git ref to fetch. Must equal the workflow's runtime checkout ref (rule-5 CI check enforces).")
    # Linux carries its own pin: the Windows/Android tag predates runtime
    # Linux support entirely (scripts/build_linux.sh first shipped in
    # v1.26.2; the VK-compositor + CI dev-lib fixes #708/#709 first ship
    # in v1.28.0). build-linux.yml's rule-5 self-check asserts this value
    # equals its RUNTIME_REF so the two can't drift.
    set(DXR_RUNTIME_GIT_TAG_LINUX "a8f6e74fdfbd26d54bcce859ca5f60bb66963828" CACHE STRING
        "Runtime git ref for Linux builds. Must equal build-linux.yml's RUNTIME_REF (rule-5 CI check enforces).")
    if(DXR_LINUX_DESKTOP)
        set(_dxr_runtime_ref "${DXR_RUNTIME_GIT_TAG_LINUX}")
        if(_dxr_runtime_ref MATCHES "^v1\\.([0-9]|1[0-9]|2[0-7])\\.")
            message(FATAL_ERROR
                "Runtime ${_dxr_runtime_ref} predates usable Linux support (first complete in v1.28.0). "
                "Set DXR_RUNTIME_GIT_TAG_LINUX to a Linux-capable ref, or point DXR_RUNTIME_SOURCE_DIR "
                "at a local runtime checkout.")
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
