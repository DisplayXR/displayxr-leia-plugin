# Linux Track B spike — hardware validation runbook

**Audience:** whoever has (a) the srSDK prototype package and (b) a Leia-panel Linux box
(George's 22.04/NVIDIA machine today). **Goal:** see the *real* interlaced weave from the
srSDK Vulkan weaver instead of the Track A passthrough SBS blit, and report friction.

**Status (2026-07-08):** the real weave is **validated end-to-end on an Acer SpatialLabs
DS1** (George, 22.04/RTX 3080, displayxr-leia-plugin#81) — the srSDK Vulkan weaver engages
behind the seam (`backend: weaver`, not stub) and the DS1's lenticular lens physically
switches to 3D (`system event 14: Lens has been enabled`). The core integration question is
answered YES. Two open items remain: eye-tracking isn't running (Blink SDK not wired) and one
display-geometry getter fails on Linux — see §0 and §5.

**Pin:** build against a freshly-built **LeiaSR ST-5525** SDK install (SR Service ≥ 1.37.0,
srSDK API **1.0.0**), matching the running SR runtime. **The old `leiasr-prototype-sdk.zip`
(2026-07-06) is now stale** — it predates `SR_WEAVER_BACKEND_VULKAN_BIT`, so the plug-in no
longer compiles against it (`SR_WEAVER_BACKEND_VULKAN_BIT undeclared`) as of #92. A different
SDK drop may work (the loader ABI is append-only) but is unpinned territory. Do NOT commit the
SDK anywhere — it is commercial-licensed.

## 0. SR runtime prerequisites (the box, not the plug-in)

The plug-in binds to an already-running SR runtime/service; three SRService build-time
settings gate whether the lens actually turns on and whether tracking runs (learned on the
2026-07-08 DS1 run):

- **Real FPC keys are required.** A default SRService build compiles dummy zero-keys
  (`FPC_AUTHENTICATION_PATH` unset → `mutualAuthenticateFPC = false`) and the lens stays off.
  Build with `FPC_AUTHENTICATION_PATH=<keys>` for real auth (calibration cached, lens
  available). Do **not** reach for the `DISABLE_FPC_AUTHENTICATION` bypass — it wasn't needed.
- **Leave `LEIASR_FPC_PORT` unset.** Auto-detect follows the FPC's `bootApplication`
  re-enumeration; pinning the port breaks the reconnect after the firmware re-enumerates.
- **Eye tracking needs the Blink SDK wired.** A default `build.py … generate eyetracker`
  links Blink but doesn't wire `BLINK_SDK_DIR`→`Blink_DIR` / `Detector_UseFaceLockBlinkSDK`
  / license+models, so `SREyeTracker` dies at `BlinkEye contextFaceTracker creation failed`
  and the weave runs at a **fixed default eye position** (no `USER_FOUND/LOST`, no steering).
  A Blink-configured eyetracker rebuild (possibly `BLINK_UN`/`BLINK_PW`) is needed for the
  head-steering test.

## 1. Build

```bash
# Point SRSDK_ROOT at a freshly-built ST-5525 SDK install (dir containing lib/cmake/srSDK/) —
# NOT the stale prototype zip, which predates SR_WEAVER_BACKEND_VULKAN_BIT and won't compile
# against #92. Never unpack into the repo (commercial-licensed):
export SRSDK_ROOT=~/sdk/leiasr-st5525-install

# Sibling runtime checkout (>= v1.28.0), as for the Track A build:
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

`libLeiaSR_runtime.so` discovery needs **no environment setup**: the SDK lib dir is baked
into the plug-in's DT_RUNPATH at configure time. If you move the SDK afterwards, either
reconfigure or `export SR_RUNTIME_PATH=/new/path/libLeiaSR_runtime.so`.

That baked rpath is a **dev-only convenience** (`DXR_LEIA_SDK_DEV_RPATH`, default ON).
Release builds pass `-DDXR_LEIA_SDK_DEV_RPATH=OFF` and resolve the runtime the
deployment way: Leia's Linux runtime installer registers it at
`/etc/leia/sr/1/active_runtime.json` (the loader's first search path).

## 2. Smoke-test headless (no window, no GPU work)

```bash
./scripts/build-linux.sh --no-test   # or reuse the build above, then:
# stage + selftest exactly like the script/CI do, but the sdk backend now
# actually probes the SR runtime:
DXR_LEIA_FORCE_PROBE=1 XRT_PLUGIN_SEARCH_PATH=build/_plugins \
    build/runtime-build/src/xrt/targets/cli/displayxr-cli selftest
```

Expected log line difference vs Track A:
`leia_sr_sdk: REAL srSDK weaver backend active (runtime <ver>, API 1.0.0 prototype pin)`
instead of `leia_sr_stub: STUB weaver backend active`. If instead you get
`SR runtime unavailable … (loader: …)`, the loader message tells you why
`libLeiaSR_runtime.so` didn't load.

`DXR_LEIA_FORCE_PROBE=1` is still required — the Track A plugin probe declines by default
and force-probe is the supported override (auto-probe is a separate work item).

## 3. Real weave on the panel

```bash
# Vulkan cube via the runtime's test app (from the runtime checkout's Linux build):
DXR_LEIA_FORCE_PROBE=1 XRT_PLUGIN_SEARCH_PATH=build/_plugins \
XR_RUNTIME_JSON=../displayxr-runtime/build/openxr_displayxr-dev.json \
    ../displayxr-runtime/build/test_apps/cube_handle_vk_linux   # name per runtime docs
```

- **Expected:** lenticular-interlaced output on the panel (blurry/striped on a normal
  monitor), NOT two side-by-side cubes. Head movement should steer the sweet spot once
  eye tracking runs (`is_tracking` flips on `USER_FOUND` events, visible as
  `leia_sr_sdk: system event 16/17` log lines).
- **First run with validation layers** (`VK_LOADER_LAYERS_ENABLE=*validation*` or
  vkconfig) — belt and braces. Render-pass compatibility has since been **verified
  against the SDK source** (v2-vulkan-weaver `vkweaver.cpp`: single color attachment,
  1 sample, no depth, loadOp LOAD, `COLOR_ATTACHMENT_OPTIMAL` in/out — same shape as
  the plug-in's pass, and fb=0 mode skips the weaver's own Begin/EndRenderPass), so
  no VUID errors are expected. If one fires anyway, `DXR_LEIA_SR_FB_SDK=1` hands the
  framebuffer to the SDK instead.

## 4. Bring-up toggles (env vars)

| Var | Meaning |
|---|---|
| `DXR_LEIA_FORCE_PROBE=1` | Bypass the plugin probe (only needed when no Leia panel is connected — a real panel now auto-binds via DRM/EDID). |
| `DXR_LEIA_SR_FB_SDK=1` | Hand the caller framebuffer to the SDK (its own render pass) instead of the default plug-in-owned pass + fb=0 mode. Fallback only; not expected to be needed. |
| `SR_RUNTIME_PATH` | Explicit path to `libLeiaSR_runtime.so` (overrides the baked rpath). |

Formerly-open behavior questions (render-pass shape, input width semantics,
recommended-texture-size units, windowless weaving, image layouts) were settled by
reading the SDK source and then **vendor-confirmed on LeiaSR#53** (fb=0 = the same
path Leia's Unity plugin uses; input dims = per-view) — see
`docs/leia-linux-sdk-contract.md` §8.

## 5. What to report back (LeiaInc/LeiaSR#75 + displayxr-leia-plugin#81)

> Note: the C99 srSDK PR **#53 is closed → superseded by [#75](https://github.com/LeiaInc/LeiaSR/pull/75)** (`ST-5532`). Direct new asks / weave-quality reports to #75; #53's thread only holds the already-answered render-pass + input-extent confirmations.

1. Does it weave, and does head movement steer the sweet spot? (The remaining
   unknowns are behavioral quality, not API mechanics.)
2. Eye tracking: latency feel, `USER_FOUND/USER_LOST` cadence, whether the MANAGED
   collapse (weaver auto-blits below 1 mm eye separation) looks right.
3. Anything from the reconciliation gap list that bites in practice
   (`docs/leia-linux-sdk-contract.md` §8): no phase origin, no refresh getter,
   teardown time on `srDestroyInstance`.

### Results so far (2026-07-08 DS1 run — displayxr-leia-plugin#81)

1. **Weaves: YES** — lens enables on the DS1, `backend: weaver`, weaver comes up
   display-scoped/windowless (`window=0x0`). **Head-steering: NOT yet** — the eye tracker
   isn't running (Blink SDK not wired; see §0), so the weave is at a fixed default eye
   position. This is the main open item.
2. **Display-geometry gap (contract §8 R-D1):** headless `selftest` fails `display_info`
   /`display_dims` — one of `srDisplayGet{PhysicalSize,PhysicalResolution,Location,
   RecommendedTextureSize,DefaultViewingPosition}` returns `SR_FAILED` on Linux (the default
   display object isn't backed by real geometry; plausibly tied to DisplayManager/eyetracker
   not being up). Does **not** block the weave — legacy `cube_handle_vk_linux` doesn't enable
   `XR_DXR_display_info` and the weaver runs display-scoped. Next debug step: per-getter
   logging in `sr_ctx_refresh_display_info_locked` to name which getter fails.
3. **Teardown clean** — no `srDestroyInstance` crash. NB: the srSDK loader pulls glog, which
   installs a `FailureSignalHandler`, so a plain SIGTERM to the host app prints a benign glog
   stack trace — don't misread it as a weaver crash.
