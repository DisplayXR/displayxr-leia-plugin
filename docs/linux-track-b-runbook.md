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
  reproduce. Eye tracking **is** running (§0). **The first on-panel weave through the
  plug-in on 26.04 is still pending** — do not read the green selftest as weave validation;
  a headless run never touches weave geometry or phase.

**Pin:** build against the **installed `leiasr-runtime` .deb** — `1.37.0.6048+gb9262217a0`
on the 26.04 box. **The .deb *is* the dev package**; there is no separate SDK dev package to
chase. It ships everything the plug-in configures against under `/opt/leiasr`:

| What | Where |
|---|---|
| headers (srSDK API **1.0.0**) | `/opt/leiasr/include/sr/*.h` |
| loader stub | `/opt/leiasr/lib/libsrSDK_loader.a` |
| CMake config package | `/opt/leiasr/lib/cmake/srSDK/srSDKConfig.cmake` |
| active-runtime registration | `/etc/leia/sr/1/active_runtime.json` (written by this .deb) |

So `SRSDK_ROOT=/opt/leiasr`. CI (`build-linux.yml`) pins `1.37.0.41935`; the build-number
difference against `1.37.0.6048` is a **non-issue** (same 1.37.0 / API 1.0.0 surface).
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
  most likely aborts. GNOME `idle-delay` is **300 s** — keep the box active during a run, or
  agree a longer `idle-delay` with David for long ones. Screen **lock** additionally disables
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

**`-DDXR_RUNTIME_SOURCE_DIR` is mandatory, not a convenience.** Building against the
pinned `DXR_RUNTIME_GIT_TAG_LINUX` (`v2.14.6`) headers produces a plug-in the runtime
**hard-rejects at load**: those headers' `struct vk_bundle` is 8 bytes smaller than runtime
`main`'s (`PFN_vkGetRefreshCycleDurationGOOGLE` was added), and the loader compares
`vk_bundle_abi_size` exactly (runtime `target_plugin_loader.c` ~:2806). Note what this is
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

Failure signatures:

- `srCreateWeaverVulkan failed`
- `no Vulkan weaver backend (weaverBackends=0x…)`
- `weaver backend creation failed (SR service unavailable)`

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

## 4. Bring-up toggles (env vars)

| Var | Meaning |
|---|---|
| `SRSDK_ROOT` | SDK root for configure — `/opt/leiasr` with the `leiasr-runtime` .deb installed. |
| `XRT_LOG=info` | Required to see weaver-create / lens / `USER_FOUND` lines (they are `U_LOG_I`). No log file on Linux; stderr only. |
| `DXR_LEIA_FORCE_PROBE=1` | Bypass the plugin probe. **Only** needed when no Leia panel is connected — a real panel auto-binds via DRM/EDID. |
| `DXR_LEIA_SR_FB_SDK=1` | Hand the caller framebuffer to the SDK (its own render pass) instead of the default plug-in-owned pass + fb=0 mode. Fallback only; reach for it if Vulkan validation complains about the render pass. |
| `SR_RUNTIME_PATH` | Explicit path to `libLeiaSR_runtime.so` (overrides the baked rpath / active_runtime.json). |

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
   (`docs/leia-linux-sdk-contract.md` §8): no phase origin, no refresh getter,
   teardown time on `srDestroyInstance`.

### Results — 2026-09-19/20, Ubuntu 26.04 box (in-house)

1. **Build: clean, no source changes.** GCC 15.2 / CMake 4.2 / Ninja, Track B against the
   installed `leiasr-runtime 1.37.0.6048+gb9262217a0` .deb — which is also the dev package.
2. **Headless selftest: ALL PASS** with `leia-sr` active (ABI v5, 3840x2160,
   0.344 x 0.193 m, MANAGED, 2D + LeiaSR 2-view). The July display-geometry gap did not
   reproduce.
3. **Eye tracking runs** (`FaceLockBlinkEyeTracker`), superseding the July "Blink SDK not
   wired" item — with the tracker-side coordinate bug of §0 (LeiaSR #227) as the live caveat.
4. **On-panel weave through the plug-in: still pending on this box.** Record it as open.
5. **Desktop-position phantom origin under XWayland** — diagnosed, fixed on the runtime side
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
