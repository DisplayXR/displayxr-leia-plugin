# Leia SR Integration

Leia is the first 3D-display vendor integrated into DisplayXR. This directory documents the Leia-specific implementation internals — the code in this repo's `src/drv_leia/`. The vendor-neutral contract every plug-in must follow lives in the runtime repo: [`docs/specs/vendor/`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/vendor/) and [`docs/guides/vendor-plugin-onboarding.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/guides/vendor-plugin-onboarding.md).

## Source layout

`src/drv_leia/`:

| Group | Files |
|---|---|
| Plug-in entry (`xrtPluginNegotiate`) | `leia_plugin.c` |
| Driver entry / device | `leia_device.c`, `leia_interface.h`, `leia_types.h`, `leia_edid_probe.c`, `leia_sr_probe.cpp` |
| SR SDK bridge | `leia_cnsdk.{cpp,h}` |
| Display processor (per API) | `leia_display_processor.{cpp,h}` (base), `leia_display_processor_d3d11.{cpp,h}`, `leia_display_processor_d3d12.{cpp,h}`, `leia_display_processor_gl.{cpp,h}` |
| Weaver (per API) | `leia_sr.{cpp,h}` (base + eye tracking), `leia_sr_d3d11.{cpp,h}`, `leia_sr_d3d12.{cpp,h}`, `leia_sr_gl.{cpp,h}` |
| Background capture (transparency) | `leia_bg_capture_win.{cpp,h}` |
| Shaders | `shaders/` |

## Docs in this directory

- **[Weaver internals](weaver.md)** — DX11 / DX12 / OpenGL / Vulkan weaver creation, inputs, weave() flow, DPI handling, phase math.
- **[Transparency model](transparency.md)** — current primary path: WGC compose-under-bg on D3D11 / D3D12 / Vulkan. Replaces the older chroma-key approach for those APIs.
- **[Chroma-key overlay (legacy / OpenGL fallback)](chroma-key-overlay.md)** — fallback path; still the only transparency path on the Leia OpenGL DP.
- **[Late latching](late-latching.md)** — which backends implement it (D3D11/GL automatic, VK needs our submit hook, D3D12 is a stub), what `srWeaverIsLateLatchingEnabled` hides, the once-per-frame / deferred-context contract from the shipped header, and the dot test — including why `pattern = 450` makes it lie.
- **[SR panel readiness and late geometry](sr-readiness.md)** — the boot race (SR "default display" until the FPC serial link answers; never while the panel sleeps), the two readiness signals that actually work (`Global\sharedDeviceSerialMemory` count byte, a *fresh* valid display handle), the ONE shared startup budget (`DXR_LEIA_SR_READY_TIMEOUT_S`, default 20 s), the 1 Hz late-identification watcher that re-derives geometry and updates the live head device in place, and the `get_display_info` contract with the runtime (#266).
- **[Window phase snapping](window-phase-snapping.md)** — the SR weaver's automatic `WndProc` subclassing for lenticular phase alignment during window drag (Windows), and the resolved `WndProcDispatcher` race.
- **[Display mode switching (2D/3D)](display-mode-switching.md)** — how the plug-in maps the neutral mode-request API onto `SwitchableLensHint` (Windows) / `leia_core_set_backlight` (Android) / `srLensEnable`/`srLensDisable` (Linux), and who owns the Linux lens preference once DisplayXR has made its first call (LeiaSR #266).
- **[CNSDK Android calibration](cnsdk-android-calibration.md)** — symptom→fix procedure for the three CNSDK convention assumptions (face axes, tile-to-eye mapping, UV flip); pending Lume Pad hardware validation.
- **[Android weaving, orientation, zones & transparency](android-weaving-and-transparency.md)** — the Android CNSDK weave model (per-subpixel phase, predicted center eye), the three-orientation reconciliation, sub-rect/display-zone weaving (the two viewport knobs + the portrait==natural test gotcha), and why per-pixel-alpha transparency is fundamentally limited on Android (+ the background-capture privilege landscape on NP02J).
- **[The untracked fallback, and how to tell which SDK shader drew a bad frame](untracked-fallback.md)** — who owns the no-viewer frame (the SDK, driven by config, not this plug-in), why the eye pair the DP reports is NOT the tracker the SDK branches on, and the alpha fingerprint that identifies which SDK shader drew a bad frame. Records #178 (untracked pulse renders black; fixed SDK-side in LeiaSR#189).
- **[LeiaSR Linux SDK contract](leia-linux-sdk-contract.md)** — PROPOSED interface contract the LeiaSR Linux SDK must expose (Vulkan weaver, eye tracking, display/calibration) so the Linux backend (#81) and the SDK can be built in parallel.
- **[Linux Track B runbook](linux-track-b-runbook.md)** — how to build and validate the real srSDK Vulkan weaver on a Leia-panel Linux box: the `leiasr-runtime` .deb as SDK, the mandatory local-runtime-checkout ABI rule, dev plug-in registration, Linux logging (no log file — stderr + `XRT_LOG=info`), and per-box bring-up gotchas (22.04/NVIDIA and Ubuntu 26.04/Mesa).
- **[Linux `.deb` packaging](linux-deb-packaging.md)** — the vendor `.deb` that drops the DP into the runtime's built-in discovery dir at probe_order 50.

## Build flags

Per-API weaver support is gated on which SR SDK libraries exist under `${SR_PATH}/lib` at configure time (see `src/drv_leia/CMakeLists.txt`):

- `SimulatedRealityDirectX.lib` → D3D11 + D3D12 weavers (`XRT_HAVE_LEIA_SR_D3D11`, `XRT_HAVE_LEIA_SR_D3D12`) and the WGC background-capture module.
- `SimulatedRealityOpenGL.lib` → GL weaver (`XRT_HAVE_LEIA_SR_GL`).
- `SimulatedRealityVulkanBeta.lib` → Vulkan weaver (`XRT_HAVE_LEIA_SR_VULKAN`) + SPIR-V shader compilation.

See the root `CLAUDE.md` "Build commands" section for the canonical build flow.

## Eye-tracking mode

Leia ships in **MANAGED** mode (the runtime polls SR SDK's `LookaroundFilter` on Leia's behalf). The MANAGED/MANUAL contract is described in [`docs/specs/vendor/eye-tracking-modes.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/vendor/eye-tracking-modes.md).
