# drv_leia_android — DisplayXR Leia CNSDK Plug-in

The Android variant of the Leia display-processor plug-in. Wraps the
**Leia Computational Display SDK (CNSDK)** as an `xrt_display_processor`
implementation, shipping as `libdxrp050_leia_cnsdk.so`.

The runtime's `target_plugin_loader.c` Android branch (PR #309 / commit
`c96c93ce8`) discovers this `.so` at `xrCreateInstance` time via
`dladdr`-of-self + dirname enumeration of files matching the
`libdxrp<NNN>_<id>.so` convention. Bundle this `.so` into the runtime
APK's `jniLibs/<ABI>/` (single-vendor mode); multi-APK discovery is v2
(tracked at runtime #310).

## Files

| File | Purpose |
|---|---|
| `leia_cnsdk.{cpp,h}` | C++ wrapper around the CNSDK C ABI: core init, interlacer lifecycle, face-tracking worker thread, atlas weave entry point, device-config caching. |
| `leia_display_processor_cnsdk.{cpp,h}` | `xrt_display_processor` vtable wired to the wrapper. Advertises `is_self_submitting=true` so the compositor flushes its pre-DP cmd buffer + skips its own post-DP submit. Atlas mode only (per-tile blit removed — CNSDK splits the SBS atlas internally via `set_interlace_view_texture_atlas`). |
| `leia_plugin_android.c` | `xrtPluginNegotiate` entry point + `xrt_plugin_iface` vtable. `create_dp_vk` is the only non-NULL factory slot. |

## Build

```bash
# CNSDK 0.7.28 extracted somewhere on disk:
export CNSDK_ROOT=/path/to/cnsdk

# Configure for arm64-v8a Android via the NDK toolchain:
cmake -S . -B build-android \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29 \
    -G Ninja

cmake --build build-android --target dxrp050_leia_cnsdk
```

Output: `build-android/src/drv_leia_android/libdxrp050_leia_cnsdk.so`.

Copy that into your runtime APK's
`src/xrt/targets/openxr_android/src/main/jniLibs/arm64-v8a/` before
running `./gradlew :src:xrt:targets:openxr_android:assembleInProcessDebug`.

(End-to-end multi-module Gradle integration is a follow-up — see the
top-level repo README's "Android — POC build flow" section.)

## CNSDK convention assumptions

Three plug-in-side conventions are assumed in this POC (face axis
signs/units, tile-to-eye mapping, UV vertical flip). All three may
need single-line flips after first hardware bring-up — see
[`docs/cnsdk-android-calibration.md`](../../docs/cnsdk-android-calibration.md)
for the symptom-→-fix table.

## Status

POC — not yet validated on a Lume Pad. Compiles clean on the host.
First hardware install is gated on Lume Pad arrival; bring-up plan
lives in the runtime repo at
`docs/getting-started/android-bringup-checklist.md`.
