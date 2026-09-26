// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Stereo camera source (runtime ADR-043, L1) over the SR eye
 *         tracker's raw-camera shared memory. See leia_stereo_camera.h.
 *
 * ## The channel (reader-defined; see leia_stereo_camera_parse.h)
 *
 * The SR eyetracker's MediaFoundation camera thread writes the newest frame as
 * `{u64 rawImageFormat=0, u64 compressedSize, u64 height, u64 width}` + one
 * side-by-side MJPEG into `Global\SREyetrackerRawCamera`, under
 * `Global\SREyetrackerRawCameraMutex`, then signals
 * `Global\SREyetrackerRawCameraEvent`. When its camera stops it zeroes the
 * block. There is no sequence number and no timestamp.
 *
 * ## Rules this file follows (ADR-043 §6, spec §6)
 *
 *  - **Never take the device.** We open the three named objects READ-ONLY
 *    (SYNCHRONIZE on the mutex, FILE_MAP_READ on the section) and never touch
 *    the camera; the tracker keeps it.
 *  - **Never wait on the event.** It is AUTO-RESET and shared by every reader:
 *    a wait here would steal the wake-up of another reader (the vendor's own
 *    apps). We do not even open it. We POLL the block at ~64 Hz (2x the 30 Hz
 *    source) and detect a new frame by content fingerprint; the runtime then
 *    fans out with its own per-stream wake handles.
 *  - **Hold the writer's mutex only for a memcpy** of the JPEG (~100-300 KB,
 *    tens of microseconds); decode OUTSIDE it, so the tracker never waits on
 *    our decoder.
 *  - **Calibration of the ACTIVE device.** The serial is the FIRST entry of
 *    `Global\sharedDeviceSerialMemory` — SRService writes the primary device
 *    first, and the SR eyetracker keys its own calibration the same way
 *    (PredictingEyeTracker: `readDeviceSerialsFromSharedMemory()[0]`). Only
 *    `%ProgramData%\Simulated Reality\Devices\<serial>\` is read; never "the
 *    first folder on the machine" (one field box carried eleven). Missing or
 *    unreadable => no CALIBRATED bit and a WARN naming the serial; never a
 *    fallback to another folder. The lens serial API (`srLensGetSerialNumber`)
 *    is deliberately NOT used: on the SRService side it force-enables and
 *    reconnects the display's controller board.
 *  - **Keep-alive.** Frames exist only while the tracker runs. While a camera
 *    is open we hold an SR v2 instance with an eye tracker initialised (the
 *    same weaver-less pattern leia_sr_predict_trace.cpp uses, which the SR SDK
 *    owner confirmed legal as a second instance per process). No v2 runtime
 *    => no keep-alive: frames then flow only while some app weaves (WARN once).
 *  - **Nothing throws across the C boundary**: every SDK / OpenCV / STL call
 *    is inside try/catch.
 *
 * JPEG decoding uses the OpenCV the plug-in already links (the SR SDK's
 * opencv_world, `cv::imdecode(IMREAD_GRAYSCALE)`) — no new dependency. The SR
 * tracker camera is greyscale, so the source is GRAY8 + MONOCHROME.
 *
 * @ingroup drv_leia
 */

#include "leia_stereo_camera.h"

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA

#include "leia_stereo_camera_parse.h"

#ifdef DXR_LEIA_HAS_SR_V2
#include "leia_sr_v2_common.h"
#include <sr/sr_eye_tracker.h>
#endif

#include "os/os_time.h"
#include "util/u_logging.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr const char *k_cam_mutex = "Global\\SREyetrackerRawCameraMutex";
constexpr const char *k_cam_memory = "Global\\SREyetrackerRawCamera";
constexpr const char *k_serial_mutex = "Global\\sharedDeviceSerialMutex";
constexpr const char *k_serial_memory = "Global\\sharedDeviceSerialMemory";

//! Poll period: ~2x the tracker's 30 Hz (Sleep(8) lands on the ~15.6 ms tick).
constexpr DWORD k_poll_ms = 8;
//! How long the writer's mutex may be waited for (it is held for a memcpy).
constexpr DWORD k_mutex_wait_ms = 20;
//! No new frame for this long while the block is zeroed = tracker stopped.
constexpr int64_t k_suspend_after_ns = 1000ll * 1000 * 1000;
//! Re-try opening the named objects at most this often.
constexpr int64_t k_reopen_ns = 500ll * 1000 * 1000;
//! Default per-eye size when no frame is present at enumerate time.
constexpr uint32_t k_default_eye_w = 640, k_default_eye_h = 480;

bool
env_is(const char *name, char c)
{
	const char *v = std::getenv(name);
	return v != nullptr && v[0] == c;
}


/*
 *
 * Enumerated camera (resolved at enumerate, read by the other slots).
 *
 */

struct resolved_camera
{
	bool present = false;
	bool calibrated = false;
	bool swap = false;
	char serial[LEIA_SCAM_SERIAL_LEN + 1] = {0};
	uint32_t eye_w = k_default_eye_w, eye_h = k_default_eye_h;
	leia_scam_eye_calibration cal = {};
};

std::mutex g_resolve_mutex;
resolved_camera g_cam;

//! Read the primary (first) device serial. Empty string = none.
void
read_primary_serial(char out[LEIA_SCAM_SERIAL_LEN + 1])
{
	out[0] = '\0';
	HANDLE mtx = OpenMutexA(SYNCHRONIZE, FALSE, k_serial_mutex);
	HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, k_serial_memory);
	if (map == nullptr) {
		if (mtx != nullptr) {
			CloseHandle(mtx);
		}
		return;
	}
	const uint8_t *view = static_cast<const uint8_t *>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
	if (view != nullptr) {
		MEMORY_BASIC_INFORMATION mbi = {};
		SIZE_T region = VirtualQuery(view, &mbi, sizeof(mbi)) != 0 ? mbi.RegionSize : 0;
		size_t n = region < LEIA_SCAM_SERIAL_MEMORY_SIZE ? (size_t)region : LEIA_SCAM_SERIAL_MEMORY_SIZE;
		bool locked = mtx != nullptr && WaitForSingleObject(mtx, 200) != WAIT_TIMEOUT;
		std::vector<uint8_t> copy(view, view + n);
		if (locked) {
			ReleaseMutex(mtx);
		}
		char serials[1][LEIA_SCAM_SERIAL_LEN + 1];
		if (leia_scam_parse_serials(copy.data(), copy.size(), serials, 1) > 0) {
			std::snprintf(out, LEIA_SCAM_SERIAL_LEN + 1, "%s", serials[0]);
		}
		UnmapViewOfFile(view);
	}
	CloseHandle(map);
	if (mtx != nullptr) {
		CloseHandle(mtx);
	}
}

bool
read_text_file(const std::string &path, std::string &out)
{
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return false;
	}
	char buf[4096];
	size_t n;
	out.clear();
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < (1u << 20)) {
		out.append(buf, n);
	}
	std::fclose(f);
	return !out.empty();
}

//! Per-eye size from a frame already in the block, if any.
void
peek_eye_size(uint32_t *eye_w, uint32_t *eye_h)
{
	HANDLE mtx = OpenMutexA(SYNCHRONIZE, FALSE, k_cam_mutex);
	HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, k_cam_memory);
	if (map != nullptr) {
		const uint8_t *view = static_cast<const uint8_t *>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
		if (view != nullptr) {
			MEMORY_BASIC_INFORMATION mbi = {};
			SIZE_T region = VirtualQuery(view, &mbi, sizeof(mbi)) != 0 ? mbi.RegionSize : 0;
			if (mtx == nullptr || WaitForSingleObject(mtx, 100) != WAIT_TIMEOUT) {
				leia_scam_header h;
				if (leia_scam_header_check(view, region, &h) == LEIA_SCAM_HEADER_OK) {
					*eye_w = (uint32_t)(h.width / 2);
					*eye_h = (uint32_t)h.height;
				}
				if (mtx != nullptr) {
					ReleaseMutex(mtx);
				}
			}
			UnmapViewOfFile(view);
		}
		CloseHandle(map);
	}
	if (mtx != nullptr) {
		CloseHandle(mtx);
	}
}

void
resolve_locked()
{
	resolved_camera c;
	if (env_is("DXR_LEIA_STEREO_CAMERA", '0')) {
		g_cam = c;
		return;
	}
	const char *serial_override = std::getenv("DXR_LEIA_STEREO_CAMERA_SERIAL");
	if (serial_override != nullptr && serial_override[0] != '\0') {
		std::snprintf(c.serial, sizeof(c.serial), "%s", serial_override);
		U_LOG_W("leia stereo camera: calibration serial OVERRIDDEN by DXR_LEIA_STEREO_CAMERA_SERIAL=%s",
		        c.serial);
	} else {
		read_primary_serial(c.serial);
	}
	if (c.serial[0] == '\0') {
		U_LOG_W(
		    "leia stereo camera: no SR device serial in %s — no camera advertised (is SRService running "
		    "with a device attached?)",
		    k_serial_memory);
		g_cam = c;
		return;
	}
	c.present = true;
	peek_eye_size(&c.eye_w, &c.eye_h);

	const char *pd = std::getenv("ProgramData");
	std::string dir =
	    std::string(pd != nullptr ? pd : "C:\\ProgramData") + "\\Simulated Reality\\Devices\\" + c.serial;
	std::string intr, extr;
	char why[160] = {0};
	leia_scam_calibration raw = {};
	if (!read_text_file(dir + "\\intrinsics.yml", intr) || !read_text_file(dir + "\\extrinsics.yml", extr)) {
		U_LOG_W(
		    "leia stereo camera: no calibration for the ACTIVE device %s (%s\\{intrinsics,extrinsics}.yml "
		    "unreadable) — RAW only; NOT falling back to another device's folder",
		    c.serial, dir.c_str());
	} else if (!leia_scam_parse_calibration(intr.c_str(), intr.size(), extr.c_str(), extr.size(), &raw, why,
	                                        sizeof(why))) {
		U_LOG_W("leia stereo camera: calibration of device %s rejected (%s) — RAW only", c.serial, why);
	} else {
		c.swap = leia_scam_decide_swap(&raw, std::getenv("DXR_LEIA_STEREO_CAMERA_SWAP"));
		leia_scam_calibration_for_runtime(&raw, c.swap, &c.cal);
		c.calibrated = true;
		U_LOG_W(
		    "leia stereo camera: device %s calibrated from %s — baseline %.2f mm (T = %.2f %.2f %.2f), "
		    "left-eye fx %.2f, %s halves (camera %s = SBS left)",
		    c.serial, dir.c_str(), std::sqrt(raw.T[0] * raw.T[0] + raw.T[1] * raw.T[1] + raw.T[2] * raw.T[2]),
		    raw.T[0], raw.T[1], raw.T[2], c.cal.k[0][0], c.swap ? "SWAPPED" : "native", c.swap ? "2" : "1");
	}
	g_cam = c;
}


/*
 *
 * Keep-alive: an SR v2 eye tracker on our own instance while any camera is open.
 *
 */

struct keepalive
{
	std::mutex m;
	int refs = 0;
	std::atomic<uint64_t> pairs{0};
#ifdef DXR_LEIA_HAS_SR_V2
	SrInstance instance = nullptr;
	SrEyeTracker tracker = nullptr;
#endif
	bool warned_unavailable = false;
};

keepalive g_ka;

#ifdef DXR_LEIA_HAS_SR_V2
void SR_CALL
on_keepalive_pair(const SrEyePair *pair, void *user_data)
{
	(void)pair;
	static_cast<keepalive *>(user_data)->pairs.fetch_add(1, std::memory_order_relaxed);
}
#endif

void
keepalive_acquire()
{
	std::lock_guard<std::mutex> lk(g_ka.m);
	if (g_ka.refs++ > 0) {
		return;
	}
	if (env_is("DXR_LEIA_STEREO_CAMERA_KEEPALIVE", '0')) {
		U_LOG_W(
		    "leia stereo camera: keep-alive disabled (DXR_LEIA_STEREO_CAMERA_KEEPALIVE=0) — frames only "
		    "while an app weaves");
		return;
	}
#ifdef DXR_LEIA_HAS_SR_V2
	try {
		g_ka.pairs = 0;
		if (!leia_sr_v2_create_instance(3.0, &g_ka.instance) || g_ka.instance == nullptr) {
			g_ka.instance = nullptr;
			throw 1;
		}
		SrEyeTrackerCreateInfo tci{};
		tci.sType = SR_TYPE_EYE_TRACKER_CREATE_INFO;
		tci.pNext = nullptr;
		tci.enablePrediction = SR_TRUE;
		SrResult r = srCreateEyeTracker(g_ka.instance, &tci, &g_ka.tracker);
		if (!SR_SUCCEEDED(r) || g_ka.tracker == nullptr) {
			U_LOG_W("leia stereo camera: keep-alive srCreateEyeTracker failed (%s)",
			        leia_sr_v2_result_str(r));
			g_ka.tracker = nullptr;
			throw 2;
		}
		r = srEyeTrackerAddCallback(g_ka.tracker, on_keepalive_pair, &g_ka);
		if (!SR_SUCCEEDED(r)) {
			U_LOG_W("leia stereo camera: keep-alive srEyeTrackerAddCallback failed (%s)",
			        leia_sr_v2_result_str(r));
			throw 3;
		}
		// Senses before srInitialize (sr_eye_tracker.h); this starts tracking.
		if (!leia_sr_v2_initialize(g_ka.instance)) {
			srEyeTrackerRemoveCallback(g_ka.tracker, on_keepalive_pair, &g_ka);
			throw 4;
		}
		U_LOG_W(
		    "leia stereo camera: keep-alive UP (own SR v2 instance + eye tracker) — the tracker stays on "
		    "while the camera is open");
		return;
	} catch (...) {
		if (g_ka.tracker != nullptr) {
			srDestroyEyeTracker(g_ka.tracker);
			g_ka.tracker = nullptr;
		}
		if (g_ka.instance != nullptr) {
			srDestroyInstance(g_ka.instance);
			g_ka.instance = nullptr;
		}
		U_LOG_W("leia stereo camera: keep-alive could not start — frames only while an app weaves");
		return;
	}
#else
	if (!g_ka.warned_unavailable) {
		g_ka.warned_unavailable = true;
		U_LOG_W(
		    "leia stereo camera: built without the SR v2 SDK — no tracker keep-alive; frames only while "
		    "an app weaves");
	}
#endif
}

void
keepalive_release()
{
	std::lock_guard<std::mutex> lk(g_ka.m);
	if (g_ka.refs <= 0 || --g_ka.refs > 0) {
		return;
	}
#ifdef DXR_LEIA_HAS_SR_V2
	try {
		if (g_ka.tracker != nullptr) {
			srEyeTrackerRemoveCallback(g_ka.tracker, on_keepalive_pair, &g_ka);
			srDestroyEyeTracker(g_ka.tracker);
			g_ka.tracker = nullptr;
		}
		if (g_ka.instance != nullptr) {
			srDestroyInstance(g_ka.instance);
			g_ka.instance = nullptr;
			U_LOG_W("leia stereo camera: keep-alive released after %llu eye pairs",
			        (unsigned long long)g_ka.pairs.load());
		}
	} catch (...) {
		g_ka.tracker = nullptr;
		g_ka.instance = nullptr;
	}
#endif
}

} // namespace


/*
 *
 * The open camera.
 *
 */

struct xrt_plugin_stereo_camera
{
	HANDLE mutex = nullptr;
	HANDLE mapping = nullptr;
	const uint8_t *view = nullptr;
	SIZE_T view_size = 0;
	int64_t next_open_try_ns = 0;

	bool swap = false;
	uint32_t eye_w = 0, eye_h = 0;

	std::vector<uint8_t> jpeg;
	std::vector<uint8_t> gray;
	uint32_t gray_w = 0, gray_h = 0;

	uint64_t last_fp = 0;
	uint64_t seq = 0;
	int64_t last_new_ns = 0;
	bool block_empty = false;

	// one-shot diagnostics
	bool warned_bad_header = false;
	bool warned_decode = false;
	bool warned_size = false;
	bool logged_first = false;
};

static void
cam_close_channel(xrt_plugin_stereo_camera *c)
{
	if (c->view != nullptr) {
		UnmapViewOfFile(c->view);
		c->view = nullptr;
	}
	if (c->mapping != nullptr) {
		CloseHandle(c->mapping);
		c->mapping = nullptr;
	}
	if (c->mutex != nullptr) {
		CloseHandle(c->mutex);
		c->mutex = nullptr;
	}
	c->view_size = 0;
}

//! Open the tracker's named objects READ-ONLY (never the event).
static bool
cam_open_channel(xrt_plugin_stereo_camera *c, int64_t now)
{
	if (c->view != nullptr) {
		return true;
	}
	if (now < c->next_open_try_ns) {
		return false;
	}
	c->next_open_try_ns = now + k_reopen_ns;
	// SYNCHRONIZE is all a wait + ReleaseMutex needs; ask for MODIFY_STATE too
	// where the ACL grants it. Never the event (see the file comment).
	c->mutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, k_cam_mutex);
	if (c->mutex == nullptr) {
		c->mutex = OpenMutexA(SYNCHRONIZE, FALSE, k_cam_mutex);
	}
	c->mapping = OpenFileMappingA(FILE_MAP_READ, FALSE, k_cam_memory);
	if (c->mutex == nullptr || c->mapping == nullptr) {
		cam_close_channel(c); // the tracker has not created them yet
		return false;
	}
	c->view = static_cast<const uint8_t *>(MapViewOfFile(c->mapping, FILE_MAP_READ, 0, 0, 0));
	MEMORY_BASIC_INFORMATION mbi = {};
	if (c->view == nullptr || VirtualQuery(c->view, &mbi, sizeof(mbi)) == 0) {
		cam_close_channel(c);
		return false;
	}
	c->view_size = mbi.RegionSize;
	U_LOG_W(
	    "leia stereo camera: reading %s (%llu bytes, read-only; polling, never waiting on the auto-reset "
	    "event)",
	    k_cam_memory, (unsigned long long)c->view_size);
	return true;
}

enum poll_result
{
	POLL_NOTHING,
	POLL_NEW,
	POLL_EMPTY,
};

//! Under the writer's mutex: copy the JPEG out iff it is a new frame.
static poll_result
cam_poll(xrt_plugin_stereo_camera *c, leia_scam_header *hdr)
{
	DWORD w = WaitForSingleObject(c->mutex, k_mutex_wait_ms);
	if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
		return POLL_NOTHING; // writer busy; next poll
	}
	poll_result res = POLL_NOTHING;
	switch (leia_scam_header_check(c->view, c->view_size, hdr)) {
	case LEIA_SCAM_HEADER_EMPTY: res = POLL_EMPTY; break;
	case LEIA_SCAM_HEADER_BAD:
		if (!c->warned_bad_header) {
			c->warned_bad_header = true;
			U_LOG_W(
			    "leia stereo camera: raw-camera header out of contract (format %llu, size %llu, %llux%llu) "
			    "— ignored (did an SR tracker update change the layout?)",
			    (unsigned long long)hdr->raw_image_format, (unsigned long long)hdr->compressed_size,
			    (unsigned long long)hdr->width, (unsigned long long)hdr->height);
		}
		break;
	case LEIA_SCAM_HEADER_OK: {
		const uint8_t *jpg = c->view + LEIA_SCAM_HEADER_SIZE;
		uint64_t fp = leia_scam_fingerprint(jpg, (size_t)hdr->compressed_size);
		if (fp != c->last_fp) {
			c->last_fp = fp;
			c->jpeg.assign(jpg, jpg + hdr->compressed_size);
			res = POLL_NEW;
		}
		break;
	}
	}
	ReleaseMutex(c->mutex);
	return res;
}

//! Decode the copied JPEG to GRAY8 SBS (outside the mutex).
static bool
cam_decode(xrt_plugin_stereo_camera *c, const leia_scam_header &hdr)
{
	uint32_t jw = 0, jh = 0, comps = 0;
	if (!leia_scam_jpeg_dims(c->jpeg.data(), c->jpeg.size(), &jw, &jh, &comps) || jw != hdr.width ||
	    jh != hdr.height) {
		if (!c->warned_decode) {
			c->warned_decode = true;
			U_LOG_W(
			    "leia stereo camera: JPEG SOF %ux%u does not match the header %llux%llu — frame dropped",
			    jw, jh, (unsigned long long)hdr.width, (unsigned long long)hdr.height);
		}
		return false;
	}
	try {
		cv::Mat buf(1, (int)c->jpeg.size(), CV_8UC1, c->jpeg.data());
		cv::Mat img = cv::imdecode(buf, cv::IMREAD_GRAYSCALE);
		if (img.empty() || img.type() != CV_8UC1 || (uint32_t)img.cols != jw || (uint32_t)img.rows != jh) {
			throw 0;
		}
		c->gray_w = jw;
		c->gray_h = jh;
		c->gray.resize((size_t)jw * jh);
		for (uint32_t y = 0; y < jh; y++) {
			std::memcpy(c->gray.data() + (size_t)y * jw, img.ptr<uint8_t>((int)y), jw);
		}
	} catch (...) {
		if (!c->warned_decode) {
			c->warned_decode = true;
			U_LOG_W("leia stereo camera: JPEG decode failed (%llu bytes, %ux%u) — frame dropped",
			        (unsigned long long)c->jpeg.size(), jw, jh);
		}
		return false;
	}
	if (c->swap) {
		leia_scam_swap_halves_gray8(c->gray.data(), c->gray_w, c->gray_h, c->gray_w);
	}
	if ((c->gray_w != 2 * c->eye_w || c->gray_h != c->eye_h) && !c->warned_size) {
		c->warned_size = true;
		U_LOG_W(
		    "leia stereo camera: frames are %ux%u but %ux%u was advertised at enumerate — the runtime drops "
		    "them; restart the service with the tracker running so enumerate sees a frame",
		    c->gray_w, c->gray_h, 2 * c->eye_w, c->eye_h);
	}
	return true;
}


/*
 *
 * Slots.
 *
 */

extern "C" uint32_t
leia_stereo_camera_enumerate(struct xrt_plugin_instance *inst,
                             uint32_t capacity,
                             struct xrt_plugin_stereo_camera_info *out)
{
	(void)inst;
	try {
		std::lock_guard<std::mutex> lk(g_resolve_mutex);
		resolve_locked();
		if (!g_cam.present) {
			return 0;
		}
		if (capacity >= 1 && out != nullptr) {
			uint32_t sz = out->struct_size;
			xrt_plugin_stereo_camera_info info;
			std::memset(&info, 0, sizeof(info));
			info.struct_size = (uint32_t)sizeof(info);
			std::snprintf(info.display_name, sizeof(info.display_name), "Leia SR tracking camera");
			std::snprintf(info.device_identity, sizeof(info.device_identity), "leia-sr:%s", g_cam.serial);
			info.platform_device_hint[0] = '\0'; // MF symbolic link: open question (roadmap §D)
			info.flags = XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING |
			             XRT_PLUGIN_STEREO_CAMERA_USER_FACING | XRT_PLUGIN_STEREO_CAMERA_MONOCHROME;
			if (g_cam.calibrated) {
				info.flags |= XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
			}
			info.eye_width = g_cam.eye_w;
			info.eye_height = g_cam.eye_h;
			info.max_frame_rate = 30.0f;
			info.native_format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8;
			std::memcpy(out, &info,
			            sz < sizeof(info) ? sz : sizeof(info)); // ADR-020: never past struct_size
			out->struct_size = sz;
		}
		return 1;
	} catch (...) {
		return 0;
	}
}

extern "C" xrt_result_t
leia_stereo_camera_get_calibration(struct xrt_plugin_instance *inst,
                                   uint32_t index,
                                   struct xrt_plugin_stereo_camera_calibration *out)
{
	(void)inst;
	try {
		std::lock_guard<std::mutex> lk(g_resolve_mutex);
		if (index != 0 || out == nullptr || !g_cam.present || !g_cam.calibrated) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		uint32_t sz = out->struct_size;
		xrt_plugin_stereo_camera_calibration c;
		std::memset(&c, 0, sizeof(c));
		c.struct_size = (uint32_t)sizeof(c);
		// The SR tracker calibrates at its native per-eye size (the frames').
		c.image_width = g_cam.eye_w;
		c.image_height = g_cam.eye_h;
		for (int e = 0; e < 2; e++) {
			for (int k = 0; k < 4; k++) {
				c.k[e][k] = g_cam.cal.k[e][k];
			}
			for (int k = 0; k < 8; k++) {
				c.distortion[e][k] = g_cam.cal.d[e][k];
			}
		}
		c.distortion_model = g_cam.cal.model == 2 ? XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN8
		                                          : XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN5;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				c.rotation_right_from_left[i][j] = g_cam.cal.R[i][j];
			}
			c.translation_right_from_left_mm[i] = g_cam.cal.T[i];
		}
		std::memcpy(out, &c, sz < sizeof(c) ? sz : sizeof(c));
		out->struct_size = sz;
		return XRT_SUCCESS;
	} catch (...) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
}

extern "C" xrt_result_t
leia_stereo_camera_open(struct xrt_plugin_instance *inst, uint32_t index, struct xrt_plugin_stereo_camera **out_cam)
{
	(void)inst;
	if (index != 0 || out_cam == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	try {
		xrt_plugin_stereo_camera *c = new xrt_plugin_stereo_camera();
		{
			std::lock_guard<std::mutex> lk(g_resolve_mutex);
			if (!g_cam.present) {
				delete c;
				return XRT_ERROR_FEATURE_NOT_SUPPORTED;
			}
			c->swap = g_cam.calibrated && g_cam.swap;
			c->eye_w = g_cam.eye_w;
			c->eye_h = g_cam.eye_h;
		}
		c->jpeg.reserve(512 * 1024);
		keepalive_acquire();
		cam_open_channel(c, os_monotonic_get_ns());
		*out_cam = c;
		return XRT_SUCCESS;
	} catch (...) {
		return XRT_ERROR_ALLOCATION;
	}
}

extern "C" uint32_t
leia_stereo_camera_wait_frame(struct xrt_plugin_stereo_camera *c,
                              int64_t timeout_ns,
                              struct xrt_plugin_stereo_camera_frame *out)
{
	if (c == nullptr || out == nullptr) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
	}
	try {
		const int64_t deadline = os_monotonic_get_ns() + (timeout_ns > 0 ? timeout_ns : 0);
		for (;;) {
			int64_t now = os_monotonic_get_ns();
			if (cam_open_channel(c, now)) {
				leia_scam_header hdr;
				poll_result p = cam_poll(c, &hdr);
				if (p == POLL_NEW) {
					// Arrival time in the plug-in: the channel has no exposure stamp.
					int64_t arrival = os_monotonic_get_ns();
					if (cam_decode(c, hdr)) {
						c->block_empty = false;
						c->last_new_ns = arrival;
						std::memset(out, 0, sizeof(*out));
						out->sequence = ++c->seq;
						out->time_ns = arrival;
						out->time_is_exposure = false;
						out->width = c->gray_w;
						out->height = c->gray_h;
						out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8;
						out->planes[0] = c->gray.data();
						out->pitches[0] = c->gray_w;
						if (!c->logged_first) {
							c->logged_first = true;
							U_LOG_W(
							    "leia stereo camera: first frame %ux%u GRAY8 (JPEG %llu "
							    "bytes)%s",
							    c->gray_w, c->gray_h, (unsigned long long)c->jpeg.size(),
							    c->swap ? ", halves swapped" : "");
						}
						return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
					}
				} else if (p == POLL_EMPTY) {
					c->block_empty = true;
				}
			}
			now = os_monotonic_get_ns();
			if (now >= deadline) {
				// The tracker zeroes the block when its camera stops: that is
				// SUSPENDED; silence without it is a (possibly long) TIMEOUT
				// the runtime turns into WAITING / SUSPENDED itself.
				if (c->block_empty && c->last_new_ns > 0 && now - c->last_new_ns > k_suspend_after_ns) {
					return XRT_PLUGIN_STEREO_CAMERA_WAIT_SUSPENDED;
				}
				return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
			}
			int64_t left_ms = (deadline - now) / 1000000;
			Sleep((DWORD)(left_ms < (int64_t)k_poll_ms ? (left_ms > 0 ? left_ms : 1) : k_poll_ms));
		}
	} catch (...) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
	}
}

extern "C" void
leia_stereo_camera_release_frame(struct xrt_plugin_stereo_camera *c)
{
	(void)c; // the GRAY8 buffer is ours until the next wait_frame
}

extern "C" void
leia_stereo_camera_close(struct xrt_plugin_stereo_camera *c)
{
	if (c == nullptr) {
		return;
	}
	try {
		U_LOG_W("leia stereo camera: closed after %llu frames", (unsigned long long)c->seq);
		cam_close_channel(c);
		keepalive_release();
		delete c;
	} catch (...) {
	}
}

#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA
