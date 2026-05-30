# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repo.

## What this repo is

The **Leia SR display-processor plug-in** for the DisplayXR runtime.
Ships `DisplayXR-LeiaSR.dll` — a vendor plug-in implementing the
`xrt_plugin_iface` ABI from
[`displayxr-runtime/src/xrt/include/xrt/xrt_plugin.h`](https://github.com/DisplayXR/displayxr-runtime/blob/main/src/xrt/include/xrt/xrt_plugin.h).
Loaded at `xrCreateInstance` time by the runtime's registry-driven
plug-in discovery (`HKLM\Software\DisplayXR\DisplayProcessors\leia-sr`).

End-user artifact: `DisplayXRLeiaSRSetup-<version>.exe`. Hard prereq:
the DisplayXR runtime must be installed first; the installer reads
`HKLM\Software\DisplayXR\Runtime\InstallPath` to drop the DLL at
`$RuntimeInstall\Plugins\LeiaSR\`.

## Where this fits in DisplayXR

```
              displayxr-runtime
                 │   │
                 │   └── xrt_plugin.h (ABI surface, ADR-020)
                 │           ▲
                 │           │ rebuilds against
                 ▼           │
              displayxr-leia-plugin  ←  this repo
                 │
                 ▼  ships DisplayXR-LeiaSR.dll
              installed at runtime tree, loaded at xrCreateInstance
```

Sibling repos worth knowing about:
- [`DisplayXR/displayxr-runtime`](https://github.com/DisplayXR/displayxr-runtime) — the OpenXR runtime + plug-in ABI + dev orchestrator
- [`DisplayXR/displayxr-installer`](https://github.com/DisplayXR/displayxr-installer) — meta-installer that bundles this plug-in alongside runtime + shell + mcp + demos
- [`DisplayXR/displayxr-shell-pvt`](https://github.com/DisplayXR/displayxr-shell-pvt) (private) — the workspace-controller shell
- [`DisplayXR/displayxr-mcp`](https://github.com/DisplayXR/displayxr-mcp) — MCP framework used by runtime + shell

## ABI contract — the most important thing to internalize

This plug-in's compatibility with a given runtime is governed by
`XRT_PLUGIN_API_VERSION_CURRENT`. The plug-in reports its supported
ABI from the runtime headers it was built against:

- `CMakeLists.txt` pins `DXR_RUNTIME_GIT_TAG` (currently a `v*` runtime tag).
- That tag's `xrt_plugin.h` defines `XRT_PLUGIN_API_VERSION_CURRENT`.
- `src/drv_leia/leia_plugin.c::xrtPluginNegotiate` reports that value.
- The runtime's loader (`target_plugin_loader.c`) **rejects** plug-ins reporting an ABI major different from the runtime's current ABI (ADR-020 rule 3).

**To bump a runtime ABI major:** update `DXR_RUNTIME_GIT_TAG` in
`CMakeLists.txt` to the new runtime tag, tag a new plug-in release.
If you forget, the loader will silently fall back to `sim_display`
and Leia weaving won't work — and `versions.json` won't auto-bump
either (see "Releasing" below).

ADR-020 spec: [`displayxr-runtime/docs/adr/ADR-020-plugin-abi-policy.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/adr/ADR-020-plugin-abi-policy.md).

## Code structure

| Path | What it is |
|---|---|
| `src/drv_leia/leia_plugin.c` | `xrtPluginNegotiate` entry point — the only symbol the runtime calls into. Reports ABI version + iface vtable. |
| `src/drv_leia/leia_device.c` | `xrt_device` impl — tracking + view config |
| `src/drv_leia/leia_display_processor*.{cpp,h}` | Per-API display processors (D3D11, D3D12, GL). Implement `xrt_display_processor_<api>` vtables; the runtime's compositor invokes them per-frame. |
| `src/drv_leia/leia_sr_*.{cpp,h}` | SR SDK weaver wrappers. The SDK throws `std::runtime_error` as routine internal control flow (~11/frame in some paths) — every call wrapped in try/catch. |
| `src/drv_leia/leia_bg_capture_win.{cpp,h}` | WGC background capture for compose-under transparency (Leia transparency model). |
| `src/drv_leia/leia_edid_probe.c` | EDID-based hardware detection — answers "is a Leia display attached?" before the SR SDK initializes. |
| `installer/DisplayXRLeiaSRInstaller.nsi` | NSIS installer. Drops DLL at `$RuntimeInstall\Plugins\LeiaSR\`; writes registry entry `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr\{Path, ProbeOrder}`. |
| `scripts/build-windows.bat` | Local Windows build entry point. |

## Build commands

### Windows (canonical)
```bat
scripts\build-windows.bat all        REM full build (configure + plugin + installer)
scripts\build-windows.bat build      REM plug-in DLL only
scripts\build-windows.bat installer  REM NSIS installer (requires DLL built)
```

Requires VS 2022 + Ninja + Vulkan SDK + GitHub CLI (the SR SDK is
downloaded from a private GH release; `gh auth status` must show
authenticated). On first run the script pulls the SR SDK at
`SR_TAG` (set in `CMakeLists.txt`).

### Local-build override of the runtime pin
If you're testing against a yet-unreleased runtime ABI:
```bat
cmake -DDXR_RUNTIME_GIT_TAG=<branch-or-sha> ...
```
DON'T commit a non-tag pin — the dev orchestrator and meta-installer
both assume `DXR_RUNTIME_GIT_TAG` is always a `vX.Y.Z` tag.

## Releasing

Preferred path: the user-level [`/dxr-release`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/versions-json-autobump.md)
skill. It detects this repo, tags HEAD, watches CI, and reports the
ABI gate + auto-bump + installer mirror outcome.

Manual fallback:
```bash
git tag -a vX.Y.Z -m "release notes ..."
git push origin vX.Y.Z
```

### What happens on tag push

1. `.github/workflows/build-windows.yml` builds the DLL + the NSIS installer.
2. `softprops/action-gh-release@v2` creates the GitHub Release and attaches `DisplayXRLeiaSRSetup-*.exe`.
3. **`DispatchVersionsBump` job** fires a `repository_dispatch` at
   `displayxr-runtime/versions-bump.yml` with `field: "leia_plugin"`.
4. The runtime side runs the ABI assertion
   ([`scripts/check_plugin_abi.py`](https://github.com/DisplayXR/displayxr-runtime/blob/main/scripts/check_plugin_abi.py)):
   - **If ABIs match** → `versions.json[leia_plugin]` bumps on runtime/main + mirrors to installer/main. The dev orchestrator and meta-installer immediately pick up the new pin.
   - **If ABIs mismatch** → bump is **skipped**, a tracking issue is
     auto-opened on THIS repo with the diagnostic + fix recipe
     (rebuild against the current runtime, tag a new release).

Full spec:
[`displayxr-runtime/docs/specs/runtime/versions-json-autobump.md`](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/versions-json-autobump.md).

## Things to be careful about

- **Don't `throw` from anywhere reachable by the runtime's loader or
  per-frame display-processor methods.** The runtime is C and won't
  catch. Every SR SDK call must have a try/catch wrapper — the SDK
  throws as routine control flow (`getPredictedEyePositions` is the
  worst offender; see v1.0.7 release notes for context).
- **Don't bump `DXR_RUNTIME_GIT_TAG` and tag the plug-in without
  testing first.** ABI mismatches are visible at runtime
  (`sim_display` fallback) but silent — the installer reports
  success. Run a `cube_handle_d3d11_win` smoke test on Leia
  hardware before tagging.
- **Don't add Vulkan support yet.** The SR SDK's Vulkan weaver
  (`SimulatedRealityVulkanBeta`) exists but the runtime currently
  routes VK apps through sim_display anyway pending issue #357.
  When that lands, this plug-in will gain a `leia_display_processor_vk`
  parallel to the D3D11/D3D12/GL ones.
- **Registry registration is at install time.** During dev,
  installer not run, the DLL won't load. Use the runtime's
  `XRT_PLUGIN_SEARCH_PATH` env var to point at the dev build's
  `Plugins/LeiaSR/` directory.
- **The pre-extraction history lives in the runtime repo.** Anything
  before commit `73e4705` was at `displayxr-runtime/src/xrt/drivers/leia/`.
  The runtime repo's `pre-leia-extraction-2026-05-04` tag preserves
  that state. For deep history, `git log` there, not here.

## License

[BSL-1.0](LICENSE) — same as all other DisplayXR repos.
