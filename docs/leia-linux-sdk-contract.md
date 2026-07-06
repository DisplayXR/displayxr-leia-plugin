# LeiaSR Linux SDK ↔ DisplayXR Leia plug-in — interface contract

**Status: PROPOSED** — awaiting ratification by the LeiaSR Linux SDK team (George / Suki, `#sw-runtime-linux`).
Tracking: [#81](https://github.com/DisplayXR/displayxr-leia-plugin/issues/81) (this repo) · [displayxr-runtime#660](https://github.com/DisplayXR/displayxr-runtime/issues/660) (Linux platform, M8).

## 1. Purpose

The DisplayXR runtime now builds and runs on desktop Linux (phases 0/1a/2a/3a of runtime#660 are code-complete; stereo rendering is hardware-validated on X11). The missing piece for Leia hardware is a **Linux backend of this plug-in** (`DisplayXR-LeiaSR.so`) — the 3rd backend after `src/drv_leia` (Windows, SR SDK 1.35.x) and `src/drv_leia_android` (CNSDK 0.10.x).

This document defines **what the LeiaSR Linux SDK must expose** so that the SDK and the plug-in can be developed **in parallel**: the plug-in scaffolds against this contract with a stub weaver today, and swaps in the real SDK the day it ships.

It is a **capability contract, SR-shaped**: each requirement says *what the SDK must provide*; the reference signatures are modeled on the existing Windows SR Vulkan surface (`SR::CreateVulkanWeaver` / `SR::IVulkanWeaver1`), so a straight SR-to-Linux port satisfies the contract with near-zero delta — but a CNSDK-derived SDK can satisfy it equally (the Android backend proves both shapes drive the same runtime interface). Requirement keywords **MUST / SHOULD / MAY** are RFC-2119.

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

### Reference shape (Windows SR, works today)

```cpp
// context / display
SR::SRContext::create();                       // throws SR::ServerNotAvailableException
SR::GetDisplayManagerInstance(ctx).getPrimaryActiveSRDisplay();
SR::SwitchableLensHint::create(ctx);
// weaver
SR::CreateVulkanWeaver(ctx, VkDevice, VkPhysicalDevice, VkQueue /*graphics*/,
                       VkCommandPool, <native window>, IVulkanWeaver1 **out);
// per frame
weaver->setViewport(rect); weaver->setScissorRect(rect);
weaver->setCommandBuffer(cmd);
weaver->setInputViewTexture(viewL, viewR, w, h, fmt);   // SBS atlas as L, R = NULL
weaver->setOutputFrameBuffer(fb, w, h, fmt);
weaver->setLatency(us);
weaver->weave();                                        // records into cmd
weaver->getPredictedEyePositions(l[3], r[3]);           // millimeters
weaver->destroy();
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

## 5. Surface (b) — eye tracking

Runtime contract this must satisfy: `docs/specs/vendor/eye-tracking-modes.md` in displayxr-runtime (MANAGED vs MANUAL).

**R-T1 — Predicted eye-position pull.** The SDK MUST provide a per-frame read of the predicted left/right eye positions. Units and frame MUST be documented; the reference is the Windows shape — **millimeters, display-center origin, X right, Y up, +Z toward the viewer** — and the plug-in converts to meters. The returned pair MUST be the **same pair the most recent `weave()` consumed** (or the best prediction for the same target time), so the app's Kooima projection and the interlacing agree; disagreement shows as swim/crosstalk.

**R-T2 — Never zeros.** The pose read MUST always return a plausible pair: real tracked positions while tracking, the SDK's animated/collapsed positions during a MANAGED grace period, and frozen last-known (or nominal) after loss. All-zero output is a contract violation (the runtime treats positions as always-valid).

**R-T3 — Explicit tracking state.** The SDK MUST expose tracking state explicitly — a poll (`bool is_tracking` + timestamp) or a state-change callback. The Windows plug-in currently *infers* loss from the eye pair collapsing below ~1 mm separation (the SDK animates the pair to a single point when dropping to 2D); that heuristic is the anti-pattern this requirement retires. The state feeds `XrEventDataEyeTrackingStateChangedEXT` to apps.

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

## 8. Open questions for ratification

1. **Ubuntu 26.04 / GCC 15 / Mesa ANV (Intel Arc Xe-3)** — in the SDK support matrix? Any known blockers? (Carried over from the Slack thread — still unanswered.)
2. **Lineage** — is the Linux SDK an SR (SimulatedReality) port or CNSDK-derived? (Contract accepts either; affects which reference signatures are literal.)
3. **Tracking service model on Linux** — separate daemon (SR-service analog) vs in-process? Camera stack (V4L2? RealSense-style?) and permission story?
4. **Phase origin API (R-W7)** — confirm the Linux SDK will expose the decoupled viewport + screen-position-phase pair (CNSDK shape) rather than the Windows SR viewport-only shape.
5. **Latency API (R-W9)** — confirm absolute-µs prediction horizon works on Linux (the Windows VK `setLatencyInFrames` no-op made us build adaptive-latency workarounds we'd like to delete, #71).
6. **N-view roadmap** — should the input API carry the tile grid now (R-W4 SHOULD), or is 2×1 stereo locked for the Linux panel generation?
7. **C vs C++ ABI** (§7) — preference?
8. **Wayland** — any SDK-side dependency on X11 beyond what the plug-in passes in (R-W3)? The runtime is X11-first; Wayland is deferred, but SDK X11 assumptions would be good to know.

## 9. Appendix — how the SDK gets driven, per frame

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
