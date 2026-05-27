# CNSDK C-ABI Surface (used by the Android plug-in)

Reference for everyone who touches `src/drv_leia_android/leia_cnsdk.cpp`.
Documents the **~30 CNSDK calls** the plug-in actually makes, plus the
init order, thread affinity, and undocumented behaviors that took
trial-and-error to figure out. Read this before changing CNSDK call
sites — most of these constraints aren't in the CNSDK headers.

Sourced from CNSDK 0.7.28 headers + the current plug-in implementation.
Bump the version note here when we move CNSDK.

## Init / teardown order

The strict timeline. Skipping a step or reordering produces silent
failures (NULL handles, async never completes, deadlocks). The
[`face_tracking_worker`](../src/drv_leia_android/leia_cnsdk.cpp) thread
runs steps 7–10 off the main thread.

```
1.  leia_platform_on_library_load()                     ← required, brackets all CNSDK use
2.  leia_core_init_configuration_alloc(CNSDK_VERSION)   ← VERSION macro is in <leia/common/version.h>
3.  leia_core_init_configuration_set_platform_android_java_vm(cfg, JavaVM*)
4.  leia_core_init_configuration_set_platform_android_handle(cfg, LEIA_CORE_ANDROID_HANDLE_ACTIVITY, jobject)
5.  leia_core_init_configuration_set_platform_log_level(cfg, kLeiaLogLevelTrace)
6.  leia_core_init_configuration_set_enable_validation(cfg, true)
7.  leia_core_init_async(cfg) → leia_core*               ← returns immediately; init runs async
8.  leia_core_init_configuration_free(cfg)               ← safe to free after init_async returns
9.  leia_core_set_backlight(core, true)                  ← does NOT block on init complete
        ─── on worker thread ────────────────────────────────
10.     while (!leia_core_is_initialized(core)) { sleep 50ms }   ← poll for async completion
11.     leia_device_config *cfg = leia_core_get_device_config(core)
12.     /* read cameraCenterX/Y/Z, displaySizeInMm, panelResolution */
13.     leia_core_release_device_config(core, cfg)
14.     leia_core_enable_face_tracking(core, true)       ← BLOCKING, NO CANCEL — can deadlock
15.     leia_core_start_face_tracking(core, true)
        ─── back to compositor / render thread ──────────────
16. leia_interlacer_init_configuration_alloc()           ← no version arg, unlike core
17. leia_interlacer_init_configuration_set_use_atlas_for_views(ic, true)
18. leia_interlacer_vulkan_initialize(core, ic, device, physDev,
        viewsFmt, targetFmt, depthFmt, swapchain_count) → leia_interlacer*
19. leia_interlacer_init_configuration_free(ic)
20. leia_interlacer_set_num_tiles(interlacer, 2, 1)      ← atlas layout, default but set explicitly
        ─── per-frame ───────────────────────────────────────
21.     leia_core_get_primary_face(core, slice)
22.     leia_interlacer_set_flip_input_uv_vertical(interlacer, bool)
23.     leia_interlacer_vulkan_set_interlace_view_texture_atlas(interlacer, atlasImage, atlasView)
24.     leia_interlacer_set_source_views_size(interlacer, w, h, true)
25.     leia_interlacer_set_shader_debug_mode(interlacer, LEIA_SHADER_DEBUG_MODE_NONE)
26.     leia_interlacer_vulkan_do_post_process(interlacer, w, h, false, fb, targetImg, NULL, NULL, NULL, 0)
        ─── teardown ────────────────────────────────────────
27. leia_interlacer_shutdown(core, interlacer)           ← needs BOTH handles
28. leia_core_shutdown(core)                             ← renamed from leia_core_release in 0.7.28
29. leia_platform_on_library_unload()
```

## Thread affinity

| Thread | CNSDK calls |
|---|---|
| Main (init/teardown) | 1, 2–9, 27, 28, 29 |
| `face_tracking_worker` (per-instance) | 10–15 |
| Render thread (per-frame, post-init) | 16–26, 21 (after worker sets `face_tracking_started` atomic) |

CNSDK doesn't document thread safety for `leia_core_get_device_config` /
`leia_core_release_device_config`. The plug-in serializes them onto the
worker thread, snapshots the values into atomic-guarded fields, and lets
the render thread read the cached values branch-free.

## Per-function notes

### `leia_platform_on_library_load` / `leia_platform_on_library_unload`
Process-global initialization for the CNSDK platform layer (logging,
TLS, JNI handles). **Must bracket every other CNSDK call.** No
return value. Calling twice without unload in between is undefined.

### `leia_core_init_async(cfg)` → `leia_core*`
Returns immediately. The actual init runs on an internal CNSDK thread.
Poll `leia_core_is_initialized()` until it returns true before calling
**any** of: `get_device_config`, `enable_face_tracking`, `on_pause`,
`on_resume`. `set_backlight` is the one exception — it's safe before
init completes.

Returns NULL on configuration error (wrong VERSION, missing JavaVM on
Android, etc.). Once you have a non-NULL core, init can still fail
asynchronously — `is_initialized` may never return true. Currently
there's no error surface for "init failed permanently"; we just keep
polling.

### `leia_core_is_initialized(core)`
Cheap. Called from the worker's poll loop at 20 Hz.

### `leia_core_set_backlight(core, on)`
Toggles the SR backlight. The plug-in calls it with `true` right after
`init_async` so the display switches to 3D mode as early as possible.
Doesn't block on init completion.

### `leia_core_get_device_config(core)` → `leia_device_config*`
Returns a heap-allocated config that **must be released** via
`leia_core_release_device_config`. Fields we read:

| Field | Type | Units | Used for |
|---|---|---|---|
| `cameraCenterX/Y/Z` | float | mm (camera frame) | Face position translation to display-relative meters |
| `displaySizeInMm[2]` | int | mm | XR_EXT_display_info physical dimensions |
| `panelResolution[2]` | int | px | XR_EXT_display_info pixel dimensions |

Thread safety undocumented. Serialized to the worker thread.

### `leia_core_enable_face_tracking(core, true)`
**Blocking. No cancel API.** Can deadlock if the device config or
camera handle is invalid. Audit ref: B10. Workaround: the plug-in's
`leia_cnsdk_destroy` runs a 2-second watchdog on the worker thread,
then detaches if it didn't join.

Returns bool — true on success. The plug-in logs and skips face
tracking if false; the rest of the SDK keeps working without it.

### `leia_core_start_face_tracking(core, true)`
Call AFTER `enable_face_tracking` returns true. No documented error
surface.

### `leia_core_get_primary_face(core, slice)` → bool
Returns the most recent face position in **millimeters relative to the
camera**. Convention left- vs right-handed is undocumented (audit B15);
treat as a `setprop`-tunable knob — see
[`cnsdk-android-calibration.md`](cnsdk-android-calibration.md).

Returns false if no face detected this frame. The plug-in passes
through to `xrLocateViews` as "use last known good" or default-centered
view if false.

### `leia_core_on_pause(core)` / `leia_core_on_resume(core)`
Required for Android lifecycle correctness. Safe no-op if the core
isn't initialized yet. The plug-in wires both to the Activity's
`onPause` / `onResume` via the runtime's DP vtable's new
`on_pause` / `on_resume` slots (added in runtime PR #268).

### `leia_core_shutdown(core)`
**Renamed in CNSDK 0.7.28.** Earlier versions (0.6.x) had
`leia_core_release` with the same semantics. If you bump CNSDK to a
later version, double-check this name didn't move again.

Must be called AFTER `leia_interlacer_shutdown` if an interlacer was
created — the interlacer holds a back-reference into the core.

### `leia_interlacer_init_configuration_alloc()`
Note: no `CNSDK_VERSION` arg, unlike the core variant. Allocates the
config; must be freed.

### `leia_interlacer_init_configuration_set_use_atlas_for_views(ic, true)`
Selects atlas mode (CNSDK splits the SBS atlas internally). The
plug-in always uses atlas mode — per-view-blit mode was removed
post-#270.

### `leia_interlacer_vulkan_initialize(core, ic, device, physDev, viewsFmt, targetFmt, depthFmt, swapchain_count)` → `leia_interlacer*`
**One-shot.** If this returns NULL, the plug-in sets
`interlacer_init_failed = true` and stops retrying. Typical NULL
causes:
- VkPhysicalDevice missing required extension (see [VK survey](../../docs/getting-started/android-vulkan-extension-survey.md))
- Memory pressure (CNSDK allocates several MB of staging buffers)
- CNSDK / runtime version mismatch (e.g. CNSDK ABI churn)

Plug-in defaults: `viewsFmt = VK_FORMAT_B8G8R8A8_UNORM` (matches the
runtime's atlas render target), `depthFmt = VK_FORMAT_D32_SFLOAT`,
`swapchain_count = 3`.

### `leia_interlacer_set_num_tiles(interlacer, cols, rows)`
Atlas layout. Plug-in passes (2, 1) for stereo SBS. Default is
already 2x1 in atlas mode but we set explicitly so multi-view layout
changes are one-line.

### `leia_interlacer_set_flip_input_uv_vertical(interlacer, bool)`
Whether to flip V coordinate during interlacing. Plug-in default is
true (assumes CNSDK uses GL Y-up convention and Vulkan NDC is Y-down).
`setprop`-tunable as `debug.dxr.leia.flip_uv`. Audit ref: B18.

### `leia_interlacer_set_source_views_size(interlacer, w, h, isHorizontalViews)`
Tells CNSDK the dimensions of the atlas. Called per-frame because the
SBS atlas can resize when the canvas resizes. `isHorizontalViews=true`
= 2x1 SBS layout.

### `leia_interlacer_set_shader_debug_mode(interlacer, mode)`
Plug-in always passes `LEIA_SHADER_DEBUG_MODE_NONE`. Other modes
(`_INTERLACED_VIEW`, `_DIRECT_VIEW0` etc.) exist for debugging but
aren't wired into the calibration knobs yet.

### `leia_interlacer_vulkan_set_interlace_view_texture_atlas(interlacer, atlasImage, atlasView)`
Per-frame: hands CNSDK the VkImage + VkImageView of the just-rendered
SBS atlas. CNSDK splits internally per the `set_num_tiles` /
`set_source_views_size` config. The plug-in doesn't manage per-view
images at all (post-atlas-mode rewrite).

### `leia_interlacer_vulkan_do_post_process(interlacer, w, h, useStencilBlt, fb, targetImage, optView, optColorImage, optDepthImage, flags)`
Per-frame: executes the weave shader. Outputs to `fb` (framebuffer) +
`targetImage`. Plug-in passes NULL for the three optional args and 0
flags.

### `leia_interlacer_shutdown(core, interlacer)`
Needs BOTH handles. Call before `leia_core_shutdown`.

## Tile-to-eye mapping

The plug-in assumes atlas column 0 = left eye, column 1 = right eye.
CNSDK's interlacer treats view 0 and view 1 as left and right per
common convention but this isn't explicit in the headers (audit B17).

**Not** runtime-tunable today — atlas mode doesn't expose a swap
parameter. If Lume Pad shows the mapping inverted, the fix is either
swapping the columns in the runtime's atlas blit (
`comp_vk_native_renderer_draw`) or switching back to per-view-blit
mode in the plug-in.

## Version drift watch-list

When bumping CNSDK, sanity-check these in particular:

| Symbol | Last seen at | Reason to recheck |
|---|---|---|
| `leia_core_shutdown` | 0.7.28 | Renamed from `leia_core_release` in 0.7.x; could rename again |
| `leia_interlacer_vulkan_initialize` signature | 0.7.28 | 0.10.56 had a different API for the same architectural idea |
| `leia_core_init_configuration_alloc(CNSDK_VERSION)` | 0.7.28 | Version-check arg — CNSDK may reject mismatched build vs runtime |
| `leia_core_enable_face_tracking` blocking behavior | 0.7.28 | No cancel API today — if CNSDK ever adds one, drop our 2-second watchdog detach |
| `leia_interlacer_init_configuration_alloc()` no-arg | 0.7.28 | Differs from core variant; could pick up a version arg |

## Why this matters pre-Lume-Pad

Three things take real Lume Pad time to figure out: thread affinity
surprises (deadlocks), init-order surprises (NULL handles), and ABI
drift surprises (silent rename). This doc compresses each into a
~30-second lookup.
