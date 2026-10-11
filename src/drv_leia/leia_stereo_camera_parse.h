// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Stereo camera source (runtime ADR-043, L1): the platform-neutral
 *         pieces of the Leia SR provider — no Win32, no SR SDK, no OpenCV — so
 *         they are unit-tested on the host (tests/test_stereo_camera_parse.c,
 *         run by the Linux CI job) instead of only on an SR panel.
 *
 *  - the SR raw-camera shared-memory FRAME HEADER (a reader-defined contract,
 *    copied from the SR eyetracker's MediaFoundationCamera.cpp: four u64
 *    {rawImageFormat, compressedSize, height, width}, then one SBS JPEG at
 *    offset 32; no sequence, no timestamp) — validated field by field;
 *  - the JPEG SOF dimensions (header dims must equal the JPEG's own);
 *  - a new-frame fingerprint (the channel carries no sequence number);
 *  - the device-serial list in `Global\sharedDeviceSerialMemory` (1 count
 *    byte, then 32-byte NUL-terminated serials, primary device FIRST — the
 *    same rule the SR eyetracker's PredictingEyeTracker uses);
 *  - one camera per SR device: the per-device section names, the live
 *    trackers in `Global\SREyetrackerStats2`, the serial -> camera index
 *    mapping (kept stable) and the camera display names;
 *  - the small OpenCV FileStorage YAML subset of `intrinsics.yml`
 *    (M1 D1 M2 D2) and `extrinsics.yml` (R T), parsed without OpenCV;
 *  - eye order: which SBS half is the camera's LEFT lens, decided from the
 *    sign of T (OpenCV x_2 = R x_1 + T), overridable.
 *
 * @ingroup drv_leia
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * Raw-camera frame header.
 *
 */

//! Bytes before the JPEG in the shared-memory block.
#define LEIA_SCAM_HEADER_SIZE 32u
//! Header rawImageFormat value: side-by-side MJPEG (the only one SR writes).
#define LEIA_SCAM_FORMAT_SBS_MJPEG 0u

struct leia_scam_header
{
	uint64_t raw_image_format;
	uint64_t compressed_size;
	uint64_t height;
	uint64_t width; //!< full SBS width
};

enum leia_scam_header_status
{
	//! A plausible frame is present.
	LEIA_SCAM_HEADER_OK = 0,
	//! compressedSize == 0: the tracker zeroes the block when its camera stops.
	LEIA_SCAM_HEADER_EMPTY = 1,
	//! Out of contract (unknown format, size past the region, silly extent).
	LEIA_SCAM_HEADER_BAD = 2,
};

/*!
 * Validate the header at the start of a mapped block of @p region_size bytes.
 * Copies the fields to @p out in every case (for diagnostics).
 */
enum leia_scam_header_status
leia_scam_header_check(const void *block, uint64_t region_size, struct leia_scam_header *out);

/*!
 * Width / height / component count from the first SOF marker of a JPEG.
 * @return false when @p jpg is not a JPEG or has no SOF before the scan.
 */
bool
leia_scam_jpeg_dims(const uint8_t *jpg, size_t size, uint32_t *out_w, uint32_t *out_h, uint32_t *out_components);

/*!
 * Content fingerprint used for new-frame detection: size + FNV-1a over the
 * head, middle and tail of the JPEG (an entropy-coded stream changes
 * everywhere when the image changes, and a sensor never repeats a frame
 * bit-exactly).
 */
uint64_t
leia_scam_fingerprint(const uint8_t *jpg, size_t size);


/*
 *
 * Device serials.
 *
 */

#define LEIA_SCAM_SERIAL_LEN 32u
#define LEIA_SCAM_SERIAL_MEMORY_SIZE (1u + 255u * LEIA_SCAM_SERIAL_LEN)

/*!
 * Parse the `Global\sharedDeviceSerialMemory` block. Writes up to @p cap
 * serials (NUL-terminated, trimmed) and returns how many the block holds.
 * Entry 0 is the PRIMARY device (SRService writes it first).
 */
uint32_t
leia_scam_parse_serials(const uint8_t *block, size_t size, char (*out)[LEIA_SCAM_SERIAL_LEN + 1], uint32_t cap);


/*
 *
 * One camera per SR device (multi-device trackers, SR ST-5481 D4).
 *
 * Every SR eye tracker process (the primary and each per-device child) writes
 * its camera to sections named after its device: the base name + "_<serial>"
 * (Global\SREyetrackerRawCamera_<serial>, ...Mutex_<serial>, ...Event_<serial>).
 * The PRIMARY also keeps the bare legacy names; a child never writes them.
 * <serial> is the FPC serial — the Devices\<serial> calibration folder name
 * and the SR display enumeration's serial, compared by plain string equality.
 *
 * The live trackers are listed in Global\SREyetrackerStats2 (SR's
 * eyetrackerstats.h, SR_TrackerStatsRow): 8 rows x 232 bytes, serial[32] at
 * offset 0 (empty = free row), u32 pid at 32 (a pid that is no longer a
 * running process = stale row), u32 state at 48 (0 paused, 1 streaming).
 *
 */

//! Most cameras the provider advertises (XRT_STEREO_CAMERA_MAX_CAMERAS).
#define LEIA_SCAM_MAX_CAMERAS 4u

#define LEIA_SCAM_STATS_ROW_SIZE 232u
#define LEIA_SCAM_STATS_ROWS 8u
#define LEIA_SCAM_STATS_MEMORY_SIZE (LEIA_SCAM_STATS_ROW_SIZE * LEIA_SCAM_STATS_ROWS)
#define LEIA_SCAM_STATS_PID_OFFSET 32u
#define LEIA_SCAM_STATS_STATE_OFFSET 48u

/*!
 * The per-device name of an SR named object: @p base when @p serial is NULL
 * or empty (the legacy, primary-only name), else "<base>_<serial>".
 * @return false (and "" in @p out) when the name does not fit or @p serial
 *         holds a character that cannot be in a kernel object name ('\\',
 *         control characters, non-ASCII) — never a name that could resolve to
 *         another object.
 */
bool
leia_scam_section_name(const char *base, const char *serial, char *out, size_t out_size);

//! One live tracker from Global\SREyetrackerStats2.
struct leia_scam_tracker
{
	char serial[LEIA_SCAM_SERIAL_LEN + 1];
	uint32_t pid;
	bool streaming; //!< row state 1; 0 = paused (no client holds it)
};

/*!
 * Parse the Global\SREyetrackerStats2 table. Keeps rows with a printable,
 * NUL-terminated, non-empty serial, a non-zero pid that @p pid_alive (may be
 * NULL = trust every pid) says is running, and state 0 or 1; drops repeats of
 * a serial (first row wins). @return the number written (<= @p cap).
 */
uint32_t
leia_scam_parse_tracker_stats(const uint8_t *block,
                              size_t size,
                              bool (*pid_alive)(uint32_t pid),
                              struct leia_scam_tracker *out,
                              uint32_t cap);

//! One advertised camera = one SR device.
struct leia_scam_camera_slot
{
	char serial[LEIA_SCAM_SERIAL_LEN + 1];
	//! The primary device: the legacy unsuffixed sections are its fallback.
	bool primary;
};

/*!
 * The serial -> camera mapping, i.e. what enumerate advertises, in index order.
 *
 *  - @p tracker_count < 0 (no Stats2 table: an SR that predates per-device
 *    trackers): exactly the primary, when @p primary_serial is non-empty —
 *    today's single camera, read from the legacy sections.
 *  - Otherwise: the primary (when known; it always has the legacy sections)
 *    plus every OTHER device that has a live tracker row AND whose own
 *    "_<serial>" section exists (@p has_section; NULL = assume it does). A
 *    paused tracker keeps its section (zeroed), so a device that has none is
 *    on an SR whose children do not publish yet: NOT advertised, because a
 *    camera that can never deliver is worse than none.
 *
 * Order: the primary first (index 0, as before), then the others by serial
 * (strcmp), so indices only move when the device set or the primary changes.
 * At most @p cap (and LEIA_SCAM_MAX_CAMERAS). @return the count.
 */
uint32_t
leia_scam_build_camera_list(const char *primary_serial,
                            const struct leia_scam_tracker *trackers,
                            int32_t tracker_count,
                            bool (*has_section)(const char *serial),
                            struct leia_scam_camera_slot *out,
                            uint32_t cap);

/*!
 * Keep indices stable for the plug-in instance's life (the runtime contract):
 * merge the current list (@p cur, from @ref leia_scam_build_camera_list) into
 * the append-only @p reg. A serial keeps the index it first got; a new one is
 * appended (while @p cap allows); one that is gone stays, marked not primary
 * and not present (its stream then just waits — the runtime shows WAITING).
 * Every entry's @ref leia_scam_camera_slot::primary is refreshed from @p cur.
 * @p present (may be NULL) receives, per registry index, whether the serial is
 * in @p cur. @return the registry count.
 */
uint32_t
leia_scam_merge_stable(struct leia_scam_camera_slot *reg,
                       uint32_t reg_count,
                       uint32_t cap,
                       const struct leia_scam_camera_slot *cur,
                       uint32_t cur_count,
                       bool *present);

//! Size of a display name (xrt_plugin_stereo_camera_info::display_name).
#define LEIA_SCAM_NAME_MAX 128u

/*!
 * Display names for @p count cameras. "Leia SR camera - <screen>" when
 * @p screen_names[i] is non-empty (the monitor's friendly name or the panel's
 * model), else "Leia SR camera <serial>". The names are guaranteed distinct
 * (the browser labels devices by them): any name two cameras share — two
 * panels of the same model — gets " (<serial>)" appended on every holder.
 */
void
leia_scam_display_names(const struct leia_scam_camera_slot *cams,
                        const char *const *screen_names,
                        uint32_t count,
                        char (*out)[LEIA_SCAM_NAME_MAX]);


/*
 *
 * Calibration (OpenCV FileStorage YAML subset).
 *
 */

/*!
 * Read the `data: [ ... ]` of the `!!opencv-matrix` named @p key. Doubles, up
 * to @p cap values; rows/cols from the node (0 if absent).
 * @return the number of values read, 0 on a missing / malformed node.
 */
uint32_t
leia_scam_yaml_matrix(
    const char *text, size_t len, const char *key, double *out, uint32_t cap, uint32_t *out_rows, uint32_t *out_cols);

//! A pair as the SR files describe it, camera 1 = M1/D1, camera 2 = M2/D2.
struct leia_scam_calibration
{
	double K[2][9];      //!< row-major 3x3
	double D[2][8];      //!< OpenCV order; zero-padded
	uint32_t d_count[2]; //!< coefficients present (4, 5, 8)
	double R[9];         //!< x_2 = R x_1 + T
	double T[3];         //!< mm
};

/*!
 * Parse intrinsics.yml + extrinsics.yml text. Refuses (false + @p why)
 * anything the runtime could not use faithfully: missing nodes, a non-3x3 K,
 * non-positive focal, a distortion vector longer than 8 with non-zero extra
 * terms (thin-prism / tilted models), a non-rotation R, a baseline outside
 * 5..500 mm.
 */
bool
leia_scam_parse_calibration(const char *intr,
                            size_t intr_len,
                            const char *extr,
                            size_t extr_len,
                            struct leia_scam_calibration *out,
                            char *why,
                            size_t why_len);

/*!
 * Eye order. The runtime wants SBS left half = the camera's LEFT lens, with
 * x_R = R x_L + T and T.x < 0. SR files name the cameras 1 and 2 (assumed:
 * camera 1 = the first, left, half of the SBS JPEG). If T.x > 0, camera 2 sits
 * at -x of camera 1, i.e. the left half holds the RIGHT lens: swap.
 * @p override_env: NULL/"auto" = decide from T; "0" = never; "1" = always.
 */
bool
leia_scam_decide_swap(const struct leia_scam_calibration *c, const char *override_env);

/*!
 * Per-eye numbers in the runtime's order (eye 0 = SBS left half AFTER any
 * swap): intrinsics fx fy cx cy, distortion model (1 = RADTAN5, 2 = RADTAN8)
 * + coefficients, and x_R = R x_L + T (mm).
 */
struct leia_scam_eye_calibration
{
	double k[2][4];
	uint32_t model;
	double d[2][8];
	double R[3][3];
	double T[3];
};

void
leia_scam_calibration_for_runtime(const struct leia_scam_calibration *c,
                                  bool swap,
                                  struct leia_scam_eye_calibration *out);

/*!
 * Swap the two halves of a GRAY8 SBS image in place (row by row).
 */
void
leia_scam_swap_halves_gray8(uint8_t *img, uint32_t width, uint32_t height, uint32_t pitch);

/*
 * Source-rate meter. The SR tracking camera's rate is not published anywhere
 * the plug-in can read (the channel header has no rate and no timestamp), and
 * the field showed it is NOT the 30 Hz once assumed: 58-64 Hz on an SR laptop.
 * So the plug-in measures it over the first LEIA_SCAM_RATE_WINDOW frames of
 * each open and advertises that at the next enumerate; until then it
 * advertises 0 = unknown (the runtime measures too).
 */
#define LEIA_SCAM_RATE_WINDOW 60u
#define LEIA_SCAM_RATE_GAP_NS (500ll * 1000 * 1000) //!< a longer gap restarts the window

struct leia_scam_rate
{
	int64_t first_ns, last_ns;
	uint32_t intervals;
	float rate; //!< last completed window, Hz; 0 = none yet
};

/*!
 * Feed one new frame's arrival time.
 * @return true exactly when this frame completed a window (@p r->rate updated).
 */
bool
leia_scam_rate_push(struct leia_scam_rate *r, int64_t t_ns);

/*
 * Keep-alive eye-pair accounting. The SR v2 eye-pair callback is an ECHO of
 * srEyeTrackerPredict (the SDK publishes to its streams only at the end of
 * predict; PredictingEyeTracker::predict -> eyePairStreams.update), so a
 * weaver-less instance whose tracker nobody predicts on sees ZERO callbacks
 * however many faces the camera sees. The keep-alive therefore polls predict
 * itself and counts DISTINCT tracker frames (frameId changes).
 */
struct leia_scam_pair_count
{
	uint64_t last_frame_id;
	bool have_last;
	uint64_t polls;      //!< predict calls that succeeded
	uint64_t new_frames; //!< ...that carried a tracker frame not seen before
	uint64_t with_eyes;  //!< ...of which both eyes are in front of the display (z > 0)
};

//! Account one successful predict. @return true if it was a NEW tracker frame.
bool
leia_scam_pair_count_push(struct leia_scam_pair_count *c, uint64_t frame_id, double left_z, double right_z);

/*
 * Platform device hint (XrStereoCameraPropertiesDXR::platformDeviceHint).
 *
 * The SR tracker holds the physical camera, so a capture stack that lists it
 * next to the runtime's stereo camera offers a device that can only ever fail
 * with "in use". The hint lets it hide that duplicate. The SR SDK does not name
 * the camera, and "the device currently open BY the tracker process" is not
 * observable from user mode without enumerating kernel handles, so the
 * provider enumerates the OS's video-capture devices and picks the SR tracking
 * camera by, in order:
 *
 *   1. its USB VID:PID, from a small list of known SR tracking cameras
 *      (LEIA_SCAM_KNOWN_TRACKING_CAMERAS); then
 *   2. its driver's friendly name containing "tracking camera"
 *      (case-insensitive) — the name the SR tracking camera reports.
 *
 * The hint is the USB id in the "vvvv:pppp" form (lower-case hex) — the same
 * string Chromium's model_id carries for a USB camera, and immune to the
 * two-interface-class path spelling (MediaFoundation vs DirectShow). A
 * candidate with no USB id contributes its device path instead. The hint is
 * filled only when the candidates reduce to exactly ONE distinct value:
 * hiding the wrong camera (a user's own webcam) is worse than hiding none.
 */

//! Size of the hint buffer (XR_STEREO_CAMERA_PLATFORM_HINT_MAX_SIZE_DXR).
#define LEIA_SCAM_HINT_MAX 256u

struct leia_scam_video_device
{
	const char *path;          //!< OS device-interface path (\\?\usb#vid_...#{guid}\...), may be NULL
	const char *friendly_name; //!< driver friendly name, may be NULL
};

/*!
 * Extract a USB "vid_XXXX&pid_YYYY" pair (any case) from a device path or
 * instance id, or accept an already-formed "XXXX:YYYY".
 */
bool
leia_scam_usb_id(const char *s, uint16_t *vid, uint16_t *pid);

//! Is this device an SR tracking camera (known VID:PID, else name match)?
bool
leia_scam_is_tracking_camera(const struct leia_scam_video_device *dev);

/*!
 * Pick the hint from the enumerated devices. Writes "" and returns false
 * unless exactly one distinct candidate exists.
 */
bool
leia_scam_pick_platform_hint(const struct leia_scam_video_device *devs, size_t count, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
