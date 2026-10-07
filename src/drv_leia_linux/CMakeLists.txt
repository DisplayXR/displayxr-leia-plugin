# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# DisplayXR-LeiaSR plug-in — Linux desktop arm (Track A).
#
# Self-contained ELF plug-in following the runtime's PROVEN
# drv_sim_display_plugin Linux model (runtime src/xrt/drivers/CMakeLists.txt,
# Linux branch): STATIC-link the aux libraries, do NOT link the runtime .so,
# do NOT define XRT_USING_RUNTIME_DLL, and hide every symbol except
# xrtPluginNegotiate via the version script (issue #496 / ADR-019 — exported
# aux symbols would interpose the runtime's copies at dlopen time).
#
# The weaver backend sits behind the leia_sr_linux.h seam (interface shaped
# by docs/leia-linux-sdk-contract.md, PROPOSED); Track A ships the stub.

find_package(Vulkan REQUIRED) # loader lib for the stub's raw vkCmd* calls

# Compose-under-bg transparency shaders (runtime#757) → SPIR-V byte-array
# headers. The runtime tree owns the spirv_shaders macro (available because the
# runtime is added as a subdirectory above). Shaders are per-backend copies
# (same pattern as drv_leia_android/shaders), sourced from ../drv_leia/shaders.
spirv_shaders(
    LEIA_LINUX_SHADER_HEADERS
    SPIRV_VERSION 1.0
    SOURCES
        shaders/fullscreen_tri.vert
        shaders/compose_under_bg.frag
        shaders/alpha_gate.frag
)

# xcb + xcb-randr for the transient panel-position query (#91 / runtime#715):
# leia_edid_probe_linux.c matches the frozen EDID table against RandR outputs
# to place the panel in the desktop. Already an apt dep of the CI image (the
# runtime's own build needs libxcb-randr0-dev).
find_package(PkgConfig REQUIRED)
pkg_check_modules(PLUGIN_XCB REQUIRED IMPORTED_TARGET xcb xcb-randr)

# Desktop background capture for transparency (runtime#757): org.gnome.Mutter.
# ScreenCast RecordArea (D-Bus), with our windows excluded by the DisplayXR GNOME
# Shell extension → PipeWire. OPTIONAL — not REQUIRED — so a CI image without
# these -dev packages still builds the graceful-decline path and stays green
# (mutter/PipeWire cannot run in CI anyway). When present, the real producer
# compiles behind DXR_LEIA_HAVE_PIPEWIRE.
pkg_check_modules(PLUGIN_PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(PLUGIN_DBUS IMPORTED_TARGET dbus-1)
if(PLUGIN_PIPEWIRE_FOUND AND PLUGIN_DBUS_FOUND)
    message(STATUS "DisplayXR-LeiaSR: desktop capture enabled (libpipewire-0.3 + dbus-1)")
else()
    message(STATUS "DisplayXR-LeiaSR: desktop capture unavailable (no libpipewire-0.3/dbus-1) "
                   "— transparency falls back to silhouette intersection")
endif()

# --- Weaver backend seam — the Track B switch point ----------------------
set(DXR_LEIA_LINUX_WEAVER "stub" CACHE STRING
    "Linux weaver backend: 'stub' (no SR SDK, passthrough SBS) or 'sdk' (Track B)")
add_library(leia_weaver_backend INTERFACE)
if(DXR_LEIA_LINUX_WEAVER STREQUAL "stub")
    target_sources(leia_weaver_backend INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/leia_sr_stub.c)
elseif(DXR_LEIA_LINUX_WEAVER STREQUAL "sdk")
    # Track B: the real LeiaSR srSDK (SPIKE — pinned to the prototype package,
    # srSDK API 1.0.0). The SDK is commercial-licensed and never vendored:
    # point SRSDK_ROOT (or CMAKE_PREFIX_PATH) at a local unpack. Absent SDK is
    # a hard error by design — a silent stub fallback would let someone
    # hardware-validate the wrong backend, the exact confusion this seam
    # exists to prevent. CI never enters this branch (default = stub).
    if(DEFINED ENV{SRSDK_ROOT})
        list(APPEND CMAKE_PREFIX_PATH "$ENV{SRSDK_ROOT}")
    endif()
    find_package(srSDK 1.0 CONFIG REQUIRED) # exports srSDK::loader (static; dlopens libLeiaSR_runtime.so)
    target_sources(leia_weaver_backend INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/leia_sr_linux_sdk.c)
    target_link_libraries(leia_weaver_backend INTERFACE srSDK::loader)
    # Lets the plug-in iface's display_name reflect the real (Track B) backend
    # instead of the stub label (#100 review / cube-hw finding D). INTERFACE →
    # propagates to leia_plugin_linux.c compiled into DisplayXR-LeiaSR.
    target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_WEAVER_SDK=1)

    # --- Drag phase-snap capability (runtime#1588) -------------------------
    # srWeaverSnapToPhase is called through the loader TRAMPOLINE, not through
    # a hand-rolled dispatch lookup (leia_sr_linux_sdk.c explains why: the two
    # v2 lines agree on slot meanings TODAY, but the Windows line already holds
    # fifteen slots past this call, so the next append here collides silently —
    # no length change, no version bump, nothing for a probe to catch. Only
    # "loader and runtime from one tree" is safe, and only this variable
    # establishes it).
    #
    # So the probe has to answer BOTH halves in one go: does the header declare
    # the call, and does this loader archive actually carry the trampoline?
    # check_symbol_exists takes the symbol's address, so it compiles AND links
    # — a header-new/archive-old SDK root fails it here, at configure time,
    # instead of at link time with --no-undefined.
    #
    # An SDK root that predates the call is a CONFIGURE ERROR (#271). It used
    # to compile the feature out to identity silently, and v2.7.3's .deb shipped
    # exactly that: built against an SDK pin without the call, every drag-lattice
    # request declined, windows stuttered in 3D, and nothing was logged anywhere.
    # A plug-in that cannot snap is not releasable for windowed apps. Bring-up
    # against an older SDK is still possible, but it has to be asked for by name.
    option(DXR_LEIA_LNX_ALLOW_NO_SNAP
        "Allow a Track B build against an SR SDK without srWeaverSnapToPhase (window drags will NOT phase-snap; never for release)"
        OFF)
    include(CheckSymbolExists)
    get_target_property(_srsdk_snap_inc srSDK::loader_dynamic INTERFACE_INCLUDE_DIRECTORIES)
    set(CMAKE_REQUIRED_INCLUDES "${_srsdk_snap_inc}")
    set(CMAKE_REQUIRED_LIBRARIES srSDK::loader)
    check_symbol_exists(srWeaverSnapToPhase "sr/sr_weaver.h" DXR_LEIA_LNX_HAVE_SR_SNAP)
    # Multi-screen M0 (SR-P1): srEnumerateDisplays (slot 97) + SrDisplayDescriptor
    # + the two capability structs. Same compile-AND-link probe as the snap: a
    # LeiaSR 1.38 root (no declaration, no trampoline) compiles the per-monitor
    # SR identity out and probe_displays falls back to EDID + lens serial.
    check_symbol_exists(srEnumerateDisplays "sr/sr.h" DXR_LEIA_LNX_HAVE_SR_DISPLAY_ENUM)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(DXR_LEIA_LNX_HAVE_SR_DISPLAY_ENUM)
        target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_LNX_HAVE_SR_DISPLAY_ENUM=1)
        message(STATUS "DisplayXR-LeiaSR: SR display enumeration ENABLED (srEnumerateDisplays declared + "
                       "linkable) — probe_displays joins SR's FPC-verified displays")
    else()
        message(STATUS "DisplayXR-LeiaSR: SR display enumeration COMPILED OUT — no srEnumerateDisplays in "
                       "${_srsdk_snap_inc} (or this loader archive); probe_displays uses EDID + lens serial")
    endif()
    # Multi-screen M4 (SR-P1/P2 of the plan; LeiaSR Linux line 876620d62
    # phases A-C): weaver routing (SrWeaverRoutingInfo, EXTERNAL) and display
    # binding (SrDisplayBindingInfo), both chained on create-infos and both
    # gated at RUN time on the capability structs. Types only — no new call,
    # so a compile check is the whole question (nothing to link). A 1.38 root
    # lacks them: everything compiles out and every weaver is the plain one.
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_INCLUDES "${_srsdk_snap_inc}")
    check_c_source_compiles("
#include <sr/sr.h>
#include <sr/sr_vk.h>
int main(void)
{
	SrWeaverRoutingInfo r = SrWeaverRoutingInfo(.mode = SR_WEAVER_ROUTING_EXTERNAL);
	SrWeaverRoutingCapabilities rc = SrWeaverRoutingCapabilities();
	SrDisplayBindingInfo b = SrDisplayBindingInfo(.displayId = 1);
	SrDisplayBindingCapabilities bc = SrDisplayBindingCapabilities();
	SrWeaverCreateInfoVulkan w = SrWeaverCreateInfoVulkan(.pNext = &r);
	(void)rc; (void)b; (void)bc; (void)w;
	return 0;
}" DXR_LEIA_LNX_HAVE_SR_ROUTING)
    # leia-plugin #184: srDisplayGetRefreshRate (slot 98) — a call, so the
    # snap's compile-AND-link probe (it must be in THIS loader archive).
    set(CMAKE_REQUIRED_LIBRARIES srSDK::loader)
    check_symbol_exists(srDisplayGetRefreshRate "sr/sr.h" DXR_LEIA_LNX_HAVE_SR_REFRESH)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(DXR_LEIA_LNX_HAVE_SR_ROUTING)
        target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_LNX_HAVE_SR_ROUTING=1)
        message(STATUS "DisplayXR-LeiaSR: SR weaver routing + display binding ENABLED (SrWeaverRoutingInfo / "
                       "SrDisplayBindingInfo declared) — used when the installed SR runtime reports them")
    else()
        message(STATUS "DisplayXR-LeiaSR: SR weaver routing + display binding COMPILED OUT — the SDK at "
                       "${_srsdk_snap_inc} predates SrWeaverRoutingInfo; weavers are SDK-routed and unbound")
    endif()
    if(DXR_LEIA_LNX_HAVE_SR_REFRESH)
        target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_LNX_HAVE_SR_REFRESH=1)
        message(STATUS "DisplayXR-LeiaSR: SR refresh rate ENABLED (srDisplayGetRefreshRate)")
    else()
        message(STATUS "DisplayXR-LeiaSR: SR refresh rate COMPILED OUT — no srDisplayGetRefreshRate in this SDK; "
                       "display info reports 60 Hz")
    endif()

    if(DXR_LEIA_LNX_HAVE_SR_SNAP)
        target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_LNX_HAVE_SR_SNAP=1)
        message(STATUS "DisplayXR-LeiaSR: drag phase-snap ENABLED "
                       "(srWeaverSnapToPhase declared + linkable from ${_srsdk_snap_inc})")
    elseif(DXR_LEIA_LNX_ALLOW_NO_SNAP)
        message(WARNING "DisplayXR-LeiaSR: drag phase-snap COMPILED OUT (DXR_LEIA_LNX_ALLOW_NO_SNAP=ON) — "
                        "no srWeaverSnapToPhase in ${_srsdk_snap_inc} (or no trampoline in this loader "
                        "archive). Window drags will NOT phase-snap. Do not release this build (#271).")
    else()
        message(FATAL_ERROR "DisplayXR-LeiaSR: the SR SDK at ${_srsdk_snap_inc} lacks srWeaverSnapToPhase "
                            "(declared in sr/sr_weaver.h AND linkable from srSDK::loader's libsrSDK_loader.a). "
                            "Window drags cannot phase-snap without it (runtime#1588, #271). "
                            "Minimum SR SDK: one that ships srWeaverSnapToPhase + srWeaverSetPresentOrigin "
                            "(LeiaSR#85), e.g. the 1.37.0.10693 or 1.38.0.10108 Linux SDK. The 1.37.0.41935 linux64 SDK "
                            "predates it despite its higher build number (different branch). "
                            "Point SRSDK_ROOT at a newer SDK (in a FRESH build dir: the probe result is cached "
                            "in CMakeCache.txt), or pass -DDXR_LEIA_LNX_ALLOW_NO_SNAP=ON for a non-release "
                            "bring-up build.")
    endif()

    # --- 2D under the lens (ADR-027 Amendment; LeiaSR ST-5801 / ST-5792) -----
    # srWeaverSetComposeOrder + srWeaverSetComposeInputsVulkan let the Vulkan
    # weaver composite the runtime's 2D over-layer inside the weave and filter it
    # for the lens (runtime set_overlay_2d VK slot). Same probe shape as the snap
    # above, for the same reason: the trampolines dispatch by slot index, so the
    # answer has to come from THIS loader archive, not from a header alone.
    # Unlike the snap this is OPTIONAL: an SDK without it (1.37.0.10693 and
    # every SDK before the Linux compose line; CI pins 1.38.0.10108, which has
    # it) compiles the feature out and the runtime keeps blending the layer
    # post-weave. The two
    # follow-ups (v14 cache hint, v15 strength) are probed separately: an SDK
    # can carry the compose pair without them.
    set(CMAKE_REQUIRED_INCLUDES "${_srsdk_snap_inc}")
    set(CMAKE_REQUIRED_LIBRARIES srSDK::loader)
    check_symbol_exists(srWeaverSetComposeInputsVulkan "sr/sr.h;sr/sr_vk.h" DXR_LEIA_LNX_HAVE_SR_COMPOSE_VK)
    check_symbol_exists(srWeaverSetComposeOrder "sr/sr.h;sr/sr_weaver.h" DXR_LEIA_LNX_HAVE_SR_COMPOSE_ORDER)
    check_symbol_exists(srWeaverSetComposeLayerUnchanged "sr/sr.h;sr/sr_weaver.h"
                        DXR_LEIA_LNX_HAVE_SR_COMPOSE_UNCHANGED)
    check_symbol_exists(srWeaverSetComposeFilterStrength "sr/sr.h;sr/sr_weaver.h"
                        DXR_LEIA_LNX_HAVE_SR_COMPOSE_STRENGTH)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
    if(DXR_LEIA_LNX_HAVE_SR_COMPOSE_VK AND DXR_LEIA_LNX_HAVE_SR_COMPOSE_ORDER)
        target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_LNX_HAVE_SR_COMPOSE=1)
        set(_compose_extras "")
        if(DXR_LEIA_LNX_HAVE_SR_COMPOSE_UNCHANGED)
            target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_LNX_HAVE_SR_COMPOSE_UNCHANGED=1)
            string(APPEND _compose_extras " +v14 cache hint")
        endif()
        if(DXR_LEIA_LNX_HAVE_SR_COMPOSE_STRENGTH)
            target_compile_definitions(leia_weaver_backend INTERFACE DXR_LEIA_LNX_HAVE_SR_COMPOSE_STRENGTH=1)
            string(APPEND _compose_extras " +v15 filter strength")
        endif()
        message(STATUS "DisplayXR-LeiaSR: SR weaver 2D compose (2D under the lens) ENABLED${_compose_extras}")
        unset(_compose_extras)
    else()
        message(STATUS "DisplayXR-LeiaSR: SR weaver 2D compose DISABLED (the SDK at ${_srsdk_snap_inc} predates "
                       "srWeaverSetComposeInputsVulkan) - the runtime keeps blending its 2D over-layer post-weave")
    endif()

    # DEV-ONLY convenience: bake the SDK lib dir as DT_RUNPATH so the static
    # loader's dlopen("libLeiaSR_runtime.so") resolves with zero environment
    # setup on a spike/bring-up box (glibc honors the CALLING object's
    # runpath — that's this plug-in .so).
    #
    # RELEASE builds MUST turn this OFF (-DDXR_LEIA_SDK_DEV_RPATH=OFF): the
    # deployment model is Windows parity — Leia's own Linux runtime installer
    # lays down the SR stack and registers it at
    # /etc/leia/sr/1/active_runtime.json (the loader's first search path,
    # with the SR_RUNTIME_PATH env as the manual override). A shipped plug-in
    # must resolve the runtime through that registration, never through a
    # build-machine path baked into the ELF.
    option(DXR_LEIA_SDK_DEV_RPATH
        "Bake the srSDK lib dir as DT_RUNPATH (dev/bring-up only; OFF for release builds)" ON)
    if(DXR_LEIA_SDK_DEV_RPATH)
        get_target_property(_srsdk_inc srSDK::loader_dynamic INTERFACE_INCLUDE_DIRECTORIES)
        get_filename_component(_srsdk_lib "${_srsdk_inc}/../lib" ABSOLUTE)
        target_link_options(leia_weaver_backend INTERFACE "LINKER:-rpath,${_srsdk_lib}")
        message(STATUS "DisplayXR-LeiaSR: Track B srSDK weaver backend (DEV rpath -> ${_srsdk_lib})")
    else()
        message(STATUS "DisplayXR-LeiaSR: Track B srSDK weaver backend (release: runtime via "
                       "/etc/leia/sr/1/active_runtime.json, no baked rpath)")
    endif()
else()
    message(FATAL_ERROR "Unknown DXR_LEIA_LINUX_WEAVER='${DXR_LEIA_LINUX_WEAVER}' (stub|sdk)")
endif()

# --- Plug-in .so target ---------------------------------------------------

add_library(DisplayXR-LeiaSR SHARED
    leia_plugin_linux.c
    leia_display_processor_linux.c
    leia_display_processor_linux.h
    leia_edid_probe_linux.c
    leia_edid_probe_linux.h
    leia_display_claims_linux.c
    leia_display_claims_linux.h
    leia_screen_linux.c
    leia_screen_linux.h
    leia_sr_routing_linux.h
    leia_bg_capture_linux.c
    leia_bg_capture_linux.h
    leia_bg_capture_worker_linux.c
    leia_bg_capture_worker_linux.h
    leia_mutter_capture_linux.c
    leia_mutter_capture_linux.h
    leia_sr_linux.h
    # Shared, platform-portable device implementation from the Windows arm.
    ../drv_leia/leia_device.c
    # Generated SPIR-V headers for the compose-under-bg pass (runtime#757).
    ${LEIA_LINUX_SHADER_HEADERS}
)

# Surface the plug-in build identity through xrt_plugin_iface::version (#47).
target_compile_definitions(DisplayXR-LeiaSR PRIVATE DXR_PLUGIN_GIT_DESC="${DXR_PLUGIN_GIT_DESC}")

target_include_directories(DisplayXR-LeiaSR PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia # leia_interface.h / leia_types.h
    ${CMAKE_CURRENT_BINARY_DIR}             # generated shaders/*.h (runtime#757)
)

target_link_libraries(DisplayXR-LeiaSR PRIVATE
    leia_weaver_backend
    xrt-interfaces
    aux_util
    aux_os
    aux_math
    aux_vk
    drv_includes
    Vulkan::Vulkan
    PkgConfig::PLUGIN_XCB
)

# Desktop-capture producer (runtime#757 WS1): link + enable the real path only
# when both libs are present; otherwise the graceful-decline path is compiled.
if(PLUGIN_PIPEWIRE_FOUND AND PLUGIN_DBUS_FOUND)
    target_compile_definitions(DisplayXR-LeiaSR PRIVATE DXR_LEIA_HAVE_PIPEWIRE)
    target_link_libraries(DisplayXR-LeiaSR PRIVATE PkgConfig::PLUGIN_PIPEWIRE PkgConfig::PLUGIN_DBUS)
endif()

# One-symbol export discipline (#496): hidden visibility + version script
# keeping only xrtPluginNegotiate global; --exclude-libs,ALL is belt-and-
# braces against the statically linked aux archives.
set_target_properties(DisplayXR-LeiaSR PROPERTIES
    OUTPUT_NAME "DisplayXR-LeiaSR"
    PREFIX ""
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_link_options(DisplayXR-LeiaSR PRIVATE
    "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/plugin_exports.version"
    "LINKER:--exclude-libs,ALL"
    # --no-undefined: an unresolved symbol must be a LINK error, not a runtime
    # surprise. On Linux a shared library links happily with dangling references
    # and only fails when the loader BINDS them — unlike MSVC and macOS's
    # two-level namespace, which reject it at link time. For a dlopen'd plug-in
    # that means a fully green build can still produce a .so the runtime cannot
    # load, and the failure surfaces on the target box as
    # `undefined symbol: <x>`, arbitrarily far from the build that shipped it.
    # (displayxr-unity#249 shipped exactly that; displayxr-runtime already sets
    # the same flag globally in CompilerFlags.cmake.)
    "LINKER:--no-undefined"
)

install(TARGETS DisplayXR-LeiaSR
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/displayxr/plugins COMPONENT Runtime
)

# --- Host unit test: lens-preference ownership (LeiaSR #266) -------------
# Pure logic from leia_lens_owner_linux.h; no SDK, no Vulkan, no panel, so it
# builds and runs in both weaver arms. scripts/build-linux.sh and CI run it.
add_executable(test_lens_owner_linux tests/test_lens_owner_linux.c)
target_include_directories(test_lens_owner_linux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME test_lens_owner_linux COMMAND test_lens_owner_linux)

# --- GPU unit test: compose-under-capture colour contract ----------------
# Runs the plug-in's own compose SPIR-V on any Vulkan device (Mesa lavapipe in
# CI) and asserts: an opaque atlas pixel reaches the weaver bit-identical with
# the desktop capture on; alpha < 1 blends premultiplied, in linear light
# (OpenXR 1.1 §10.6.2). Offscreen only — no display, panel or SR SDK. Exits 77
# (skipped) when no Vulkan device exists.
add_executable(test_compose_color_linux tests/test_compose_color_linux.c ${LEIA_LINUX_SHADER_HEADERS})
target_include_directories(test_compose_color_linux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(test_compose_color_linux PRIVATE Vulkan::Vulkan m)
add_test(NAME test_compose_color_linux COMMAND test_compose_color_linux)
set_tests_properties(test_compose_color_linux PROPERTIES SKIP_RETURN_CODE 77)

# --- Host unit test: stereo camera source parsing (runtime ADR-043, L1) ---
# The Windows provider's platform-neutral half (SR raw-camera frame header,
# JPEG SOF, device-serial block, the OpenCV-YAML calibration subset, eye
# order). No SDK, no Win32 — so the Linux job runs it for the Windows arm.
add_executable(test_stereo_camera_parse
    ../drv_leia/tests/test_stereo_camera_parse.c
    ../drv_leia/leia_stereo_camera_parse.c)
target_include_directories(test_stereo_camera_parse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia)
target_link_libraries(test_stereo_camera_parse PRIVATE m)
add_test(NAME test_stereo_camera_parse COMMAND test_stereo_camera_parse)

# --- Host unit test: EDID panel list + descriptor->claim matching (M0) ----
# Pure parse/match logic: leia_edid_probe_linux.c is linked for its EDID parser
# (fixture blobs, no sysfs read is exercised) and leia_display_claims_linux.c
# for the matching. No SDK, no Vulkan device, no panel — both weaver arms.
add_executable(test_display_claims_linux
    tests/test_display_claims_linux.c
    leia_edid_probe_linux.c
    leia_display_claims_linux.c
)
target_include_directories(test_display_claims_linux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../drv_leia
)
target_link_libraries(test_display_claims_linux PRIVATE xrt-interfaces aux_util PkgConfig::PLUGIN_XCB)
add_test(NAME test_display_claims_linux COMMAND test_display_claims_linux)

# --- Host unit test: per-DP screen state (multi-screen M4) ----------------
# leia_screen_linux.c resolves one DP's screen from a binding + the EDID list +
# the SR query, into state the DP owns. Fixtures only — no SDK, no Vulkan
# device, no panel, no X — so both weaver arms and both runtime pins run it.
add_executable(test_screen_linux
    tests/test_screen_linux.c
    leia_screen_linux.c
)
target_include_directories(test_screen_linux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_screen_linux PRIVATE xrt-interfaces Vulkan::Vulkan m)
add_test(NAME test_screen_linux COMMAND test_screen_linux)

# --- Host unit test: SR weaver chain plan (multi-screen M4) ---------------
# leia_sr_routing_linux.h decides routing + binding from the SR caps; pure
# C, so it runs everywhere. Where the SDK headers carry the structs, the same
# test also builds the REAL pNext chains (leia_sr_chain_sdk_linux.h: SR types
# and macros only, no SR call, no loader) and walks them.
add_executable(test_sr_routing_linux tests/test_sr_routing_linux.c)
target_include_directories(test_sr_routing_linux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(DXR_LEIA_LINUX_WEAVER STREQUAL "sdk" AND DXR_LEIA_LNX_HAVE_SR_ROUTING)
    target_include_directories(test_sr_routing_linux PRIVATE ${_srsdk_snap_inc})
    target_compile_definitions(test_sr_routing_linux PRIVATE DXR_LEIA_LNX_HAVE_SR_ROUTING=1)
endif()
add_test(NAME test_sr_routing_linux COMMAND test_sr_routing_linux)
