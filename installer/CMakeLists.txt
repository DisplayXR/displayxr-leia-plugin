# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: BSL-1.0
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

# Path to the SR Vulkan Beta runtime DLL — fetched into ${SR_PATH}/bin by
# scripts/build-windows.bat from the sr-sdk-v* release. Optional; if it
# isn't present the installer just omits the Vulkan-weaver fallback DLL.
if(EXISTS "${SR_PATH}/bin/SimulatedRealityVulkanBeta.dll")
    set(SR_VK_BETA_DLL "${SR_PATH}/bin/SimulatedRealityVulkanBeta.dll")
    file(TO_NATIVE_PATH "${SR_VK_BETA_DLL}" SR_VK_BETA_DLL)
    message(STATUS "Bundling SR VK Beta DLL: ${SR_VK_BETA_DLL}")
else()
    # NSI script tolerates missing SR_VK_BETA_DLL — File line is conditional.
    set(SR_VK_BETA_DLL "")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/DisplayXRLeiaSRInstaller.nsi"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building DisplayXR Leia SR plug-in installer..."
    VERBATIM
)
add_dependencies(installer DisplayXR-LeiaSR)
