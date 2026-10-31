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

1. `leia_shared_camera_sink_on_intrinsics_change` with the main lens's
   intrinsics at 640×480, mirrored. It runs first, then every 60 frames.
2. `leia_shared_camera_sink_on_image` with a tight 640 stride, rotation 0, and
   a BOOTTIME timestamp.

Detection runs on the caller's thread and can stall for 500 ms with no face in
view, so frames are latest-wins and never queued. The tracking frame is cut
from every main-lens image *before* pairing. CNSDK resets its predictor on a
gap above 70 ms, so a lost pair must not become a lost tracking frame.

**CNSDK #762.** The Java/JNI `SharedCameraSink.onImage` bugs it fixes (padded
Y stride, non-zero rotation) are not on this path. Nothing here changes when
the pin moves to CNSDK ≥ 0.10.88.

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
