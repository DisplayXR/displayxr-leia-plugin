# Android stereo camera source

The Android arm fills the runtime's stereo-camera slots (runtime ADR-043,
`XR_DXR_stereo_camera`) from the tablet's own stereo camera pair. The runtime
stays vendor-neutral. It rectifies, converts, transports, applies consent and
fans frames out. Everything Leia- or OEM-specific lives here:

- the logical camera ids,
- the factory calibration block,
- the hand-over of head tracking.

Code:

- `src/drv_leia_android/leia_stereo_camera_android.{h,cpp}`: NDK Camera2 source and the six slots.
- `src/drv_leia_android/leia_stereo_camera_android_calib.{h,c}`: factory-block parse and stream conversion. Pure C, host-tested by `tests/test_stereo_camera_android_calib.c` (Linux CI).
- `leia_cnsdk_ext_tracking_*` in `leia_cnsdk.cpp`: in-app tracking hand-over.

Design and the measurements behind it: runtime
`docs/roadmap/stereo-camera-android.md` (§11 A0 results, §12 plan).

## Cameras

| Index | Camera | Flags | Frames |
|---|---|---|---|
| 0 | front pair ("3D Camera") | `USER_FACING`, `SHARED_WITH_EYE_TRACKING`, `CALIBRATED` when the block parses | 2 × 1280×720, SBS NV12 (2560×720) |
| 1 | rear pair ("3D Camera, rear") | `CALIBRATED` when the block parses | same |

**Discovery.** The pairs are Camera2 *logical* multi-cameras that are hidden
from the camera id list. The source probes ids 0–15 and keeps the first logical
camera with two physical lenses per facing:

- Lume Pad 2 / nubia Pad 3D II: front = 5 (lenses 1, 3), rear = 4 (lenses 0, 2).
- Phone-class panels expose a different id; the scan finds it.

No id is hard-coded.

**Streams.** The source configures one `YUV_420_888` output per physical lens
at the sensor's 4:3 aspect, 1280×960 for a 1280×720 eye, and nothing else:

- **Eyes:** each eye is the centre 1280×720 crop of its lens's image.
- **Tracking frame:** the main lens's image, 2×2-box-decimated to 640×480. That
  is the whole sensor field of view, which is what the vendor tracking service
  feeds its own detector, at the same intrinsics.

This configuration is forced by two measured constraints:

- **No CPU-readable PRIVATE reader.** The NDK refuses one:
  `AImageReader_newWithUsage(PRIVATE, CPU_READ_OFTEN)` returns
  `AMEDIA_ERROR_INVALID_PARAMETER`. So a CPU NV12 path needs YUV eyes.
- **No second YUV output on a lens.** It aborts the camera HAL; the A0 probe
  measured this with 1280×720 + 640×480.

The vendor camera app's configuration (GPU preview eyes plus a 640×480 YUV
tracking stream) suits the GPU / AHardwareBuffer path (runtime phase A4), not
this one.

**Timestamps.** The sensor clock is `CLOCK_BOOTTIME` (timestamp source
`REALTIME`). Published frames carry the start-of-exposure time converted to
`CLOCK_MONOTONIC` per frame (`time_is_exposure = true`). CNSDK gets BOOTTIME,
which is the clock its "system" timestamps use.

## Factory calibration block

The OEM publishes the stereo calibration as a vendor characteristics key. It
sits on the logical camera and on the **main** lens only: `…eeprom3d_front`
for the front pair, `…eeprom3d_rear` for the rear. The prefix is overridable
with `debug.dxr.leia.scam_calib_key_prefix`.

Vendor keys are not listed by `CameraCharacteristics.getKeys()`. The source
therefore builds the key by name over JNI, the same way an app would.

**When the block can be read (#505).** An empty read is not proof that the
block is absent. A read is final only when the host JavaVM / Context exists
(`leia_cnsdk_get_jni`), the process holds `android.permission.CAMERA`, and
`getKeys()` listed the characteristics of the logical id and both lenses.
Without the permission, the camera service strips the permission-gated vendor
keys from the characteristics. The keys then list without the block although
the HAL publishes it. This was measured on a nubia Pad 3D II: a fresh runtime
install drops the CAMERA grant, and the runtime asks for it again only at the
first stream's consent. Until a read is final, the pair stays **unlisted**:

- no permission: re-checked on every enumerate, with no bound;
- no JNI context: retried for up to 30 s;
- a characteristics call that threw: retried for at most 3 reads.

Each reason logs one WARN (`factory block read deferred: ...`). The pair is
then described once and appears as a NEW camera. The runtime keeps a camera's
first description until the service restarts, and `device_identity` carries
the block's hash, so a camera is never listed raw and upgraded later. The
runtime then fetches the calibration and sends
`XrEventDataStereoCamerasChangedDXR`, and the browser lists "3D Camera".
While any pair is pending, none is listed, so indices stay stable.

**Calibration source, in order** (one WARN per pair names the source that
won and why each higher one was skipped, and again whenever it changes):

1. **The served display config record**: the `stereoCalibrations` entry of
   the pair in the display config the display-config service
   (`com.leialoft.display.config`, `/sdcard/displayConfig.json`) serves,
   as CNSDK (>= 0.10.91) parses it (`leia_device_config_get_stereo_calibration`,
   looked up at run time; the struct is mirrored with CNSDK's layout
   asserts). The first CNSDK core of the process snapshots it when it reads
   its device config (`CNSDK display config stereoCalibrations: ...` WARN).
2. **The calibration app's provider** (`content://com.leiainc.calibrationapp.stereo/pairs/<logical id>`,
   column `json`, schema 1, keyed to the block's SHA-256) or its cached
   copy: R only, as before. Only a fallback now: the OEM's auto-launch policy
   blocks a cold start of that provider, so it answers only while the
   calibration app is alive, and a shipped camera cannot depend on it.
3. **The factory block.**
4. Nothing: raw only, an ideal parallel pair downstream.

**Which served entry, and what it replaces.** The entry is matched by
`pair.logicalCameraId` (the last entry of an id wins, as in CNSDK), then by
`pair.lensIds` against the block's main and sub lens: `[main, sub]` as is,
`[sub, main]` with the two lenses swapped and the extrinsics inverted
(R ← Rᵀ, T ← −Rᵀ T), no ids = `[main, sub]` (the writer puts the main lens
first), other ids refused. Tablet front: `["1","3"]`, lens0 = camera 1 =
main, lens1 = camera 3 (the tracking lens) = sub. Phone front: `["1","0"]`,
main = camera 1. R and T keep the record's meaning, `x1 = R·x0 + T` in
each sensor's native (unturned) OpenCV frame, so lens0/lens1 is the block's
main/sub and the SBS left eye is decided afterwards from the geometry,
exactly as for the block.

Two writers fill the section (leia-calibration-suite `StereoConfigSection`),
and CNSDK serves neither `source` nor `method`, so the block SHA-256 tells
them apart:

- **mirror record** (`source: "mirror"`, `factory: null`): both lenses FITTED
  at the pair streams' size (`imageSize`, 1280x960 on the tablet), R and a
  metric T solved jointly with them. It replaces the **whole lens model**
  (intrinsics, distortion, R, T). Taking only its R and T on top of the
  factory intrinsics would mix two solutions: R trades off against the
  principal points in a fit, and T's scale against the focal lengths.
- **user record** (`source: "user"`, keyed to the block's SHA-256):
  R re-estimated against the factory intrinsics. It replaces **R only**, and
  only on the block it was made against (as the provider's record does).

The mirror record's intrinsics go to the active array through the HAL's
array → stream step inverted per axis: the 1280x960 stream is the centred
region of its aspect (the whole 3264x2448 front array; 4160x3120 of a
4208x3120 rear array), `s_x = width / region.w`, `s_y = height / region.h`,
pixel-centre aware like the record (OpenCV): `c_array = x0 + (c + ½)/s − ½`.
The two ratios are equal up to the stated size's rounding; scaling the record
straight to the whole array (4208/1280 vs 3120/960) would stretch it by
1.2 %. From there the usual pipeline brings it to each stream, so at the
1280x960 capture the eyes carry exactly the record's numbers. A mirror record
whose eye order contradicts the HAL's lens poses is refused (never silently
mirrored); one whose R is more than 5° from the block's is refused; without a
factory block a mirror record still calibrates a side-by-side pair.

**When a new calibration is seen.** CNSDK reads the device config once, at
core init. A calibration saved later is seen after the next start of the
process that hosts the plug-in (the runtime service). Until the first core of
the process has read its config, the pair uses the provider / factory and
re-checks on every enumerate. The runtime keeps a camera's first calibration
until it re-reads it, so a record that appears after the camera was first
described reaches clients after the next camera open or service restart.
`debug.dxr.leia.scam_served_calib=0` ignores the served record.

The rear pair has no entry today: it stays on the factory block, and the log
says so (`served display config record skipped (the served display config has
no stereoCalibrations entry for logical 4)`).

Layout, version 1. Byte 0 is the version. Every value after it is a
little-endian IEEE-754 double:

| Bytes | Content |
|---|---|
| 1–128 | main lens: fx fy cx cy k1 k2 p1 p2 k3 k4 k5 k6 s1–s4, pixels at the reference size (usually the full active array, see below) |
| 129–256 | sub lens, same order |
| 257–328 | R (3×3, row-major) |
| 329–352 | T (mm) |
| 353– | (optional) a precomputed rectification, ignored |

**Convention.** `x_sub = R · x_main + T`, as in OpenCV `stereoCalibrate` with
camera 1 = main.

**Eye order.** In the PUBLISHED frame (after any turn), the lens whose
centre lies at −x of the other, i.e. on the camera's own left, is the SBS left
eye. On the tablets' front pair that is the sub lens (T = (+15, 0, 0) mm). The
source hands the runtime `R_rl`/`T_rl` in its `x_right = R_rl · x_left + T_rl`
convention.

The block's convention is not the same on every module: a tablet's block is
`x_sub = R · x_main + T`, a phone-class front module's is
`x_main = R · x_sub + T` (its factory tool took the sub lens as stereoCalibrate's
first camera). The block cannot tell the two apart, so the HAL's own lens
poses decide when both lenses publish them (`ACAMERA_LENS_POSE_TRANSLATION`,
Android sensor-coordinate axes: x right as the user sees the screen, y up). The
device-frame offset sub − main goes to the upright camera image (x mirrored for
a front lens, which faces the user; y flipped because image y points down), then
by (turn − sensor orientation) clockwise into the published frame
(`leia_scam_pose_sub_side`). When that puts the sub lens on the other side from
the block, the block is re-read in the other convention (R ← Rᵀ, T ← −Rᵀ T,
`leia_scam_factory_orient_by_pose`) before anything is derived from it, and one
WARN says so. No poses, poses at the origin, a reference that cannot be
compared (UNDEFINED / AUTOMOTIVE), or an offset more than 45° off the published
x axis: the block decides alone, as before.

Measured on the phone-class device: lens 0 (sub) sits at (−15.3, −0.75) mm of
lens 1 (main), i.e. on the user's left, the front camera's own right, so main
is the left eye. The block read as `x_sub = R · x_main + T` said the opposite,
and the depth was inverted. The scan log prints the EFFECTIVE order and what
decided it (`SBS left = main lens 1 (HAL lens poses; block convention
swapped)`). `debug.dxr.leia.scam_swap=0|1` still forces the order;
`debug.dxr.leia.scam_pose=0` ignores the poses.

**Main lens.** On the tablets the main lens is the one the vendor tracking
service tracks from: its intrinsics match the service's logged 640×480 values
(fx 338.6, fy 345.6).

**Tracking lens.** CNSDK tracks from the display config's `trackingCameras[0]`
(or `debug.cnsdk.cam`). Its C API before 0.10.89 only indexes `cameras[]`,
whose order is not the tracker's (phone-class: `cameras[0]` = "0",
`trackingCameras[0]` = "1"). So the in-app tracking lens, and the cores' camera
offset for the frame-listener face fallback, are: CNSDK's
`leia_device_config_get_tracking_camera_data` when the loader has it (looked up
at run time, CNSDK PR #763); else the `cameras[]` entry of the factory main lens
(WARN); else `cameras[0]` (WARN), with the in-app feed then on the factory main
lens. `debug.dxr.leia.scam_track_lens=<id>` forces the feed.

**In-app tracking intrinsics.** The vendor tracking service is calibrated on
its OWN capture of the tracking camera (the config's `calibResolution`, e.g.
640×480 at 90 fps), and that capture can be a narrower cut of the sensor than
ours: on the phone-class front lens the 90 fps mode is 2288×1284 (2×2-binned
4576×2568), so its 4:3 640×480 spans 3424 sensor columns, while our 1280×960 at
30 fps spans 4192 (mode 4192×3144). Our 640×480 then sees a field 1.224× wider.
So each core feeds its in-app tracker the tracking camera's CONFIGURED
intrinsics re-expressed for our frame (`leia_scam_track_intrinsics_from_config`:
f / ratio, principal point scaled about the centre; phone: fx 350 → 285.9),
whenever the core knows that camera's intrinsics and it is the lens we feed.
Otherwise it falls back to the factory block's values, with a WARN. The ratio
(`leia_scam_track_fov_ratio`) comes from the HAL's sensor-mode table
(`org.quic.camera2.sensormode.info.SensorModeTable`): each stream is served by
the largest mode that runs at its rate, at the largest integer bin that fits
the array, and the stream is the largest centred cut of its aspect. With no
table the ratio is 1, because Camera2 promises every stream the same field.
`debug.dxr.leia.scam_track_fps` (default 90) is the tracker's rate, and
`debug.dxr.leia.scam_track_fov_ratio` overrides the ratio. Each choice is
logged as one WARN.

**Stream conversion.** A 16:9 stream from the 4:3 array is the array scaled to
the stream width, with the rows centre-cropped:
`cy' = (cy − H/2)·s + h/2`.

**Reference size.** The block is usually expressed at the full active array.
Some modules use a listed output size instead. The phone-class front module's
block centres on its largest 4:3 output, **4192×3144**: the principal points
(2098.2, 1565.6) / (2094.8, 1568.1) sit 2–6 px from that size's centre. They
are 190 px off the 4576×3168 array's centre, and about 6 px off a centred
4208×3120. Rule (`leia_scam_pick_calib_reference`): keep the array unless both
principal points are more than 3 % off its centre and a listed YUV / JPEG
output size has them within 1 %. The closest such size wins. The block is
then re-expressed at the array through the HAL's own array → stream step
(largest centred region of the stream's aspect, scaled). For the phone that
puts the principal point at (2290, 1578), close to the array centre (2288,
1584). The tablets' blocks sit within 0.6 % of their array centre, so nothing
changes for them.

**Vertical pairs (phone held upright).** When `|T.y| > |T.x|`, the two lenses
are stacked along the sensor's y axis (phone-class front: T = (0.37, 14.89,
−0.48) mm). The runtime's rectifier only takes side-by-side pairs. The source
therefore turns each eye clockwise by the sensor orientation (270 on the
phone). It publishes 720×1280 portrait eyes, upright in the device's natural
orientation. The calibration turns with them, as the **last** step after any
calibration source (a later recalibrated R included):
`fx' = fy`, `cx' = H−1−cy`, `cy' = cx` per 90° step, `p1' = p2`, `p2' = −p1`,
`R' = Q R Qᵀ`, `T' = Q T` with `Q = [[0,−1,0],[1,0,0],[0,0,1]]`. The eye
order is then decided on the turned geometry. The 640×480 tracking frame is
cut from the unturned capture, so the in-app tracker's intrinsics do not
change.

**Frame orientation (runtime `XR_DXR_stereo_camera` v7).** Each camera
reports how its published frame is turned, so a consumer (the DisplayXR
Browser) does not stack its own turn on top of ours:
`info.frame_orientation` = the clockwise turn that makes the published frame
upright in the device's natural orientation (Camera2 `SENSOR_ORIENTATION`
semantics), i.e. `(sensor orientation − turn) mod 360`
(`leia_scam_frame_orientation_deg`). Portrait-natural tablets publish unturned
frames and report their sensors' **270** (front) and **90** (rear), exactly the
Android CDD fallback the browser used before; the phone-class front pair, turned
by its sensor's 270, reports **0**. Without `ACAMERA_SENSOR_ORIENTATION` the
value is left UNKNOWN. Compiled only against runtime headers that define
`XRT_PLUGIN_STEREO_CAMERA_INFO_HAS_FRAME_ORIENTATION`; built against older ones
the field is absent and the runtime reports UNKNOWN.

Measured on a Lume Pad 2:

| Pair | Baseline | f at 1280×720 | Rotation between lenses |
|---|---|---|---|
| Front | 14.99 mm | ≈ 676 / 690 px | ≈ 0.46° (mostly roll) |
| Rear | 30.03 mm | — | ≈ 0.3° |

## Head tracking while the front pair is open

Opening the front pair takes the tracking service's camera away, because both
use the same lens. While a front stream is open, every CNSDK core in this
process tracks **in-app**. `leia_cnsdk_ext_tracking_begin` performs the switch
on each core:

1. Stop and disable face tracking.
2. Attach a fresh shared-camera sink.
3. Set the in-app runtime.
4. Re-enable and restart tracking, re-attach the frame listener, and re-apply
   the eye-tracking mode.

The weave is gated (`tracking_cycling`) for those few milliseconds. A core
created while a stream is open joins in-app when its worker first starts
tracking. On close, `leia_cnsdk_ext_tracking_end` switches every core back to
in-service.

**Feeding.** A feeder thread pushes each 640×480 Y8 frame through the C API:

1. `leia_shared_camera_sink_on_intrinsics_change` with the tracking lens's
   intrinsics at 640×480, mirrored. It runs first, then every 60 frames.
2. `leia_shared_camera_sink_on_image` with a tight 640 stride, rotation 0, and
   a BOOTTIME timestamp.

Detection runs on the caller's thread, so frames are latest-wins and never
queued, and the capture callback only overwrites the slot. The tracking frame is
cut from every tracking-lens image *before* pairing. CNSDK resets its predictor
on a gap above 70 ms, so a lost pair must not become a lost tracking frame.

**The engine's idle throttle sleeps the feeder.** CNSDK's `FrameThrottle`
(`Engine::ProcessFrame`, 0.10.95) turns on 5 s after tracking starts once no face
was seen for 2 s. From then on every frame that arrives within 500 ms of the last
processed one is skipped *and the caller sleeps until 500 ms have passed*, under
the engine's frame lock. With its own camera the caller is CNSDK's camera
thread; with a shared-camera sink it is our feeder. The sink then gets about 4
frames/s in pairs (one processed, one skipped and slept on), whatever the camera
delivers. Nothing upstream of the feeder slows down: pairs, the 3D stream and
the capture callback keep the camera rate. The fix for the sleep belongs in
CNSDK (a shared-camera sink should be skipped without sleeping, or not throttled).

**Which lens.** The engine applies its configured tracking camera's pose and
intrinsics, so the fed frames must come from that lens (on a nubia Pad 3D II
it is the pair's sub lens, `"3"`). A stream can open before this process's
core has read its device config; the lens is then guessed (the factory main
lens), and the feeder asks again every second and switches to the configured
lens, re-pushing intrinsics, as soon as a core knows it
(`tracking lens 1 -> 3 once the cores knew their tracking camera`).

**Rate WARN.** Every 10 s while the pair is owned:

```
tracking feed 30.0 captured/s -> 3.9 fed/s to the sink (call avg 254.0 ms max 501 ms, longest gap 501 ms,
20 engine stall(s) >= 400 ms), faces 0.0/s of 118.6 polls/s, lens 3
```

`fed/s` far below `captured/s` with stalls is the throttle; `faces/s` counts face
polls (`leia_cnsdk_get_primary_face`) that got a face from the core.

**Frame dump.** `adb shell touch /sdcard/Android/data/<runtime-pkg>/files/dxr_track_dump_trigger`
writes the next fed frame, exactly as the sink got it, to `dxr_track_frame.pgm`
in the same directory (Y8, 640×480, sensor-native: the engine rotates it).

**CNSDK #762.** The Java/JNI `SharedCameraSink.onImage` bugs it fixes (padded
Y stride, non-zero rotation) are not on this path. Nothing here changes when
the pin moves to CNSDK ≥ 0.10.88.

## Capture modes, controls and stills (runtime `XR_DXR_stereo_camera` v8)

The eight v8 slots (runtime PR #2803, spec §5.5–§5.7 and §9b) are compiled
only against runtime headers that define `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES`,
`_CONTROLS` and `_STILL`. Built against older headers (the pinned Android tag),
the source behaves as before: one mode, no controls, the template's focus.
The pure-C half (mode list, metering mapping, control plan, capture result)
is `leia_stereo_camera_android_modes.{h,c}`, host-tested by
`tests/test_stereo_camera_android_modes.c`.

The numbers below come from the stage 0 measurements on an NP02J rear pair
(DisplayXR Camera, `docs/STAGE0.md`): 29.80 fps in every mode that keeps up
(never 30.0), YUV at full rate up to 3264x2448 per lens, 4000x3000 and
4208x3120 YUV at ~27.4 fps with ~220 ms latency.

### Modes

Every mode has the default eye's **horizontal** field of view about the same
optical axis, with square pixels; only the vertical extent differs. That is
what lets the runtime derive each mode's calibration from the default's
(§5.5: `s = w / w_default`, `f' = f s`, `cx' = (cx + ½) s − ½`,
`cy' = (cy + ½ − h_default/2) s + h/2 − ½`). For a listed size the HAL serves
the centred region of its aspect; the eye keeps exactly the default's columns
of it (a whole, even number, centred at an even offset) and either every row
or the default's 16:9 cut. 16:9 capture sizes see the whole array width
(4208 columns on the rear pair) while the 4:3 default sees 4160, so they never
become modes of their own.

| Pair | Mode | Capture per lens | Flags | Switching to it |
|---|---|---|---|---|
| both | **1280x720** (default) | 1280x960 YUV | stream, still | — |
| both | 1280x960 | 1280x960 YUV | stream, still | new crop of the running capture: no frame lost |
| rear | 1920x1080 | 1920x1440 YUV | stream | session reconfigure |
| rear | 1920x1440 | 1920x1440 YUV | stream | session reconfigure (free between 1920x1080 and 1920x1440) |
| rear | 4096x3072 | JPEG output, 4096x3072 | still | none: the JPEG outputs are in every rear session |

- **One further stream capture only**: the largest listed YUV size within the
  stream cap (default 1920x1440, `debug.dxr.leia.scam_max_stream`) whose
  listed minimum frame duration is at most 1/30 s. The runtime serves any size
  in between by downscaling, so more captures would only add reconfigures.
- **Reconfigure** (rear, `set_mode` to the other capture): the device stays
  open, the new session replaces the old one (stage 0: 86–124 ms to
  configure, ~85 ms to the first frame at 1920x1440), the sequence keeps
  counting. One WARN per switch gives the time. A failed switch rebuilds the
  previous capture and returns BUSY; if that also fails the watchdog re-opens.
- **Lazy session**: `open` no longer starts the capture; the first `wait_frame`
  or `set_mode` does, so a `set_mode` right after open opens straight into the
  wanted mode.
- `maxFrameRate` is the **measured** rate of the mode's capture (the default
  capture's or the further capture's, once it has run), else 0. The HAL lists
  33.33 ms for every size, including the ones that deliver 27.4 fps, so its
  table is never reported.
- A turned pair (phone held upright) lists the default only: its published
  horizontal is the sensor's vertical.

**The calibration is pixel-centre aware** (changed with v8, also for older
runtimes): the array → stream step is `c' = (c − offset + ½)·s − ½` (pixel
centres at integer coordinates, as the block's OpenCV numbers and the
runtime's rule use) instead of `c' = c·s`. The two differ by `(s − 1)/2`
pixels, about −0.35 px for 1280 of a 4208-column array; with the old rule the
runtime's derivation would put a 4096x3072 still's principal point ~1.1 px off.
The host test checks, for every listed mode on the tablets' numbers, that the
runtime's rule applied to the default calibration equals the calibration
computed for the mode directly from the factory block. The tracker's 640x480
intrinsics keep the old rule (unchanged).

### Front pair (shared with head tracking)

The front pair lists only the default and 1280x960: both are cuts of the same
1280x960 capture, so a mode switch changes nothing for the camera session and
nothing for the **tracking feed** (the 640x480 frame is cut from every
tracking-lens image of that capture, before pairing, whatever the mode). No
larger stream mode (a reconfigure stops the feed for well over CNSDK's 70 ms
predictor window) and no JPEG output (an extra output on the tracking lens
changes its session; not measured). A still on the front pair is the next
stream pair, at 1280x720 or 1280x960: no session change, no tracking gap.

### Controls

`get_control_caps` advertises what the source can apply:

- **Focus.** On a pair whose lens focuses, the source runs the AF state
  machine **OFF** and drives both lenses with one logical
  `LENS_FOCUS_DISTANCE`. Stage 0: one logical distance moves both lenses
  (they agree within ~0.03 D, settled ~260 ms after the request), while
  continuous AF and an AF trigger move lens 0 only, and the HAL lists no
  per-lens request keys. So only **MANUAL** and **LOCKED** are advertised;
  the app focuses from the stereo pair itself. With no focus control set
  (`valid_controls == 0`, or the focus bits clear) the source's default is a
  fixed **0.5 D (2 m)**: the lens's ~0.31 D floor is about its hyperfocal
  distance (~3.2 m), so at 2 m the depth of field spans ~1.2–5.3 m, the range
  people and tabletop scenes are shot at, at the cost of sharpness at
  infinity (0.31 D would cover ~1.6 m–∞). LOCKED, and MANUAL without a
  distance, keep the distance in force. The caps say so:
  `default_focus = FIXED`, `default_focus_distance_diopters = 0.5` (AUTO with
  HAL AF; UNKNOWN on the fixed-focus front pair, whose minimum focus distance
  0 already tells). There is no HAL signal that tells
  whether its AF drives both lenses of a logical pair, so this is the default
  for every pair; `debug.dxr.leia.scam_hal_af=1` opts a device back into the
  HAL's AF (CONTINUOUS_AUTO, AUTO_ONCE with one `AF_TRIGGER_START` per
  `focus_trigger` change, LOCKED = AUTO without a trigger).
- **Metering point**: one `AE_REGIONS` rectangle (and `AF_REGIONS` with HAL
  AF), 5 % of the logical array's shorter side, centred on the point. The
  point arrives normalised in the raw left eye of the delivered frame; it is
  mapped through the turn (if any) and the centred crop into the capture, then
  through the HAL's array → stream step into the lens's active array, and
  proportionally into the logical camera's array (regions are relative to it).
  No mirror: the published frames are sensor images. Mapped once when the
  controls arrive, so a later crop change keeps pointing at the same spot.
- **Exposure compensation**: the HAL's range and step (rounded, clamped).
- **Front pair**: fixed-focus lenses, and metering / compensation would move
  the exposure the tracker sees, so it advertises **no controls** unless
  `debug.dxr.leia.scam_front_controls=1` (metering + compensation).

A cleared bit puts back the request template's own value (or removes the
tag). The state is applied to the repeating request at once and to every
session built later (mode switch, watchdog re-open; with HAL AF and
AUTO_ONCE a re-open searches again).

**Capture result** (`get_capture_result`, only what the HAL reported), from
`ACaptureSessionLogicalCamera` callbacks:

| Field | Source |
|---|---|
| focus distance | the logical result (it commands both lenses). With APPROXIMATE calibration it may be the commanded value, not a sensed one |
| focus state | AF off: the two lenses' `LENS_STATE`, MOVING on either = SEARCHING, both STATIONARY = LOCKED. HAL AF: the logical `AF_STATE`. Fixed focus: FIXED |
| exposure, ISO | the left eye lens's physical result, else the logical one |
| per-eye exposure start | the two physical `SENSOR_TIMESTAMP`s, **only once they have proved independent** |

Per-eye times: stage 0 found this HAL stamps both lenses with one synthesized
time on 99.7 % of frames, although lens 2's buffers arrive 12–16 ms after
lens 0's. Equal stamps are therefore not evidence of sync. The source sets
the per-eye bit only after 60 frames with both stamps of which at least one in
ten differed (`leia_scam_eye_times_tracker`); on the NP02J that never
happens, so the bit stays clear and `captureTime` is the only time.

### Pairing and per-eye times

Two different clocks are in play, and they disagree on this HAL:

- **Image stamps** (`AImage_getTimestamp`, one per lens): what **pairing** uses.
  On the NP02J they are equal for both lenses (stage 0: one synthesized stamp,
  interval std 0.000 ms), although lens 2's buffers arrive 12–16 ms later.
- **Physical result stamps** (`SENSOR_TIMESTAMP` in each lens's physical
  result of an `ACaptureSessionLogicalCamera` callback): what the per-eye
  capture times report, and only once they have proved independent.

A pair is the left image with the right image whose stamp is the closest
within the tolerance (default 8 ms, `debug.dxr.leia.scam_pair_tol_ms`,
always capped at 0.3 of the frame period measured from each lens's own
consecutive stamps), so a constant lens offset of a few ms still pairs and two
different frames (one period, 33.56 ms, apart) never do. Before v8 the rule
was the first image within 1 ms.

**What stamps cannot show.** Because the image stamps are synthesized, a
pair whose lenses delivered DIFFERENT sensor frames under one request would
still carry equal image stamps and pair. The physical stamps are the only
witness. Device runs of this branch saw them at a constant −6.5 ms (R − L)
in one stream (a plausible sensor phase: reported, and harmless to pairing)
and, in another, jump to −39.1 ms (one period + 5.6 ms) right after a
ZSL still and stay there: that would be a one-frame temporal mismatch in
every later pair if the physical stamps are right. The still request
therefore uses `TEMPLATE_PREVIEW` by default; whether ZSL really slips a
lens, and whether the pixels follow the physical stamps, needs the
diagnostics below and an optical check (a moving object or a stopwatch on a
screen in both eyes of one pair).

The still request's results come through their own callbacks and never
stand in for a stream frame's result, nor count towards the per-eye-time
decision. Per-eye times stay withheld until 60 frames had both physical
stamps and at least one in ten differed; the first ~60 frames of a stream
therefore always report no per-eye times, even when the offset is constant.

**Diagnostics** (WARN):

- at each stream start: the pairing tolerance;
- every 10 s: min/median/max of R − L image stamp over the paired frames, of
  R − L physical result stamp, and of image − physical stamp per lens; the
  candidates rejected for exceeding the tolerance; images dropped unpaired
  per lens; whether per-eye times are reported;
- once 30 pairs followed a still: `after still <id>: R-L median image stamp
  a -> b ms, physical result stamp c -> d ms`, flagged `CHANGED` when either
  median moved by more than 1 ms.

Image − physical per lens is the decisive number: equal image stamps beside
a physical skew mean the HAL synthesizes the image stamps.

### Lens phase

**Measured** (dev runtime, rear pair at 1920x1440, the 10 s pairing WARN):
`R-L image stamp 0.000/0.000/0.000 ms; R-L physical result stamp
-40.615/-40.615/-40.615 ms; image-physical L -40.615, R 0.000`. Over four
opens of the tablet's rear pair the R − L gap between the two lenses'
start of exposure was **0, −6.5, −39.1 and −40.6 ms**: fixed within a
session, re-rolled at each open, unchanged by a still. The two sensors
free-run (sync type APPROXIMATE) and the HAL stamps both images with one
time (here the right lens's), so pairing by image stamp pairs the two
buffers of one REQUEST, whatever their real exposure times. The calibration
session measured the same on a phone's front pair (0 ms in about half the
opens, else up to 40 ms; visible as depth error when the device rotates).
It is a real exposure-time offset between the eyes.

**Lock** (`leia_scam_phase_*`, after every start):

1. Measure the gap G = R − L of the two lenses' physical `SENSOR_TIMESTAMP`
   of one request over the first 12 pairs (median).
2. **Re-pair to the nearest frame**: both lenses run at the same period P, so
   the right frame `k + n`, `n = round(−G/P)` (±1 at most), is nearer in time
   to left frame `k`. No per-lens buffer matching is needed: the image stamps
   are one per request and periodic, so the right image of request `k + n`
   carries stamp `t + n·P`. The residual is `G + n·P`, at most P/2
   (−40.6 ms → right eye one frame later → −7.06 ms). Costs `n = +1` one frame
   of latency. Each eye's capture result (exposure, ISO, per-eye time) then
   comes from its own request.
3. If the judged gap still exceeds 3 ms, **restart** to re-roll the phase,
   at most 4 times, then accept the last session as it is (a restart re-rolls
   the phase, so an earlier better session cannot be restored; the log gives
   the best seen). One WARN per restart (gap, re-paired residual, attempt) and
   a final `lens phase LOCKED | ACCEPTED ...` line.
4. **Judged on**: the rear pair, whose JPEG stills take both eyes from one
   request (no re-pairing possible for them), is judged on the raw gap G; a
   pair without same-request stills (the front) on the re-paired residual.

While a start locks, no pair is published: the camera stays WAITING and
consumers see one clean start (~12 frames, 400 ms, plus any restart).
After a mode reconfigure (a new session, so a new phase) the lock re-measures
and re-pairs but neither restarts nor holds frames, to keep the switch inside
its 500 ms budget. A HAL with no per-lens stamps: no lock (after 36 pairs).

**Restart level** (`debug.dxr.leia.scam_phase_restart`): `session`
(default) recreates only the capture session on the open device; the HAL
reconfigures and restarts both sensors' streaming, which should re-roll the
phase (the next device run decides). `device` closes and re-opens the
logical camera (sensors powered down and up: a sure re-roll, ~2× the cost).

**Front pair**: re-pairing only by default. A restart there stops the
tracking feed for its duration (a session restart ~200–250 ms, a device
restart more, plus the lens-free wait): each one is a CNSDK predictor reset
(gap > 70 ms); the in-app hand-over itself is kept, since a restart never
hands tracking back. `debug.dxr.leia.scam_phase_restart_front=1` enables it.

The accepted gap reaches the runtime as the per-eye capture times (once the
stamps proved independent); the 10 s WARN ends with the lock's state, shift
and accepted gap.

### Stills

| Still | How | Stream | Expected latency |
|---|---|---|---|
| 1280x720 / 1280x960 (both pairs) | the next stream pair, packed at the still's crop | no frame lost | ~1 frame |
| 4096x3072 (rear) | one request on the running session targeting the stream outputs **and** one JPEG output per lens (`TEMPLATE_PREVIEW`; `debug.dxr.leia.scam_still_template=zsl` for `TEMPLATE_ZERO_SHUTTER_LAG`, see *Pairing* below), then each JPEG decoded (`AImageDecoder`) and converted to NV12 | no frame lost | ~320 ms to both JPEGs (stage 0) + decode/convert, ~0.6–0.8 s to READY (estimate, not measured) |

- **Never a reconfigure** for a still (stage 0: 1.35 s without frames) and
  never `TEMPLATE_STILL_CAPTURE` on the live stream (a ~400 ms freeze).
- **Why JPEG.** The HAL takes exactly one YUV output per lens (a second one
  aborts it), and the runtime wants a RAW pair in GRAY8 / NV12 / BGRA8. With
  YUV stream outputs the still output per lens has to be JPEG, decoded here.
  Moving the stream outputs to PRIVATE (the AHardwareBuffer transport step)
  would allow a YUV still output instead; not this change.
- **Decode.** `AImageDecoder` is API 30; the plug-in targets API 29, so it is
  looked up in `libjnigraphics.so` at run time (an API 29 system: the JPEG
  still fails, the stream-pair stills work). The JPEGs are requested with
  `JPEG_ORIENTATION` 0 (pixels as captured, no EXIF turn) and quality 95
  (`debug.dxr.leia.scam_still_quality`); RGBA → NV12 is full-range BT.601,
  like the camera's own YUV. The JPEG round trip is lossy (quality 95), so the
  still carries `XRT_PLUGIN_STEREO_CAMERA_STILL_LOSSY_SOURCE` (runtime
  `XR_STEREO_CAMERA_STILL_SOURCE_LOSSY_BIT_DXR`); stills from stream pairs do not.
- **Memory.** The 4096x3072 NV12 pair is 37.7 MB, freed at `release_still`.
  While a still is decoded: the two JPEGs (~5–10 MB each), one RGBA eye
  (50 MB, the eyes are decoded one after the other) and the NV12 pair. The
  rear session also carries two JPEG readers of 2 images each.
- `debug.dxr.leia.scam_still=0` drops the JPEG outputs (stream-pair stills only).
- A still of the default capture asked while the rear stream runs its
  1920x1440 capture is refused (BUSY): it is a cut of a capture that is not
  running.

## Watchdog

The pair counts as stalled when no frame pair has arrived for 1.5 s. A stall
can be silent: CRC recovery reports nothing. The device callbacks
(`onDisconnected`, `onError`) cover eviction and HAL restarts.

On either, the source:

1. Tears the session down.
2. Reopens with a 0.5 s → 4 s back-off.
3. Reports `SUSPENDED` from `wait_frame` meanwhile.
4. Reports `ERROR` after 15 s.

## Knobs (system properties, read by the process that loads the plug-in)

| Property | Effect |
|---|---|
| `debug.dxr.leia.scam=0` | advertise no camera |
| `debug.dxr.leia.scam_rear=0` | front pair only |
| `debug.dxr.leia.scam_size=WxH` | per-eye size (default 1280x720; captured at W×(3W/4) when the lenses list it) |
| `debug.dxr.leia.scam_track=0` | do not hand tracking over (the service then loses its camera) |
| `debug.dxr.leia.scam_swap=auto\|0\|1` | SBS eye order |
| `debug.dxr.leia.scam_track_fov_ratio=<r>` | our tracking frame's field / the tracker's own capture field (default: from the sensor-mode table, else 1) |
| `debug.dxr.leia.scam_track_fps=<n>` | the tracking service's capture rate, used to pick its sensor mode (default 90) |
| `debug.dxr.leia.scam_pose=0` | ignore the HAL's lens poses for the eye order (the block decides alone) |
| `debug.dxr.leia.scam_calib_key_prefix` | vendor key prefix of the factory block |
| `debug.dxr.leia.scam_rotate=auto\|0\|90\|180\|270` | clockwise turn of each eye (auto: the sensor orientation when the lenses are stacked along the sensor's y axis, else 0) |
| `debug.dxr.leia.scam_calib_ref=auto\|array\|WxH` | the size the factory block's intrinsics are expressed at (auto: see *Reference size*) |
| `debug.dxr.leia.scam_served_calib=0` | ignore the served display config record (provider / factory only) |
| `debug.dxr.leia.scam_modes=0` | v8: list the default mode only |
| `debug.dxr.leia.scam_max_stream=WxH` | v8: largest mode on a further stream capture (rear; default 1920x1440). The front pair never reconfigures |
| `debug.dxr.leia.scam_max_still=WxH` | v8: largest still from the JPEG output (default 4096 wide) |
| `debug.dxr.leia.scam_still=0` | v8: no JPEG still outputs (stills from stream pairs only) |
| `debug.dxr.leia.scam_still_template=preview\|zsl` | v8: template of the still request (default preview) |
| `debug.dxr.leia.scam_phase=0` | no lens-phase lock (pairs as delivered) |
| `debug.dxr.leia.scam_phase_samples=<n>` | pairs measured per session (default 12) |
| `debug.dxr.leia.scam_phase_max_ms=<ms>` | accepted lens-phase gap (default 3) |
| `debug.dxr.leia.scam_phase_restarts=<n>` | restarts at most (default 4) |
| `debug.dxr.leia.scam_phase_restart=session\|device` | restart level (default session) |
| `debug.dxr.leia.scam_phase_repair=0` | no nearest-frame re-pairing |
| `debug.dxr.leia.scam_phase_restart_rear=0` / `_front=1` | restarts on the rear (default on) / front (default off) pair |
| `debug.dxr.leia.scam_pair_tol_ms=<ms>` | pairing tolerance between the two lenses' image stamps (default 8; always capped at 0.3 frame period) |
| `debug.dxr.leia.scam_still_quality=<50..100>` | v8: JPEG quality of the still (default 95) |
| `debug.dxr.leia.scam_hal_af=1` | v8: use the HAL's AF (continuous, tap-to-focus) instead of AF off + one distance |
| `debug.dxr.leia.scam_default_focus_d=<D>` | v8: the fixed focus distance with no focus control set (default 0.5 D) |
| `debug.dxr.leia.scam_front_controls=1` | v8: metering point + exposure compensation on the front (tracking) pair |
