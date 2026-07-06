# DisplayXR — Leia SR Plug-in

**The first hardware integration for DisplayXR — and the reference example of a vendor plug-in.**

This plug-in turns Leia SR glasses-free 3D displays (e.g. Samsung Odyssey 3D, Acer SpatialLabs) into OpenXR targets for the [DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime). Building a plug-in for your own display? Start from this worked example, alongside the [vendor onboarding guide](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/guides/vendor-plugin-onboarding.md).

**Get it:** download `DisplayXRLeiaSRSetup-<version>.exe` from the [Releases page](https://github.com/DisplayXR/displayxr-leia-plugin/releases). The DisplayXR runtime must be installed first (the installer reads `HKLM\Software\DisplayXR\Runtime\InstallPath`).

## How it works

Ships `DisplayXR-LeiaSR.dll`, a vendor plug-in DLL implementing `xrt_plugin_iface` from the runtime's public ABI (`xrt/xrt_plugin.h`). It loads at `xrCreateInstance` time via registry-driven discovery (`HKLM\Software\DisplayXR\DisplayProcessors\leia-sr`).

## Architecture

- **`src/drv_leia/`** — Windows driver source: device, EDID probe, plug-in entry point (`xrtPluginNegotiate`), per-API display processors (D3D11, D3D12, OpenGL, Vulkan), SR SDK weavers, eye-tracking listener, WGC background capture.
- **`src/drv_leia_android/`** — Android arm (CNSDK): `libdxrp050_leia_cnsdk.so` for the runtime APK.
- **`src/drv_leia_linux/`** — Linux desktop arm (**Track A scaffold**): `DisplayXR-LeiaSR.so` with a **stub weaver** (passthrough, no SR SDK). The SDK-facing interface is fixed by the [LeiaSR Linux SDK contract](docs/leia-linux-sdk-contract.md) (PROPOSED); Track B drops the real SDK behind the same seam. Build/validate with `scripts/build-linux.sh`; CI covers Ubuntu 22.04/24.04/26.04.
- **`installer/DisplayXRLeiaSRInstaller.nsi`** — NSIS installer that drops the DLL at `$RuntimeInstall\Plugins\LeiaSR\` and registers the plug-in under `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr`.

## Documentation

Implementation internals live in [`docs/`](docs/) (migrated from the runtime's `docs/vendors/leia/`):

- [Integration overview](docs/README.md) — source layout, build flags, eye-tracking mode
- [Weaver internals](docs/weaver.md) — DX11 / DX12 / GL / Vulkan weaver creation, weave() flow, DPI, phase math
- [Transparency model](docs/transparency.md) — WGC compose-under-bg (primary) on D3D11 / D3D12 / Vulkan
- [Chroma-key overlay](docs/chroma-key-overlay.md) — legacy fallback; still the only path on the GL DP
- [Window phase snapping](docs/window-phase-snapping.md) — SR weaver WndProc subclassing for lenticular phase alignment
- [Display mode switching](docs/display-mode-switching.md) — 2D/3D via `SwitchableLensHint` / backlight
- [CNSDK Android calibration](docs/cnsdk-android-calibration.md) — face axes / tile-to-eye / UV-flip validation procedure for Lume Pad
- [LeiaSR Linux SDK contract](docs/leia-linux-sdk-contract.md) — what the Linux SDK must expose (PROPOSED, #81); `src/drv_leia_linux/leia_sr_linux.h` mirrors it

The vendor-neutral contract (what any plug-in must implement) stays in the runtime repo: `docs/specs/vendor/`, `docs/specs/runtime/plugin-discovery.md`, `docs/guides/vendor-plugin-onboarding.md`.

## License

[Apache-2.0](LICENSE).

## Related

- ADR-019 — vendor plug-in / aux boundary: `displayxr-runtime/docs/adr/ADR-019-vendor-plugin-aux-boundary.md`
- Plug-in discovery spec: `displayxr-runtime/docs/specs/runtime/plugin-discovery.md`
- Vendor onboarding guide (post-#263): `displayxr-runtime/docs/guides/vendor-plugin-onboarding.md`
