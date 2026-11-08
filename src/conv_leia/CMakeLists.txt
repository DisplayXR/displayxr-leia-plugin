# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: Apache-2.0
#
# DisplayXR-LeiaConversion.dll — the Leia conversion provider (runtime ADR-053,
# docs/conversion-provider.md). Desktop Linux builds DisplayXR-LeiaConversion.so
# from the same provider source with the CPU transport (LEIA_CONV_CPU_TRANSPORT:
# host-memory images, NeurD's CPU entry point; see the POSIX section below). A SEPARATE DLL from DisplayXR-LeiaSR.dll: no SR
# SDK, no weaver, no SR session, no link to the display DLL. It compiles the
# same NeurD module source the display DLL's lift slots use
# (../drv_leia/leia_lift_neurd.cpp + the pure viewpoint / depth helpers) and
# exports exactly one symbol, xrtConversionPluginNegotiate.
#
# Needs:
#  - runtime headers with xrt/xrt_conversion_plugin.h (ABI 1, runtime >= v2.41.0);
#  - the private NeurD headers (NEURD_SDK_ROOT, fetched by build-windows.bat /
#    the CI from LeiaInc/media_sdk). Without them the DLL is not built (the
#    rest of the plug-in is unaffected) — exactly like the lift slots.
# The pure policy layer and its test build regardless.

enable_testing()

# --- Pure policy (no Windows, no runtime header): every lane -------------
add_library(leia_conv_policy STATIC leia_conv_policy.c leia_conv_policy.h)
target_include_directories(leia_conv_policy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The Linux arm's src/drv_leia_linux already builds this test for the Windows
# provider's policy layer (it is added first); one target per name.
if(NOT TARGET test_conv_leia_policy)
    add_executable(test_conv_leia_policy tests/test_conv_leia_policy.c)
    target_link_libraries(test_conv_leia_policy PRIVATE leia_conv_policy)
    add_test(NAME test_conv_leia_policy COMMAND test_conv_leia_policy)
endif()

# --- Pure CPU transport (POSIX arm's pixel moves; plain C): every lane ----
add_library(leia_conv_cpu STATIC leia_conv_cpu.c leia_conv_cpu.h)
target_include_directories(leia_conv_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT WIN32)
    target_link_libraries(leia_conv_cpu PUBLIC m)
endif()
add_executable(test_conv_leia_cpu tests/test_conv_leia_cpu.c)
target_link_libraries(test_conv_leia_cpu PRIVATE leia_conv_cpu)
add_test(NAME test_conv_leia_cpu COMMAND test_conv_leia_cpu)

# --- Gate: conversion ABI header + NeurD headers ---------------------------
set(LEIA_CONV_AVAILABLE FALSE)
if(NOT DEFINED NEURD_SDK_ROOT AND DEFINED ENV{NEURD_SDK_ROOT})
    set(NEURD_SDK_ROOT "$ENV{NEURD_SDK_ROOT}")
endif()
get_target_property(_lc_xrt_inc xrt-interfaces INTERFACE_INCLUDE_DIRECTORIES)
set(_lc_have_abi FALSE)
foreach(_d IN LISTS _lc_xrt_inc)
    string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" _d "${_d}")
    if(EXISTS "${_d}/xrt/xrt_conversion_plugin.h")
        set(_lc_have_abi TRUE)
    endif()
endforeach()
if(NOT _lc_have_abi)
    message(STATUS "Leia conversion provider: runtime headers predate xrt_conversion_plugin.h "
                   "(need runtime >= v2.41.0) - DisplayXR-LeiaConversion.dll NOT built")
elseif(NOT (NEURD_SDK_ROOT
            AND EXISTS "${NEURD_SDK_ROOT}/include/NeurD.h"
            AND EXISTS "${NEURD_SDK_ROOT}/include/NeurD_version.h"
            AND EXISTS "${NEURD_SDK_ROOT}/include/_NeurD_detail.h"
            AND EXISTS "${NEURD_SDK_ROOT}/include/_NeurD_table.h"))
    message(STATUS "Leia conversion provider: NeurD headers NOT found (NEURD_SDK_ROOT='${NEURD_SDK_ROOT}') - "
                   "DisplayXR-LeiaConversion.dll NOT built")
else()
    set(LEIA_CONV_AVAILABLE TRUE)
endif()
set(LEIA_CONV_AVAILABLE ${LEIA_CONV_AVAILABLE} PARENT_SCOPE)
if(NOT LEIA_CONV_AVAILABLE)
    return()
endif()

if(NOT WIN32)
    # ---- POSIX arm (desktop Linux; the caller only adds this directory
    # there). Same provider source, CPU transport. NeurD is dlopen'ed
    # (libNeurD.so), never linked: a box without NeurD loads the provider and
    # reports ABSENT. The D3D11 module (../drv_leia/leia_lift_neurd.cpp) is
    # replaced by leia_conv_neurd_posix.cpp; the convert_ex / per-feature header
    # greps do not apply (the CPU path has neither depth nor off-axis).
    set(LEIA_CONV_POSIX_SOURCES
        leia_conv_plugin.cpp
        leia_conv_policy.c
        leia_conv_policy.h
        leia_conv_cpu.c
        leia_conv_cpu.h
        leia_conv_neurd_posix.cpp
        leia_conv_neurd_cpu.h
        ../drv_leia/leia_lift_neurd.h)
    function(leia_conv_configure_posix tgt)
        target_include_directories(${tgt} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia
            "${NEURD_SDK_ROOT}/include")
        target_compile_definitions(${tgt} PRIVATE
            LEIA_CONV_CPU_TRANSPORT
            DXR_LEIA_HAS_NEURD
            DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}")
        set_target_properties(${tgt} PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
            POSITION_INDEPENDENT_CODE ON)
    endfunction()
    include(GNUInstallDirs)
    message(STATUS "Leia conversion provider: building DisplayXR-LeiaConversion.so (CPU transport; NeurD "
                   "headers at ${NEURD_SDK_ROOT})")

    add_library(DisplayXR-LeiaConversion SHARED ${LEIA_CONV_POSIX_SOURCES})
    leia_conv_configure_posix(DisplayXR-LeiaConversion)
    # aux_util / aux_os linked statically and kept local by the version script
    # (the runtime's own POSIX conversion providers do the same).
    target_link_libraries(DisplayXR-LeiaConversion PRIVATE xrt-interfaces aux_util aux_os ${CMAKE_DL_LIBS})
    set_target_properties(DisplayXR-LeiaConversion PROPERTIES
        OUTPUT_NAME "DisplayXR-LeiaConversion"
        PREFIX ""
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_link_options(DisplayXR-LeiaConversion PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/conversion_plugin_exports.version"
        "LINKER:--exclude-libs,ALL"
        "LINKER:--no-undefined")
    install(TARGETS DisplayXR-LeiaConversion
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/displayxr/plugins COMPONENT Conversion)

    # Dev manifest next to the build output (absolute binary_path, POSIX rule):
    # point XRT_PLUGIN_SEARCH_PATH at this directory for a dev run.
    set(LEIA_CONV_MANIFEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/_conv_manifest")
    file(MAKE_DIRECTORY "${LEIA_CONV_MANIFEST_DIR}")
    file(GENERATE OUTPUT "${LEIA_CONV_MANIFEST_DIR}/050-leia-conversion-conversion-provider.json" CONTENT
"{
    \"file_format_version\": \"1.0\",
    \"plugin\": {
        \"id\":           \"leia-conversion\",
        \"display_name\": \"Leia conversion (NeurD)\",
        \"vendor\":       \"Leia Inc.\",
        \"version\":      \"${DXR_PLUGIN_GIT_DESC}\",
        \"binary_path\":  \"$<TARGET_FILE:DisplayXR-LeiaConversion>\",
        \"probe_order\":  50
    }
}
")

    # Headless harness: loads the provider .so through its one export (as the
    # runtime does), converts a still image to side-by-side PNGs and prints
    # timings. Not a ctest when NeurD is absent from the box; the ABSENT /
    # DISABLED gates below are.
    set(_lc_stb "")
    foreach(_d IN LISTS _lc_xrt_inc)
        string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" _d "${_d}")
        if(EXISTS "${_d}/../../external/stb/stb_image.h")
            get_filename_component(_lc_stb "${_d}/../../external/stb" ABSOLUTE)
        endif()
    endforeach()
    if(_lc_stb)
        add_executable(conv_leia_harness tests/conv_leia_harness.c)
        target_include_directories(conv_leia_harness PRIVATE "${_lc_stb}")
        target_link_libraries(conv_leia_harness PRIVATE xrt-interfaces ${CMAKE_DL_LIBS} m)
        add_dependencies(conv_leia_harness DisplayXR-LeiaConversion)
        add_test(NAME test_conv_leia_posix_absent
            COMMAND conv_leia_harness --provider $<TARGET_FILE:DisplayXR-LeiaConversion>
                    --neurd-path "${CMAKE_CURRENT_BINARY_DIR}/no-such-neurd" --expect absent)
        add_test(NAME test_conv_leia_posix_disabled
            COMMAND conv_leia_harness --provider $<TARGET_FILE:DisplayXR-LeiaConversion>
                    --enable 0 --expect disabled)
    else()
        message(STATUS "Leia conversion provider: stb not found next to the runtime headers - harness not built")
    endif()
    return()
endif()

# The same per-feature header greps as the display DLL's lift module.
file(STRINGS "${NEURD_SDK_ROOT}/include/NeurD.h" _lc_stream_props REGEX "NeurD_set_stream_prop_1i[ ]*[(]")
file(STRINGS "${NEURD_SDK_ROOT}/include/NeurD.h" _lc_convert_ex REGEX "NeurD_convert_stream_dx_ex[ ]*[(]")
set(_lc_sp FALSE)
set(_lc_ex FALSE)
if(_lc_stream_props)
    set(_lc_sp TRUE)
endif()
if(_lc_convert_ex)
    set(_lc_ex TRUE)
endif()
message(STATUS "Leia conversion provider: building DisplayXR-LeiaConversion.dll (NeurD headers at "
               "${NEURD_SDK_ROOT}; per-stream props=${_lc_sp}, convert_ex=${_lc_ex})")

set(LEIA_CONV_SOURCES
    leia_conv_plugin.cpp
    leia_conv_d3d11.cpp
    leia_conv_d3d11.h
    leia_conv_policy.c
    leia_conv_policy.h
    ../drv_leia/leia_lift_neurd.cpp
    ../drv_leia/leia_lift_neurd.h
    ../drv_leia/leia_lift_viewpoint.c
    ../drv_leia/leia_lift_viewpoint.h
    ../drv_leia/leia_lift_depth.c
    ../drv_leia/leia_lift_depth.h
)

function(leia_conv_configure tgt)
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia
        "${NEURD_SDK_ROOT}/include")
    target_compile_definitions(${tgt} PRIVATE
        DXR_LEIA_HAS_NEURD
        DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}"
        WIN32_LEAN_AND_MEAN NOMINMAX)
    if(_lc_stream_props)
        target_compile_definitions(${tgt} PRIVATE DXR_LEIA_NEURD_HAS_STREAM_PROPS)
    endif()
    if(_lc_convert_ex)
        target_compile_definitions(${tgt} PRIVATE DXR_LEIA_NEURD_HAS_CONVERT_EX)
    endif()
    set_target_properties(${tgt} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endfunction()

# --- The provider DLL --------------------------------------------------------
configure_file(version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
add_library(DisplayXR-LeiaConversion SHARED ${LEIA_CONV_SOURCES} "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
leia_conv_configure(DisplayXR-LeiaConversion)
# The aux surface (u_log*) comes from the runtime core DLL the host preloads
# (ADR-019), as for the display DLL and the runtime's sim-conversion provider.
target_compile_definitions(DisplayXR-LeiaConversion PRIVATE XRT_USING_RUNTIME_DLL)
target_link_libraries(DisplayXR-LeiaConversion PRIVATE
    $<TARGET_LINKER_FILE:${RUNTIME_TARGET}>
    xrt-interfaces
    aux_util
    aux_os
    d3d11 dxgi d3dcompiler version advapi32)
add_dependencies(DisplayXR-LeiaConversion ${RUNTIME_TARGET})
set_target_properties(DisplayXR-LeiaConversion PROPERTIES
    OUTPUT_NAME "DisplayXR-LeiaConversion"
    PREFIX ""
    LINKER_LANGUAGE CXX
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
install(
    TARGETS DisplayXR-LeiaConversion
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime
    LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime)

# --- Tests: the provider's own entry point, linked statically -------------
# leia_conversion_static: the same sources WITHOUT XRT_USING_RUNTIME_DLL (aux
# linked statically), so the test calls xrtConversionPluginNegotiate directly
# — no registry, no runtime DLL (the runtime's sim_conversion_static pattern).
add_library(leia_conversion_static OBJECT ${LEIA_CONV_SOURCES})
leia_conv_configure(leia_conversion_static)
target_link_libraries(leia_conversion_static PUBLIC xrt-interfaces aux_util aux_os)

add_executable(test_conv_leia_provider tests/test_conv_leia_provider.cpp $<TARGET_OBJECTS:leia_conversion_static>)
target_link_libraries(test_conv_leia_provider PRIVATE
    xrt-interfaces aux_util aux_os d3d11 dxgi d3dcompiler version advapi32)
set_target_properties(test_conv_leia_provider PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

# Fake conversion modules (a DLL named NeurD.dll with a VERSIONINFO and no
# entry point): one older than the default floor (0.4.6), one newer (0.4.9:
# saliency auto-convergence), and one with the stereographer rule (0.4.10). Both
# newer ones declare XRT_CONV_CAP_AUTO_CONVERGENCE.
function(leia_conv_fake_module name major minor patch product)
    set(FAKE_MAJOR ${major})
    set(FAKE_MINOR ${minor})
    set(FAKE_PATCH ${patch})
    set(FAKE_PRODUCT "${product}")
    configure_file(tests/fake_module.rc.in "${CMAKE_CURRENT_BINARY_DIR}/${name}.rc" @ONLY)
    add_library(${name} SHARED tests/fake_module.c "${CMAKE_CURRENT_BINARY_DIR}/${name}.rc")
    set_target_properties(${name} PROPERTIES
        OUTPUT_NAME "NeurD"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake_modules/${name}/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fake_modules/${name}/lib/$<CONFIG>")
    add_dependencies(test_conv_leia_provider ${name})
endfunction()
leia_conv_fake_module(leia_conv_fake_old 0 4 1 "Fake conversion module (internal test build)")
leia_conv_fake_module(leia_conv_fake_noentry 0 4 9 "Fake conversion module (commercial)")
# First version with NeurD's stereographer auto-convergence rule.
leia_conv_fake_module(leia_conv_fake_autoconv 0 4 10 "Fake conversion module (commercial)")

add_test(NAME test_conv_leia_provider_missing
    COMMAND test_conv_leia_provider missing "$<SHELL_PATH:${CMAKE_CURRENT_BINARY_DIR}/fake_modules/none/NeurD.dll>")
add_test(NAME test_conv_leia_provider_old
    COMMAND test_conv_leia_provider old "$<SHELL_PATH:$<TARGET_FILE:leia_conv_fake_old>>")
add_test(NAME test_conv_leia_provider_noentry
    COMMAND test_conv_leia_provider noentry "$<SHELL_PATH:$<TARGET_FILE:leia_conv_fake_noentry>>")
add_test(NAME test_conv_leia_provider_autoconv
    COMMAND test_conv_leia_provider autoconv "$<SHELL_PATH:$<TARGET_FILE:leia_conv_fake_autoconv>>")
add_test(NAME test_conv_leia_provider_disabled
    COMMAND test_conv_leia_provider disabled "$<SHELL_PATH:$<TARGET_FILE:leia_conv_fake_noentry>>")
