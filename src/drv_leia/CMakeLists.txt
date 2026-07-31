# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# DisplayXR-LeiaSR plug-in DLL.
#
# Mirrors the historical drv_leia_plugin target from displayxr-runtime
# (pre-#263). Source files are now owned by this repo; the runtime is
# consumed via add_subdirectory (see top-level CMakeLists.txt).

# Common sources — always present.
set(LEIA_SOURCES
    leia_device.c
    leia_edid_probe.c
    leia_interface.h
    leia_types.h
    leia_sr_probe.cpp
    leia_plugin.c
)

# Vulkan weaver path. The weaver DLL is resolved and loaded at RUNTIME (see
# leia_vk_weaver_select.cpp), so no SR Vulkan import library is linked and the
# gate below only asks "were Vulkan weaver bits fetched at all". Either name
# satisfies it: SimulatedRealityVulkanBeta.lib (SR <= 1.36 SDKs) or
# SimulatedRealityVulkan.lib (>= 1.37, and the 1.36 stamp-aware rescue SDK).
set(LEIA_SR_VULKAN_AVAILABLE FALSE)
if(EXISTS "${SR_PATH}/lib/SimulatedRealityVulkanBeta.lib" OR EXISTS "${SR_PATH}/lib/SimulatedRealityVulkan.lib")
    message(STATUS "Found SR Vulkan SDK bits - enabling Vulkan weaver (runtime-selected DLL)")
    list(APPEND LEIA_SOURCES
        leia_sr.cpp leia_sr.h
        leia_vk_weaver.h leia_vk_weaver_select.cpp
        leia_display_processor.cpp leia_display_processor.h)

    # Chroma-key + compose-under-bg shaders compiled to SPIR-V headers.
    # The runtime tree owns the spirv_shaders macro; it's available
    # because the runtime is added as a subdirectory above.
    spirv_shaders(
        LEIA_SHADER_HEADERS
        SPIRV_VERSION 1.0
        SOURCES
            shaders/fullscreen_tri.vert
            shaders/compose_under_bg.frag
            shaders/alpha_gate.frag
    )
    list(APPEND LEIA_SOURCES ${LEIA_SHADER_HEADERS})
    set(LEIA_SR_VULKAN_AVAILABLE TRUE)

    # --- Per-ABI weaver dispatch tables ------------------------------------
    #
    # SR::IVulkanWeaver1 is NOT binary-stable: 1.37.1 (ST-5318) inserted a mono
    # setInputViewTexture overload ahead of the stereo one, and MSVC orders
    # contiguous same-name virtual overloads in REVERSE, so every method after
    # that group shifts down a slot. A binary built against the old header calls
    # setOutputFrameBuffer and lands on mono setInputViewTexture.
    #
    # So leia_vk_weaver_impl.cpp is compiled ONCE PER ABI, each build seeing a
    # different vendored vkweaver.h and exporting a differently-named ops getter.
    # The vendored headers live in sr_vk_abi/ rather than being taken from
    # ${SR_PATH} so the ABI pair is deterministic regardless of which SR SDK
    # happens to have been fetched into the build tree.
    foreach(_abi legacy stamp)
        add_library(leia_vk_abi_${_abi} OBJECT leia_vk_weaver_impl.cpp)
        target_include_directories(leia_vk_abi_${_abi} BEFORE PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/sr_vk_abi/${_abi}")
        target_include_directories(leia_vk_abi_${_abi} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}"
            ${SR_PATH}/include
            ${SR_PATH}/third_party/OpenCV/include)
        target_link_libraries(leia_vk_abi_${_abi} PRIVATE xrt-interfaces aux_util)
        target_compile_definitions(leia_vk_abi_${_abi} PRIVATE
            LEIA_VK_OPS_SYMBOL=leia_vk_weaver_ops_${_abi})
        set_target_properties(leia_vk_abi_${_abi} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            POSITION_INDEPENDENT_CODE ON)
    endforeach()
else()
    message(STATUS "SR Vulkan SDK bits not found - Vulkan weaver disabled")
endif()

# D3D11 / D3D12 / GL weavers (SimulatedRealityDirectX.lib covers both DX paths).
set(LEIA_SR_D3D11_AVAILABLE FALSE)
set(LEIA_SR_D3D12_AVAILABLE FALSE)
if(EXISTS "${SR_PATH}/lib/SimulatedRealityDirectX.lib")
    message(STATUS "Found SimulatedRealityDirectX.lib - enabling D3D11 + D3D12 weavers")
    list(APPEND LEIA_SOURCES
        leia_sr_d3d11.cpp leia_sr_d3d11.h
        leia_display_processor_d3d11.cpp leia_display_processor_d3d11.h
        leia_sr_d3d12.cpp leia_sr_d3d12.h
        leia_display_processor_d3d12.cpp leia_display_processor_d3d12.h)
    set(LEIA_SR_D3D11_AVAILABLE TRUE)
    set(LEIA_SR_D3D12_AVAILABLE TRUE)
endif()

set(LEIA_SR_GL_AVAILABLE FALSE)
if(EXISTS "${SR_PATH}/lib/SimulatedRealityOpenGL.lib")
    message(STATUS "Found SimulatedRealityOpenGL.lib - enabling GL weaver")
    list(APPEND LEIA_SOURCES
        leia_sr_gl.cpp leia_sr_gl.h
        leia_display_processor_gl.cpp leia_display_processor_gl.h)
    set(LEIA_SR_GL_AVAILABLE TRUE)
endif()

# Windows Graphics Capture for compose-under-bg transparency. The GL DP imports
# the captured D3D11 desktop texture into GL via WGL_NV_DX_interop2, so it needs
# this module too.
if(LEIA_SR_D3D11_AVAILABLE OR LEIA_SR_D3D12_AVAILABLE OR LEIA_SR_GL_AVAILABLE)
    list(APPEND LEIA_SOURCES leia_bg_capture_win.cpp leia_bg_capture_win.h)
endif()

# --- Plug-in DLL target -------------------------------------------------

# #461 (runtime repo): embed a VERSIONINFO resource tracking PROJECT_VERSION
# so the runtime's plug-in loader can detect registry/disk version skew
# (installer silently skipped a locked DLL).
configure_file(version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
list(APPEND LEIA_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

add_library(DisplayXR-LeiaSR SHARED ${LEIA_SOURCES})

# leia_display_processor.cpp uses C++20 designated initializers for the
# VkPipelineCreateInfo + chroma-key pipeline-state structs; match the
# original runtime build's per-target bump.
set_target_properties(DisplayXR-LeiaSR PROPERTIES CXX_STANDARD 20)

target_compile_definitions(DisplayXR-LeiaSR PRIVATE XRT_USING_RUNTIME_DLL)
# Surface the plug-in build identity through xrt_plugin_iface::version
# (reported by the loader log, `displayxr-cli info`, the Control Panel, and
# the Android diagnostics dashboard). git-describe supersedes the older
# PROJECT_VERSION wiring (#47) so dev builds report their real commit and a
# release DLL reports its tag — and a stale hand-dropped .so is identifiable.
target_compile_definitions(DisplayXR-LeiaSR PRIVATE DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}")

target_include_directories(DisplayXR-LeiaSR PRIVATE
    ${SR_PATH}/include
    ${SR_PATH}/third_party/OpenCV/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}    # generated spirv headers
)

target_link_libraries(DisplayXR-LeiaSR
    PRIVATE
        # Import lib for DisplayXRClient.dll — exposes the runtime's
        # XRT_API_FUNC-decorated aux surface (ADR-019).
        $<TARGET_LINKER_FILE:${RUNTIME_TARGET}>
        # Static aux helpers (non-exported obj files pulled in to satisfy
        # unresolved symbols).
        xrt-interfaces
        aux_util
        aux_os
        aux_math
        aux_vk
        drv_includes
        # SR SDK core (always-on).
        ${SR_PATH}/lib/SimulatedRealityCore.lib
        ${SR_PATH}/lib/SimulatedRealityDisplays.lib
        ${SR_PATH}/third_party/OpenCV/lib/x64/opencv_world343.lib
)
add_dependencies(DisplayXR-LeiaSR ${RUNTIME_TARGET})

if(LEIA_SR_VULKAN_AVAILABLE)
    # Both ABI dispatch tables are linked in; which one is used is decided at
    # runtime by leia_vk_weaver_select_backend().
    target_link_libraries(DisplayXR-LeiaSR PRIVATE
        leia_vk_abi_legacy
        leia_vk_abi_stamp
        # GetFileVersionInfo* — reads DimencoWeaving.dll's version to decide
        # whether the installed core emits the subpixel stamp.
        version)
    target_compile_definitions(DisplayXR-LeiaSR PRIVATE XRT_HAVE_LEIA_SR_VULKAN)
    # NOTE: no SR Vulkan import library and no /DELAYLOAD for it. The weaver is
    # LoadLibrary'd by absolute path and CreateVulkanWeaver resolved via
    # GetProcAddress. That is required, not merely tidy: the mangled export name
    # is identical across versions, so linking both SimulatedRealityVulkanBeta.lib
    # and SimulatedRealityVulkan.lib would collide on a single symbol and there
    # would be no way to bind two weavers in one process.
    #
    # The Vulkan loader itself is still delay-loaded — it is genuinely absent on
    # some machines, and the D3D11/D3D12/GL paths must stay loadable there.
    target_link_options(DisplayXR-LeiaSR PRIVATE "/DELAYLOAD:vulkan-1.dll")
    target_link_libraries(DisplayXR-LeiaSR PRIVATE delayimp)
endif()

if(LEIA_SR_D3D11_AVAILABLE)
    target_link_libraries(DisplayXR-LeiaSR PRIVATE
        ${SR_PATH}/lib/SimulatedRealityDirectX.lib
        d3d11 d3dcompiler dxgi)
    target_compile_definitions(DisplayXR-LeiaSR PRIVATE XRT_HAVE_LEIA_SR_D3D11)
endif()

if(LEIA_SR_D3D12_AVAILABLE)
    target_link_libraries(DisplayXR-LeiaSR PRIVATE d3d12 d3dcompiler)
    target_compile_definitions(DisplayXR-LeiaSR PRIVATE XRT_HAVE_LEIA_SR_D3D12)
endif()

# WinRT (WGC) + DXGI GUID helpers — needed by leia_bg_capture_win.cpp, which the
# GL path also pulls in for compose-under-bg.
if(LEIA_SR_D3D11_AVAILABLE OR LEIA_SR_D3D12_AVAILABLE OR LEIA_SR_GL_AVAILABLE)
    target_link_libraries(DisplayXR-LeiaSR PRIVATE runtimeobject dxguid)
endif()

if(LEIA_SR_GL_AVAILABLE)
    target_link_libraries(DisplayXR-LeiaSR PRIVATE
        ${SR_PATH}/lib/SimulatedRealityOpenGL.lib
        opengl32 aux_ogl
        # WGC desktop capture imported into GL via WGL_NV_DX_interop2 needs a
        # DP-owned D3D11 device (D3D11CreateDevice).
        d3d11 dxgi)
    target_compile_definitions(DisplayXR-LeiaSR PRIVATE XRT_HAVE_LEIA_SR_GL)
endif()

# One-symbol export discipline — only xrtPluginNegotiate is visible.
set_target_properties(DisplayXR-LeiaSR PROPERTIES
    OUTPUT_NAME "DisplayXR-LeiaSR"
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

install(
    TARGETS DisplayXR-LeiaSR
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime
    LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime
)

# Stage the bundled SR Vulkan weavers alongside the plug-in in _package/bin/plugins
# so the installer packs them from the *signed* tree (the raw SDK-dir copy bypasses
# the code-signing pass that covers _package/bin, so they would ship unsigned and
# trip Smart App Control at load time). These are loaded explicitly by absolute
# path, so CI's app-local dependency staging never picks them up — copy them.
#
# Two weavers ship, and the runtime picks between them:
#   SimulatedRealityVulkanBeta.dll  pre-stamp vtable, for SR <= 1.34.x
#   SimulatedRealityVulkan.dll      stamp-aware, for SR 1.36.x (which ships none)
# SR >= 1.37 installs its own SimulatedRealityVulkan.dll on PATH and neither of
# these is used — the machine weaver always wins.
if(LEIA_SR_VULKAN_AVAILABLE AND EXISTS "${SR_PATH}/bin/SimulatedRealityVulkanBeta.dll")
    install(FILES "${SR_PATH}/bin/SimulatedRealityVulkanBeta.dll"
        DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime)
endif()
if(LEIA_SR_VULKAN_AVAILABLE AND EXISTS "${SR_PATH}/bin/SimulatedRealityVulkan.dll")
    install(FILES "${SR_PATH}/bin/SimulatedRealityVulkan.dll"
        DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime)
else()
    message(WARNING
        "Stamp-aware SimulatedRealityVulkan.dll not found in ${SR_PATH}/bin - "
        "SR 1.36.x machines will fall back to the pre-stamp weaver and show the "
        "coloured-replica artefact. Fetch it from the sr-sdk-v*-vkstamp release.")
endif()
