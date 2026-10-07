# Linux Track B spike — hardware validation runbook

**Audience:** whoever has (a) the LeiaSR Linux stack installed and (b) a Leia-panel Linux
box. Two reference boxes today:

- **22.04 / NVIDIA RTX 3080 / X11** (George) — the box the 2026-07-08 weave ran on.
- **Ubuntu 26.04 / Intel Panther Lake iGPU / Mesa 26 / GNOME 50 Wayland + XWayland**,
  Acer SpatialLabs DS1 on `HDMI-1` — the first in-house 26.04 box (2026-09-19/20).
  Facts below tagged **[26.04 box]** are specific to it.

**Goal:** see the *real* interlaced weave from the srSDK Vulkan weaver instead of the
Track A passthrough SBS blit, and report friction.

**Status (2026-09-20):**

- **Weave, 22.04/NVIDIA:** validated end-to-end on an Acer SpatialLabs DS1 (2026-07-08,
  displayxr-leia-plugin#81) — the srSDK Vulkan weaver engages behind the seam
  (`backend: weaver`, not stub) and the DS1's lenticular lens physically switches to 3D
  (`system event 14: Lens has been enabled`). The core integration question is answered YES.
- **Bring-up, 26.04/Mesa:** Track B **compiles clean with no source changes** against the
  *installed* `leiasr-runtime` .deb, and `displayxr-cli selftest` is **all-pass with
  `leia-sr` active** (§2). The July `display_info`/`display_dims` failure did **not**
  reproduce. Eye tracking **is** running (§0). **Fullscreen on-panel weave through the
  plug-in is validated on this box** (2026-09-20, David on the DS1: weave *and* Kooima
  projection correct) — but only with **displayxr-runtime#1579** or newer; on an older
  runtime a panel that is not at the desktop origin gets a wrong Kooima projection.
  **Windowed** on-panel weave is not validated yet, and a **native Wayland** session cannot
  weave at all on 1.37 (§3). A green headless selftest is still not weave validation — it
  never touches weave geometry or phase.

**Pin:** build against the **installed `leiasr-runtime` .deb** — `1.37.0.6048+gb9262217a0`
on the 26.04 box. **The .deb *is* the dev package**; there is no separate SDK dev package to
chase. It ships everything the plug-in configures against under `/opt/leiasr`:

| What | Where |
|---|---|
| headers (srSDK API **1.0.0**) | `/opt/leiasr/include/sr/*.h` |
| loader stub | `/opt/leiasr/lib/libsrSDK_loader.a` |
| CMake config package | `/opt/leiasr/lib/cmake/srSDK/srSDKConfig.cmake` |
| active-runtime registration | `/etc/leia/sr/1/active_runtime.json` (written by this .deb) |

So `SRSDK_ROOT=/opt/leiasr`. CI (`build-linux.yml`, `Deb` job `SR_TAG`) pins `1.38.0.10108`
(asset `LeiaSR-SDK-1.38.0.10108-linux64.tar.gz`, sha256 checked against `SR_SDK_SHA256`;
the first Linux SDK with the SR compose API — `srWeaverSetComposeInputsVulkan` etc. —
that the 2D-under-the-lens build probe keys on; the previous pin was `1.37.0.10693`).
**The SDK must ship `srWeaverSnapToPhase` + `srWeaverSetPresentOrigin` (LeiaSR#85)** —
configure now fails without the snap call (#271) unless you pass
`-DDXR_LEIA_LNX_ALLOW_NO_SNAP=ON` (bring-up only; the plug-in then logs
`drag phase-snap UNAVAILABLE` at DP creation and dragged windows never snap). SR build
numbers are **not ordered across branches**: the earlier CI pin `1.37.0.41935` has the
higher number but predates the call, and v2.7.3's `.deb` shipped without drag snapping
because of it. Check `grep srWeaverSnapToPhase <sdk>/include/sr/sr_weaver.h`, not the
number.
The old `leiasr-prototype-sdk.zip` (2026-07-06) is dead — it predates
`SR_WEAVER_BACKEND_VULKAN_BIT` and hasn't compiled since #92. Do NOT commit any SDK
material anywhere — it is commercial-licensed.

## 0. SR runtime prerequisites (the box, not the plug-in)

The plug-in binds to an already-running SR runtime/service. What gates whether the lens
turns on and whether tracking runs:

- **Real FPC keys are required** *when building SRService yourself*. A default build compiles
  dummy zero-keys (`FPC_AUTHENTICATION_PATH` unset → `mutualAuthenticateFPC = false`) and the
  lens stays off. Build with `FPC_AUTHENTICATION_PATH=<keys>`. Do **not** reach for the
  `DISABLE_FPC_AUTHENTICATION` bypass — it wasn't needed. (Not a concern on a box running the
  packaged `leiasr-runtime` .deb.)
- **Leave `LEIASR_FPC_PORT` unset.** Auto-detect follows the FPC's `bootApplication`
  re-enumeration; pinning the port breaks the reconnect after the firmware re-enumerates.
- **Eye tracking runs out of the box on 1.37.0.6048** — the July "Blink SDK not wired"
  blocker is **gone**. `SREyeTracker` logs `FaceLockBlinkEyeTracker … face acquired/lost`
  on the 26.04 box. **But** the *packaged* tracker in 1.37.0.6048 has a Linux bug: it never
  loads the per-device `Tracker2DisplayTransform.ini` and therefore reports eye positions in
  **camera** coordinates, offset roughly **(-61, +104) mm** on the DS1 (LeiaSR PR #227).
  A phase-shifted or L/R-swapped weave with that stack points at the **tracker**, not at the
  plug-in — rule the tracker out before filing anything here.
- **[26.04 box] The tracker is a MANUAL process, with no auto-restart.** A locally built
  #227-fixed `SREyeTracker` runs from `/tmp/leiasr-local/bin/SREyeTracker` as user `leiasr`,
  with the packaged `leiasr-eyetracker.service` **stopped**. Pre-run gate:

  ```bash
  pgrep -x SREyeTracker || echo "tracker is DOWN"
  # to fall back to the packaged (buggy, camera-coords) tracker:
  sudo systemctl start leiasr-eyetracker.service
  ```

- **[26.04 box] Screen blank powers the panel's USB hub down (LeiaSR #231).** On blank the
  DS1 drops its hub: the camera and the FPC vanish, SRService marks the FPC disconnected
  (it re-authenticates on re-enumeration via `FpcHotplugMonitor`), and the manual tracker
  most likely aborts. On this box GNOME `idle-delay` is **0 — permanently, by David's
  decision** (the box is always on power): the screen never blanks, the hub stays powered,
  and the tracker survives idle periods. Do not "restore" it to a timeout. If a different
  box blanks on idle, expect exactly that failure chain. Screen **lock** additionally disables
  the GNOME window-geometry extension, so the runtime's #817 consumer falls back to
  display-scoped geometry.

## 1. Build

```bash
# The installed leiasr-runtime .deb is the SDK. Nothing to unpack, nothing to fetch:
export SRSDK_ROOT=/opt/leiasr

# Runtime checkout — see the ABI note below; a sibling clone of `main` is what you want:
git clone https://github.com/DisplayXR/displayxr-runtime ../displayxr-runtime

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DDXR_RUNTIME_SOURCE_DIR=$(pwd)/../displayxr-runtime \
    -DDXR_LEIA_LINUX_WEAVER=sdk        # <-- Track B switch; default is 'stub'
cmake --build build && cmake --build build --target cli
```

Configure must print `Track B srSDK weaver backend (SDK at …/lib)`. If it fails with
"srSDK not found", `SRSDK_ROOT` isn't pointing at the directory that contains
`lib/cmake/srSDK/`.

**Toolchain:** verified on Ubuntu 26.04 with **GCC 15.2 / CMake 4.2 / Ninja** — Track B
compiles clean, **no source changes**.

**Match the runtime's `struct vk_bundle`.** The loader compares `vk_bundle_abi_size`
exactly (runtime `target_plugin_loader.c` ~:2806) and refuses the VK DP on a mismatch.
`vk_bundle` changed layout at runtime v2.16.0 (`PFN_vkGetRefreshCycleDurationGOOGLE` was
added, +8 bytes) and is identical from v2.16.0 through `main`. With the Linux pin at
`v2.17.1` a tag-pinned build therefore loads into any runtime v2.16.0..main; under the old
`v2.14.6` pin it was 8 bytes short and **hard-rejected**, which is why
`-DDXR_RUNTIME_SOURCE_DIR` used to be mandatory. It is still the safest way to build a
plug-in you are about to load. Note what this is
*not*: `XRT_PLUGIN_API_VERSION_CURRENT` is **5** on both, and the runtime's
`scripts/check_plugin_abi.py` does not model this struct fingerprint — so neither the
version number nor the ABI gate warns you. Point the build at the same runtime checkout you
will load the plug-in into.

**Expected configure noise (both benign):**

- `libpipewire-0.3` absent → desktop-capture transparency is unavailable and falls back to
  the 2D-under backdrop.
- CMP0144 warning, caused by the lower-case `SRSDK_ROOT` variable name.

`libLeiaSR_runtime.so` discovery needs **no environment setup**: the SDK lib dir is baked
into the plug-in's DT_RUNPATH at configure time. If you move the SDK afterwards, either
reconfigure or `export SR_RUNTIME_PATH=/new/path/libLeiaSR_runtime.so`.

That baked rpath is a **dev-only convenience** (`DXR_LEIA_SDK_DEV_RPATH`, default ON).
Release builds pass `-DDXR_LEIA_SDK_DEV_RPATH=OFF` and resolve the runtime the
deployment way, via `/etc/leia/sr/1/active_runtime.json` — which the `leiasr-runtime` .deb
now registers itself (confirmed on 1.37.0.6048).

## 2. Smoke-test headless (no window, no GPU work)

### Dev registration

Drop a manifest `050-leia-sr.json` with an **absolute** `binary_path` into the *runtime
checkout's* `build/_plugins/`. `probe_order` 50 beats sim-display's 200, and the runtime's
`build_linux.sh` never deletes foreign manifests, so it survives runtime rebuilds. The
runtime's generated `build/run_cube_*_vk_linux.sh` then pick Leia up **with no env at all**.

```bash
./scripts/build-linux.sh --no-test   # or reuse the build above, then:
../displayxr-runtime/build/runtime-build/src/xrt/targets/cli/displayxr-cli selftest
```

**No `DXR_LEIA_FORCE_PROBE` is needed on a real panel.** EDID auto-bind matched
`{29188, 1}` (ACR / DS1) via sysfs `card1-HDMI-A-1` on the 26.04 box. Force-probe is only
for boxes with no Leia panel attached.

### [26.04 box] selftest result — ALL PASS

`leia-sr` active; ABI **v5** loader-verified; **3840x2160**; **0.344 x 0.193 m**;
eye tracking **MANAGED**; modes **2D + LeiaSR (2 views)**.

The July failure (`display_info` / `display_dims` failing from `SR_FAILED`, §5 item 2)
**did not reproduce** — every `srDisplayGet*` succeeded.

One WARN is expected on the DS1 and is **observed behaviour, not an error**:

```
leia_sr_sdk: SR reported its DEFAULT display geometry (34.4x19.4 cm) —
             overriding physical size from EDID: …
```

EDID says 340x190 mm, and the override path yields the 0.344 x 0.193 m reported above.

### Reading the logs on Linux

**There is no `DisplayXR_<exe>.<pid>_<ts>.log` file on Linux** — file logging in the
runtime's `u_logging.c` is Windows-only. Everything goes to **stderr**. The weaver-create,
lens and `USER_FOUND` lines are `U_LOG_I`, so you need `XRT_LOG=info` to see them.

Decisive Track-B proof line (WARN, always printed):

```
leia_lnx_dp: Linux VK display processor created (backend: weaver)
```

`backend: stub passthrough` means you are on Track A. Other lines worth grepping:

- `leia_sr_sdk: REAL srSDK weaver backend active (runtime 1.0.0, API 1.0.0 prototype pin)`
- `Vulkan weaver created (window=…)`
- `leia_sr_sdk: system event 14: Lens has been enabled`
- `leia_sr_sdk: system event 16 / 17` — `USER_FOUND` / `USER_LOST`

Windowed-weave **present-origin** proof lines (runtime side, `vk_native` compositor —
displayxr-runtime#1579). These are what tells a box that really is feeding a windowed
phase apart from one that silently is not:

- `X11 present origin accepted: panel desktop rect WxH == panel native size — root
  coordinates are physical panel pixels, windowed weave phase is fed.` (INFO, one-shot)
- `get_window_metrics: the display processor reports its panel at (0, 0) but the runtime
  resolved it at (3456, 0) — using the runtime's origin for window-scoped metrics (Kooima
  projection + present origin), per ADR-033` (WARN, one-shot) — the #1579 override firing,
  i.e. the plug-in's (0,0) is being corrected rather than believed.
- The plug-in logs **nothing** on the success path: `srWeaverSetPresentOrigin` is issued
  before every weave and only logs on failure, so **absence of the failure line below is
  the success signal.**

Failure signatures:

- `srCreateWeaverVulkan failed`
- `no Vulkan weaver backend (weaverBackends=0x…)`
- `weaver backend creation failed (SR service unavailable)`
- `leia_sr_sdk: srWeaverSetPresentOrigin failed: …` (WARN, once) — the SDK rejected the
  phase origin; the weave falls back to display-scoped. A runtime predating LeiaSR#85
  reports `SR_ERROR_FUNCTION_UNSUPPORTED` here.
- `X11 present origin refused: the panel's desktop rect … is not the panel's native size
  …` (WARN, runtime side) — root coordinates are not physical panel pixels (display scaling,
  or XWayland), so nothing is fed and weaving stays display-scoped.
- `Window handle is invalid in VulkanWeaver::setWindowHandle: weaving is disabled.` (SDK)
  — a window handle was cleared to 0 *after* construction. NOT the native-Wayland case:
  a weaver *constructed* with `window = 0` weaves unconditionally. See §3.

### Per-monitor claims (`probe_displays`, multi-screen M0)

The Linux arm implements `xrt_plugin_iface::probe_displays` (it was `NULL`). One
`/sys/class/drm` pass now returns **every** connector whose EDID is in the frozen panel
table, with its DRM connector name (`HDMI-A-1`), EDID ids + serial (bytes 12-15), native
px, image mm, and, when an X server is reachable, the RandR CRTC origin (joined by EDID
identity, never by name). The runtime's monitors are matched against that list
(`leia_display_claims_linux.c`):

- **By EDID ids** when the descriptor carries them; identical twins are told apart by the
  RandR origin.
- **By origin + size**, else by an unambiguous size match, when it does not (the runtime's
  XWayland path, #251). An eDP that shares the panel's resolution is never claimed.
- **Confidence, SR 1.38** (the installed `.deb`): `EDID` (50), or `VERIFIED` (100) with
  `serial` = the SR service's active device. That serial is read from the service's device
  store: `<root>/active` is a symlink to `Devices/<serial>`, with `<root>` =
  `$XDG_CACHE_HOME/leiasr`, else `/var/lib/leiasr/leiasr` (the service runs with
  `XDG_CACHE_HOME=/var/lib/leiasr`). This is the file the SR runtime's own
  `resolveLinuxPrimaryDeviceSerial` reads. A dangling link means no active device. The
  store is consulted **only while a live SR context exists** (SRService reachable).
  `Devices/` keeps every device ever seen, and with the service stopped `active` can name
  one that is gone. The monitor is `VERIFIED` only when the service is reachable, has an
  active device, **and exactly one** Leia panel is connected. The serial is system-wide, so with two panels it cannot be
  attributed and both stay at `EDID`. **`srLensGetSerialNumber` is a stub on the Linux
  line** (`SR_ERROR_FEATURE_NOT_SUPPORTED` on 1.38). It is kept only as a last resort,
  and on 1.38 `VERIFIED` comes from the device store, never from that call. The source is
  logged once at INFO (`FPC serial '…' from the SR device store (…)`), and so is a miss.
- **Confidence, new SR API** (`srEnumerateDisplays`, LeiaSR Linux line 876620d62+):
  `VERIFIED` + FPC serial when SR reports the display `FPC_VERIFIED`, joined by DRM
  connector, then EDID ids + serial. SR's list is authoritative, so a monitor SR lists but
  the frozen table lacks is still claimed. Each claim's `displayId` goes into a
  plug-in-private table for the later per-DP binding (M4/M5). Nothing reads that table yet.
- **Identical twins** (same EDID ids) are paired only by RandR origin, or when exactly one
  candidate is left. Two unplaced twins (the XWayland norm) are **ambiguous**: both
  monitors are claimed at `EDID` with no serial, no SR display and no connector, never
  with each other's identity.
- **Fallback claim.** When `probe()` bound the plug-in (`DXR_LEIA_FORCE_PROBE=1`, or a
  panel the matcher cannot pair with any descriptor) but no monitor matched, the plug-in
  still claims exactly one monitor at `EDID` confidence. It picks the monitor whose pixel
  size is the panel's, else the primary monitor, else the first. A plug-in that implements
  `probe_displays` gets no fallback claim from the runtime, so without this the bound
  plug-in would own no monitor and sim-display would win the panel. Logged once:
  `probe() bound but no monitor matched a Leia panel — fallback claim on monitor …`.
- `supported_apis` = Vulkan only (this arm has no GL DP).

**Cost and logging.** `probe_displays` runs on every runtime registry rebuild, which happens per client connect. It therefore reads the panel list from a shared cache with a 1 s TTL (one sysfs scan and one X connection per burst). It logs the `claim monitor … confidence=…` lines at INFO, and only when the answer changes. The same cache feeds the per-frame position getters. It is refreshed by that TTL and invalidated by SR display connect/topology events, so a hot-plugged or moved panel resolves on the next rebuild instead of never. A failed `srEnumerateDisplays` is retried on the next probe (only `SR_ERROR_FUNCTION_UNSUPPORTED` is final for a context).

`probe_displays` **never creates an SR context**: it reuses the one `probe()` already
brought up and otherwise answers at EDID confidence. The new-API path is compiled only
when CMake's `check_symbol_exists(srEnumerateDisplays)` compiles **and links** against
`SRSDK_ROOT`. Configure then prints `SR display enumeration ENABLED`. Against 1.38 it
prints `COMPILED OUT`. A new-header build running on a 1.38 runtime gets
`SR_ERROR_FUNCTION_UNSUPPORTED` and falls back to the 1.38 path.

Today's runtime builds no monitor list on Linux (`os_display_edid` is a stub there), so
nothing calls `probe_displays` on this platform until the runtime half of M0 lands.
`test_display_claims_linux` covers the parser and the matching without hardware.

### One DP per screen, windowless EXTERNAL weaver (multi-screen M4)

M4's goal on this box: one window straddling the laptop panel (eDP-1, sim_display,
anaglyph) and the DS1 (HDMI-A-1, leia-sr) is woven on the DS1 half by the Leia DP, with
DS1 eye tracking, and anaglyph on the other half. The runtime splits the window into
per-screen **segments** and creates one DP per screen through the appended plug-in slot
`create_dp_vk_for_screen` (runtime M2, #1853). The plug-in side:

**The weaver is always windowless (`window = 0`).** `leiasr_lnx_create` no longer forwards
the X11 window to `SrWeaverCreateInfoVulkan.window`, on every path (plain factory too). The
runtime supplies the phase origin per frame (`set_present_origin` →
`srWeaverSetPresentOrigin`, ADR-033), which is all the Linux SDK ever used a window for (its
`getScreenRect` is (0,0), contract R-W3). With an X11 id the SDK refuses a resampled panel,
which forced 2D under the fractionally scaled XWayland desktop this box runs. A weaver
*constructed* windowless always weaves (§3, "Native Wayland weaves windowless"). The weave
log line now always reads `window=0x0 = windowless`.

**The runtime owns the resampled refusal, from 2.27.1.** That SDK refusal, not
the window, was what kept a scaled X11 window from weaving into a double image.
Since runtime v2.27.1 (#1831) the runtime itself presents 2D when an X11 window's
pixels are resampled (and gates each segment the same way, using the DP's
`get_scanout_caps`, flags 0 here). That is why the weaver can be windowless.
The Linux runtime pin is therefore v2.29.0, and the `.deb` Depends on
`displayxr-runtime (>= 2.27.1)`. Do not reintroduce the X11 window: on a scaled
XWayland desktop it forces 2D even with an explicit present origin, which is
exactly the M4 straddling case. If an X11 window was handed in, one
INFO line says it was not forwarded.

**What a DP does with a screen binding.** `create_dp_vk_for_screen` hands the DP an
`xrt_screen_binding` (monitor id, desktop rect, native px, EDID mm, serial, vendor display
id). The DP resolves it **once, at creation**, into a `struct leia_lnx_screen` it owns
(`leia_screen_linux.c`), and from then on `get_display_dimensions` and
`get_display_pixel_info` answer from that and nothing else. They no longer read the
process-wide SR display or the "first panel" RandR cache, so two DPs can never answer for
each other. The resolution rules:

| Fact | Source |
|---|---|
| desktop origin | the binding's rect, always (the runtime placed the screen) |
| panel / connector | the `probe_displays` claim's connector → the binding's device name (DRM or RandR spelling) → the panel at the binding's origin |
| pixels | binding native mode → panel EDID native → desktop size |
| "is this the SR-driven panel?" | by SR `displayId` when both sides know one; else the first panel in connector order (SR 1.38's single panel); else (no EDID list, forced probe) yes |
| size, nominal viewer, recommended view, refresh | the SR panel: SR's numbers (the same `get_display_info` reports, so system and per-screen answers agree); any other Leia panel: binding mm → EDID mm, centred viewer at SR's distance/height ratio, no recommended view |

`get_display_info_for_monitor` (runtime M1 slot, `XR_DXR_display_info` v22) uses the same
resolution and answers only for a claimed monitor that **is** the SR-driven panel
(MANAGED eye tracking, the probe-seeded view scale). It returns `false` for any other Leia
panel, so the runtime derives EDID defaults with no eye tracking.

A **segment DP** (one made by `create_dp_vk_for_screen`) with a sub-rect canvas confines
every write to the canvas, clamped to the target. The 2D (1×1) blit goes to the canvas
rect only, and the weave's render pass begins with `renderArea` = the canvas (viewport and
scissor already were). The whole-target post-weave alpha-gate is skipped, with one WARN, so
a transparent window spanning screens presents opaque on the Leia segment. The plain
`create_dp_vk` keeps today's behaviour exactly. `get_scanout_caps` now states the answer
explicitly: weave scope `CANVAS`, `flags = 0` (no `TOLERATES_RESAMPLE`, the weave needs
1:1 pixels), so the runtime's per-segment 1:1 gate keeps a resampled Leia segment flat.

**The SR routing / binding chain** (LeiaSR Linux line 876620d62, phases A-C). Compiled
only when the SDK headers declare the structs (CMake `DXR_LEIA_LNX_HAVE_SR_ROUTING`, a
type-only compile check), and used only when the **installed** runtime says it honours
them. `srGetRuntimeCapabilities` is called with `SrWeaverRoutingCapabilities` →
`SrDisplayBindingCapabilities` chained, once per context, and logs
`SR multi-display caps: externalRouting=… displayBinding=… maxBoundDisplays=…`. Each weaver
create then chains:

- `SrWeaverRoutingInfo{mode = SR_WEAVER_ROUTING_EXTERNAL, flags = 0}` when
  `externalRouting`. This means no `canWeave` gate, one full-input region, phase =
  present origin + viewport only, no lens vote, and no polling. `KEEP_DRAG_SNAP` is not
  set: it only acts on a real window, and drag snapping is the runtime's
  `snap_window_rect` → `srWeaverSnapToPhase`.
- `SrDisplayBindingInfo{displayId}` when the screen has an SR `displayId` (the binding's,
  else the `probe_displays` table's), the runtime reports `displayBinding`, and
  `maxBoundDisplays >= 1`.

If the create is refused, the binding is dropped first (`SR_ERROR_DEVICE_NOT_AVAILABLE`
for an EDID-only display, `SR_ERROR_DISPLAY_NOT_FOUND` for an unknown id). If it is still
refused, the routing is dropped too. Each step logs one INFO line and ends in exactly the
pre-M4 weaver. The weaver-created INFO line names the routing (`EXTERNAL`/`SDK`) and the
display (`bound to 0x…` / `active (unbound)`). Against the installed 1.38 runtime, the
caps come back all false, and nothing is chained even from new headers.

**Lens ownership under EXTERNAL routing.** An EXTERNAL weaver never votes the lens. The
header says the controller owns it, so "leave 3D to the weaver until something asks for 2D"
(LeiaSR #266) cannot hold while one is alive. The plug-in therefore sends
`srLensEnable` when an EXTERNAL weaver is created, unless the last request was 2D. A 3D
`request_display_mode` is always sent while one is alive. When the **last** EXTERNAL
weaver is destroyed with the lens on because of us, the plug-in sends `srLensDisable`.
That release is recorded as "nothing to re-apply", not as a 2D wish. Without it the
long-lived service process would hold an ENABLE preference forever, and SRService keeps
the lens on while any client does (§4, "Co-existing"). The rules and their test live in
`leia_lens_owner_linux.h` / `test_lens_owner_linux`.

**Build the plug-in with the runtime's build type.** The loader compares `sizeof(struct
vk_bundle)` and its function-table offset exactly (#1243). `struct os_mutex` carries two
debug-only fields under `#ifndef NDEBUG`, and `vk_bundle` holds two of them, so a
RelWithDebInfo/Release plug-in (NDEBUG) is 16 bytes short of a Debug runtime (2176/784 vs
2192/800). The runtime then logs `vk_bundle ABI mismatch … Refusing the VK DP factory` and
no Leia code runs. `scripts/build_linux.sh` builds the runtime as Debug, so configure the
plug-in with `-DCMAKE_BUILD_TYPE=Debug` against it. To check without loading anything, dlopen
both `.so`s and read `vk_bundle_abi_size` / `vk_bundle_fn_table_offset` from the iface
`xrtPluginNegotiate` returns (negotiate does not probe or create an SR context).

**Refresh** (#184): `srDisplayGetRefreshRate` when the SDK has it (CMake
`DXR_LEIA_LNX_HAVE_SR_REFRESH`, compile + link probe) and the runtime answers a plausible
value. Otherwise it stays at 60 Hz, with one INFO line saying why. The display line logs
`refresh … Hz from srDisplayGetRefreshRate` or the fallback reason.

**Not per DP yet, by design.** The SR instance, eye tracker, lens and the process display
handle are still one per process and unbound. With **one** Leia panel that is correct:
`maxBoundDisplays` is 1, so only the FPC-verified display binds, and that is the active
display these objects already follow. The tracker also has to exist before
`srInitialize`, while display ids only come from `srEnumerateDisplays` on an initialised
context. **Two Leia panels wait on LeiaSR phase D** (a service driving more than one
device). Until then a second Leia panel's segment DP resolves as "not the SR-driven panel"
(EDID facts, no tracking), and a bound weaver for it is refused and falls back to the
active display.

Hardware-free coverage: `test_screen_linux` (binding → screen, two instances never share
state), `test_sr_routing_linux` (the chain plan everywhere, plus the real `pNext` chains
and the caps chain against SDK headers that carry the structs), and
`test_lens_owner_linux` (the EXTERNAL lens rules).

## 3. Real weave on the panel

```bash
# Vulkan cube via the runtime's own generated run script (Leia is picked up from
# build/_plugins, so no env vars are needed):
../displayxr-runtime/build/run_cube_handle_vk_linux.sh
```

- **Expected:** lenticular-interlaced output on the panel (blurry/striped on a normal
  monitor), NOT two side-by-side cubes. Head movement should steer the sweet spot
  (`system event 16/17`).
- **Validation layers** (`VK_LOADER_LAYERS_ENABLE=*validation*` or vkconfig) on a first run
  — belt and braces. Render-pass compatibility has been **verified against the SDK source**
  (v2-vulkan-weaver `vkweaver.cpp`: single color attachment, 1 sample, no depth, loadOp
  LOAD, `COLOR_ATTACHMENT_OPTIMAL` in/out — same shape as the plug-in's pass, and fb=0 mode
  skips the weaver's own Begin/EndRenderPass), so no VUID errors are expected. If one fires
  anyway, `DXR_LEIA_SR_FB_SDK=1` hands the framebuffer to the SDK instead.

### [26.04 box] Desktop position under XWayland — fixed on the runtime side

`leia_lnx_edid_panel_desktop_position()` finds **no RandR output** under XWayland: its RandR
emulation exposes **no EDID property** at all (`xrandr --prop` shows only `RANDR Emulation`
and `non-desktop`). So `display_screen_left/top` stay at what `srDisplayGetLocation` returns
on Linux — **(0, 0)** (LeiaSR hands back the CRTC rect: right size, bogus origin) — and the
compositor then computes a **phantom present origin**, `ox = window_left − display_left ≈ 3456`.

Fixed **in the runtime** by **displayxr-runtime#1579**: the resolver matches the panel
monitor by pixel size (mm as tiebreaker) and overrides a (0,0) plug-in origin. Verified
against this plug-in:

```
screen pos: (3456, 0) [runtime override by size match; plug-in reported (0, 0)]
```

A plug-in-side fallback (query the origin without RandR EDID) is **optional** and tracked
here as **#251**. Separately, Mutter may veto pre-map window positions (runtime #729).

Scope note: `srDisplayGetLocation`'s bogus (0,0) origin (LeiaSR #225) is **not** on the
windowed-phase path. The weave phase comes from `srWeaverSetPresentOrigin`, which the
runtime feeds with a panel-relative origin; the bogus display rect only affects consumers
of that rect — which the runtime now overrides (#1579). A window at desktop (3556, 100)
with the panel at x = 3456 is fed (100, 100).

### Lookaround looks quantized? It is the SDK's noise-rejection hold, not the plug-in

Seen on the DS1 (2026-09-20): head-driven lookaround stepping rather than moving
continuously. Cause (from LeiaSR source, confirmed by A/B): our plug-in consumes
`srEyeTrackerPredict`, whose output goes through `[LookaroundFilter]` in
`/opt/leiasr/products/D1/ft_user.ini` — `useNoiseRejection=true`,
`noiseRejectionThreshold_cm=[0.2,0.2,3]`, `noiseRejectionAlpha=[0,0,0.001]`: the eye
position is **frozen** until the head moves >2 mm laterally or **>3 cm in depth**, and a
3 cm depth step is very visible in an off-axis projection. The file is byte-identical to
the Windows install and the filter has no platform `#ifdef`s — a product-tuning question
for LeiaSR, not a Linux or plug-in bug. (`[WeavingPoseFilter]` is the *weaver's* filter and
is unrelated to this path; LeiaSR #236's decode-time timestamps only matter there.)

A/B without rebuilding anything — the SR client resolves its products tree through
`$LEIASR_DATA_DIR` first:

```bash
cp -r /opt/leiasr/products "$AB"/          # any writable dir
sed -i 's/^useNoiseRejection=true/useNoiseRejection=false/' "$AB/products/D1/ft_user.ini"   # ONLY the [LookaroundFilter] one
LEIASR_DATA_DIR="$AB" <run the app>        # ft_user.ini is read at eye-tracker creation → restart the client
```

Result at the panel: with the hold off, David reports lookaround "looks good" (noisier,
continuous). Note the client-side `[filterconfiguration] runtime data root: …` line does
**not** reach the runtime's stderr, so the visual A/B is the confirmation.

### [26.04 box] Native Wayland weaves windowless — the "does not weave" finding was WRONG

Under a *native* Wayland surface there is no X11 `Window`, so the plug-in creates the
weaver with `window = 0`. An earlier revision of this section recorded that
`leiasr-runtime 1.37.0.6048` then logs

```
Window handle is invalid in VulkanWeaver::setWindowHandle: weaving is disabled.
```

and never weaves, and filed a LeiaSR ask (#224/#225) to lift the gate. **That was a
misreading, withdrawn on LeiaSR#248.** The SDK has two regimes decided by call order
(`WeaverBaseImpl.ipp:690-706`): a weaver *constructed* with a null window sets
`constructedWithoutWindow` and **always weaves** — which is what `srCreateWeaverVulkan(...,
window = 0)` does, and what the plug-in's create path in `leia_sr_linux_sdk.c` does; only
`setWindowHandle(0)` on a weaver that was *created with* a window disables weaving and logs
the line above. Confirmed empirically against 1.37 on the same box: `Vulkan weaver created
(window=0x0 = windowless/display-scoped)`, weave path entered, no `weaving is disabled`
anywhere. Phase comes from `srWeaverSetPresentOrigin` exactly as on X11 (contract R-W7);
on Wayland the runtime feeds it from the GNOME geometry publisher (runtime#817), or (0,0)
when fullscreen on the panel.

What is **not** yet validated is the on-panel Wayland weave itself — phase lock on a real
3D panel — see runtime#817's checklist. LeiaSR#248 (make windowless an explicit mode
instead of a constructor accident) is hygiene, not a gate. **No LeiaSR change is required
for windowless weaving**, whether under native Wayland or under a windowless direct-scanout
present (runtime#1698).

## 4. Bring-up toggles (env vars)

| Var | Meaning |
|---|---|
| `SRSDK_ROOT` | SDK root for configure — `/opt/leiasr` with the `leiasr-runtime` .deb installed. |
| `XRT_LOG=info` | Required to see weaver-create / lens / `USER_FOUND` lines (they are `U_LOG_I`). No log file on Linux; stderr only. |
| `DXR_LEIA_FORCE_PROBE=1` | Bypass the plugin probe. **Only** needed when no Leia panel is connected — a real panel auto-binds via DRM/EDID. |
| `DXR_LEIA_SR_FB_SDK=1` | Hand the caller framebuffer to the SDK (its own render pass) instead of the default plug-in-owned pass + fb=0 mode. Fallback only; reach for it if Vulkan validation complains about the render pass. |
| `SR_RUNTIME_PATH` | Explicit path to `libLeiaSR_runtime.so` (overrides the baked rpath / active_runtime.json). |
| `DXR_LEIA_SR_EXTERNAL_ROUTING=0` | Do not chain `SrWeaverRoutingInfo{EXTERNAL}` (multi-screen M4): weavers stay SDK-routed even on a runtime that honours routing. An on-panel A/B, not a setting. |
| `SR_WEAVER_ROUTING=external\|external,keep-drag-snap\|sdk` | **SR's own** override, read by the SR runtime at weaver creation. It wins over whatever the plug-in chained, in either direction, so it tests EXTERNAL without a plug-in rebuild. Caveat: with `sdk` forced over a chained EXTERNAL, the plug-in still takes the lens (it believes the weaver will not vote). |

Formerly-open behavior questions (render-pass shape, input width semantics,
recommended-texture-size units, windowless weaving, image layouts) were settled by
reading the SDK source and then **vendor-confirmed on LeiaSR#53** (fb=0 = the same
path Leia's Unity plugin uses; input dims = per-view) — see
`docs/leia-linux-sdk-contract.md` §8.

### Co-existing with other SR clients

A headless `displayxr-cli` run is a full `srCreateInstance → … → srCreateLens →
srDestroyInstance` cycle of about a second, and it registers a **transient lens DISABLE
preference**. That cannot disturb another client's 3D: SRService's rule
(`lensswitchbehavior.cpp:57-60`) keeps the lens ON while **any** client holds an ENABLE
preference and no force-2D hint is set. If you see the lens drop, check whether the *last*
enabling client just exited — that case is legitimate, not a bug.

## 5. What to report back (LeiaInc/LeiaSR#75 + displayxr-leia-plugin#81)

> Note: the C99 srSDK PR **#53 is closed → superseded by [#75](https://github.com/LeiaInc/LeiaSR/pull/75)** (`ST-5532`). Direct new asks / weave-quality reports to #75; #53's thread only holds the already-answered render-pass + input-extent confirmations.

1. Does it weave, and does head movement steer the sweet spot? (The remaining
   unknowns are behavioral quality, not API mechanics.)
2. Eye tracking: latency feel, `USER_FOUND/USER_LOST` cadence, whether the MANAGED
   collapse (weaver auto-blits below 1 mm eye separation) looks right.
3. Anything from the reconciliation gap list that bites in practice
   (`docs/leia-linux-sdk-contract.md` §8): no refresh getter, teardown time on
   `srDestroyInstance`, and the null-window weave gate (§3). **Not** the phase origin —
   `srWeaverSetPresentOrigin` shipped (LeiaSR#85) and the plug-in drives it.

### Results — 2026-09-19/20, Ubuntu 26.04 box (in-house)

1. **Build: clean, no source changes.** GCC 15.2 / CMake 4.2 / Ninja, Track B against the
   installed `leiasr-runtime 1.37.0.6048+gb9262217a0` .deb — which is also the dev package.
2. **Headless selftest: ALL PASS** with `leia-sr` active (ABI v5, 3840x2160,
   0.344 x 0.193 m, MANAGED, 2D + LeiaSR 2-view). The July display-geometry gap did not
   reproduce.
3. **Eye tracking runs** (`FaceLockBlinkEyeTracker`), superseding the July "Blink SDK not
   wired" item — with the tracker-side coordinate bug of §0 (LeiaSR #227) as the live caveat.
4. **Fullscreen on-panel weave through the plug-in: VALIDATED** (2026-09-20, David on the
   DS1) — weave *and* Kooima projection correct, with **displayxr-runtime#1579** or newer
   (older runtimes mis-project a panel that is not at the desktop origin). **Windowed**
   on-panel weave is not validated yet.
5. **Phase origin works.** `srWeaverSetPresentOrigin` exists in srSDK 1.0.0 as shipped in
   `leiasr-runtime 1.37.0.6048` (LeiaSR#85 landed) and the plug-in issues it before every
   weave — contract §8 R-W7 flipped from ❌ to ✅. Needed in *every* layout: the Linux
   weaver never tracks the window's desktop position itself.
6. **Native Wayland weaves windowless on 1.37** — `window = 0` at construction takes the
   always-weave branch; the earlier "weaving is disabled" finding was withdrawn (LeiaSR#248,
   §3). On-panel Wayland phase is still unvalidated (runtime#817).
7. **Desktop-position phantom origin under XWayland** — diagnosed, fixed on the runtime side
   (runtime#1579); optional plug-in fallback is #251 (§3).

### Results — 2026-07-08, DS1 on 22.04/NVIDIA (displayxr-leia-plugin#81)

1. **Weaves: YES** — lens enables on the DS1, `backend: weaver`, weaver comes up
   display-scoped/windowless (`window=0x0`). Head-steering was untestable at the time (no
   eye tracker); **superseded** — tracking runs on 1.37.0.6048 (§0).
2. **Display-geometry gap (contract §8 R-D1):** headless `selftest` failed `display_info`
   /`display_dims` with `SR_FAILED` from one of the `srDisplayGet*` getters.
   **Superseded** — did not reproduce on 26.04 / 1.37.0.6048; every getter succeeded.
3. **Teardown clean** — no `srDestroyInstance` crash. NB: the srSDK loader pulls glog, which
   installs a `FailureSignalHandler`, so a plain SIGTERM to the host app prints a benign glog
   stack trace — don't misread it as a weaver crash.

### Not our bugs (don't chase them here)

- **Head-motion "quantization"** in LeiaSR's own weaving example is the
  `[WeavingPoseFilter]` deadband in `products/D1/ft_user.ini` — a 1 cm window, by design,
  same as Windows. Not a plug-in concern. LeiaSR #236 separately notes that the Linux libuvc
  frame timestamps are decode-time.
- **Late-latch GL coherency hazard** lives in LeiaSR's `glweaver2.cpp`, not in our code.
