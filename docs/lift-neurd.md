# 2D→3D lift via NeurD (Windows, D3D11)

The Leia D3D11 display processor implements the runtime's **lift** slots — convert a
single 2D RGBA frame into depth, a side-by-side stereo pair, or an N-view grid — by
driving Leia's **NeurD** library. Source: `src/drv_leia/leia_lift_neurd.{h,cpp}`
(module) and the slot glue at the bottom of `src/drv_leia/leia_display_processor_d3d11.cpp`,
built against the real NeurD headers fetched at build time (see *Building*).

The runtime owns the lift *policy* — which content is lifted, the async worker thread,
the result mailbox/ring, and how the output reaches the compositor. The plug-in owns
only "turn this texture into that texture, now".

## Contract (runtime side)

Slots appended to `struct xrt_display_processor_d3d11` (runtime `xrt_dp_lift.h`,
ADR-042), announced by `XRT_DP_D3D11_HAS_LIFT` (runtime branch `feat/lift-ext`). The
plug-in fills the four texture-mode slots; the fifth, `lift_convert_blob`
(GAUSSIANS, photo → splats), is left NULL — NeurD has no splat path — so `modes` never
carries bit 8.

| Slot | Plug-in behaviour |
|---|---|
| `lift_get_caps` | Non-blocking. `modes` = DEPTH\|SBS\|NVIEW (1\|2\|4) once NeurD is present, else 0. `state` 0 unavailable / 1 activating / 2 ready. `max_streams` 32 (NeurD's process limit), `max_views` 8, `depth_semantics` 0 (relative), `backend` e.g. `neurd-directml`, `typical_latency_ns` = measured EMA (prior: 22 ms DirectML, 14 ms CUDA). |
| `lift_stream_create` | Non-blocking. Succeeds while NeurD is still activating — the NeurD stream is created lazily on the first convert. |
| `lift_stream_destroy` | Releases the stream's NeurD stream and bridge resources. |
| `lift_convert` | **Synchronous, blocking** (≈ bridge + inference). Returns an `ID3D11Texture2D*` on the caller's device — `R8G8B8A8_UNORM` for SBS/NVIEW, `R8_UNORM` for DEPTH — owned by the stream and valid until the next convert on that stream. Returns false while activating/unavailable. |

Every versioned struct is read only as far as its `struct_size` covers.
`xrt_dp_lift_params.focal_px` (appended) is ignored — it only matters to a
photo → Gaussians module, and `lift_convert_blob` stays NULL.

### The lift-only DP (`create_dp_d3d11_lift`)

The plug-in fills the optional `xrt_plugin_iface::create_dp_d3d11_lift` factory
(runtime header macro `XRT_PLUGIN_IFACE_HAS_D3D11_LIFT_FACTORY`). The runtime creates
exactly one lift DP per process on a dedicated device, calls only the `lift_*` slots on
it from its lift thread, and always passes explicit `viewpoints_xyz`. So this DP has
**no SR weaver, no window, no tracker, no lens control** — just the NeurD handle; every
non-lift slot is NULL. This is what keeps a lift session from perturbing the lens.
The weaving DP (`create_dp_d3d11`) still carries the lift slots too (they load nothing
unless called), which is what a runtime without the lift factory falls back to.

Output layouts (runtime contract): DEPTH = one channel at the inference resolution
(NeurD writes the normalised disparity replicated into RGB; the unpack keeps R);
SBS = 2 views side by side; NVIEW = `view_count` views side by side in **one row**,
view 0 leftmost. Tile size = the *inference* resolution (after autoscaling), not the
input size — use `out_w/out_h`. One row is why `max_views` is 8: 8 × 2560 (1440p) is
the widest that fits D3D11's 16384-texel limit; a wider request fails with a WARN.

## Process model

- **Dynamic load only.** `NeurD.dll` is found in NeurD's own loader order: `PATH` →
  `NEURD_PATH` (full path to the DLL) → `HKLM\SOFTWARE\LeiaInc\NeurD` (default value =
  install dir), loaded with `LOAD_WITH_ALTERED_SEARCH_PATH`. No import lib: the module
  resolves only the DLL's `NeurD_load` export and then calls through the returned
  function table using the **real** `NeurD.h`'s header-inline wrappers and PFN types,
  which version-gate every entry (an older runtime's table is shorter) — so any NeurD
  signature drift is a compile error, not a runtime mismatch.
- **Absent NeurD → nothing changes.** A presence probe (no `LoadLibrary`) runs on the
  first caps/stream call; if the DLL is not found the process state becomes
  *absent*, caps report `modes=0 / state=0`, and the plug-in behaves exactly as before.
- **One NeurD instance per process** (NeurD's rule). DP handles ref-count it. It is
  loaded and initialised on a **detached background thread**, never on a caller's
  thread and never at DP create (the DP factory runs on the service critical path).
- **Never de-initialised.** DPs are recreated on focus changes; re-init would re-run
  licensing and model load. When the last DP handle goes the plug-in only calls
  `NeurD_shrink_memory_pool`.

### Licensing / activation

`NeurD_init_with_options(multithreaded = TRUE)` runs NeurD's LexActivator licence
check. First activation on a machine needs the network (and WMI). Outcomes:

| NeurD result | Plug-in state | Caps `state` |
|---|---|---|
| `SUCCESS` | ready | 2 |
| `LICENSE_NETWORK_ERROR` / `NETWORK_ERROR` | retry-wait; re-attempted on the next caps/stream/convert call ≥ 15 s later | 1 (activating) |
| `INVALID_LICENSE`, model/filesystem errors, anything else | failed for the process | 0 |

Each transition logs one WARN (`Leia lift: ...`).

### Backend

`NeurD_set_backend` is called immediately before init; NeurD keeps a forced backend for
the process, so the **first** DP to activate NeurD decides it. The D3D11 path needs
NeurD's D3D11 device (`NeurD_get_dx_device`), which only the **DirectML** backend
provides — CUDA and OpenVINO return none, and lift then reports unavailable with a WARN
naming the fix. Hence the default is `directml`, not NeurD's own `auto`.

## Threading

- Every entry point may be called from any thread. `lift_convert` blocks for tens of
  ms: the runtime calls it only from its lift thread.
- NeurD properties (output type, tile grid, gain, convergence, inpaint, autoscale) are
  **process-global** inside NeurD, so one process-wide mutex makes *set properties →
  convert → stage output* atomic across streams. Properties are re-sent only when they
  change.
- The caller's immediate context must be `ID3D11Multithread`-protected (NeurD's and our
  own copies run on it from the lift thread while the compositor renders). The plug-in
  enables protection if it is off — the same mechanism the async weaver already relies
  on (`enable_context_multithread_protection`, `leia_sr_d3d11.cpp`). NeurD's own
  immediate context is protected too.

## Device bridge

NeurD runs on **its own** D3D11 device, possibly on a different adapter than the
runtime's service device on hybrid boxes. Its DX input must be a flat RGBA8
`ID3D11Buffer` (stride `w*4`) with `D3D11_RESOURCE_MISC_SHARED`, and it hands the
output back on the input buffer's device. Buffers do not share reliably across devices;
textures do. So (the LeiaMeet `DxStereoConverter` pattern):

```
caller device                         NeurD device
-------------                         ------------
input tex ──CopySubresourceRegion──▶ in_bridge (shared tex, created on NeurD's device)
          event-query drain (CPU)
                                      in_bridge SRV ──pack CS──▶ raw RGBA8 buffer ; drain
                                      NeurD_convert_stream_dx[_interactive]  (blocking)
                                      NeurD output ──unpack CS / copy──▶ out_bridge ; drain
out_bridge (opened) ◀───────────────── returned to the runtime
```

Each hop is CPU-drained with an event query because NeurD's DirectML work runs on its
own queue, unordered against any D3D11 context. Bridges are per stream, sized lazily,
and rebuilt on a size/format/device change. The adapter LUIDs of both devices are
logged once per stream (`same-GPU copy` vs `cross-adapter copy`); the bridge is used
either way. Accepted inputs: single-sampled `R8G8B8A8_*` or `B8G8R8A8_*` textures
(the pack shader reads through a typed view, so BGRA is swizzled for free and sRGB
bytes pass through encoded — what NeurD expects).

## Tracked eyes → NeurD viewpoints

NeurD's `*_interactive` converts take one `(x, y, z)` triplet per output view in
**dimensionless model units**: `x` horizontal disparity gain, `y` vertical offset, `z`
depth offset. Its default stereo pattern is `x = −0.5 / +0.5`, i.e. one unit of `x` is
one nominal eye baseline. The plug-in maps display-space positions in **metres** (the
same eyes `get_predicted_eye_positions` reports) with:

```
x_n = clamp(G · x_m / 0.063, ±3)
y_n = clamp(G · y_m / 0.063, ±3)
z_n = 0
G   = DXR_LEIA_LIFT_VIEW_GAIN (default 1.0)
```

A centred viewer at 63 mm IPD therefore reproduces NeurD's default pattern exactly
(G = 1), and head motion becomes look-around parallax. `G = 0` pins every view to the
centre; `G > 1` exaggerates. Head `z` is deliberately not mapped: NeurD does not specify
how its `z` relates to viewing distance, so mapping it would be a guess.

Viewpoint source, in order:

1. **Explicit viewpoints** from the runtime (always sent to the lift-only DP) (`viewpoints_xyz`, metres, display space):
   `view_count` triplets are used 1:1; exactly two are treated as an eye pair.
2. **Tracked eyes** from the SR eye path, when tracking is live (inter-eye distance
   > 1 mm — the same tracking-loss test as the eye slot).
3. **Untracked**: SBS uses NeurD's own default pattern (non-interactive convert); NVIEW
   uses `x = i − (N−1)/2`.

For N views from an eye pair, views are spaced one eye baseline apart and centred on
the eye midpoint. If NeurD predates the interactive API (< 0.4.5), SBS falls back to the
default pattern and NVIEW is refused.

## Parameters

| `xrt_dp_lift_params` | NeurD |
|---|---|
| `convergence < 0` | `AUTO_CONVERGENCE = TRUE` |
| `convergence` in [0, 1] | `AUTO_CONVERGENCE = FALSE`, `CONVERGENCE = clamp(K · (c − 0.5), ±0.2)`, K = `DXR_LEIA_LIFT_CONV_GAIN` |
| `strength` | `GAIN_MULTIPLIER` (1 = NeurD's calibrated budget, 0 = flat), clamped [0, 10]; negative → 1.0 |
| `inpaint` | NeurD always fills disocclusions: 0 → `V1_STRETCH` (cheapest), non-zero → `V1_BLUR` (the DX video path supports V1 only) |
| `view_count` | NVIEW row width (2..8) |

**Convergence calibration.** The runtime's `convergence` is the relative depth placed
at the display plane, normalised to [0, 1] over the frame's depth range (0 = nearest
content on the glass, 1 = farthest, 0.5 = middle). NeurD's `CONVERGENCE` is its own
disparity offset in [−0.2, 0.2]. The plug-in maps linearly about mid-range with gain
K (default 0.4, which spans NeurD's whole range). This is **uncalibrated**: on a panel,
submit c = 0 and check that the nearest content sits on the glass; if the far content
does instead, the sign is reversed — set `DXR_LEIA_LIFT_CONV_GAIN=-0.4`; then tune |K|
until c = 0 and c = 1 put the extremes on the glass. Auto-convergence (the default,
c < 0) is unaffected.

`xrt_dp_lift_stream_info.input_scale` in (0, 1] (1 = native) picks the smallest NeurD
autoscaling bucket (720p / 1080p / 1440p / none) that covers `input_scale × input
height`; outside that range the 720p default applies, and an explicitly set
`DXR_LEIA_LIFT_SCALE` overrides every stream. `content_hint` is recorded but
advisory: NeurD's DX stream path always runs its video model (photo mode is an
init-time, process-wide property).

Depth output is NeurD's **relative** disparity, min-max normalised per frame at the
inference resolution.

## Knobs

Read once per DP at create (`leia_lift_neurd_create`).

| Env | Default | Effect |
|---|---|---|
| `DXR_LEIA_LIFT` | on | `0` / `off` → caps `modes=0, state=0`; NeurD is never probed or loaded. |
| `DXR_LEIA_LIFT_BACKEND` | `directml` | `auto` \| `directml` \| `cuda` \| `openvino`. First activation in the process wins (NeurD's forced backend is sticky). Only DirectML yields a D3D11 device. |
| `DXR_LEIA_LIFT_SCALE` | unset (→ stream `input_scale`, else 720p) | Inference height bucket: `720` \| `1080` \| `1440` \| `none`. When set it overrides every stream's `input_scale`. NeurD's own default is 1440p; 720p is the fallback for latency. |
| `DXR_LEIA_LIFT_VIEW_GAIN` | `1.0` | `G` in the eye → viewpoint mapping above, [0, 10]. |
| `DXR_LEIA_LIFT_CONV_GAIN` | `0.4` | `K` in the convergence map above, [−2, 2]; negative flips the sign. Calibration knob. |

Under the service, remember these are read by `displayxr-service.exe`'s environment,
not the client's.

## Building

Lift compiles in only when BOTH of these hold; otherwise the four slots stay NULL, the
runtime reports lift unavailable, and the rest of the plug-in is unchanged. Neither
condition ever fails the plug-in build.

### 1. The NeurD headers (private)

`NeurD.h` and its closure (`_NeurD_detail.h`, `_NeurD_table.h`, and a generated
`NeurD_version.h`) are **Leia-private** and are never committed here. They are fetched
from the private **`LeiaInc/media_sdk`** repo at a pinned ref by
`scripts/fetch-neurd-headers.ps1` into the gitignored `NeurD-SDK-<ref>/include`,
exactly like the SR SDK:

| | |
|---|---|
| Pins | `NEURD_SDK_REF` + `NEURD_SDK_REPO` in `scripts/build-windows.bat` **and** `.github/workflows/build-windows.yml` (`jobs.Build.env`), kept equal by `scripts/check_sr_pins.py` (lint.yml). |
| Current pin | `v0.4.6` — the newest media_sdk release tag; its headers are byte-identical to `dev@25a713d93` (the tip this module was written against). Move it to a newer **release tag** when the module needs a newer NeurD API. |
| Auth | `gh` with read access to `LeiaInc/media_sdk` (`gh auth login`, or `GH_TOKEN`). CI uses `secrets.LEIALOFT_GITHUB_TOKEN`, the SR SDK token — that token must be granted read on `media_sdk`. |
| Failure | Soft. The .bat prints a WARN and builds without lift; CI emits a `::warning::` annotation (`continue-on-error`) and ships without lift. CMake prints `NeurD headers NOT found ... lift compiled OUT`. |
| Override | `set NEURD_SDK_ROOT=<dir with include\NeurD.h + NeurD_version.h>` (e.g. a local media_sdk drop) skips the fetch. |

`NeurD_version.h` is generated the way media_sdk's CMake does it, from
`sdk/NeurD_version.h.in` and the top-level `project(mediasdk VERSION x.y.z)` at the same
ref; it is the table version the module requests from `NeurD_load`.

### 2. Runtime headers with the lift slots

The slots compile only when the runtime headers define `XRT_DP_D3D11_HAS_LIFT`. The
Windows pin `DXR_RUNTIME_GIT_TAG` is currently `v2.16.9`, which predates it — a pinned
build ships the plug-in with the lift slots compiled out (it logs
`lift slots NOT COMPILED`), and the runtime sees them absent via `struct_size`.

To build with lift before the runtime tags it, use a local runtime checkout — nothing
is committed:

```bat
:: gh must be able to read LeiaInc/media_sdk (and the SR SDK repo, as always)
gh auth status
set DXR_RUNTIME_SOURCE_DIR=C:\dev\displayxr-runtime.wt-lift
scripts\build-windows.bat build
```

Confirm the configure output says `NeurD headers found ... enabling the D3D11 2D->3D lift
slots`, and at runtime the service log says `lift slots WIRED`. Set
`DXR_RUNTIME_SOURCE_DIR` explicitly: the script otherwise auto-detects a sibling
`displayxr-runtime` checkout, which may not carry the lift headers.

or `cmake -DDXR_RUNTIME_GIT_TAG=feat/lift-ext ...` for a fetched build. **Do not commit
a branch or SHA pin**: `installer/CMakeLists.txt` derives `MIN_RUNTIME_VERSION` from
`DXR_RUNTIME_GIT_TAG`, and the orchestrator/meta-installer assume a `vX.Y.Z` tag. Once
the runtime ships the lift contract in a tag, move `DXR_RUNTIME_GIT_TAG` (and the
workflow's runtime ref, which the rule-5 check keeps equal) to that tag in the plug-in
release that should carry lift. Because the slots are appended (ADR-020), that re-pin
raises the installer floor to the lift tag — the price of shipping the capability.

## Verifying

On a Windows box with NeurD installed (`HKLM\SOFTWARE\LeiaInc\NeurD` present) and the
lift-enabled runtime + this plug-in registered:

1. `displayxr-cli lift caps` — first call reports `state=activating` (the background
   worker is loading NeurD); repeat until `state=ready`, `modes=0x7`,
   `backend=neurd-directml`. The runtime log shows `Leia lift: loaded NeurD x.y.z from
   ...` and `Leia lift: NeurD READY`.
2. `displayxr-cli lift probe` — runs a stream create + convert + destroy. Check the log
   for the adapter-LUID line and no `Leia lift:` WARN after it; the probe's reported
   latency should sit near 22 ms (DirectML) at 720p.
3. Absent-NeurD check: `set DXR_LEIA_LIFT=0` (or rename the NeurD install key) →
   `lift caps` reports `modes=0 state=unavailable`, and a normal
   `cube_handle_d3d11_win` session weaves exactly as before.
4. Licence path: first activation offline → `state=activating` with a
   `LICENSE_NETWORK_ERROR` WARN; reconnect → ready within ~15 s of the next call.

## Known limits

- **Fallback path only:** against a runtime without `create_dp_d3d11_lift`, the runtime
  calls the ordinary factory with a NULL window, which builds an async SR weaver (it
  cannot be told apart from a NULL-HWND `_texture` DP). That extra weaver can briefly
  perturb the lens. The lift-only factory removes this on runtimes that have it.
- Windows / D3D11 only. The D3D12, GL and Vulkan DPs do not implement lift.
- The activation thread is detached; unloading the plug-in DLL while it runs
  (a first activation in progress) is unsafe. The runtime does not unload plug-ins
  mid-session today.
- NeurD's 32-stream limit is process-wide and shared with anything else in the
  service process that uses NeurD.
