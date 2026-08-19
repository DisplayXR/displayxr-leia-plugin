# DisplayXR on Android (Leia CNSDK): weaving, orientation, zones & transparency

Developer notes for the **Android** Leia display processor (`src/drv_leia_android/`), which
weaves through the prebuilt **CNSDK** interlacer (`leia_interlacer_*`) rather than the
Windows SR SDK. These are the non-obvious, hard-won facts a DisplayXR developer needs before
touching the Android weave, zone framing, or transparency paths.

For the Windows/SR-SDK weaver see [weaver.md](weaver.md); for the CNSDK C ABI see
[cnsdk-c-abi-surface.md](cnsdk-c-abi-surface.md); for per-unit calibration see
[cnsdk-android-calibration.md](cnsdk-android-calibration.md).

---

## 1. How CNSDK weaving actually works (the one fact that explains everything else)

The CNSDK interlacer (`leia/interlacing/assets/shaders/interlace_oled.pshader` in the CNSDK
source) does **not** decide "left vs right view" in eye space. For every **color subpixel** it
computes a scalar **phase ∈ [0,1)** — the subpixel's position inside the repeating lightfield
period — and a soft window (`smoothbox`) maps that phase to a view band:

```
phase = phc
      + (x + sl*y) / px / pixelPitch          // (A) static optical ramp  — LINEAR in panel position
      + sign * q_full * ((faceX-x) + (faceY-y)*sl)   // (B) eye-dependent parallax (predicted CENTER eye)
      + residualPhaseMap                       // (C) per-cell calibration residual
phase = fract(phase)
```

Three consequences every Android integrator must internalize:

1. **Weaving is per-subpixel.** `phase` is computed independently for R, G, B (each color
   subpixel has a different physical position `p0_red/green/blue`). A single output pixel can draw
   **R from one view and G/B from another**. This is the root cause of the transparency limit in
   §4 — keep it in mind.
2. **The weave is driven by a single *predicted center eye*** (`_faceX/Y/Z`), not by explicit
   left/right eye coordinates. The optics (slant `sl`, period `px`, `donopx`, phase center `phc`)
   fan that one point into N views. `_leftEye/_rightEye` uniforms exist but are **calibration-mode
   only**.
3. **The phase depends on the subpixel's true physical position on the panel** (`x,y` in mm,
   derived from the viewport mapping `scrnUV = lerp(_viewportP0,_viewportP1,screenUv)`). If you draw
   content into a sub-region of the panel you **must** tell CNSDK where that sub-region physically
   sits, or the lenticular phase is wrong → the 3D registers shifted. See §3.

Calibration inputs (`px`, `sl`, `donopx`, `phc`, `n`, ACT kernels, correction maps) come from the
device's per-unit `cellCalibration` (config v4) and are **stored and consumed in the panel's
natural orientation** — never re-derived per orientation.

---

## 2. Three orientations — and which one weaving lives in

CNSDK tracks **three independent orientation values**. Confusing them is the most common Android
bug. The enum is a 4-step clockwise ring (`LANDSCAPE=0, PORTRAIT=1, REVERSE_LANDSCAPE=2,
REVERSE_PORTRAIT=3`); `GetRelativeClockwiseAngle(from,to)` returns the 90°-step delta.

| Value | Meaning | Changes at runtime? | Nubia Pad 2 / NP02J |
|---|---|---|---|
| `deviceNaturalOrientation` | Orientation the panel was **calibrated in / weaves in** (vendor-defined) | No | **Portrait** |
| `cameraSensorOrientation` | How the eye-tracking **camera is physically mounted** | No | **Landscape** |
| `deviceOrientation` | How the user is **currently holding it** | **Yes** | live |

**Weaving is always done in `deviceNaturalOrientation` space** (CNSDK comment: *"Interlacing is
done in the natural orientation space"*). The runtime rotates everything else to meet it:

- **Tracked eye → natural (static, camera-mount fix).** CNSDK rotates every deprojected eye point
  by `GetRelativeClockwiseAngle(cameraSensorOrientation, deviceNaturalOrientation)` about the
  optical Z axis, once, in `FaceTracker::UpdateConfiguration`. So the eye position the weave sees is
  **already in the natural/weaving frame**, regardless of how the camera is bolted on. On NP02J this
  is the `LANDSCAPE→PORTRAIT = 90°` term.
- **Detector input image → current (live).** A *separate* per-frame angle
  `(cameraSensorOrientation → deviceOrientation)` rotates only the 2D image fed to the face
  detector. It never touches the 3D point.
- **Weave pattern → current.** The interlacer applies the third delta
  `(deviceNaturalOrientation → deviceOrientation)` itself (`interlaceParams.deviceRotationAngle`);
  on AGSL this is the `_deviceRotation` shader scalar, on desktop it's baked into the viewport
  matrices.

**Takeaway:** hand CNSDK eye coordinates in the **natural** frame and viewport/zone data in the
**current** frame — it reconciles the two. Don't pre-rotate the eye position yourself.

---

## 3. Sub-rect / display-zone weaving (the two-knob model)

When 3D content is confined to a **canvas sub-rect** (e.g. the avatar's bottom-75% band via
`XR_DXR_display_zones`), you are weaving into part of the panel. CNSDK exposes **two decoupled
knobs**, and you must set **both**:

| API | Feeds | Effect |
|---|---|---|
| `leia_interlacer_set_viewport(posX,posY,w,h)` | `mViewportXPosition…` → GPU render-target viewport | **Pixel placement only.** Where the woven output lands in the target. **Does NOT touch the phase.** |
| `leia_interlacer_set_viewport_screen_position(x,y)` | `mViewportXScreenPosition/Y` → `_viewportP0/P1` → phase math | **Phase reference.** The on-panel origin the lenticular interlace is referenced to. Defaults to `(0,0)`. |

The trap: `set_viewport` alone positions the pixels but leaves the phase referenced to the panel
origin, so a band drawn 25% down the panel weaves at the **full-panel phase** and the 3D registers
shifted (this was leia-plugin bug **#53**). The fix (`leia_cnsdk_weave`) sets the screen position to
the band's panel origin alongside the placement viewport.

**The screen position is `window origin + zone offset` (#150).** It used to be reset to `(0,0)` on
full-target frames, on the assumption that the render target always *is* the panel. With
per-window compositor instances (ADR-036, runtime#1033) that assumption is dead: a full-target
frame inside a window that sits at `1280,60` still needs a real origin, and zeroing it there is
precisely what collapsed the 3D in the right-hand window of a side-by-side pair. The window rect
arrives through the DP slot `xrt_display_processor_vk::set_window_screen_rect` (see §3a) and
becomes the **base** every zone offset is added to; a prior band's phase can no longer leak
because the zone offset is recomputed from the base each frame.

This is the CNSDK analog of the Windows SR path's `xOffset = window_WeavingX + vpX`. It is a
**first-class, intentional CNSDK feature**, not a workaround.

**Assumption that makes "pass the same x,y to both" correct:** the render target is the **full panel
surface mapped 1:1 to the current-orientation panel at origin (0,0)** (true for the Android OOP
session framebuffer). If the target were ever offset or sub-panel, the screen position would have to
be panel-relative and would diverge from the placement viewport.

### 3a. The window origin (`set_window_screen_rect`) — per-window weave phase

One compositor instance weaves into **one window**, so the phase must be referenced to where that
window physically sits on the panel. The runtime reports it through the append-only VK DP slot
`xrt_display_processor_vk::set_window_screen_rect(x, y, w, h, display_id)`
(runtime#1033, this repo #150, ADR-036 D6); `leia_cnsdk_set_window_screen_rect` caches it and
`leia_cnsdk_weave` uses it as the base:

```
set_viewport(vp_x, vp_y, vp_w, vp_h)                      # placement, render-target relative
set_viewport_screen_position(win_x + vp_x, win_y + vp_y)  # phase, panel absolute
```

Three things to know:

- **Android forces this.** A pure window *move* raises no resize
  (`WindowFrames.didFrameSizeChange` compares w/h only) and SurfaceFlinger repositions the layer
  with the *old* buffer, so nothing in the graphics path observes it. The client samples
  `View.getLocationOnScreen()` from a `Choreographer` callback instead. Windows solves the same
  problem inside the weaver by subclassing the HWND; Android exposes no such hook, which is why
  the runtime reports.
- **Do not pre-rotate.** Coordinates arrive in **current**-orientation screen space and go to
  CNSDK unrotated — `interlacer.cpp` states "the user provides data in the current orientation
  space, we convert it to the natural one", and CNSDK's own
  `InterlacedSurfaceView._updatePosition` passes `getLocationOnScreen` straight through.
- **Do not snap.** Phase, including any grid snapping, is the weaver's (ADR-033). The runtime
  reports raw geometry on purpose.

Never calling the slot leaves the base at `(0,0)` — display-scoped weaving, bit-identical to the
pre-#150 behaviour — so an older runtime, or a single full-screen window, is unaffected.

### Portrait/landscape gotcha (read before trusting a "verified" zone)

CNSDK *is* designed to rotate the screen position from current → natural orientation
(`GetTransforms`: it swaps width/height for 90/270, rotates by `-deviceRotationRadians`, and applies
a bottom-left Y-flip). **But on NP02J portrait *is* the natural orientation, so any portrait test
runs with `deviceRotationAngle == 0` — which makes that whole rotation path the identity.** A
zone that looks perfect in portrait has therefore **not exercised the rotation/Y-flip/​dimension-swap
logic at all.** Landscape is where it does real work and is the prime suspect for a residual shift.
When validating sub-rect weaving, **test landscape explicitly** and confirm:

1. the runtime hands the band rect in **current-orientation (landscape) target pixels**, not a
   portrait rect;
2. CNSDK's `deviceRotationAngle` is correct on the OOP landscape path;
3. the bottom-left Y-flip combined with the 90/270 dimension swap lands the band where you expect
   (a `viewportSize.y`-sized shift or a mirror = wrong coordinate convention upstream).

---

## 4. Transparency: why per-pixel alpha can't fully work on Android

DisplayXR's see-through avatar needs the live screen to show through the transparent regions of a
woven 3D image. The Windows DP does this by **capturing the background (WGC) and compositing it into
each view *before* weaving** ([transparency.md](transparency.md)) — the output is opaque and a simple
binary mask handles the flat regions. Android has no equivalent, and the reason is fundamental.

**The limit.** The weave assigns views **per subpixel** (§1), but an RGBA buffer carries **one alpha
per pixel**, and every OS compositor (SurfaceFlinger) blends `out.c = src.c + bg.c·(1−a)` with that
single `a` shared across R, G, B. So:

- **All views transparent at a pixel** → `a=0` is correct (live screen shows through). ✅
- **All views opaque** → `a=1` is correct. ✅
- **Mixed pixel** (some subpixels from a transparent view, some from an opaque view — i.e. the avatar
  **silhouette / parallax de-occlusion band**) → the required alpha differs per channel
  (`a_r=1`, `a_g=0`, …). **No single `a` can be correct.** ❌

This is a **representational** limit, not an algorithmic one. The only true fixes are (1) a per-channel
alpha format + compositor (doesn't exist), or (2) resolve transparency **before** the per-subpixel
collapse by compositing the background into each view pre-weave (the Windows model). **CNSDK exposes
no per-pixel alpha** — `leia_interlacer_set_alpha` is a *global* overlay-blend — so the weave output
is opaque RGB.

### What the Android DP does today (and why it's a palliative, not a fix)

`src/drv_leia_android/shaders/alpha_gate.frag` runs **post-weave** and reconstructs a per-pixel alpha
by matching the woven RGB against each atlas tile and emitting that view's alpha (mode 1, the default;
`debug.dxr.alphagate` toggles A/B). It is **good enough** because the mixed band is only as wide as
the avatar's silhouette **disparity** (≈1px near the screen plane; wider with strong pop-out), but it
**cannot** make mixed pixels correct. Do not expect more from gate tuning — see the analysis in
DisplayXR runtime issue **#568**. The honest stopping point is: ship the gate, accept a thin
silhouette artifact at high pop-out.

### Can we capture the background on Android (port the Windows model)?

Only via a **privileged** path, and not from our code today. Findings on NP02J (Android 13):

- The public **MediaProjection** API captures the *composited* display **including our own avatar
  overlay** → feedback loop (image converges to black). There is no public way to exclude your own
  layer on the default display, and Android 13 lacks the Android-14 single-app-capture escape. **Leia's
  own on-device capture PoC (`com.leia.mediaprojectionpoc`) uses MediaProjection** — i.e. even Leia
  fell back to the consent route.
- The correct mechanism is **`SurfaceControl.captureDisplay` + `LayerCaptureArgs.setExcludeLayers`**
  (exclude our avatar layer → true background, no feedback). It needs `READ_FRAME_BUFFER` /
  `CAPTURE_VIDEO_OUTPUT` / `ACCESS_SURFACE_FLINGER`, all **`signature`-level**.
- **"System app" is not enough.** On NP02J those perms are held *only* by platform-signed
  ZTE/Qualcomm services (`cn.nubia.gamelab`, `com.qualcomm.wfd.service`), all signed with the platform
  key. Every Leia/CNSDK service (`com.leialoft.display.config`, `com.leia.headtrackingservice`,
  `com.leiainc.media.service`, `LeiaViewerK68`) is pre-installed as a **SYSTEM** app but signed with a
  **Leia** key, not the platform key, and holds **zero** capture permissions. So the CNSDK device
  service **cannot** be leveraged for capture as-is.
- **Path to the true fix:** an OEM ask to ZTE to platform-sign (or `/system/priv-app` + a
  `privapp-permissions` allowlist + sepolicy grant) a small Leia capture service that does
  `captureDisplay(excludeLayers=[avatar])` and ships frames to the runtime over the existing zero-copy
  `AHardwareBuffer`-over-IPC primitive. The captured background is at the panel plane (zero
  inter-view disparity), so you composite **one** image identically into all views — even simpler
  than Windows. The device proves the capability exists; only the signing/sepolicy gate (ZTE's to
  open) stands in the way.

---

## 5. The 3D backlight is DISPLAY-GLOBAL — and `on_pause` cannot express "not right now"

The panel's switchable 3D backlight is **not** per-app state. On this hardware it is held by
`BacklightMultiClientControlService` as a binder **bind-refcount across processes**: the first
client to bind turns 3D on, the last to unbind turns it off. The plug-in resolves to that
multi-client service (tier 1) and never to the legacy `BacklightControlService`, which forces
2D and `stopSelf`s on unbind.

`leia_core_enable_3d(false)` does **both** things at once — it drops the global backlight *and*
unbinds this process. So a single compositor instance calling it from `leia_cnsdk_on_pause`
takes the refcount to 0 and flattens the panel **underneath a sibling app that is still
weaving**. Two producers reach `on_pause` on the Android OOP path, and neither is Activity
lifecycle: surface loss (`MonadoView.surfaceDestroyed` → `clearAppSurface` →
`android_window_transition_locked`) and `multi_compositor_end_session`. A multi-window relayout
genuinely destroys and recreates an app's `SurfaceView` surface, so the pause is legitimate —
what is wrong is taking the whole display down with it.

### `debug.dxr.multiapp` — opt in to holding the bind

| Property | Default | Effect |
|---|---|---|
| `debug.dxr.multiapp` | `0` (off) | `1` ⟹ `leia_cnsdk_on_pause` **skips** `force_backlight_2d`: this process keeps its bind (its share of the refcount) and the panel stays 3D. `leia_cnsdk_destroy` still forces 2D, so it remains the sole release path. |

```bash
adb shell setprop debug.dxr.multiapp 1     # before launching the apps
```

Read once via `__system_property_get` and cached, so it must be set before the first
`on_pause`. With the property unset, single-client behaviour is byte-identical to before.

**Why this is opt-in and not the Android default.** Holding the bind is unambiguously right when
another window is still visible and weaving — but `on_pause` cannot tell that case apart from
"this is the only app and it just went to the background". In the single-app case the
compositor process survives as a foreground service, `destroy` never runs, and the panel would
sit in 3D over the launcher or over an ordinary 2D app: exactly the stuck-3D regression the
force-2D exists to prevent (runtime #563).

The clean fix is a *preference* rather than a bind: each client says "bound, but I want 2D right
now" and the service aggregates. The service has no such API today — it is a binary
bind-refcount (limitation **L2**) — and the runtime side does not yet distinguish "my window is
hidden" from "my window is one of N visible" (DisplayXR/displayxr-runtime#1039, #1038). Until
one of those lands, the safe default is the old behaviour and the property is how a
multi-window run opts out of it.

---

## 6. Quick reference — pitfalls checklist

- **Don't pre-rotate the tracked eye.** Hand CNSDK eyes in natural orientation; viewport/zone data in
  current orientation. It reconciles them (§2).
- **Sub-rect weave needs both viewport knobs.** Placement (`set_viewport`) **and** phase
  (`set_viewport_screen_position`). The phase base is the **window origin**, not `(0,0)` — do not
  reset it on full-target frames (§3, §3a).
- **A portrait-only zone test proves nothing about rotation** on a portrait-natural panel — the
  rotation path is identity. Test landscape (§3).
- **Per-pixel-alpha transparency is fundamentally lossy** at avatar silhouettes; the alpha-gate is a
  palliative. The true fix is privileged background capture, which needs an OEM-signed service (§4).
- **Never force the backlight to 2D on behalf of the whole display from a per-window pause.** The
  backlight is display-global and refcounted across processes; `debug.dxr.multiapp=1` holds the
  bind instead, and `destroy` stays the release path (§5).
- **Any view-keyed Vulkan resource cache on Android OOP must also key on (width,height)** — Adreno
  recycles destroyed view handles across portrait⇄landscape, so a view-only key returns a stale
  wrong-dimension resource (this bit the alpha-gate framebuffer cache; fixed by keying on dims).
