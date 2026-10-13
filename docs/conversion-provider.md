# Leia conversion provider (`DisplayXR-LeiaConversion.dll`)

The Leia **conversion provider** serves the runtime's 2D→3D conversion tasks — depth from an
image or a video, and a stereo pair from one image — through Leia's NeurD module. It is the
third DisplayXR plug-in type (runtime [ADR-053](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/adr/ADR-053-conversion-providers.md)),
next to the display processor this repo has always shipped, and it replaces the runtime's
legacy `dp-lift` adapter over the display processor's lift slots ([2D→3D lift via NeurD](lift-neurd.md)).

Contract (runtime repo): header `src/xrt/include/xrt/xrt_conversion_plugin.h` (ABI 1, runtime
≥ v2.41.0), [discovery spec](https://github.com/DisplayXR/displayxr-runtime/blob/main/docs/specs/runtime/conversion-provider-discovery.md)
§2 (C ABI, threading, hosting rules), §3 (registration) and §3.8 (installer contract), and the
vendor onboarding guide §9.

## Shape: a separate DLL

| | Display processor | Conversion provider |
|---|---|---|
| Binary | `DisplayXR-LeiaSR.dll` | `DisplayXR-LeiaConversion.dll` |
| Export | `xrtPluginNegotiate` | `xrtConversionPluginNegotiate` (the only one; CI asserts it) |
| Registry row | `DisplayProcessors\leia-sr` | `ConversionProviders\leia-conversion` |
| Loaded by | the service and in-process apps | `displayxr-conversion-host.exe` (the satellite), only |
| Needs LeiaSR | delay-loaded, reports its platform state | **never**: no SR SDK import, no weaver, no SR session |

Separate DLLs are the recommended shape (discovery spec §3.7): a combined DLL would keep NeurD
mapped in the service through its display half, which is exactly the file lock satellite
hosting removes. The new DLL does **not** link or load `DisplayXR-LeiaSR.dll`; it compiles the
same module sources (`src/drv_leia/leia_lift_neurd.cpp`, `leia_lift_viewpoint.c`,
`leia_lift_depth.c`), so both use one NeurD search order, one version floor and one convert
path. Source: `src/conv_leia/`.

| File | What |
|---|---|
| `leia_conv_plugin.cpp` | the negotiate entry point and the v1 iface |
| `leia_conv_d3d11.{h,cpp}` | the provider's own D3D11 device and its image import (NT handle / CPU), sync, and the output blit |
| `leia_conv_policy.{h,c}` | pure decisions, host-tested: the readiness gate, the licence class, output extents, convergence units |
| `tests/` | the policy test (every lane) and the provider test against fake modules (Windows) |

## Capabilities

| `cap_id` | Task | Lane | Outputs | Notes |
|---|---|---|---|---|
| 1 | `DEPTH_MONO` | stream | depth | per-frame normalised disparity |
| 2 | `DEPTH_VIDEO` | stream | depth | `TEMPORAL`; `FRAME_RESET` drops the module stream (fresh temporal state) |
| 3 | `VIEWS_MONO` | stream | views (+ depth) | exactly 2 views (`min_views = max_views = 2`), `EXPLICIT_VIEWPOINTS`, `OFFAXIS_CAMERA` when the module has the metric off-axis camera; aux depth of the **same inference** iff the module reports it |

- **Why 2 views.** NeurD's public output types fix the tile grid ([N-view](lift-neurd.md#n-view)). A
  stream for more views is served by the runtime's composed pipeline (a depth capability + its
  reference view synthesis), never by this capability.
- **Depth.** A depth target is `R32_FLOAT`. `DEPTH_*` write the module's per-frame min-max
  normalised disparity as `AFFINE_INVERSE` (decoded value = disparity, larger = nearer).
  `VIEWS_MONO`'s aux depth keeps the module's own units: `METRIC` (metres) with
  `VideoModel=metric`, else `AFFINE_INVERSE` — mapped field by field exactly as the runtime's
  `dp-lift` adapter maps the display processor's `lift_get_depth`, including the relief mapping
  (ADR-046 A1, `relief_*`) of an off-axis convert and the assumed pinhole (rescaled to the target).
- **Per-request geometry in, relief out.** Display metres, resolved viewpoints, the axis mode,
  clamp and nominal distance arrive on every call in `xrt_conv_params`; the provider reads no
  tracker and no panel geometry (it has no SR session). The runtime's convergence comes as a
  plane in the decoded units of the stream's last depth; the provider maps it back to the
  module's terms (relative: `1 − value`; metric: metres, converted with the stream's own model
  scale) and otherwise lets the module run its own auto-convergence.
- **Extents** (`stream_get_output_desc`, pure): views = the input size per view (scaled down to fit
  16384 texels in a row); depth = the input size with its long edge capped at 1280 (aux depth:
  1024). The module's output is resampled into the runtime-allocated target.
- **Transport.** Every image is runtime-allocated and imported, never exported: an NT handle opened
  on the provider's own D3D11 device (created on the adapter `get_adapter_hint` names, else next to
  NeurD's device), or CPU memory. Sync: `NONE`, `KEYED_MUTEX`, `FENCE_NT`. Imports are cached by
  `import_key` and dropped when unseen for 64 calls or at `stream_destroy`.
- **No job lane** in this version (no capability declares it; `job_execute` refuses).

## Readiness and the version gate

The three capabilities share one module, so they share one state. Until the first `warm`, the
state comes from **discovery only** (no load): the module's file, found in NeurD's own order
(PATH → `NEURD_PATH` → `HKLM\SOFTWARE\LeiaInc\NeurD`, or only the `NeurDPath` setting when set),
and its version resource.

| Situation | State | `failure_reason` | `retryable` |
|---|---|---|---|
| no module found | `ABSENT` | `MODEL` | yes — installed later, the next warm finds it |
| module found, **older than the floor** (`MinVersion`, default 0.4.6) | **`FAILED`** | **`VERSION`** (never `MODEL`) | yes — an update replaces the file |
| module found, cold | `INSTALLED` | | |
| `warm` running (load, licence, model) | `WARMING` | | |
| ready | `READY` | | |
| licence activation needs the network | `FAILED` | `NETWORK` | yes (the runtime's backoff retries) |
| licence refused | `UNLICENSED` | `LICENCE` | no |
| found but unloadable / no entry point | `FAILED` | `LOAD` | yes when a new file can fix it |
| `Enable=0` | `DISABLED` | | |
| the provider's D3D11 device was lost | `FAILED` | `DEVICE_LOST` | yes (re-warm re-creates it) |

A module refused as too old is refused from its version resource **before it is loaded**, so an
update can replace it under a running host. `get_provider_status` reports the same discovery as
the ADR-045 platform state: `PLATFORM_ABSENT` (no module), `INCOMPATIBLE` (too old), else `READY`.

`warm` runs the module's discovery, load and initialisation **on the runtime's thread** (no
activation thread of its own) and then creates the provider's device. NeurD's initialisation (the
licence check, the model load — seconds) cannot be interrupted; `is_cancelled` is honoured before
and after it. `evict` is a no-op (NeurD is never de-initialised: a reload re-runs licensing).

## Licence class

From the module's **own** words, never invented: its licence line once it has printed one, else
its file's product name. A module that calls itself internal / non-commercial / research /
evaluation is `RESEARCH_ONLY`; one that says commercial is `COMMERCIAL`; otherwise `UNKNOWN`.
The runtime selects `RESEARCH_ONLY` and `UNKNOWN` only with its `AllowResearchOnly` opt-in, which
signed DisplayXR installers never write (ADR-053 decision 2 (e)). An internal build of the module
therefore needs that opt-in on the box; the module's own installer may set it for an internal
build. `licence_spdx` is `LicenseRef-LeiaInc`.

## Settings

Only through `host->get_setting` — never this process's environment, never
`HKLM\SOFTWARE\DisplayXR\Leia\Lift` (that key stays the display processor's). The host resolves
`DXR_CONV_LEIA_CONVERSION_<KEY>` in its environment (the service's), else
`HKLM\Software\DisplayXR\ConversionProviders\leia-conversion\Settings\<KEY>`.

| Key | Meaning |
|---|---|
| `Enable` | `0` turns the provider off (`DISABLED`) |
| `NeurDPath` | absolute path of the module DLL; when set it is the **only** place looked at |
| `MinVersion`, `Backend`, `VideoModel`, `InteractiveMin`, `Scale` | as in [lift-neurd.md § Knobs](lift-neurd.md#knobs) (process-wide; the first warm in a host decides) |
| `ViewGain`, `YGain`, `ZGain`, `ConvGain`, `DepthGain`, `Dilate`, `OffAxis`, `ReliefDepthM`, `MetricReliefMaxM` | as in [lift-neurd.md § Knobs](lift-neurd.md#knobs) (snapshotted per stream) |

## Registration

The plug-in installer writes, in the 64-bit view:

| `HKLM\Software\DisplayXR\ConversionProviders\leia-conversion` | |
|---|---|
| `Binary` | `<install dir>\DisplayXR-LeiaConversion.dll` |
| `DisplayName` | `Leia conversion (NeurD)` |
| `Vendor` | `Leia Inc.` |
| `ProbeOrder` | `50` (the vendor band) |
| `Supersedes` | `dp-lift` — hides the legacy adapter over this plug-in's own lift slots, so the two are never listed twice; it never affects selection among other providers, so an absent module still falls through |
| `UninstallString` | the plug-in uninstaller |
| `Version` | written **last** (removed first on an update): the "row complete" marker |

`Hosting` is not written (the runtime default, satellite). A `Settings` subkey the user created
survives an update; the uninstaller removes the whole row.

## Installer contract: close the host, never the service

The NSIS installer (`installer/DisplayXRLeiaSRInstaller.nsi`, built with `LEIA_CONV` when the DLL
was built) follows the discovery spec §3.8 sequence for the provider's files:

1. Create **and own** `Global\DisplayXR-ConvMaintenance-leia-conversion`, held to the end (wait up
   to 120 s if another installer — NeurD's own — holds it). While it is held the service spawns
   no conversion host for `leia-conversion`.
2. A Restart Manager session (`dxr-rm-close.exe session`) over the provider's DLL **only**: it
   lists `displayxr-conversion-host.exe`, which exits 20 ("closed for update"); the provider's
   tasks report `WARMING(updating)` and every app session and stream stays alive. No `taskkill`
   fallback: nothing in this session may stop the service.
3. Then the display processor's own Restart Manager session (unchanged: its DLL is mapped by the
   service, so that session closes and restores the service as before).
4. Copy the files; write the `ConversionProviders` row with `Version` last.
5. End the provider's session restarting **nothing** (the host never registers for restart; the
   service respawns it on demand), release the mutex, then let the display session restore the
   service.

Uninstall runs the same sequence, deleting the row first. An aborted install releases the mutex
and ends both sessions. The display half still closes the service on every plug-in update —
that is the display DLL's cost, not the provider's.

**The NeurD installer** (a separate, internal installer) should take the same mutex and run its
Restart Manager session over the NeurD files; with the provider satellite-hosted, that session
lists the conversion host, not the service. Until the runtime stops loading NeurD in the service
altogether (the display processor's lift slots stay compiled for older runtimes), the service may
still appear there on a runtime that routes lift through `dp-lift` — that is runtime roadmap
§11.2's open must-verify item.

## Building and testing

The DLL needs the runtime headers with `xrt_conversion_plugin.h` (the Windows pin
`DXR_RUNTIME_GIT_TAG` ≥ v2.41.0) and the private NeurD headers (`NEURD_SDK_ROOT`, fetched by
`scripts\build-windows.bat` / CI, see [lift-neurd.md § Building](lift-neurd.md#building)). Without
the NeurD headers the DLL is skipped (CMake says so) and nothing else changes.

```bat
scripts\build-windows.bat build
ctest --test-dir build\src\conv_leia -C Release --output-on-failure
```

| Test | What it pins |
|---|---|
| `test_conv_leia_policy` | the readiness gate (too old → `FAILED(VERSION)`, missing → `ABSENT(MODEL)`, network / licence / load / device-loss rows), the licence class from the module's words, extents, decoded-convergence mapping, depth kinds. Pure C, every lane |
| `test_conv_leia_provider_{missing,old,noentry,disabled}` | the provider's entry point linked statically, a fake host (settings only): negotiate (another major refused), every iface slot present, the describe_cap contract (count, well-formed capabilities, index past the end untouched, writes only within `struct_size`, one generation per walk), `get_state` truncation, no stream before `READY` — and the gate on fake modules: no module → `ABSENT(MODEL)` + `PLATFORM_ABSENT`; a module older than the floor → `FAILED(VERSION)` at probe **and** after `warm`, `INCOMPATIBLE`, **never mapped**; a new-enough module without the entry point → `INSTALLED`, then `FAILED(LOAD)` with a pushed `notify_changed`; `Enable=0` → `DISABLED`; a cancelled `warm` changes nothing |

CI (`build-windows.yml`) also asserts the DLL's export set (exactly `xrtConversionPluginNegotiate`)
and that it imports neither the SR SDK, the display DLL nor the NeurD module.

On a box with the runtime: register the dev DLL with the runtime's
`scripts\register_dev_plugin.bat conv <dll>` (elevated), then `displayxr-cli conv list` /
`conv status` and `displayxr-cli lift probe`. The conversion host logs to
`%LOCALAPPDATA%\DisplayXR\DisplayXR_conversion-host_leia-conversion.<pid>_<ts>.log`; this
provider's lines start `Leia conversion:` and the module's `Leia lift:` / `NeurD:`.
