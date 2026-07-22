# LeiaSR Linux SDK ↔ DisplayXR Leia plug-in — interface contract

**Status: RECONCILED against the srSDK 1.0.0 prototype** (2026-07-06) — the real C99 API exists and has been diffed against this contract requirement-by-requirement (§8); final ratification awaits the merged `sr-sdk-v*` tag. **Landing target: [LeiaInc/LeiaSR#75](https://github.com/LeiaInc/LeiaSR/pull/75)** (`ST-5532`, "Supersedes #53" — the original C99-API PR #53 is closed). Re-pin Track B off the prototype BuildID onto #75's merged tag once it lands.
Tracking: [#81](https://github.com/DisplayXR/displayxr-leia-plugin/issues/81) (this repo) · [displayxr-runtime#660](https://github.com/DisplayXR/displayxr-runtime/issues/660) (Linux platform, M8).

**Reconciled against:** `leiasr-prototype-sdk.zip` (George, 2026-07-06) — srSDK API version **1.0.0** (`SR_CURRENT_API_VERSION`), `libLeiaSR_runtime.so` BuildID `fcf21021eeb277bac06fdb0e484bd4a8f31ad36b`. The SDK is commercial-licensed; its headers/libs are **never** committed to this repo — Track B builds against a local unpack (`SRSDK_ROOT`).

## 1. Purpose

The DisplayXR runtime now builds and runs on desktop Linux (phases 0/1a/2a/3a of runtime#660 are code-complete; stereo rendering is hardware-validated on X11). The missing piece for Leia hardware is a **Linux backend of this plug-in** (`DisplayXR-LeiaSR.so`) — the 3rd backend after `src/drv_leia` (Windows, SR SDK 1.35.x) and `src/drv_leia_android` (CNSDK 0.10.x).

This document defines **what the LeiaSR Linux SDK must expose** so that the SDK and the plug-in can be developed **in parallel**: the plug-in scaffolds against this contract with a stub weaver today, and swaps in the real SDK the day it ships.

It is a **capability contract**: each requirement says *what the SDK must provide*. Originally the reference signatures were modeled on the Windows SR Vulkan surface (`SR::CreateVulkanWeaver` / `SR::IVulkanWeaver1`); as of the 2026-07-06 reconciliation (§8) they are the **real srSDK 1.0.0 C99 API** shipped in the prototype package. Requirement keywords **MUST / SHOULD / MAY** are RFC-2119.

## 2. Where the SDK sits

```
OpenXR app (Vulkan)
        |
DisplayXR runtime  —  vk_native compositor (in-process, XCB present)
        |                      renders per-eye tiles into ONE atlas VkImage
        |
DisplayXR-LeiaSR.so  (this repo, src/drv_leia_linux — Vulkan display processor)
        |
LeiaSR Linux SDK   ← THIS CONTRACT
        |
Leia SR panel (lenticular lens + eye-tracking camera)
```

Isolation rule (runtime ADR-019): **the runtime never calls the SDK** — only the plug-in does. The runtime discovers the plug-in at `xrCreateInstance` via a JSON manifest (`<probe-order>-leia-sr.json` in an XDG/system `DisplayProcessors/` root) and talks to it exclusively through the `xrt_plugin_iface` / `xrt_display_processor` C ABI (currently **v4**, per `XRT_PLUGIN_API_VERSION_CURRENT` in the runtime's `src/xrt/include/xrt/xrt_plugin.h`).

## 3. Fixed constraints from the runtime side

These are set by the runtime's Linux architecture and shape everything below; the SDK cannot change them.

1. **Vulkan only.** The Linux compositor is `vk_native`; there is no D3D/GL/Metal path. The plug-in implements the runtime's `xrt_display_processor_vk` vtable and nothing else.
2. **The weave input is one atlas image.** The compositor renders all views into a single `VkImage` (side-by-side 2×1 for stereo; grid layouts exist for N-view modes) and **crops it to the active mode's content dimensions before the DP sees it**. The DP receives `VkImage + VkImageView + per-view WxH + tile columns/rows`. There are no separate per-eye images on this path.
3. **The weaver does not own presentation.** The compositor owns the VkSurface/swapchain (XCB) and presents. The weave must land in a compositor-provided target (`VkFramebuffer` + `VkImage`, seed format `VK_FORMAT_B8G8R8A8_UNORM`). Recording into a **caller-provided command buffer** is the preferred model (Windows SR shape); a self-submitting weaver (Android CNSDK shape) is tolerated by the runtime (`is_self_submitting`) but costs an extra queue-idle sync per frame — see R-W6.
4. **Eye poses are pulled, not pushed.** The compositor never passes eye positions into the weave; it separately pulls predicted positions from the DP every frame to build the Kooima off-axis projections the app renders with. The weaver consumes the tracking stream internally for interlacing (both shipping SDKs already work this way).
5. **X11 first.** X11/XCB was chosen over Wayland precisely because a client can query its own absolute screen position — which lenticular phase tracking needs. The DP factory receives `window_handle = NULL` on Linux today (display-scoped weaving, full-screen); per-window phase (app-window weaving, runtime Phase 3b) will supply an X11 `Window`. The SDK's window-dependent features MUST degrade gracefully when no window is supplied.
6. **Resize/target-recreation happens.** The compositor rebuilds its target images on resize and notifies the DP (`notify_target_recreated`); the SDK must tolerate output-target changes at frame granularity (see R-W8).
7. **Calibration never crosses the boundary.** Interlace parameters (lens pitch, slant, subpixel phase, correction textures) stay inside the SDK, exactly as on Windows and Android. The plug-in feeds geometry (viewport, phase origin, input/output images) and reads eye poses + display metrics; it never sees calibration data.

## 4. Surface (a) — Vulkan weaver

### Reference shape (srSDK 1.0.0 prototype — the REAL C API)

This replaces the pre-reconciliation Windows-SR C++ reference (that shape lives on in
`docs/weaver.md`). The consumer links the SDK's **static loader** (`srSDK::loader`, via
`find_package(srSDK CONFIG)`), which `dlopen`s `libLeiaSR_runtime.so` on first call and
dispatches through an append-only function table; `srGetLastLoaderError()` diagnoses
load failures.

```c
#include <sr/sr.h>      /* umbrella: instance/display/eye_tracker/system/weaver/lens */
#include <sr/sr_vk.h>   /* Vulkan weaver extension (no Vulkan headers; opaque aliases) */

/* --- context: senses + callbacks MUST be registered BEFORE srInitialize --- */
SrInstance inst;
srCreateInstance(&(SrInstanceCreateInfo){ .sType = SR_TYPE_INSTANCE_CREATE_INFO,
        .apiVersion = SR_CURRENT_API_VERSION,
        .networkMode = SR_NETWORK_MODE_STANDALONE,
        .applicationName = "DisplayXR-LeiaSR" }, &inst);   /* SR_ERROR_RUNTIME_UNAVAILABLE = R-W1 signal */
srCreateEyeTracker(inst, &(SrEyeTrackerCreateInfo){ .enablePrediction = SR_TRUE }, &tracker);
srCreateSystemMonitor(inst, &monitor_ci, &monitor);
srSystemMonitorAddCallback(monitor, on_event, user);       /* USER_FOUND/USER_LOST, DEVICE_*, LENS_* */
srInitialize(inst);

srCreateDisplay(inst, &(SrDisplayCreateInfo){ .window = 0 }, &display);  /* 0 = primary SR display */
srCreateLens(inst, &lens_ci, &lens);                       /* 2D/3D switch (R-D2) */

/* --- weaver (check srGetRuntimeCapabilities: SR_WEAVER_BACKEND_VULKAN_BIT) --- */
srCreateWeaverVulkan(inst, &(SrWeaverCreateInfoVulkan){
        .device = dev, .physicalDevice = phys, .graphicsQueue = q, .commandPool = pool,
        .window = x11_window /* X11 Window XID, or 0 = windowless/display-scoped */ }, &weaver);
srWeaverSetLatency(weaver, latency_us);                    /* absolute µs — functional (R-W9) */

/* --- per frame --- */
srWeaverSetCommandBufferVulkan(weaver, cmd);               /* recording state; caller submits (R-W6) */
srWeaverSetInputTextureVulkan(weaver, sbs_view, w, h, vk_format);   /* ONE SBS VkImageView (R-W4) */
srWeaverSetOutputFrameBufferVulkan(weaver, fb, w, h, fmt); /* or fb=0 if a render pass is already begun */
srWeaverSetViewportVulkan(weaver, l, t, r, b);
srWeaverSetScissorRectVulkan(weaver, l, t, r, b);
srWeaverWeave(weaver);                                     /* records the interlace pass into cmd */
srWeaverGetPredictedEyePositions(weaver, &l_mm, &r_mm);    /* SrPoint3f, millimeters (R-T1) */

/* --- teardown --- */
srDestroyWeaver(weaver);
srDestroyLens(lens); srDestroyDisplay(display);
srDestroySystemMonitor(monitor); srDestroyEyeTracker(tracker);
srDestroyInstance(inst);                                   /* blocks until SDK threads join */
```

### Requirements

**R-W1 — Context/server connection.** The SDK MUST provide a context-creation call with a distinguishable "SR service not available" failure (exception or error code) so the plug-in can retry with a bounded budget (Windows uses a ~5 s retry loop at DP creation). Document the service/daemon model on Linux: what process serves tracking + calibration, how it starts (systemd user unit? D-Bus activation?), and what happens when it dies mid-session.

**R-W2 — Weaver creation from an existing Vulkan device.** The SDK MUST create its weaver against the compositor's already-created Vulkan objects: `VkInstance` (or none, if not needed), `VkPhysicalDevice`, `VkDevice`, a **graphics `VkQueue` + family index**, and a `VkCommandPool`. It MUST NOT create its own `VkDevice`, own a `VkSwapchainKHR`, or present. If the weaver requires specific device extensions or features, they MUST be enumerable **before** device creation (a static query or documented list) so the runtime/plug-in can enable them.

**R-W3 — Optional native window.** Weaver creation MUST accept an optional X11 `Window` (plus `Display*`/`xcb_connection_t*` as the SDK prefers) and MUST also accept **no window**: with no window, the weaver operates display-scoped (full-panel phase, monitor chosen via the display query in §6). The window, when given, is used only for position/phase and monitor association — never as a render target. (This mirrors Windows, where the HWND drives monitor detection and draw-region only.)

**R-W4 — Input: single tiled atlas.** Per frame the SDK MUST accept the source as **one `VkImageView` (+ `VkImage`) containing all views side-by-side**, with explicit per-view width/height, view format, and tile-grid dimensions (columns × rows). 2×1 stereo is the required baseline; the API SHOULD carry the grid so >2-view panels are expressible later (the runtime already produces 2×2 grids for quad-view modes). Expected input layout (e.g. `SHADER_READ_ONLY_OPTIMAL`) MUST be documented. A UV-flip toggle SHOULD be provided (Vulkan apps render Y-down; the Android SDK exposes exactly this switch).

**R-W5 — Output: caller-provided framebuffer/image.** Per frame the SDK MUST accept the destination as a caller-provided `VkFramebuffer` + `VkImage` + dimensions + `VkFormat`. `VK_FORMAT_B8G8R8A8_UNORM` is the minimum required format (it is what the Linux compositor seeds today); supported formats MUST be documented or queryable. Expected output layout on completion MUST be documented (the plug-in inserts its own barriers around the weave). If the weave renders via its own `VkRenderPass`, that render pass MUST be exposed so the plug-in can pre-build compatible framebuffers (the runtime's `get_render_pass` DP slot exists for exactly this).

**R-W6 — Record into the caller's command buffer.** `weave()` SHOULD record all work into a caller-supplied `VkCommandBuffer` and MUST NOT submit to any queue or block on the GPU in that mode. If the SDK can only self-record/self-submit (CNSDK `do_post_process` shape), that is acceptable but MUST be documented — the plug-in will then declare `is_self_submitting` and the compositor pays an extra `vkQueueWaitIdle` per frame. Record-mode is strongly preferred.

**R-W7 — Viewport AND phase origin, decoupled.** The SDK MUST expose two independent knobs:
- *viewport/scissor* — where on the target the woven pixels land, and
- *phase / screen-position origin* — the absolute panel coordinate the lenticular interlacing pattern is computed against.

Weaving into a sub-rectangle of the panel with correct lens phase is required for display-zones (mixed 2D/3D regions) and per-window weaving. This is the one capability the Windows SR SDK lacks today (its canvas offsets are ignored — runtime#85; the Android CNSDK has `set_viewport` + `set_viewport_screen_position`, which is the shape to copy).

**R-W8 — Target recreation.** The weaver MUST tolerate the output target (image/framebuffer/dimensions) changing between frames without recreation of the weaver itself, or MUST provide a cheap explicit "output invalidated" call. The compositor guarantees the device is idle when it recreates targets and notifies the plug-in (`notify_target_recreated`); Vulkan recycles handles, so any SDK-internal cache keyed on `VkImage`/`VkImageView` handles MUST be invalidatable.

**R-W9 — Latency input that works.** The SDK MUST accept a prediction horizon for the internal eye-pose consumer, as **absolute microseconds** (`setLatency(us)` shape), and it MUST be functional. On Windows, VK `setLatencyInFrames()` is a silent no-op (this repo carries an adaptive-latency workaround, issue #71/#67) — the Linux SDK MUST NOT reproduce that. A frames-based convenience MAY exist, but the µs path is the contract.

**R-W10 — Deterministic teardown.** Destroy MUST be safe on the render thread with the device idle, MUST have a documented order (weaver before context, or independent), MUST NOT require pumping a windowing event loop (the Windows teardown needs Win32 message pumps to dodge a WndProc race — do not port that), and MUST complete or time out in bounded time even if the SR service is unreachable (the Android SDK has a core-release join that can hang — issue #39 class).

**R-W11 — Exceptions stay inside.** If the SDK is C++, no exception may escape the documented API boundary on the per-frame path, or all throwing calls MUST be documented so the plug-in can wrap them (Windows SR throws `std::runtime_error` as routine per-frame control flow; the plug-in try/catches every call because a C++ exception must never cross the plug-in's C ABI). A C API (CNSDK style) sidesteps this entirely and is preferred — see §7.

**R-W12 — Output alpha semantics (documentation MUST; fused compose-under / alpha-native SHOULD).** Transparent-overlay apps (a 3D avatar floating over the live desktop, its transparent regions showing the desktop through — the launch Lenovo use case) need a defined story for the weaver's alpha channel. The SDK MUST **document** whether `weave()` output is opaque (per-pixel alpha flattened, the Windows/CNSDK interlacer behavior) or alpha-preserving. The contract is silent on this today, and the runtime must know which model it is getting.

*This requirement does NOT block transparency.* If the output is opaque (the expected default — the 1.0.0 pipeline uses `loadOp LOAD` and authors no alpha, §8), the plug-in reproduces the **Windows compose-under-bg** model entirely DP-side: it captures the desktop region behind the window and composites it UNDER each atlas tile in a pre-pass (`mix(bg, atlas.rgb, atlas.a)`, out α=1) so the weaver consumes genuinely-opaque, correct pixels. That needs **no SDK change** — R-W4 already takes the atlas, and the compose-under is invisible to the weaver (exactly as on Windows, where transparency lives in the DP, not the SR weaver). See `docs/transparency.md`.

Two SHOULD enhancements, in priority order, that let the runtime retire the per-frame DP pre-pass:
- **(a) Fused background input (preferred).** A per-frame optional background `VkImageView` (+ per-tile UV origin/extent) the weaver composites under each view *before* interlacing. This is strictly the most correct: foreground-over-background happens per-subpixel inside the weave, so anti-aliased silhouette edges and stereo de-occlusion bands are exact, and it saves the DP a full-screen pass and a captured-image import barrier every frame. Shape mirrors the Windows `compose_under_bg` inputs (`bg_uv_origin`/`bg_uv_extent`).
- **(b) Alpha-native output.** Guarantee `weave()` preserves per-pixel alpha end-to-end (the macOS/Metal SR-weaver behavior, commit `e94d07292`). This is especially valuable on Linux: unlike Windows DWM (where the DP shows a *captured copy* of the desktop), an X11 32-bit-ARGB / Wayland premultiplied surface under a compositing WM blends the woven output over the **live** desktop directly — so alpha-native output means **zero desktop capture, lowest latency, always-live background**, the cleanest possible architecture (identical to macOS). The subtlety vs (a): alpha-native composites *after* interlacing, so a single compositor alpha-blend per pixel is applied to an already-interleaved L/R mix — correct for hard silhouettes, approximate at soft stereo edges. (a) is therefore preferred for fidelity, (b) for simplicity/latency; supporting either unblocks the ideal path.

## 5. Surface (b) — eye tracking

Runtime contract this must satisfy: `docs/specs/vendor/eye-tracking-modes.md` in displayxr-runtime (MANAGED vs MANUAL).

**R-T1 — Predicted eye-position pull.** The SDK MUST provide a per-frame read of the predicted left/right eye positions. Units and frame MUST be documented; the reference is the Windows shape — **millimeters, display-center origin, X right, Y up, +Z toward the viewer** — and the plug-in converts to meters. The returned pair MUST be the **same pair the most recent `weave()` consumed** (or the best prediction for the same target time), so the app's Kooima projection and the interlacing agree; disagreement shows as swim/crosstalk.

**R-T2 — Never zeros.** The pose read MUST always return a plausible pair: real tracked positions while tracking, the SDK's animated/collapsed positions during a MANAGED grace period, and frozen last-known (or nominal) after loss. All-zero output is a contract violation (the runtime treats positions as always-valid).

**R-T3 — Explicit tracking state.** The SDK MUST expose tracking state explicitly — a poll (`bool is_tracking` + timestamp) or a state-change callback. The Windows plug-in currently *infers* loss from the eye pair collapsing below ~1 mm separation (the SDK animates the pair to a single point when dropping to 2D); that heuristic is the anti-pattern this requirement retires. The state feeds `XrEventDataEyeTrackingStateChangedDXR` to apps.

**R-T4 — MANAGED mode (required).** In MANAGED mode the SDK owns the loss lifecycle: grace period (~0.5–2 s), animated collapse toward nominal, automatic 3D→2D hardware drop, revival animation on reacquisition. `is_tracking` SHOULD stay `true` through the grace period and flip `false` at the actual 2D switch. This is the Windows SR behavior today and the plug-in's advertised default.

**R-T5 — MANUAL mode (SHOULD).** In MANUAL mode the SDK stands down: no animation, no auto 2D/3D switching, `is_tracking` reflects loss/reacquisition immediately, and positions keep reporting per R-T2. The Android CNSDK implements this as the NoFaceMode-off toggle; a runtime-per-mode switch (`set_eye_tracking_mode`-shaped) is the expected control. If MANUAL ships, the plug-in advertises `supported_eye_tracking_modes = MANAGED|MANUAL`; if not, MANAGED-only (Windows parity) is acceptable for v1.

**R-T6 — Threading.** Tracking runs on SDK threads; all reads in R-T1/R-T3 MUST be callable from the compositor render thread without blocking on camera I/O (lock-free snapshot or brief mutex). Camera device access/permissions on Linux (udev rules, group membership) MUST be documented and handled by the SDK/service installation, not the plug-in.

## 6. Surface (c) — display & calibration

**R-D1 — SR display enumeration.** The SDK MUST identify the (primary) SR display and report, at minimum:

| SDK query (reference: `SR::IDisplay`) | → `xrt_plugin_display_info` field |
|---|---|
| physical size (specify cm or m) | `display_width_m` / `display_height_m` |
| native panel resolution (px) | `display_pixel_width` / `display_pixel_height` |
| position in the X11 virtual desktop (px) | `display_screen_left` / `display_screen_top` |
| recommended per-view render size (px) | `recommended_view_scale_x/y` (= recommended ÷ native) |
| refresh rate | `refresh_mhz` (milli-Hz) |
| recommended viewing position (m, display-center) | `nominal_viewer_x/y/z_m` |
| tracking capability (per §5) | `supported_eye_tracking_modes` / `default_eye_tracking_mode` |

The screen-position query matters doubly on Linux: it anchors lenticular phase (constraint §3.5) and lets the plug-in claim the right monitor in multi-display setups. Values SHOULD be readable **before** weaver creation (the runtime asks for display info at instance creation, before any session/compositor exists — on Windows this works headless via context + display manager only).

**R-D2 — 2D/3D switch.** The SDK MUST expose the lens/backlight 2D⇄3D switch (`SR::SwitchableLensHint` enable/disable/isEnabled shape) independent of weaving, plus a way to read the current hardware state. This backs the runtime's `request_display_mode` / `get_hardware_3d_state` DP slots and MANAGED auto-drop (R-T4).

**R-D3 — Calibration stays internal.** Lens calibration, correction textures, and interlace math remain wholly inside the SDK (loaded from the panel/service, as on Windows/Android). The contract deliberately exposes **no** interlace parameters.

**R-D4 — Headless-tolerant queries.** Display queries SHOULD fail soft (documented error) rather than crash when no SR display is attached — `displayxr-cli selftest` and CI probe the plug-in on machines without hardware, and the plug-in's `probe()` must be able to decline cleanly.

## 7. Packaging & platform

- **Target platform:** Ubuntu 26.04 LTS (GCC 15), x86_64, **Intel Arc Xe-3 iGPU / Mesa ANV** (the launch hardware). Ubuntu 22.04 + NVIDIA is the interim dev box. → open question Q1: please confirm 26.04 + Mesa/ANV are in the SDK's support matrix.
- **ABI: C strongly preferred.** A C API (CNSDK precedent) avoids GCC C++ ABI/version coupling between SDK builds and plug-in builds, and R-W11 for free. If C++ (SR port), the SDK MUST document its compiler/stdlib baseline and keep exceptions inside (R-W11).
- **CMake config package.** Ship `find_package`-able config files, as both existing SDKs do (`simulatedreality`/`srDirectX` on Windows, `CNSDKConfig` on Android): headers + `.so`s + `lib/cmake/<pkg>/<pkg>Config.cmake`, relocatable via a single root env/CMake var (`LEIASR_SDKROOT` shape).
- **Runtime-optional loading.** The plug-in must remain loadable (and cleanly declining in `probe()`) on machines without the SDK/service installed. Either the SDK core is dlopen-able behind a thin always-present loader stub (CNSDK loader model), or the plug-in will dlopen the SDK itself (Windows uses `/DELAYLOAD` for the same effect). Loader-stub model preferred.
- **Service lifecycle:** document install (packages?), start (systemd user unit / D-Bus activation), camera permissions (udev), and crash/restart semantics (R-W1).

## 8. Reconciliation vs srSDK 1.0.0 (prototype, 2026-07-06)

Requirement-by-requirement verdict against the prototype headers (`include/sr/`). ✅ =
honored as asked; ◐ = satisfiable with a documented workaround; ❌ = gap, carried as an
ask on the C99-API PR (send new asks to **LeiaInc/LeiaSR#75**, which supersedes the now-closed #53; the "vendor-confirmed on #53" rows below cite where Cyrus answered on the original thread).

| Req | Verdict | srSDK 1.0.0 reality |
|---|---|---|
| R-W1 context connect | ✅ | `srCreateInstance` returns `SR_ERROR_RUNTIME_UNAVAILABLE`, distinguishable; retry loop is caller-side (no budget param — fine). Service model still undocumented (→ Q3). |
| R-W2 existing VK objects | ◐ | `SrWeaverCreateInfoVulkan{device, physicalDevice, graphicsQueue, commandPool}` ✅; no queue-family param (pool implies it) ✅; **no pre-device-creation enumeration of required device extensions/features** ❌ ask. |
| R-W3 optional window | ✅ | `.window = 0` = windowless/display-scoped, documented "texture-in / weave-out" **and verified in source**: `window==NULL` maps to the legacy `constructedWithoutWindow` path, whose `canWeaveInternal` always weaves; on Linux the window screen-rect helper returns (0,0), so phase = viewport offset in panel coordinates. X11 `Window` XID only — no `Display*`/xcb param (SDK opens its own connection). NB: window-scoped phase is impossible in 1.0.0 at the *implementation* level too, not just the API (see R-W7). |
| R-W4 single tiled atlas | ◐ | ONE SBS `VkImageView` ✅; **no tile-grid param** (2×1 locked, no N-view) ❌; **no UV-flip toggle** ❌. Width/height = **PER-VIEW, vendor-confirmed** (LeiaSR#53 answer: "The size is for a single view, not SBS"); the 1.0.0 implementation stores and never reads the values (source-verified), so this is a forward contract. Input sampled at `SHADER_READ_ONLY_OPTIMAL` — **vendor-confirmed**: the weaver performs no image transitions, caller transitions before `weave()`. |
| R-W5 caller output target | ✅ | Caller `VkFramebuffer` + w/h/format accepted, **or `framebuffer = 0` with a render pass already begun** — **vendor-confirmed** (LeiaSR#53 answer: fb=0 → "weave() will use the currently bound renderpass and it's the caller's responsibility to set it up correctly"; "no depth/multi-subpass"; the fb=0 path is what **Leia's own Unity plugin uses**). Source-verified pipeline pass shape: single color attachment @ outputFormat, `VK_SAMPLE_COUNT_1_BIT`, loadOp LOAD, `COLOR_ATTACHMENT_OPTIMAL` initial/final, one subpass. Output stays writeable/`COLOR_ATTACHMENT_OPTIMAL` (vendor-confirmed). |
| R-W6 record, don't submit | ✅ | `srWeaverSetCommandBufferVulkan` + "caller owns the swapchain, command buffer submission, and synchronisation" (sr_vk.h header note). |
| R-W7 viewport + phase origin | ❌ | `SetViewportVulkan`/`SetScissorRectVulkan` only. **No phase/screen-position-origin API** — the exact Windows gap (runtime#85) reproduced; display-zones and per-window weaving remain inexpressible. Top ask. |
| R-W8 target recreation | ◐ | All `srWeaverSet*` bindings are per-frame settable ✅; **no explicit invalidation call**, and whether the SDK caches on handle values (Vulkan recycles them) is undocumented ❌. |
| R-W9 latency in µs | ✅ | `srWeaverSetLatency(weaver, latencyUs)` — the contract ask, honored **and verified functional in source** (the stored latency feeds `predictingWeaverTracker->predict(latency, …)` every weave). (`SetLatencyInFrames` convenience also exists.) NB found in source: `enableLateLatching` is a **no-op on the Vulkan weaver** (`/*Not implemented*/`) despite sr_weaver.h advertising it — doc ask. |
| R-W10 deterministic teardown | ◐ | `srDestroyWeaver`/`srDestroyInstance` documented, no event-loop pumping ✅; but `srDestroyInstance` "blocks until all internal threads have been joined" with **no timeout** — unbounded if the service dies ❌ (forces a process-lifetime context in the plug-in). |
| R-W11 no exceptions | ✅ | Pure C99 API. `SrResult` everywhere, `srResultToString` for diagnostics. |
| R-W12 output alpha semantics | ◐ | **Undocumented, but opaque by construction:** the 1.0.0 Vulkan pipeline is single-color-attachment @ outputFormat, `loadOp LOAD`, and the interlacer authors no meaningful per-pixel alpha (source-verified, same shape as R-W5). So output is effectively opaque/flattened — the plug-in reproduces the Windows DP-side compose-under-bg model, which needs no SDK change ✅. **No fused background input** (compose-under happens in the DP pre-pass, not the weave) ❌ enhancement ask (a). **No alpha-native guarantee** ❌ enhancement ask (b) — would enable the capture-free ARGB/Wayland live-desktop path on Linux. Neither blocks transparency; both are forward asks on #75. |
| R-T1 predicted pull | ✅ | `srWeaverGetPredictedEyePositions` (mm, display-centre, the pair the weave consumes) + `srEyeTrackerPredict(latencyUs)`; **no timestamp out-param on the weaver pull** ◐. |
| R-T2 never zeros | ◐ | Not documented; `SR_ERROR_DEVICE_NOT_AVAILABLE` when no data yet — plug-in guards with `srDisplayGetDefaultViewingPosition` fallback. |
| R-T3 explicit tracking state | ◐ | **No pollable per-sample flag.** State is event-edge-driven: `srSystemMonitor` + `SR_EVENT_TYPE_USER_FOUND/USER_LOST` (plus `DEVICE_READY/DISCONNECTED`, `SR_UNAVAILABLE/RESTORED`). Workable — plug-in latches events into an atomic — but a different shape than asked; callbacks must be registered **before `srInitialize`** (no late sense attach). |
| R-T4 MANAGED | ◐ | Grace/collapse/auto-2D semantics now understood **from source**: the weaver internally decides weave-vs-blit per frame by `eyeSeparation > 1 mm` (the collapse animation drives the 2D drop), and flips the lens hint on that transition. Still no *observable* grace-period state for the plug-in, so `is_tracking` flips at raw `USER_LOST`, earlier than R-T4 prefers ❌ doc/API ask. |
| R-T5 MANUAL | ❌ | No stand-down toggle. MANAGED-only (acceptable for v1 per contract). |
| R-T6 threading | ✅ | `srEyeTrackerPredict` documented safe from any thread incl. render thread; callbacks on SDK thread. |
| R-D1 display enumeration | ◐ | `srCreateDisplay({window:0})` + `GetPhysicalSize(cm)/GetPhysicalResolution/GetLocation/GetRecommendedTextureSize/GetDefaultViewingPosition(mm)/GetIdentifier` map 1:1 ✅, readable pre-weaver ✅; **no refresh-rate getter** ❌ (weaver knows it internally for `SetLatencyInFrames`); `GetRecommendedTextureSize` = **per-view, settled**: it wraps the same legacy `getRecommendedViewsTextureWidth/Height` the Windows arm already consumes and logs as "per eye". |
| R-D2 2D/3D switch | ✅ | `sr_lens.h`: `srLensEnable/Disable/IsEnabled` (+ `lensPreference` at instance create, `SR_EVENT_TYPE_LENS_ON/OFF` events). |
| R-D3 calibration internal | ✅ | No interlace parameters exposed anywhere. |
| R-D4 headless-tolerant | ✅ | `srCreateDisplay` succeeds with no panel; getters return `SR_ERROR_DISPLAY_NOT_FOUND`; `srDisplayIsValid` is the probe primitive. |
| §7 packaging | ✅ | C API ✅; `find_package(srSDK CONFIG)` package, relocatable ✅; loader-stub dlopen model ✅ (static `libsrSDK_loader.a` → `libLeiaSR_runtime.so`, search order: `/etc/leia/sr/1/active_runtime.json` → `SR_RUNTIME_PATH` → plain dlopen). Support matrix (26.04/Mesa ANV) unconfirmed (→ Q1). |

## 9. Open questions for ratification

Post-reconciliation status per question — answered ones kept for the record:

1. **Ubuntu 26.04 / GCC 15 / Mesa ANV (Intel Arc Xe-3)** — **still open.** The prototype ships x86_64 Linux binaries but the support matrix is unconfirmed.
2. **Lineage** — **answered:** new C99 srSDK (LeiaInc/LeiaSR#53 loader/runtime split), neither a straight SR C++ port nor CNSDK; SR-derived semantics with a Vulkan-style extensible-struct C API.
3. **Tracking service model on Linux** — **still open.** `SrNetworkMode` (STANDALONE/CLIENT/…) exists in the API but the intended Linux deployment (in-process runtime vs daemon), camera stack, and udev/permission story are undocumented.
4. **Phase origin API (R-W7)** — **answered: NOT in 1.0.0.** Viewport/scissor only; the decoupled screen-position-phase knob is the top carried ask (runtime#85 class).
5. **Latency API (R-W9)** — **answered: yes.** `srWeaverSetLatency(µs)` is a first-class dispatch-table entry.
6. **N-view roadmap** — **answered: 2×1 locked in 1.0.0.** `srWeaverSetInputTextureVulkan` carries no tile grid; N-view stays a contract SHOULD for a later API rev.
7. **C vs C++ ABI** — **answered: C99**, with an append-only dispatch-table ABI (frozen slot layout for major version 1).
8. **Wayland** — **effectively answered:** `SrNativeWindowHandle` is an X11 `Window` (`unsigned long`) on Linux; the SDK opens its own X connection. Wayland unaddressed in 1.0.0.
9. **Transparency / output alpha (R-W12)** — **new, open.** (i) Confirm the weaver output is opaque/alpha-flattened (source suggests yes) so we can commit to the DP-side compose-under-bg model. (ii) Is a **fused background input** (compose-under inside `weave()`, ask a) on the roadmap? (iii) Is **alpha-native output** (ask b, macOS-weaver parity) feasible — it would let the Linux plug-in skip desktop capture entirely and blend over the live desktop via an ARGB/Wayland surface. Neither (ii) nor (iii) blocks the launch (DP-side compose-under ships without them); both are cheap-before-tag.

## 10. Appendix — how the SDK gets driven, per frame

The plug-in's Linux DP implements the runtime's `xrt_display_processor_vk` vtable (`src/xrt/include/xrt/xrt_display_processor_vk.h`, ABI v4). Trace:

```
xrCreateInstance
  └─ runtime loads DisplayXR-LeiaSR.so (JSON manifest) → xrtPluginNegotiate (ABI v4)
       └─ iface.get_display_info → SDK display queries (R-D1)          [no weaver yet]
session start (vk_native compositor)
  └─ iface.create_dp_vk(vk_bundle, cmd_pool, window=NULL, VK_FORMAT_B8G8R8A8_UNORM, &dp)
       └─ SDK: context connect (R-W1) → weaver create on compositor's
          device/queue/pool (R-W2/W3) → latency set (R-W9)
each frame
  ├─ dp.get_predicted_eye_positions ──→ SDK pose pull (R-T1..T3)   [feeds app Kooima views]
  └─ dp.process_atlas(cmd, atlas_img/view, view_wh, cols×rows,
                      target_fb/img/wh/fmt, canvas_off/size)
       ├─ 1×1 grid  → plain blit, weaver bypassed (2D)
       └─ else      → SDK: set input atlas (R-W4) + output fb (R-W5)
                      + viewport/phase (R-W7) + weave() into cmd (R-W6)
resize
  └─ dp.notify_target_recreated → SDK output-invalidate (R-W8)
mode/tracking policy
  ├─ dp.set_eye_tracking_mode(MANAGED|MANUAL) → R-T4/T5
  └─ dp.request_display_mode(3D on/off)       → R-D2
teardown
  └─ dp.destroy → SDK weaver + context destroy (R-W10)
```

Related reading: `docs/weaver.md` (Windows SR weaver internals), `docs/cnsdk-c-abi-surface.md` + `docs/android-weaving-and-transparency.md` (Android CNSDK shape), and in displayxr-runtime: `docs/reference/xrt_plugin_iface.md`, `docs/specs/runtime/plugin-discovery.md`, `docs/specs/vendor/eye-tracking-modes.md`, `docs/roadmap/linux-support.md`.
