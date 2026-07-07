# Linux Track B spike — hardware validation runbook

**Audience:** whoever has (a) the srSDK prototype package and (b) a Leia-panel Linux box
(George's 22.04/NVIDIA machine today). **Goal:** see the *real* interlaced weave from the
srSDK Vulkan weaver instead of the Track A passthrough SBS blit, and report friction.

**Pin:** this spike is built against `leiasr-prototype-sdk.zip` (2026-07-06) — srSDK API
**1.0.0**, `libLeiaSR_runtime.so` BuildID `fcf21021eeb277bac06fdb0e484bd4a8f31ad36b`.
A different SDK drop may work (the loader ABI is append-only) but is unpinned territory.
Do NOT commit the SDK anywhere — it is commercial-licensed.

## 1. Build

```bash
# Unpack the SDK anywhere (never into the repo):
unzip leiasr-prototype-sdk.zip -d ~/sdk
export SRSDK_ROOT=~/sdk/leiasr-prototype-sdk

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
reading the SDK source — see `docs/leia-linux-sdk-contract.md` §8.

## 5. What to report back (LeiaInc/LeiaSR#53 + displayxr-leia-plugin#81)

1. Does it weave, and does head movement steer the sweet spot? (The remaining
   unknowns are behavioral quality, not API mechanics.)
2. Eye tracking: latency feel, `USER_FOUND/USER_LOST` cadence, whether the MANAGED
   collapse (weaver auto-blits below 1 mm eye separation) looks right.
3. Anything from the reconciliation gap list that bites in practice
   (`docs/leia-linux-sdk-contract.md` §8): no phase origin, no refresh getter,
   teardown time on `srDestroyInstance`.
