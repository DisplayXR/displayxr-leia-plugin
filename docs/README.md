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
- **[Window phase snapping](window-phase-snapping.md)** — the SR weaver's automatic `WndProc` subclassing for lenticular phase alignment during window drag (Windows), and the resolved `WndProcDispatcher` race.
- **[Display mode switching (2D/3D)](display-mode-switching.md)** — how the plug-in maps the neutral mode-request API onto `SwitchableLensHint` (Windows) / `leia_core_set_backlight` (Android).

## Build flags

Per-API weaver support is gated on which SR SDK libraries exist under `${SR_PATH}/lib` at configure time (see `src/drv_leia/CMakeLists.txt`):

- `SimulatedRealityDirectX.lib` → D3D11 + D3D12 weavers (`XRT_HAVE_LEIA_SR_D3D11`, `XRT_HAVE_LEIA_SR_D3D12`) and the WGC background-capture module.
- `SimulatedRealityOpenGL.lib` → GL weaver (`XRT_HAVE_LEIA_SR_GL`).
- `SimulatedRealityVulkanBeta.lib` → Vulkan weaver (`XRT_HAVE_LEIA_SR_VULKAN`) + SPIR-V shader compilation.

See the root `CLAUDE.md` "Build commands" section for the canonical build flow.

## Eye-tracking mode

Leia ships in **MANAGED** mode (the runtime polls SR SDK's `LookaroundFilter` on Leia's behalf). The MANAGED/MANUAL contract is described in [`docs/specs/vendor/eye-tracking-modes.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/vendor/eye-tracking-modes.md).
