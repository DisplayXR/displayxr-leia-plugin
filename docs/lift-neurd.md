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
| `lift_get_caps` | Non-blocking. `modes` = DEPTH\|SBS (1\|2) once NeurD is present, else 0. **Never NVIEW**: not available on NeurD ≤ 0.4.6 (public output types fix the tile grid); SBS with explicit viewpoints only. `state` 0 unavailable / 1 activating / 2 ready. `max_streams` 32 (NeurD's process limit), `max_views` 8, `depth_semantics` 0 (relative), `backend` e.g. `neurd-directml`, `typical_latency_ns` = measured EMA (prior: 22 ms DirectML, 14 ms CUDA). |
| `lift_stream_create` | Non-blocking. Re-probes NeurD first (see *Absent is not permanent*). Succeeds while NeurD is still activating — the NeurD stream is created lazily on the first convert. Fails soft (returns false, latches nothing in the plug-in) while unavailable. The runtime (`d3d11_lift.cpp`) calls it only once caps say READY, and a false there marks that runtime stream permanently failed — which is why caps report READY only when the module is READY. |
| `lift_stream_destroy` | Releases the stream's NeurD stream and bridge resources. |
| `lift_convert` | **Synchronous, blocking** (≈ bridge + inference). Returns an `ID3D11Texture2D*` on the caller's device — `R8G8B8A8_UNORM` for SBS, `R8_UNORM` for DEPTH (polarity flipped at the bridge, in the R8 unpack, from NeurD's near = high disparity to the spec's RELATIVE larger = farther) — owned by the stream and valid until the next convert on that stream. Returns false while activating/unavailable. |

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
SBS = 2 views side by side. (NVIEW — `view_count` views in one row — is not offered
by this module; see *N-view* below.) Every convert checks NeurD's actual layout
(output aspect ÷ input aspect = tile count) against the mode and drops a frame that
doesn't match, so a result can never be mislabelled. Tile size = the *inference* resolution (after autoscaling), not the
input size — use `out_w/out_h`. One row is why `max_views` is 8: 8 × 2560 (1440p) is
the widest that fits D3D11's 16384-texel limit; a wider request fails with a WARN.

## Process model

**Supported NeurD versions.** By default the plug-in accepts **NeurD 0.4.6 or newer** —
the only release tested on hardware. An older `NeurD.dll` is refused from its version
resource before it is loaded (or, lacking one, right after it is loaded), before backend
selection, init, licensing or any model load: one WARN names the
found version and the minimum, and lift reports unavailable (`modes=0 / state=0`) exactly
as if NeurD were absent, so callers fall back to their own conversion path. (Older NeurD
would otherwise *look* available and then fail or degrade: 0.3.11–0.4.2 need the CUDA
backend for the D3D path, 0.4.3–0.4.4 have no head-tracked viewpoints, 0.4.5 lacks the
metric video model.) The floor is the `MinVersion` knob (`DXR_LEIA_LIFT_MIN_VERSION`, see
*Knobs*); lower it only for demos and testing. 0.3.11 remains the hard floor of the code
(see *NeurD 0.3.x*).

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
- **Absent is not permanent (runtime#1803 P-d).** While absent, discovery re-runs at most
  every **30 s** from `lift_get_caps` (the runtime polls caps ~1 Hz while lift is not
  READY, so that poll is the slow timer — no extra thread) and **immediately on every
  `lift_stream_create`**. The registry step is what finds a NeurD installed after the
  service started (the service's `PATH` is a snapshot from its start). When NeurD appears:
  one WARN `Leia lift: NeurD.dll appeared at <path> (<where>) — loading`, then the normal
  LOADING → READY path. Each re-probe is a `SearchPath`, one registry read and a file
  attribute query; nothing is loaded and nothing is logged while nothing changed.
- **A NeurD refused for a file reason is re-checked when the file changes.** "Older than
  `MinVersion`", "LoadLibrary failed" and "no usable `NeurD_load`/stream API" store the
  DLL's path, size and last-write time; the same 30 s / stream-request re-probe retries only
  when NeurD.dll now resolves to another path or its size/mtime differ (one WARN `NeurD.dll
  changed since it was refused (...) — retrying`). The version floor is checked from the
  DLL's **VERSIONINFO resource before it is loaded** (NeurD.dll carries `FileVersion`,
  e.g. 0.4.6), so a too-old NeurD is never mapped and its installer can replace it under a
  running service; a DLL without the resource is checked after the load as before (and then
  stays mapped — a replacement at the same path is reported as "still mapped, restart the
  process"). Licence, device and init failures stay permanent for the process.
- **One NeurD instance per process** (NeurD's rule). DP handles ref-count it. It is
  loaded and initialised on a **detached background thread**, never on a caller's
  thread and never at DP create (the DP factory runs on the service critical path).
- **Never de-initialised.** DPs are recreated on focus changes; re-init would re-run
  licensing and model load. When the last DP handle goes the plug-in only calls
  `NeurD_shrink_memory_pool`.
- **Consequence for NeurD's installer.** Once anything has used lift, `NeurD.dll` and its
  dependencies stay mapped in the long-lived `displayxr-service.exe` until that process
  exits, so their files cannot be replaced while it runs. Unloading NeurD when idle was
  tried (v2.8.2) and withdrawn: a reload costs 8-10 s of unconverted frames on the next
  conversion, and OpenSSL pins `libcrypto-3-x64.dll` in the process regardless. The
  installer is the place to handle it: close the processes holding its DLLs
  (`displayxr-service.exe`), replace the files, then start the service again
  **non-elevated** (e.g. `explorer.exe "<Runtime>\displayxr-service.exe"` from an elevated
  installer) — a service started elevated cannot be reached by normal-integrity apps.

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
the process, so the **first** DP to activate NeurD decides it. On NeurD 0.4.3+ the D3D11
path needs NeurD's D3D11 device (`NeurD_get_dx_device`), which only the **DirectML**
backend provides — CUDA and OpenVINO return none, and lift then reports unavailable with
a WARN naming the fix. Hence the default is `directml`, not NeurD's own `auto`. NeurD
0.3.x is the other way round — see below.

### NeurD 0.3.x

**Refused by default** — like every NeurD before 0.4.6 (see *Supported NeurD versions*).
This section describes what happens only when `MinVersion` is lowered to admit it.

The code can drive NeurD from **0.3.11** (the first release with the stream API: `create_stream`,
`convert_stream_dx`, `set_prop_1i/1f`). The function table is append-only, so a plug-in
built against the 0.4.x header loads a 0.3.x `NeurD.dll` unchanged, and every newer entry
is version-gated (`LEIA_NEURD_HAS`).

- **`NeurD_get_dx_device` is optional** (added in 0.4.3). Without it the plug-in creates
  its own D3D11 device on the **default adapter** (as NeurD's own example does), builds
  the bridge on it, and releases it itself. 0.3.x's DX convert takes the device from the
  input buffer (`GetDevice`) and returns its output as an `R8G8B8A8_UNORM` texture on that
  same device, so the bridge is otherwise unchanged. One WARN names the case and the LUID.
- **CUDA backend required for the DX path on 0.3.x.** Its DX convert is implemented on
  CUDA↔D3D11 interop; the DirectML build's DX path is not verified. Set
  backend `cuda` — `DXR_LEIA_LIFT_BACKEND=cuda` in the **service's** environment, or
  better the `Backend` registry value (see *Knobs*), which survives service respawns. The default adapter must be the NVIDIA GPU for
  the interop to bind — on a hybrid box, pin the service to the dGPU.
- **SBS and DEPTH only** (as on every NeurD today — see *N-view*); without interactive
  convert (0.4.5+) tracked-eye viewpoints are ignored (SBS uses NeurD's default pattern).
  Because 0.3.x hands back a texture, DEPTH arrives as `R8G8B8A8_UNORM` (copied as-is),
  not `R8_UNORM`; `lift_convert`'s `out_format` reports which. That texture path is **not**
  polarity-flipped: DEPTH stays NeurD's near = high, contrary to RELATIVE (known gap).
- **Default models only** — `init_with_options` (model selection) is newer; `NeurD_init`
  is used instead.

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
depth offset. Its default stereo pattern is `x = −0.5 / +0.5`, i.e. **one unit of `x` is
one nominal (63 mm) eye baseline**. The mapping lives in `src/drv_leia/leia_lift_viewpoint.{h,c}` (pure C,
host-tested by `tests/test_lift_viewpoint.c` on the Linux CI lane) and has two contracts.

### Viewpoint policy (runtime ADR-048, `XRT_DP_LIFT_HAS_VIEWPOINT_POLICY`)

A runtime whose `xrt_dp_lift_params::struct_size` covers the appended policy block
(`rect_width_m`, `rect_height_m`, `baseline_m`, `axis_mode`, `max_offset_m`,
`viewpoint_frame`) owns the policy: the viewpoints it passes are **relative to the lifted
rect's centre** (display axes, +z toward the viewer, metres), with its ipd / parallax
factors, axis mask, clamp and recentering ease **already applied**. The plug-in only
translates units:

```
b   = baseline_m (0 / unknown -> 0.063)  -- clamp margin only
L   = G · (max_offset_m + b/2) / 0.063  when max_offset_m > 0, else 3
x_n = clamp(G · x_m / 0.063, ±L)
y_n = clamp(Gy · G · y_m / 0.063, ±Gy·L)   when axis_mode >= XY, else 0
z_n = clamp(Gz · (z_m − z_ref) / z_ref, ±1)   when axis_mode == XYZ, else 0
G   = ViewGain (1.0)   Gy = YGain (1.0)   Gz = ZGain (0.5)
```

- **Unit = the nominal 63 mm, not `baseline_m`.** NeurD's default `±0.5` pair stands for a
  63 mm IPD. The runtime has already applied its ipd factor to the eye positions, so
  dividing by the post-factor `baseline_m` would undo it (every pair would land on
  `±0.5`). With the fixed unit an app that halves `ipdFactor` gets a 32 mm pair →
  `±0.254·G`, i.e. softer stereo, as it asked. `baseline_m` is used only for the clamp
  margin and the log.
- **Clamp.** The runtime clamps the eyes' *midpoint*; an eye sits half the pair's
  separation (`b/2`) off it — clamping each eye at the midpoint's limit would collapse
  the pair at the edge. A runtime clamp wider than `±3` is honoured (the runtime owns the
  policy).
- **`y`** is honoured whenever the runtime lets it through (`axis_mode` XY / XYZ; the
  default is X, so nothing changes unless an app asks). The legacy contract dropped y
  because raw panel-centred eye height made the image jump; now y is rect-relative and
  recentred by the runtime. The first time a process honours y it logs
  `Leia lift: honouring VERTICAL look-around (axis XY, y_gain 1.00) — …`; if the image
  still jumps, lower `YGain`.
- **`z` assumption.** With z pinned (X / XY) the runtime places the eyes at its nominal
  viewing distance, which it takes from this plug-in's own display info. The DP reads
  the same cached panel geometry (`leiasr_geometry_get` → `nominal_z_m`; unknown →
  0.5 m, the runtime's own default) as `z_ref`, so a viewer at the reference distance
  maps to `z_n = 0`. `z_n` is the fractional distance change times `ZGain`, clamped to
  `±1`. NeurD does not specify how its `z` relates to viewing distance: **uncalibrated** —
  the conservative 0.5 default and the sign (negative `ZGain` flips it) are tuning knobs.
- `rect_width_m` / `rect_height_m` / `viewpoint_frame` are logged, not used: the runtime
  already rebased the viewpoints, and NeurD's units are baseline-relative.

### Legacy contract (older runtime, or eyes from the DP's own tracker)

A short `struct_size` (a runtime before the policy) — and any eyes the DP read from its
own SR tracker rather than from the runtime — keep the original mapping exactly:

```
x_n = clamp(G · x_m / 0.063, ±3)
y_n = 0
z_n = 0
```

Panel-centred eyes, x only: a centred viewer at 63 mm IPD reproduces NeurD's default
pattern (G = 1), and head motion becomes look-around. Vertical offset is dropped because
raw eye height made NeurD render the frame from above/below the panel centre (jumps and a
filled top band, panel 2026-09-26); z has no reference here.

Which contract a stream is on is logged once per change:
`Leia lift: stream N viewpoints = runtime policy (frame rect, axis X, baseline 63.0 mm, …)`
or `… = legacy mapping (x only, 63 mm unit, ±3: runtime predates the viewpoint policy)`.

Viewpoint source, in order:

1. **Viewpoints from the runtime** (always sent to the lift-only DP when it has them —
   the app's explicit viewpoints, else the tracked eyes; `viewpoints_xyz`, metres, rect- or
   panel-relative per the contract above): `view_count` triplets are used 1:1; exactly two
   are treated as an eye pair.
2. **Tracked eyes** from the DP's own SR eye path (only when the runtime passed none),
   when tracking is live (inter-eye distance > 1 mm — the same tracking-loss test as the
   eye slot). Always mapped with the legacy contract.
3. **Untracked**: SBS uses NeurD's own default pattern (non-interactive convert); NVIEW
   uses `x = i − (N−1)/2`.

For N views from an eye pair, views are spaced one eye baseline apart and centred on
the eye midpoint. If NeurD predates the interactive API (< 0.4.5), SBS falls back to the
default pattern. (The N-view paths are unreachable today: NVIEW streams are refused.)

## N-view

**Not available on NeurD ≤ 0.4.6.** NeurD's `Config::adjust()` runs on every convert
(including the interactive one) and re-derives the tile grid from the output type —
SBS → 2×1, TB → 1×2, DEPTH → 1×1 — and the public `NEURD_PROP_OUTPUT_TYPE` accepts only
those, so `OUTPUT_TILES_W/H` are overridden. Panel-verified: a 4-view request came back
as a plain 2-tile SBS. Caps never advertise NVIEW, and `lift_stream_create(NVIEW)` fails
with a WARN. What works: SBS with explicit viewpoints (two, from the runtime or tracked
eyes).

### Asks for Leia media_sdk

- A tiled / multiview output type usable with `convert_stream_dx_interactive`
  (N viewpoints → N tiles in one convert).
- A public release of the 0.4.4 *internal-interactive* build (interactive entries in a
  build whose reported version matches its table).
- A working 0.4.6 package.

## Parameters

| `xrt_dp_lift_params` | NeurD |
|---|---|
| `convergence < 0` | `AUTO_CONVERGENCE = TRUE` |
| `convergence` in [0, 1] | `AUTO_CONVERGENCE = FALSE`, `CONVERGENCE = clamp(K · (c − 0.5), ±0.2)`, K = `DXR_LEIA_LIFT_CONV_GAIN` |
| `strength` | `GAIN_MULTIPLIER` (1 = NeurD's calibrated budget, 0 = flat), clamped [0, 10]; negative → 1.0 |
| `inpaint` | NeurD always fills disocclusions: 0 → `V1_STRETCH` (cheapest), non-zero → `V1_BLUR` (the DX video path supports V1 only) |
| `view_count` | NVIEW row width — unused (NVIEW not offered on NeurD) |

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

Each knob comes from the **environment**, else the **registry**
(`HKLM\SOFTWARE\DisplayXR\Leia\Lift`, REG_SZ, 64-bit view, same grammar as the env var),
else its default — env > registry > default. Two lifetimes:

- **Per stream** — `ViewGain`, `YGain`, `ZGain`, `ConvGain`, `DepthGain`, `Dilate`,
  `Scale`: re-read and snapshotted at each `lift_stream_create`, so a registry edit
  applies to the next stream without a service restart. Each create logs them:
  `Leia lift: stream 3 created (mode 2, video) knobs view_gain=1.00(default) y_gain=1.00(default) z_gain=0.50(default) conv_gain=0.40(default) depth_gain=2.00(default) dilate=2(default)`.
- **Per process / DP** — `DXR_LEIA_LIFT`, `Backend`, `MinVersion`, `InteractiveMin`,
  `VideoModel`: read at DP create; NeurD's init-time state follows the first activation.

At activation one WARN lists every knob's effective value (the per-stream ones as the
defaults the first DP saw) and where it came from:

```
Leia lift: knobs backend=directml(reg) interactive_min=0.4.4(reg) min_version=0.4.4(reg) scale=per-stream(default) view_gain=1.00(default) y_gain=1.00(default) z_gain=0.50(default) conv_gain=0.40(default) video_model=fast(default) depth_gain=2.00(default) dilate=2(default) [env > HKLM\SOFTWARE\DisplayXR\Leia\Lift > default; per-convert knobs re-read per stream]
```

That line prints only after a successful init. A NeurD refused by `MinVersion` logs only
its refusal WARN, which names the minimum and where it came from.

**Why the registry.** Under the service the knobs are read from `displayxr-service.exe`'s
environment, not the client's. Any respawn — tray relaunch, the HKLM `Run` key at logon,
a crash restart — starts the service with the logon environment, and env-only knobs
vanish silently (the only tell was "interactive viewpoints UNAVAILABLE"). A registry
value survives every respawn. Env still wins when set, so `.bat`-driven runs behave as
before; with neither set, nothing changes.

| Env | Registry value | Default | Effect |
|---|---|---|---|
| `DXR_LEIA_LIFT` | — (env only) | on | `0` / `off` → caps `modes=0, state=0`; NeurD is never probed or loaded. |
| `DXR_LEIA_LIFT_BACKEND` | `Backend` | `directml` | `auto` \| `directml` \| `cuda` \| `openvino`. First activation in the process wins (NeurD's forced backend is sticky). On NeurD 0.4.3+ only DirectML yields a D3D11 device; on 0.3.x use `cuda` (see *NeurD 0.3.x*). |
| `DXR_LEIA_LIFT_SCALE` | `Scale` | unset (→ stream `input_scale`, else 720p) | Inference height bucket: `720` \| `1080` \| `1440` \| `none`. When set it overrides every stream's `input_scale`. NeurD's own default is 1440p; 720p is the fallback for latency. |
| `DXR_LEIA_LIFT_VIEW_GAIN` | `ViewGain` | `1.0` | `G` in the eye → viewpoint mapping above, [0, 10]. Per stream. |
| `DXR_LEIA_LIFT_Y_GAIN` | `YGain` | `1.0` | `Gy`: extra vertical look-around scale, [0, 10]. Only matters when the runtime sends y (axis mode XY / XYZ). Per stream. |
| `DXR_LEIA_LIFT_Z_GAIN` | `ZGain` | `0.5` | `Gz`: viewer distance → NeurD `z`, [−2, 2]; negative flips the sign. Only matters on axis mode XYZ. Uncalibrated. Per stream. |
| `DXR_LEIA_LIFT_CONV_GAIN` | `ConvGain` | `0.4` | `K` in the convergence map above, [−2, 2]; negative flips the sign. Calibration knob. Per stream. |
| `DXR_LEIA_LIFT_DEPTH_GAIN` | `DepthGain` | `2.0` | NeurD `GAIN_MULTIPLIER` at `strength` 1, [0, 10] (the element's strength multiplies it). Panel-calibrated 2026-09-26. Per stream. |
| `DXR_LEIA_LIFT_DILATE` | `Dilate` | `2` | NeurD `DILATE_RADIO` (disparity-map dilation, px), [0, 16]; NeurD's own default 3 grew the foreground past its silhouette. Per stream. |
| `DXR_LEIA_LIFT_VIDEO_MODEL` | `VideoModel` | `fast` | Video streams' depth model: `fast` (relative real-time) \| `metric` (`NEURD_MODEL_VIDEO_METRIC_QUALITY`, NeurD ≥ 0.4.6). Init-time: the first activation in the process wins. |
| `DXR_LEIA_LIFT_INTERACTIVE_MIN` | `InteractiveMin` | unset (→ header, 0.4.5) | **Demo-only.** A NeurD version, e.g. `0.4.4` (clamped to ≥ 0.4.4), from which `convert_stream_dx_interactive` is trusted. For the 0.4.4 *internal-interactive* dev package, which reports 0.4.4 but carries the interactive entries. The version is the only discriminator (only `NeurD_load` is exported, and a stock 0.4.4 table is too short to probe), so on a **stock 0.4.4 this crashes** — never set it elsewhere. One extra WARN when in effect. When it admits a NeurD older than 0.4.5, the plug-in calls the table slot directly (the header's inline wrapper re-checks 0.4.5). Only matters when `MinVersion` admits that NeurD (the default 0.4.6 floor refuses 0.4.4 before this is consulted); the two knobs are independent. |
| `DXR_LEIA_LIFT_MIN_VERSION` | `MinVersion` | `0.4.6` | Oldest NeurD lift accepts, `major.minor.patch`. Older → refused at load (one WARN), lift unavailable exactly as if NeurD were absent, so callers fall back. Lower it only for demos/testing (e.g. `0.4.4`, `0.3.11`); below 0.3.11 changes nothing — the stream-API check still refuses those. Garbage → WARN, default kept. |

The demo knobs for the 0.4.4 internal-interactive package, set once on the box (elevated
prompt), then restart the service:

```bat
reg add "HKLM\SOFTWARE\DisplayXR\Leia\Lift" /v Backend /t REG_SZ /d directml /f /reg:64
reg add "HKLM\SOFTWARE\DisplayXR\Leia\Lift" /v MinVersion /t REG_SZ /d 0.4.4 /f /reg:64
reg add "HKLM\SOFTWARE\DisplayXR\Leia\Lift" /v InteractiveMin /t REG_SZ /d 0.4.4 /f /reg:64
```

`scripts\set-lift-knobs.bat` writes `Backend` and `InteractiveMin` only — add `MinVersion`
by hand (`--clear` deletes the whole key).

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

The slots compile only when the runtime headers define `XRT_DP_D3D11_HAS_LIFT`, which
first ships in runtime `v2.22.0` — the Windows pin `DXR_RUNTIME_GIT_TAG`. Built against
older headers the plug-in ships with the lift slots compiled out (it logs
`lift slots NOT COMPILED`), and the runtime sees them absent via `struct_size`.

To build against an unreleased runtime, use a local runtime checkout — nothing
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
   worker is loading NeurD); repeat until `state=ready`, `modes=0x3` (DEPTH | SBS — NVIEW is never offered),
   `backend=neurd-directml`. The runtime log shows `Leia lift: loaded NeurD x.y.z from
   ...` and `Leia lift: NeurD READY`.
2. `displayxr-cli lift probe` — runs a stream create + convert + destroy. Check the log
   for the adapter-LUID line and no `Leia lift:` WARN after it; the probe's reported
   latency should sit near 22 ms (DirectML) at 720p.
2b. Viewpoint policy: with a runtime that has `XRT_DP_LIFT_HAS_VIEWPOINT_POLICY`, a lifted
   stream with a tracked viewer logs `Leia lift: stream N viewpoints = runtime policy
   (frame rect, axis X, baseline ~6x mm, …)` once; against an older runtime the same line
   reads `legacy mapping (… runtime predates the viewpoint policy)`.
3. Absent-NeurD check: `set DXR_LEIA_LIFT=0` (or rename the NeurD install key) →
   `lift caps` reports `modes=0 state=unavailable`, and a normal
   `cube_handle_d3d11_win` session weaves exactly as before.
4. Licence path: first activation offline → `state=activating` with a
   `LICENSE_NETWORK_ERROR` WARN; reconnect → ready within ~15 s of the next call.
5. Version floor: with NeurD 0.4.6 installed, set `DXR_LEIA_LIFT_MIN_VERSION=0.4.7` in the
   environment of the process that loads the plug-in (the service, under IPC) → the log
   shows `Leia lift: NeurD.dll at <path> (...) is version 0.4.6, older than the minimum
   0.4.7 (env) — not loaded` and no `READY`, and `lift caps` reports
   `modes=0 state=unavailable`.
6. NeurD installed after the service: with NeurD absent, start the service and an app
   that asks for lift (`lift caps` → `state=unavailable`, log `NeurD.dll not found ...
   Re-checked every 30s`), install NeurD, wait ≤ 30 s (or create a lift stream) → log
   `NeurD.dll appeared at ... — loading`, then `NeurD READY`, with no service restart.

## Known limits

- **Fallback path only:** against a runtime without `create_dp_d3d11_lift`, the runtime
  calls the ordinary factory with a NULL window, which builds an async SR weaver (it
  cannot be told apart from a NULL-HWND `_texture` DP). That extra weaver can briefly
  perturb the lens. The lift-only factory removes this on runtimes that have it.
- Windows / D3D11 only. The D3D12, GL and Vulkan DPs do not implement lift.
- The activation thread is detached; unloading the plug-in DLL while it runs
  (a first activation in progress) is unsafe. The runtime does not unload plug-ins
  mid-session today.
- Caps are frozen once READY: the runtime (`d3d11_lift.cpp`) polls `lift_get_caps` only
  while the module is not READY. A convert-time `NEURD_UNAVAILABLE_OUTDATED_RUNTIME`
  latch (SBS then falls back to the default pattern) is therefore not reflected in caps.
- NeurD's 32-stream limit is process-wide and shared with anything else in the
  service process that uses NeurD.
