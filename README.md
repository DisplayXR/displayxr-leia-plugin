# displayxr-leia-plugin

Leia SR display-processor plug-in for [DisplayXR Runtime](https://github.com/DisplayXR/displayxr-runtime).

Ships `DisplayXR-LeiaSR.dll`, a vendor plug-in DLL implementing `xrt_plugin_iface` from the runtime's public ABI (`xrt/xrt_plugin.h`). Loaded at `xrCreateInstance` time via registry-driven discovery (`HKLM\Software\DisplayXR\DisplayProcessors\leia-sr`).

Distributed as `DisplayXRLeiaSRSetup-<version>.exe`. Hard prereq: the DisplayXR runtime must be installed (the installer reads `HKLM\Software\DisplayXR\Runtime\InstallPath`).

## Status

This repo was extracted from `DisplayXR/displayxr-runtime` per issue [#263](https://github.com/DisplayXR/displayxr-runtime/issues/263) (ADR-019 follow-up). The source under `src/drv_leia/` was previously at `src/xrt/drivers/leia/` in the runtime tree. The pre-extraction history lives in the runtime repo at the `pre-leia-extraction-*` tag.

## Architecture

- **`src/drv_leia/`** — driver source: device, EDID probe, plug-in entry point (`xrtPluginNegotiate`), per-API display processors (D3D11, D3D12, OpenGL), SR SDK weavers, eye-tracking listener, WGC background capture.
- **`installer/DisplayXRLeiaSRInstaller.nsi`** — NSIS installer that drops the DLL at `$RuntimeInstall\Plugins\LeiaSR\` and registers the plug-in under `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr`.

CMake build glue + CI workflow + `scripts/build-windows.bat` follow in a separate commit (see issue #263 plan).

## Documentation

Implementation internals live in [`docs/`](docs/) (migrated from the runtime's `docs/vendors/leia/`):

- [Integration overview](docs/README.md) — source layout, build flags, eye-tracking mode
- [Weaver internals](docs/weaver.md) — DX11 / DX12 / GL / Vulkan weaver creation, weave() flow, DPI, phase math
- [Transparency model](docs/transparency.md) — WGC compose-under-bg (primary) on D3D11 / D3D12 / Vulkan
- [Chroma-key overlay](docs/chroma-key-overlay.md) — legacy fallback; still the only path on the GL DP
- [Window phase snapping](docs/window-phase-snapping.md) — SR weaver WndProc subclassing for lenticular phase alignment
- [Display mode switching](docs/display-mode-switching.md) — 2D/3D via `SwitchableLensHint` / backlight
- [CNSDK Android calibration](docs/cnsdk-android-calibration.md) — face axes / tile-to-eye / UV-flip validation procedure for Lume Pad

The vendor-neutral contract (what any plug-in must implement) stays in the runtime repo: `docs/specs/vendor/`, `docs/specs/runtime/plugin-discovery.md`, `docs/guides/vendor-plugin-onboarding.md`.

## License

[BSL-1.0](LICENSE) — same as the DisplayXR runtime.

## Related

- ADR-019 — vendor plug-in / aux boundary: `displayxr-runtime/docs/adr/ADR-019-vendor-plugin-aux-boundary.md`
- Plug-in discovery spec: `displayxr-runtime/docs/specs/runtime/plugin-discovery.md`
- Vendor onboarding guide (post-#263): `displayxr-runtime/docs/guides/vendor-plugin-onboarding.md`
