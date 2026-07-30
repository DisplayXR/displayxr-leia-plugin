# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# NSIS installer for the DisplayXR Leia SR plug-in.

if(NOT WIN32)
    return()
endif()

find_program(NSIS_EXECUTABLE makensis
    PATHS
        "$ENV{PROGRAMFILES}/NSIS"
        "$ENV{PROGRAMFILES\(X86\)}/NSIS"
        "C:/Program Files/NSIS"
        "C:/Program Files (x86)/NSIS"
)

if(NOT NSIS_EXECUTABLE)
    message(WARNING "NSIS not found - installer target will not be available")
    return()
endif()

message(STATUS "Found NSIS: ${NSIS_EXECUTABLE}")

set(DXR_LEIA_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(DXR_LEIA_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(DXR_LEIA_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(DXR_LEIA_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

set(DXR_BIN_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")

# Optional code-signing command, sourced from the SIGN_CMD environment
# variable at configure time. Empty on any machine without it (clones,
# GitHub-hosted CI) -> installer + uninstaller build unsigned. On a
# signing-capable build machine SIGN_CMD points at the configured signer.
# Passed to makensis for !finalize/!uninstfinalize.
# No cert/secret lives in this repo. Mirrors displayxr-runtime.
#
# FORCE so the live SIGN_CMD env always wins on reconfigure: a plain CACHE
# var is seeded only on the first configure, so a build tree first configured
# with SIGN_CMD unset would keep caching "" even after SIGN_CMD is later set,
# silently leaving the embedded uninstaller unsigned (#664 validation).
set(SIGN_CMD "$ENV{SIGN_CMD}" CACHE STRING "Code-signing command for installer/uninstaller (empty = unsigned)" FORCE)

# SR Vulkan weaver DLLs — fetched into ${SR_PATH}/bin by
# scripts/build-windows.bat from the sr-sdk-v* releases. BOTH are REQUIRED:
# the plug-in loads one by absolute path from its own directory depending on
# the installed LeiaSR core (leia_vk_weaver_select.cpp).
#   SimulatedRealityVulkanBeta.dll  pre-stamp vtable, for SR <= 1.34.x
#   SimulatedRealityVulkan.dll      stamp-aware (ST-5318), for SR 1.36.x
# Missing the legacy DLL crashes VK apps at xrCreateSession on boxes without it
# on PATH (issue #314). Missing the stamp-aware DLL is quieter and worse: SR
# 1.36.x machines fall back to the pre-stamp weaver and render a coloured
# replica of the scene with a completely clean log. Fail loudly for both.
if(EXISTS "${SR_PATH}/bin/SimulatedRealityVulkanBeta.dll")
    set(SR_VK_BETA_DLL "${SR_PATH}/bin/SimulatedRealityVulkanBeta.dll")
    file(TO_NATIVE_PATH "${SR_VK_BETA_DLL}" SR_VK_BETA_DLL)
    message(STATUS "Bundling SR VK legacy (pre-stamp) DLL: ${SR_VK_BETA_DLL}")
    if(NOT EXISTS "${SR_PATH}/bin/SimulatedRealityVulkan.dll")
        message(FATAL_ERROR
            "SimulatedRealityVulkan.dll (stamp-aware weaver) not found at "
            "${SR_PATH}/bin. Without it, SR 1.36.x machines render a coloured "
            "replica of the scene and nothing in the logs reports a fault. Run "
            "scripts/build-windows.bat first — it fetches the DLL from the "
            "sr-sdk-v*-vkstamp release into the SR SDK's bin/ dir.")
    endif()
else()
    message(FATAL_ERROR
        "SimulatedRealityVulkanBeta.dll not found at ${SR_PATH}/bin. The "
        "plug-in loads it for pre-stamp cores and the installer must bundle it "
        "(issue #314); a missing DLL ships an installer whose VK apps crash at "
        "xrCreateSession. Run scripts/build-windows.bat first — it fetches "
        "the DLL from the sr-sdk-v* release into the SR SDK's bin/ dir.")
endif()

add_custom_target(installer
    COMMAND "${NSIS_EXECUTABLE}"
        "/DVERSION=${DXR_LEIA_VERSION}"
        "/DVERSION_MAJOR=${DXR_LEIA_VERSION_MAJOR}"
        "/DVERSION_MINOR=${DXR_LEIA_VERSION_MINOR}"
        "/DVERSION_PATCH=${DXR_LEIA_VERSION_PATCH}"
        "/DBUILD_NUM=${BUILD_NUM}"
        "/DBIN_DIR=${DXR_BIN_DIR}"
        "/DSR_VK_BETA_DLL=${SR_VK_BETA_DLL}"
        "/DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "/DOUTPUT_DIR=${CMAKE_INSTALL_PREFIX}"
        "/DSIGN_CMD=${SIGN_CMD}"
        "${CMAKE_CURRENT_SOURCE_DIR}/DisplayXRLeiaSRInstaller.nsi"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building DisplayXR Leia SR plug-in installer..."
    VERBATIM
)
add_dependencies(installer DisplayXR-LeiaSR)
