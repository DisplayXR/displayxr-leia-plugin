# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.22)
project(
    DisplayXR-LeiaSR
    VERSION 1.0.0
    LANGUAGES C CXX
    DESCRIPTION "DisplayXR Leia SR display-processor plug-in"
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "The Leia SR plug-in is Windows-only.")
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
# The runtime's own drv_leia_plugin target is gated on
# XRT_BUILD_DRV_LEIA_IN_TREE (default ON in the runtime today; goes away
# entirely after the #263 cleanup lands). We force it OFF here so the
# runtime build never tries to also produce DisplayXR-LeiaSR.dll.

set(XRT_BUILD_DRV_LEIA_IN_TREE OFF CACHE BOOL "Disable in-tree drv_leia inside the fetched runtime" FORCE)

if(DXR_RUNTIME_SOURCE_DIR)
    message(STATUS "Using local runtime checkout: ${DXR_RUNTIME_SOURCE_DIR}")
    add_subdirectory("${DXR_RUNTIME_SOURCE_DIR}" runtime-build EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    set(DXR_RUNTIME_GIT_TAG "feat/extract-leia-plugin" CACHE STRING
        "Runtime git ref to fetch. Bootstrap: branch ref. Post-#263 merge: replace with a v* tag.")
    message(STATUS "Fetching runtime via FetchContent @ ${DXR_RUNTIME_GIT_TAG}")
    FetchContent_Declare(
        displayxr_runtime
        GIT_REPOSITORY https://github.com/DisplayXR/displayxr-runtime.git
        GIT_TAG "${DXR_RUNTIME_GIT_TAG}"
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(displayxr_runtime)
endif()

# --- SR SDK -------------------------------------------------------------
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
