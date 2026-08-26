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

Use the script — it resolves the NDK toolchain, ninja, `CNSDK_ROOT`,
`DXR_RUNTIME_SOURCE_DIR` and `Eigen3_DIR`, all of which drift across
machines:

```bash
export CNSDK_ROOT=/path/to/cnsdk      # extracted CNSDK 0.10.54+ android tree
./scripts/build-android.sh            # -> libdxrp050_leia_cnsdk.so
./scripts/build-android.sh install-runtime-jnilibs   # + copy into the runtime APK
```

CNSDK is a **build-time dependency only**. The plug-in links the loader
shim (`CNSDK::leiaCore` → `libleiaCore-loader.so`), which at runtime
`dlopen`s `libleiaCore-impl.so` out of the **on-device** package — the
same shape as the Windows arm building against the SR SDK while the SR
runtime is installed separately.

Get it from the private LeiaInc repo (the public `leiainc.github.io`
copy is 0.7.28 and no longer works — 0.10.x moved to the loader
architecture this plug-in compiles against):

```bash
gh release download <tag> -R LeiaInc/CNSDK -p 'cnsdk-android-*.zip'
```

Output: `build-android/src/drv_leia_android/libdxrp050_leia_cnsdk.so`.

CI builds the same thing on every PR and attaches it to `v*` releases —
see [`.github/workflows/build-android.yml`](../../.github/workflows/build-android.yml).

## CNSDK convention assumptions

Three plug-in-side conventions are assumed in this POC (face axis
signs/units, tile-to-eye mapping, UV vertical flip). All three may
need single-line flips after first hardware bring-up — see
[`docs/cnsdk-android-calibration.md`](../../docs/cnsdk-android-calibration.md)
for the symptom-→-fix table.

## Status

Shipping. Validated on Lume Pad-class hardware and, since the panel
geometry moved off compiled-in constants, on non-Lume-Pad panels too
(measured on a 1080x2400 OLED phone). Built by CI on every PR and
attached to `v*` releases.
