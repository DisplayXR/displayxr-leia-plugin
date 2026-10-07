# CLAUDE.md

Guidance for Claude Code (claude.ai/code) when working in this repo.

## What this repo is

The **Leia SR display-processor plug-in** for the DisplayXR runtime.
Three platform arms, all implementing the `xrt_plugin_iface` ABI from
[`displayxr-runtime/src/xrt/include/xrt/xrt_plugin.h`](https://github.com/DisplayXR/displayxr-runtime/blob/main/src/xrt/include/xrt/xrt_plugin.h):

- **Windows** (`src/drv_leia`) → `DisplayXR-LeiaSR.dll`, SR SDK weavers
  (D3D11/D3D12/GL/VK). Loaded via registry discovery
  (`HKLM\Software\DisplayXR\DisplayProcessors\leia-sr`).
- **Android** (`src/drv_leia_android`) → `libdxrp050_leia_cnsdk.so`, CNSDK.
- **Linux desktop** (`src/drv_leia_linux`) → `DisplayXR-LeiaSR.so`. Two weaver
  backends behind the seam `leia_sr_linux.h`, whose interface is shaped by the
  [LeiaSR Linux SDK contract](docs/leia-linux-sdk-contract.md) (PROPOSED, #81):
  **Track B = the real srSDK Vulkan weaver** (`-DDXR_LEIA_LINUX_WEAVER=sdk`) and
  **Track A = a STUB weaver** (passthrough SBS blit, no SR SDK; still the
  default, for CI and SDK-less boxes). Track B **builds clean on Ubuntu 26.04
  against the installed `leiasr-runtime` .deb and passes `displayxr-cli
  selftest` with `leia-sr` active**; the weave itself is validated on
  22.04/NVIDIA (#81), with **on-panel weave validation on 26.04 still
  pending** — see [`docs/linux-track-b-runbook.md`](docs/linux-track-b-runbook.md).
  Discovery is JSON-manifest (`XRT_PLUGIN_SEARCH_PATH` / XDG
  `DisplayProcessors/` roots). The **stub** probe declines by default and
  `DXR_LEIA_FORCE_PROBE=1` force-binds it; on a real panel the SDK backend
  auto-binds via DRM/EDID and needs no env.

End-user artifact: `DisplayXRLeiaSRSetup-<version>.exe`, installed to
`%ProgramFiles%\DisplayXR\Plugins\LeiaSR\`. **No prerequisites** (runtime#1803):
it installs before or after the DisplayXR runtime and the LeiaSR platform; the only
gate is the ABI floor (exit 5) when a runtime IS installed. It releases the DLL from
the service / in-process apps with Windows Restart Manager (`dxr-rm-close.exe`) and
never starts the service elevated. Never run it (or its uninstaller) on a shared dev
box. Details: [`docs/installer.md`](docs/installer.md).

The plug-in itself loads WITHOUT the LeiaSR platform (SR client DLLs are `/DELAYLOAD`ed and
resolved from the SR install dir) and reports a platform state —
[`docs/install-order-and-platform-state.md`](docs/install-order-and-platform-state.md).
**No SR SDK call may run before `leia_sr_client_bind()` returned OK** — a missing delay-loaded
DLL faults at the call site.

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

- `CMakeLists.txt` pins `DXR_RUNTIME_GIT_TAG` (a `v*` runtime tag, or a runtime
  `main` SHA pre-release when tracking an ABI bump that hasn't tagged yet —
  re-pin to the tag at the coupled release). **Linux carries its own pin**
  (`DXR_RUNTIME_GIT_TAG_LINUX`) because the Windows tag can predate runtime
  Linux support; `build-linux.yml`'s rule-5 self-check keeps it equal to that
  workflow's `RUNTIME_REF`. **Android likewise**
  (`DXR_RUNTIME_GIT_TAG_ANDROID`, floor `v2.7.2`, kept equal to
  `build-android.yml`'s `RUNTIME_REF_ANDROID`) — it holds the same value as the
  Windows pin today; the point is that it can move independently without
  dragging the Windows installer's derived `MIN_RUNTIME_VERSION` with it.
- That ref's `xrt_plugin.h` defines `XRT_PLUGIN_API_VERSION_CURRENT`.
- `src/drv_leia/leia_plugin.c::xrtPluginNegotiate` reports that value.
- The runtime's loader (`target_plugin_loader.c`) **rejects** plug-ins reporting an ABI major different from the runtime's current ABI (ADR-020 rule 3).

**Appended DP slots are NOT an ABI major.** A new vtable slot (e.g. D3D11 slot
19 `set_frame_timing`, slot 20 `set_window` / displayxr-runtime#1008) is
compiled only when the runtime headers announce it (`XRT_DP_D3D11_HAS_*`), and
`struct_size` then tells the runtime the slot is filled — so the plug-in builds
against both old and new headers and old runtimes keep their fallback. What is
still required: re-pin `DXR_RUNTIME_GIT_TAG` to the runtime tag that carries the
slot, or a released plug-in silently ships without it.

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
| `src/drv_leia/leia_lift_neurd.{cpp,h}` | 2D→3D lift slots on the D3D11 DP, backed by a dynamically loaded NeurD.dll (absent ⇒ caps unavailable, nothing loaded). Compiles only with the private NeurD headers (fetched at build time, below) AND runtime headers with `XRT_DP_D3D11_HAS_LIFT`; otherwise the slots stay NULL. See [`docs/lift-neurd.md`](docs/lift-neurd.md). |
| `src/drv_leia/leia_sr_delayload_win.c`, `leia_platform_state.{c,h}` | SR client DLL delay-load hook (registry-resolved full paths) + `leia_sr_client_bind()` gate; the platform state machine (READY / PLATFORM_ABSENT / PLATFORM_NOT_RUNNING / NO_DISPLAY / INCOMPATIBLE) behind `probe()`, `probe_displays()` and the runtime's `get_platform_state` slot. |
| `src/drv_leia/leia_stereo_camera.{h,cpp}` | Stereo camera source (runtime ADR-043 / `XR_DXR_stereo_camera`, phase L1): the SR eye tracker's camera read from the SR raw-camera shared memory by POLLING (never the auto-reset event), calibration of the ACTIVE device's serial (`Global\sharedDeviceSerialMemory[0]` → `%ProgramData%\Simulated Reality\Devices\<serial>`), SR v2 eye-tracker keep-alive while open (it PREDICTS itself to count eye pairs — the v2 callback only echoes predict). Advertises the MEASURED source rate, 0 = unknown. Compiled only when the runtime headers define `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA`. Platform-neutral half in `leia_stereo_camera_parse.{h,c}`, host-tested by `tests/test_stereo_camera_parse.c` (Linux CI). |
| `src/drv_leia/leia_edid_probe.c` | EDID-based hardware detection — answers "is a Leia display attached?" before the SR SDK initializes. |
| `src/drv_leia_linux/leia_sr_linux.h` | **Linux weaver-backend seam** — interface shaped 1:1 by `docs/leia-linux-sdk-contract.md` (every declaration cites its R-* requirement). Track B implements it against the real SDK. |
| `src/drv_leia_linux/leia_sr_stub.c` | Track A stub backend: canned panel info + passthrough SBS blit, `TODO(Track B)` at every body. |
| `src/drv_leia_linux/leia_sr_linux_sdk.c` | Track B backend: the real srSDK (C99, API 1.0.0) behind the same seam — instance/display/lens/weaver + event latching. Selected by `-DDXR_LEIA_LINUX_WEAVER=sdk`. |
| `src/drv_leia_linux/leia_plugin_linux.c` | Linux `xrtPluginNegotiate` + iface (VK-only factories; env-gated probe). |
| `src/drv_leia_linux/leia_display_claims_linux.{c,h}` | Linux `probe_displays` matching (multi-screen M0): runtime monitors × EDID panel list × SR display enumeration → per-monitor claims + private `displayId` table. Pure logic, unit-tested. |
| `src/drv_leia_linux/leia_screen_linux.{c,h}` | Per-DP screen state (multi-screen M4): a `create_dp_vk_for_screen` binding + EDID list + SR query → the `struct leia_lnx_screen` one DP owns and answers its display getters from. Pure logic, unit-tested. |
| `src/drv_leia_linux/leia_sr_routing_linux.h`, `leia_sr_chain_sdk_linux.h` | SR weaver create chain (multi-screen M4): the plan (EXTERNAL routing + display binding, gated on the SR runtime's caps; pure) and its SR-typed `pNext` builder. Both unit-tested; the chain half only against SDK headers that carry the structs. |
| `src/drv_leia_linux/leia_display_processor_linux.c` | Linux VK DP — 1×1 grid blits, multi-view goes through the seam. Reuses `../drv_leia/leia_device.c`. |
| `installer/DisplayXRLeiaSRInstaller.nsi` | NSIS installer. Drops DLL at `C:\Program Files\DisplayXR\Plugins\LeiaSR\`; writes registry entry `HKLM\Software\DisplayXR\DisplayProcessors\leia-sr` with values `Binary`, `ProbeOrder`, `Version`, `DisplayName`, `Vendor`, `UninstallString`. No prerequisites; see `docs/installer.md`. |
| `installer/rm-helper/` | `dxr-rm-close.exe` — generic Restart Manager driver the installer/uninstaller use to close and restart whatever maps the plug-in's files (one `session` process spans both phases). `rm_test_holder.cpp` is a test-only holder (EXCLUDE_FROM_ALL). |
| `scripts/build-windows.bat` | Local Windows build entry point. |
| `docs/` | Leia implementation internals (weaver, transparency, chroma-key, phase snapping, mode switching) — migrated from the runtime's `docs/vendors/leia/`. Start at `docs/README.md`. |

## Build commands

### Windows (canonical)
```bat
scripts\build-windows.bat all        REM full build (configure + plugin + installer)
scripts\build-windows.bat build      REM plug-in DLL only
scripts\build-windows.bat installer  REM NSIS installer (requires DLL built)
```

Requires VS 2022 + Ninja + Vulkan SDK + GitHub CLI. **The SR SDK bits
(SDK zips, the Vulkan weaver rescue DLLs, the Linux SDK) are Leia SDK
material and live ONLY on the private
`LeiaInc/SR-SDK-Windows-Releases-Internal-Public` release tags
(`sr-sdk-v*`) — never re-host them on this public repo** (2026-09-03
incident: an outside developer found the Linux SDK on a public release
here). `gh auth status` must show an account that can read that repo; CI
reads it with the `LEIALOFT_GITHUB_TOKEN` secret (same one the Android arm
uses for `LeiaInc/CNSDK`), so fork PRs cannot build the Windows/Linux arms.
On first run the script pulls the SR SDK at `SR_TAG` / `SR_VKSTAMP_TAG` /
`SR_V2_TAG` (set in `scripts/build-windows.bat`; keep in sync with
`build-windows.yml`). CI enforces that sync: `scripts/check_sr_pins.py`,
run by `.github/workflows/lint.yml` on every PR, asserts all five pins
(`SR_TAG`, `SR_VKSTAMP_TAG`, `SR_V2_TAG`, `SR_V2_DIR`, `SR_SDK_REPO`) are
declared exactly once and identically in both files, and that
`SR_V2_TAG`/`SR_V2_DIR` name the same SR v2 build.

**NeurD headers (2D→3D lift) follow the same rule.** They are Leia-private
and come from the private **`LeiaInc/media_sdk`** repo at `NEURD_SDK_REF`
(pinned next to the SR pins in both files, also checked by
`check_sr_pins.py`), fetched by `scripts/fetch-neurd-headers.ps1` into the
gitignored `NeurD-SDK-<ref>/`. **Never commit or mirror them.** Your `gh`
account (and CI's `LEIALOFT_GITHUB_TOKEN`) needs read access to
`media_sdk`; without it the fetch fails SOFT and the plug-in builds without
lift. Details: [`docs/lift-neurd.md`](docs/lift-neurd.md).

### Linux
```bash
./scripts/build-linux.sh            # Track A stub: .so + displayxr-cli, stage manifest, selftest
./scripts/build-linux.sh --no-test  # build + stage only

# Track B (real srSDK weaver) on a box with the leiasr-runtime .deb installed:
SRSDK_ROOT=/opt/leiasr cmake -S . -B build -G Ninja \
    -DDXR_RUNTIME_SOURCE_DIR=$(pwd)/../displayxr-runtime \
    -DDXR_LEIA_LINUX_WEAVER=sdk
```
Needs a local runtime checkout (default `../displayxr-runtime`, or set
`DXR_RUNTIME_SOURCE_DIR`). Deps = the apt list in
`.github/workflows/build-linux.yml`. CI builds on Ubuntu 22.04/24.04/26.04
containers and asserts single-export + discovery + ABI-green selftest.

**The `.deb` is built on the OLDEST supported release (`ubuntu:22.04`), never
on the runner's default.** A package's glibc floor is its build host's, and
package names are not stable across releases — v2.7.0 was built on 24.04, so it
needed `GLIBC_2.38` behind an unversioned `libc6` and named
`libpipewire-0.3-0t64`, which does not exist on jammy. `package_deb_leia.sh`
now derives versioned `Depends` with `dpkg-shlibdeps`, fails on a DT_NEEDED
outside its cross-release `STABLE_SONAMES` list, and honours
`DXR_DEB_MAX_GLIBC`; CI's `DebInstall` matrix installs and runs the package in
pristine 22.04/24.04/26.04 containers. Same shape as the runtime's fix
(displayxr-runtime #1656). Details:
[`docs/linux-deb-packaging.md`](docs/linux-deb-packaging.md).

**Build against headers whose `struct vk_bundle` matches the runtime you will
load into.** The runtime's loader compares `vk_bundle_abi_size` exactly and
refuses the VK DP on a mismatch (the session runs unwoven), even though
`XRT_PLUGIN_API_VERSION_CURRENT` is 5 throughout and
`scripts/check_plugin_abi.py` does not model that struct fingerprint.
`vk_bundle` changed layout once in this range — at runtime v2.16.0 — and is
identical from v2.16.0 through `main`. The Linux pin is now `v2.17.1`, so a
tag-pinned build loads into any runtime v2.16.0..main; the old `v2.14.6` pin
produced a plug-in 8 bytes short that every runtime >= v2.16.0 refused, which
is why a local runtime checkout used to be mandatory. Building against the
exact checkout you load into is still the safest habit.

The installed **`leiasr-runtime` .deb is the SDK dev package** (headers under
`/opt/leiasr/include/sr/`, `libsrSDK_loader.a`, `lib/cmake/srSDK/`, and it
registers `/etc/leia/sr/1/active_runtime.json`). There is no separate SDK
download for Linux. Full recipe + bring-up gotchas:
[`docs/linux-track-b-runbook.md`](docs/linux-track-b-runbook.md).

### Android (CNSDK)
```bash
export CNSDK_ROOT=/path/to/cnsdk     # extracted CNSDK 0.10.54+ android tree
./scripts/build-android.sh                          # -> libdxrp050_leia_cnsdk.so
./scripts/build-android.sh install-runtime-jnilibs  # + drop into the runtime APK
```
Plain CMake + NDK (no gradle here), arm64-v8a only. Needs an NDK
(`ANDROID_NDK_HOME`, or an SDK containing `ndk/<ANDROID_NDK_VERSION>/`) and a
local runtime checkout. **CNSDK is a build-time dependency only** — the plug-in
links the loader shim `libleiaCore-loader.so`, which at runtime `dlopen`s
`libleiaCore-impl.so` out of the *on-device* package, exactly as the Windows arm
build-depends on the SR SDK while the SR runtime is installed separately. Fetch
it with `gh release download <tag> -R LeiaInc/CNSDK -p 'cnsdk-android-*.zip'`;
the public `leiainc.github.io` copy is 0.7.28 and no longer works.

CI (`build-android.yml`) builds it on every PR and attaches it to `v*` releases.
It asserts single-export, the exact DT_NEEDED closure, and — because an
under-pinned runtime *compiles fine* and only fails on device — an ABI witness
string read back out of the `.so`.

**Never publish CNSDK material.** Leia's Creator Toolkit licence permits
distribution "as incorporated into your Products" (§3) but forbids distributing
it standalone (§4b), so releases carry only our own `libdxrp050_leia_cnsdk.so`.

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
- **The Linux weaver-backend interface tracks a PROPOSED contract.**
  `docs/leia-linux-sdk-contract.md` is awaiting ratification by the SDK
  team; if it shifts, realign `src/drv_leia_linux/leia_sr_linux.h` (and the
  stub) in the same change — the two must never drift.
- **Registry registration is at install time.** During dev,
  installer not run, the DLL won't load. On Windows discovery is
  registry-only (`XRT_PLUGIN_SEARCH_PATH` is POSIX-only): register the
  dev DLL with the runtime's `scripts\register_dev_plugin.bat leia <dll>`
  (elevated).
- **The pre-extraction history lives in the runtime repo.** Anything
  before commit `73e4705` was at `displayxr-runtime/src/xrt/drivers/leia/`.
  The runtime repo's `pre-leia-extraction-2026-05-04` tag preserves
  that state. For deep history, `git log` there, not here.

## License

[Apache-2.0](LICENSE) — same as the other wholly-owned DisplayXR repos (the runtime stays BSL-1.0 as a Monado-aligned fork).
