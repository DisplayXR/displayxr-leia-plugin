// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Android stereo camera source. See the header for the contract and
 *         knobs; docs/android-stereo-camera.md for the design and the device
 *         results it rests on.
 *
 * Shape: ONE YUV_420_888 stream per physical lens of the logical pair, at the
 * sensor's 4:3 aspect (1280x960 for a 1280x720 eye), and nothing else:
 *
 *   logical pair ──┬─ main lens: 1280x960 YUV ─┬─ centre 1280x720 crop -> SBS eye
 *                  │                           └─ 2x2 box -> 640x480 Y8 -> CNSDK in-app tracking
 *                  └─ sub lens:  1280x960 YUV ──── centre 1280x720 crop -> SBS eye
 *
 * Why this and not the vendor camera app's shape (GPU eye streams + a third
 * 640x480 YUV stream on one lens): a CPU NV12 path needs CPU-readable eyes, the
 * NDK refuses a CPU-readable PRIVATE reader (AImageReader_newWithUsage ->
 * AMEDIA_ERROR_INVALID_PARAMETER, measured), and two YUV outputs on one lens
 * abort this HAL (measured in the A0 probe). The 4:3 capture keeps the tracking
 * frame at the full sensor field of view, i.e. exactly what the vendor tracking
 * service itself feeds its detector, with the same intrinsics. A GPU path
 * (PRIVATE eyes, AHardwareBuffer transport) is the A4 performance phase.
 *
 * - The two eyes arrive on the image readers' own threads with identical
 *   sensor timestamps; the second of a pair packs both into one SBS NV12
 *   buffer (one copy), publishes it latest-wins, and wakes wait_frame.
 * - The tracking frame is never processed on a camera thread: the pack step
 *   writes it into a latest-frame slot and a feeder thread hands it to CNSDK
 *   (detection runs on the caller's thread and can block for 500 ms when no
 *   face is in view). Stale frames are dropped, never queued.
 * - The main lens is the one whose characteristics carry the factory block,
 *   which is also the lens the vendor tracking service tracks from, so the
 *   in-app tracker sees the same camera pose and intrinsics.
 * - Timestamps: the sensor clock is BOOTTIME (timestamp source REALTIME);
 *   published frames are converted to CLOCK_MONOTONIC per frame, CNSDK gets
 *   BOOTTIME (its "system" timestamp space).
 * - Watchdog: a pair that stops for kStallNs is treated like a device error
 *   (the HAL's silent-stall recovery reports nothing): the session is torn
 *   down and reopened with back-off, wait_frame reports SUSPENDED meanwhile and
 *   ERROR when the device stays gone. Losing the device right after opening
 *   it twice in a row is also terminal: that is a HAL failing the open, and
 *   reopening into it only crashes it again.
 * - The pair is opened only once the camera service reports both lenses free,
 *   after tracking has been handed in-app (so the tracking service has
 *   released its lens), never by evicting a client that still streams one.
 *
 * @ingroup drv_leia_android
 */

#include "leia_stereo_camera_android.h"

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA

#include "leia_stereo_camera_android_calib.h"
#include "leia_cnsdk.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <sys/system_properties.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef LEIA_SCAM_HARNESS
#include <android/log.h>
#define SCAM_LOG_W(...) __android_log_print(ANDROID_LOG_WARN, "DXRSCAM", __VA_ARGS__)
#define SCAM_LOG_I(...) __android_log_print(ANDROID_LOG_INFO, "DXRSCAM", __VA_ARGS__)
#else
#include "util/u_logging.h"
#define SCAM_LOG_W(...) U_LOG_W(__VA_ARGS__)
#define SCAM_LOG_I(...) U_LOG_I(__VA_ARGS__)
#endif

namespace {

constexpr int64_t kNsPerMs = 1000000;
constexpr int64_t kStallNs = 1500 * kNsPerMs;      //!< no pair for this long = stalled
constexpr int64_t kReopenFirstNs = 500 * kNsPerMs;  //!< first reopen delay
constexpr int64_t kReopenMaxNs = 4000 * kNsPerMs;   //!< back-off cap
constexpr int64_t kReopenFastMaxNs = 1000 * kNsPerMs; //!< back-off cap for the first 10 s down
constexpr int64_t kGiveUpNs = 15000 * kNsPerMs;     //!< device gone this long -> ERROR
constexpr int64_t kEarlyFailNs = 3000 * kNsPerMs;   //!< a loss this soon after open counts as an open failure
constexpr int kEarlyFailMax = 2;                    //!< consecutive open failures -> ERROR, no more reopens
constexpr int64_t kLensFreeNs = 3000 * kNsPerMs;    //!< how long the tracking service gets to release its lens
constexpr int64_t kPairToleranceNs = 1 * kNsPerMs;  //!< measured skew is 0; be lenient
constexpr uint32_t kTrackW = 640, kTrackH = 480;    //!< the in-app detector accepts only this
constexpr int kEyeMaxImages = 4;
constexpr size_t kPendingPerEye = 2;
constexpr int kRateWindow = 60;
//! Where this OEM publishes the factory stereo block (a vendor characteristics
//! key on the logical camera and its main lens). debug.dxr.leia.scam_calib_key_prefix overrides it.
constexpr const char *kCalibKeyPrefix = "com.zte.chi.eeprom.";

int64_t
clock_ns(clockid_t id)
{
	struct timespec ts;
	clock_gettime(id, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

//! BOOTTIME - MONOTONIC, sampled now (grows only across suspend).
int64_t
boot_minus_mono_ns()
{
	const int64_t m0 = clock_ns(CLOCK_MONOTONIC);
	const int64_t b = clock_ns(CLOCK_BOOTTIME);
	const int64_t m1 = clock_ns(CLOCK_MONOTONIC);
	return b - (m0 + (m1 - m0) / 2);
}

std::string
prop_str(const char *name, const char *dflt)
{
	char v[PROP_VALUE_MAX] = {0};
	if (__system_property_get(name, v) > 0) {
		return std::string(v);
	}
	return std::string(dflt);
}

bool
prop_bool(const char *name, bool dflt)
{
	std::string v = prop_str(name, "");
	if (v.empty()) {
		return dflt;
	}
	return !(v == "0" || v == "false" || v == "off");
}

/*
 *
 * JNI: the factory block is a vendor characteristics key, which the NDK
 * cannot look up by name. Read it through CameraCharacteristics.getKeys().
 *
 */

struct jni_env
{
	JavaVM *vm{nullptr};
	JNIEnv *env{nullptr};
	bool attached{false};
	jobject ctx{nullptr};

	bool
	acquire()
	{
		void *vmp = nullptr, *ctxp = nullptr;
		if (!leia_cnsdk_get_jni(&vmp, &ctxp) || vmp == nullptr || ctxp == nullptr) {
			return false;
		}
		vm = (JavaVM *)vmp;
		ctx = (jobject)ctxp;
		jint r = vm->GetEnv((void **)&env, JNI_VERSION_1_6);
		if (r == JNI_EDETACHED) {
			if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
				return false;
			}
			attached = true;
		} else if (r != JNI_OK) {
			return false;
		}
		return env != nullptr;
	}
	~jni_env()
	{
		if (attached && vm != nullptr) {
			vm->DetachCurrentThread();
		}
	}
	bool
	ok()
	{
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
			return false;
		}
		return true;
	}
};

/*!
 * Bytes of the characteristics key of @p cam_id named @p full_name, else of
 * the first listed key whose name ends with @p suffix. Vendor keys are usually
 * NOT in getKeys() (measured on a Lume Pad 2), so the named lookup comes
 * first; it builds the Key exactly as an app would.
 */
bool
read_vendor_block(const char *cam_id, const char *full_name, const char *suffix, std::vector<uint8_t> &out,
                  std::string *out_key)
{
	jni_env j;
	if (!j.acquire()) {
		return false;
	}
	JNIEnv *env = j.env;
	bool found = false;
	env->PushLocalFrame(64);
	do {
		jclass ctx_cls = env->FindClass("android/content/Context");
		if (!j.ok() || ctx_cls == nullptr) {
			break;
		}
		jmethodID get_service = env->GetMethodID(ctx_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
		if (!j.ok()) {
			break;
		}
		jobject cm = env->CallObjectMethod(j.ctx, get_service, env->NewStringUTF("camera"));
		if (!j.ok() || cm == nullptr) {
			break;
		}
		jclass cm_cls = env->FindClass("android/hardware/camera2/CameraManager");
		jmethodID get_cc = env->GetMethodID(cm_cls, "getCameraCharacteristics",
		                                    "(Ljava/lang/String;)Landroid/hardware/camera2/CameraCharacteristics;");
		if (!j.ok()) {
			break;
		}
		jobject cc = env->CallObjectMethod(cm, get_cc, env->NewStringUTF(cam_id));
		if (!j.ok() || cc == nullptr) {
			break;
		}
		jclass cc_cls = env->FindClass("android/hardware/camera2/CameraCharacteristics");
		jclass key_cls = env->FindClass("android/hardware/camera2/CameraCharacteristics$Key");
		jclass list_cls = env->FindClass("java/util/List");
		jmethodID get_keys = env->GetMethodID(cc_cls, "getKeys", "()Ljava/util/List;");
		jmethodID get = env->GetMethodID(cc_cls, "get",
		                                 "(Landroid/hardware/camera2/CameraCharacteristics$Key;)Ljava/lang/Object;");
		jmethodID key_name = env->GetMethodID(key_cls, "getName", "()Ljava/lang/String;");
		jmethodID l_size = env->GetMethodID(list_cls, "size", "()I");
		jmethodID l_get = env->GetMethodID(list_cls, "get", "(I)Ljava/lang/Object;");
		if (!j.ok()) {
			break;
		}
		jclass bytes_cls = env->FindClass("[B");
		if (full_name != nullptr && full_name[0] != '\0') {
			jmethodID key_ctor = env->GetMethodID(key_cls, "<init>", "(Ljava/lang/String;Ljava/lang/Class;)V");
			if (j.ok() && key_ctor != nullptr) {
				jobject key = env->NewObject(key_cls, key_ctor, env->NewStringUTF(full_name), bytes_cls);
				jobject v = j.ok() && key != nullptr ? env->CallObjectMethod(cc, get, key) : nullptr;
				if (j.ok() && v != nullptr && env->IsInstanceOf(v, bytes_cls)) {
					jbyteArray arr = (jbyteArray)v;
					const jsize len = env->GetArrayLength(arr);
					out.resize((size_t)len);
					env->GetByteArrayRegion(arr, 0, len, (jbyte *)out.data());
					found = j.ok() && len > 0;
					if (found && out_key != nullptr) {
						*out_key = full_name;
					}
				}
			}
			if (found) {
				break;
			}
		}
		jobject keys = env->CallObjectMethod(cc, get_keys);
		if (!j.ok() || keys == nullptr) {
			break;
		}
		const jint n = env->CallIntMethod(keys, l_size);
		const size_t slen = strlen(suffix);
		for (jint i = 0; i < n && !found; i++) {
			jobject key = env->CallObjectMethod(keys, l_get, i);
			if (!j.ok() || key == nullptr) {
				continue;
			}
			jstring jname = (jstring)env->CallObjectMethod(key, key_name);
			if (!j.ok() || jname == nullptr) {
				env->DeleteLocalRef(key);
				continue;
			}
			const char *name = env->GetStringUTFChars(jname, nullptr);
			const size_t nlen = name != nullptr ? strlen(name) : 0;
			const bool match = name != nullptr && nlen >= slen && strcmp(name + nlen - slen, suffix) == 0;
			if (match && out_key != nullptr) {
				*out_key = name;
			}
			if (name != nullptr) {
				env->ReleaseStringUTFChars(jname, name);
			}
			if (match) {
				jobject v = env->CallObjectMethod(cc, get, key);
				if (j.ok() && v != nullptr && env->IsInstanceOf(v, bytes_cls)) {
					jbyteArray arr = (jbyteArray)v;
					const jsize len = env->GetArrayLength(arr);
					out.resize((size_t)len);
					env->GetByteArrayRegion(arr, 0, len, (jbyte *)out.data());
					found = j.ok() && len > 0;
				}
			}
			env->DeleteLocalRef(jname);
			env->DeleteLocalRef(key);
		}
	} while (false);
	j.ok();
	env->PopLocalFrame(nullptr);
	return found;
}

/*
 *
 * Discovery.
 *
 */

struct pair_desc
{
	char logical_id[16]{};
	char main_id[16]{}; //!< lens that carries the factory block (= the tracking service's)
	char sub_id[16]{};
	bool front{false};
	uint32_t array_w{0}, array_h{0}; //!< main lens active array
	bool calibrated{false};
	//! The factory block re-expressed at the active array (see ref_w / ref_h).
	leia_scam_factory_calib factory{};
	leia_scam_stream_calib stream{};
	uint64_t block_hash{0};
	//! Size the block's intrinsics were expressed at (= array unless the block
	//! centres on a listed output size, leia_scam_pick_calib_reference).
	uint32_t ref_w{0}, ref_h{0};
	int32_t sensor_orientation{0};
	//! ACAMERA_SENSOR_ORIENTATION was read (the frame orientation is unknown otherwise).
	bool have_sensor_orientation{false};
	//! Clockwise turn applied to each eye after the crop: 90 / 270 when the
	//! lenses are stacked along the sensor's y axis (a phone held upright), so
	//! the published pair is side by side and upright in the device's natural
	//! orientation. debug.dxr.leia.scam_rotate=0|90|180|270 forces it.
	uint32_t rot{0};
	//! Crop of the capture, before the turn (= the eye size when rot is 0 / 180).
	uint32_t crop_w{1280}, crop_h{720};
	//! Published per-eye size (after the turn).
	uint32_t eye_w{1280}, eye_h{720};
	//! What each lens streams: 4:3 at the eye width when the lenses list it
	//! (the eye is then a centre crop and the tracking frame keeps the full
	//! field of view), else the eye size itself.
	uint32_t cap_w{1280}, cap_h{720};
	bool swap_override{false}, swap_value{false};
	float measured_rate{0.0f};
};

std::mutex g_mu;
bool g_scanned = false;
std::vector<pair_desc> g_pairs; //!< index 0 = front when present
ACameraManager *g_mgr = nullptr;

/*
 * Which camera ids the camera service reports in use, from its own
 * availability callbacks. Opening the logical pair while another client (the
 * tracking service) still streams one of its lenses makes the camera service
 * evict that client inside the vendor HAL, and on a phone-class device that
 * aborted the camera provider (SIGABRT in the CHI multicamera notify path)
 * every time, taking the tracking camera down with it. So the pair is opened
 * only once both lenses are free.
 */
std::mutex g_avail_mu;
std::condition_variable g_avail_cv;
std::vector<std::string> g_in_use;

void
avail_on(void *, const char *id)
{
	std::lock_guard<std::mutex> lk(g_avail_mu);
	g_in_use.erase(std::remove(g_in_use.begin(), g_in_use.end(), std::string(id)), g_in_use.end());
	g_avail_cv.notify_all();
}

void
avail_off(void *, const char *id)
{
	std::lock_guard<std::mutex> lk(g_avail_mu);
	if (std::find(g_in_use.begin(), g_in_use.end(), std::string(id)) == g_in_use.end()) {
		g_in_use.emplace_back(id);
	}
	g_avail_cv.notify_all();
}

ACameraManager_AvailabilityCallbacks g_avail_cb{nullptr, avail_on, avail_off};

ACameraManager *
mgr()
{
	if (g_mgr == nullptr) {
		g_mgr = ACameraManager_create();
		// Registered once, long before any open (enumerate scans first), so the
		// initial status of every camera has arrived by the time it is needed.
		if (g_mgr != nullptr && ACameraManager_registerAvailabilityCallback(g_mgr, &g_avail_cb) != ACAMERA_OK) {
			SCAM_LOG_W("leia stereo camera (android): no camera availability callback; lenses are not checked "
			           "before an open");
		}
	}
	return g_mgr;
}

/*!
 * Wait until neither lens of @p d is in use, or @p timeout_ns, or @p abort.
 * Returns the id still in use ("" = both free).
 */
std::string
wait_lenses_free(const pair_desc &d, int64_t timeout_ns, const std::atomic<bool> &abort)
{
	auto busy = [&]() -> std::string {
		for (const char *id : {d.main_id, d.sub_id}) {
			if (std::find(g_in_use.begin(), g_in_use.end(), std::string(id)) != g_in_use.end()) {
				return id;
			}
		}
		return "";
	};
	const int64_t deadline = clock_ns(CLOCK_MONOTONIC) + timeout_ns;
	std::unique_lock<std::mutex> lk(g_avail_mu);
	for (;;) {
		std::string b = busy();
		const int64_t now = clock_ns(CLOCK_MONOTONIC);
		if (b.empty() || abort.load() || now >= deadline) {
			return b;
		}
		g_avail_cv.wait_for(lk, std::chrono::nanoseconds(std::min<int64_t>(deadline - now, 100 * kNsPerMs)));
	}
}

bool
get_i32(const ACameraMetadata *m, uint32_t tag, std::vector<int32_t> &out)
{
	ACameraMetadata_const_entry e{};
	if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count == 0) {
		return false;
	}
	out.assign(e.data.i32, e.data.i32 + e.count);
	return true;
}

bool
get_u8(const ACameraMetadata *m, uint32_t tag, uint8_t *out)
{
	ACameraMetadata_const_entry e{};
	if (ACameraMetadata_getConstEntry(m, tag, &e) != ACAMERA_OK || e.count == 0) {
		return false;
	}
	*out = e.data.u8[0];
	return true;
}

bool
stream_supported(const ACameraMetadata *m, int32_t format, uint32_t w, uint32_t h)
{
	std::vector<int32_t> c;
	if (!get_i32(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, c)) {
		return false;
	}
	for (size_t i = 0; i + 3 < c.size(); i += 4) {
		if (c[i] == format && c[i + 1] == (int32_t)w && c[i + 2] == (int32_t)h &&
		    c[i + 3] == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
			return true;
		}
	}
	return false;
}

void
parse_size(const std::string &s, uint32_t *w, uint32_t *h)
{
	unsigned a = 0, b = 0;
	if (sscanf(s.c_str(), "%ux%u", &a, &b) == 2 && a >= 160 && b >= 120 && a <= 4096 && b <= 4096) {
		*w = a & ~1u;
		*h = b & ~1u;
	}
}

void
scan_locked()
{
	if (g_scanned) {
		return;
	}
	g_scanned = true;
	if (!prop_bool("debug.dxr.leia.scam", true)) {
		SCAM_LOG_W("leia stereo camera (android): disabled by debug.dxr.leia.scam=0");
		return;
	}
	ACameraManager *m = mgr();
	if (m == nullptr) {
		return;
	}
	uint32_t want_w = 1280, want_h = 720;
	parse_size(prop_str("debug.dxr.leia.scam_size", ""), &want_w, &want_h);
	const std::string swap = prop_str("debug.dxr.leia.scam_swap", "auto");

	// The logical pairs are hidden from the camera id list on these devices,
	// so probe ids directly (cheap: an absent id fails at once).
	for (int id = 0; id < 16; id++) {
		char ids[16];
		snprintf(ids, sizeof(ids), "%d", id);
		ACameraMetadata *meta = nullptr;
		if (ACameraManager_getCameraCharacteristics(m, ids, &meta) != ACAMERA_OK || meta == nullptr) {
			continue;
		}
		size_t nphys = 0;
		const char *const *phys = nullptr;
		if (!ACameraMetadata_isLogicalMultiCamera(meta, &nphys, &phys) || nphys != 2) {
			ACameraMetadata_free(meta);
			continue;
		}
		uint8_t facing = 0xff;
		get_u8(meta, ACAMERA_LENS_FACING, &facing);
		pair_desc p;
		snprintf(p.logical_id, sizeof(p.logical_id), "%s", ids);
		p.front = facing == ACAMERA_LENS_FACING_FRONT;
		if (!p.front && facing != ACAMERA_LENS_FACING_BACK) {
			ACameraMetadata_free(meta);
			continue;
		}
		std::string a = phys[0], b = phys[1];
		ACameraMetadata_free(meta);
		std::vector<uint32_t> ref_sizes;
		if ((p.front && !g_pairs.empty() && g_pairs[0].front) ||
		    (!p.front && std::any_of(g_pairs.begin(), g_pairs.end(), [](const pair_desc &q) { return !q.front; }))) {
			continue; // one pair per facing
		}

		// Factory block: on the logical id and on the main lens only.
		const char *suffix = p.front ? "eeprom3d_front" : "eeprom3d_rear";
		const std::string full = prop_str("debug.dxr.leia.scam_calib_key_prefix", kCalibKeyPrefix) + suffix;
		std::vector<uint8_t> blk;
		std::string key;
		bool have = read_vendor_block(p.logical_id, full.c_str(), suffix, blk, &key);
		std::vector<uint8_t> tmp;
		std::string lens_key;
		const bool a_has = read_vendor_block(a.c_str(), full.c_str(), suffix, tmp, &lens_key);
		const bool b_has = !a_has && read_vendor_block(b.c_str(), full.c_str(), suffix, tmp, &lens_key);
		const std::string main_id = b_has ? b : a, sub_id = b_has ? a : b;
		if (!have && (a_has || b_has)) {
			blk = tmp;
			key = lens_key;
			have = true;
		}
		snprintf(p.main_id, sizeof(p.main_id), "%s", main_id.c_str());
		snprintf(p.sub_id, sizeof(p.sub_id), "%s", sub_id.c_str());

		ACameraMetadata *mm = nullptr;
		if (ACameraManager_getCameraCharacteristics(m, p.main_id, &mm) == ACAMERA_OK && mm != nullptr) {
			std::vector<int32_t> arr;
			if (get_i32(mm, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, arr) && arr.size() >= 4) {
				// NDK layout: xmin, ymin, width, height.
				p.array_w = (uint32_t)arr[2];
				p.array_h = (uint32_t)arr[3];
			}
			p.crop_w = want_w;
			p.crop_h = want_h;
			p.eye_w = want_w;
			p.eye_h = want_h;
			p.cap_w = want_w;
			p.cap_h = want_h;
			const uint32_t w43 = want_w, h43 = (want_w * 3 / 4) & ~1u;
			ACameraMetadata *sm = nullptr;
			bool sub_ok = false;
			if (ACameraManager_getCameraCharacteristics(m, p.sub_id, &sm) == ACAMERA_OK && sm != nullptr) {
				sub_ok = stream_supported(sm, AIMAGE_FORMAT_YUV_420_888, w43, h43);
				ACameraMetadata_free(sm);
			}
			if (h43 >= want_h && stream_supported(mm, AIMAGE_FORMAT_YUV_420_888, w43, h43) && sub_ok) {
				p.cap_h = h43;
			} else if (!stream_supported(mm, AIMAGE_FORMAT_YUV_420_888, want_w, want_h)) {
				SCAM_LOG_W("leia stereo camera (android): %ux%u not listed for lens %s; trying it anyway",
				           want_w, want_h, p.main_id);
			}
			std::vector<int32_t> so;
			if (get_i32(mm, ACAMERA_SENSOR_ORIENTATION, so)) {
				p.sensor_orientation = so[0];
				p.have_sensor_orientation = true;
			}
			// Candidate calibration reference sizes: every listed YUV / JPEG output.
			std::vector<int32_t> cfg;
			if (get_i32(mm, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, cfg)) {
				for (size_t i = 0; i + 3 < cfg.size(); i += 4) {
					if ((cfg[i] == AIMAGE_FORMAT_YUV_420_888 || cfg[i] == AIMAGE_FORMAT_JPEG) &&
					    cfg[i + 3] == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT && cfg[i + 1] > 0 &&
					    cfg[i + 2] > 0) {
						ref_sizes.push_back((uint32_t)cfg[i + 1]);
						ref_sizes.push_back((uint32_t)cfg[i + 2]);
					}
				}
			}
			ACameraMetadata_free(mm);
		}

		leia_scam_factory_calib raw{};
		double ref_off = 0.0;
		if (have && leia_scam_factory_calib_parse(blk.data(), blk.size(), &raw) && p.array_w > 0) {
			// 1. The size the block is expressed at, then the block at the array.
			const std::string ref = prop_str("debug.dxr.leia.scam_calib_ref", "auto");
			ref_off = leia_scam_pick_calib_reference(&raw, p.array_w, p.array_h, ref_sizes.data(),
			                                         ref_sizes.size() / 2, &p.ref_w, &p.ref_h);
			if (ref == "array") {
				p.ref_w = p.array_w;
				p.ref_h = p.array_h;
			} else if (ref != "auto") {
				parse_size(ref, &p.ref_w, &p.ref_h);
			}
			leia_scam_factory_to_array(&raw, p.ref_w, p.ref_h, p.array_w, p.array_h, &p.factory);
			// 2. Lenses stacked along the sensor's y axis: turn each eye so the
			// pair is side by side, by the sensor orientation (upright in the
			// device's natural orientation).
			if (leia_scam_factory_is_vertical(&p.factory)) {
				p.rot = (p.sensor_orientation == 90 || p.sensor_orientation == 270) ? (uint32_t)p.sensor_orientation
				                                                                    : 90u;
			}
		}
		{
			const std::string r = prop_str("debug.dxr.leia.scam_rotate", "auto");
			if (r == "0" || r == "90" || r == "180" || r == "270") {
				p.rot = (uint32_t)atoi(r.c_str());
			}
		}
		if (p.rot == 90 || p.rot == 270) {
			p.eye_w = p.crop_h;
			p.eye_h = p.crop_w;
		}
		if (p.ref_w != 0) {
			p.calibrated = leia_scam_stream_calib_from_factory(&p.factory, p.array_w, p.array_h, p.cap_w, p.cap_h,
			                                                   p.crop_w, p.crop_h, p.rot, &p.stream);
			p.block_hash = leia_scam_block_hash(blk.data(), blk.size());
		}
		if (swap == "0" || swap == "1") {
			p.swap_override = true;
			p.swap_value = swap == "1";
		}
		SCAM_LOG_W(
		    "leia stereo camera (android): %s pair = logical %s, main lens %s, sub lens %s, array %ux%u, eye "
		    "%ux%u (captured %ux%u, cropped %ux%u, turned %u deg cw, sensor orientation %d), factory block %s "
		    "(%zu bytes, %s, expressed at %ux%u, principal points %.1f %% off its centre) -> %s, baseline %.2f mm, "
		    "SBS left = %s lens",
		    p.front ? "front" : "rear", p.logical_id, p.main_id, p.sub_id, p.array_w, p.array_h, p.eye_w, p.eye_h,
		    p.cap_w, p.cap_h, p.crop_w, p.crop_h, p.rot, p.sensor_orientation, have ? "found" : "MISSING",
		    blk.size(), have ? key.c_str() : suffix, p.ref_w, p.ref_h, ref_off * 100.0,
		    p.calibrated ? "CALIBRATED" : "raw only",
		    p.calibrated ? leia_scam_factory_baseline_mm(&p.factory) : 0.0,
		    p.calibrated ? (p.stream.left_is_sub ? "sub" : "main") : "sub (assumed)");
		if (p.front) {
			g_pairs.insert(g_pairs.begin(), p);
		} else if (prop_bool("debug.dxr.leia.scam_rear", true)) {
			g_pairs.push_back(p);
		}
	}
	if (g_pairs.empty()) {
		SCAM_LOG_W("leia stereo camera (android): no logical stereo pair found");
	}
}

//! Which lens is the SBS left eye.
bool
left_is_sub(const pair_desc &p)
{
	if (p.swap_override) {
		return p.swap_value;
	}
	return p.calibrated ? p.stream.left_is_sub : true;
}

} // namespace

/*
 *
 * Open camera.
 *
 */

struct sbs_buf
{
	std::vector<uint8_t> data;
	uint64_t seq{0};
	int64_t time_mono_ns{0};
};

struct pending_img
{
	AImage *img{nullptr};
	int64_t ts{0};
};

struct xrt_plugin_stereo_camera
{
	pair_desc desc;
	bool lsub{true};
	bool track{false};
	//! Tracking is handed to the in-app runtime only while the camera is open
	//! (begun right before openCamera, ended on a failed start, a source loss
	//! or close), so a camera that cannot open never leaves the cores untracked.
	bool handed{false};
	//! SBS eye (0 = left, 1 = right) whose lens feeds the tracker, and its id.
	int trk_eye{1};
	char trk_lens[16]{};
	struct leia_cnsdk_ext_intrinsics trk_intr{};
	uint32_t w{0}, h{0};           //!< per-eye output (after the turn)
	uint32_t crop_w{0}, crop_h{0}; //!< per-eye crop of the capture (before the turn)
	uint32_t rot{0};               //!< clockwise turn of each eye
	uint32_t cap_w{0}, cap_h{0};   //!< per-lens capture

	// Camera objects (camera thread = whoever runs (re)open; the HAL calls back
	// on its own threads).
	std::mutex cam_mu;
	ACameraDevice *dev{nullptr};
	ACameraCaptureSession *session{nullptr};
	ACaptureSessionOutputContainer *outputs{nullptr};
	ACaptureSessionOutput *out_l{nullptr}, *out_r{nullptr};
	ACameraOutputTarget *tgt_l{nullptr}, *tgt_r{nullptr};
	ACaptureRequest *req{nullptr};
	AImageReader *rd_l{nullptr}, *rd_r{nullptr};
	ACameraDevice_StateCallbacks dev_cb{};
	ACameraCaptureSession_stateCallbacks ses_cb{};
	std::atomic<bool> device_failed{false};
	std::atomic<int> device_error{0};

	// Pairing (reader threads).
	std::mutex pair_mu;
	std::deque<pending_img> pend[2]; // [0] = left eye, [1] = right eye
	sbs_buf bufs[3];
	int ready{-1}, held{-1}, packing{-1};
	uint64_t seq{0};
	std::condition_variable cv;
	std::atomic<int64_t> last_pair_ns{0};
	std::atomic<uint64_t> pairs{0}, drops{0}, singles{0};
	int64_t rate_t0{0};
	int rate_n{0};
	std::vector<uint8_t> trk_scratch;

	// Tracking feed.
	std::mutex trk_mu;
	std::condition_variable trk_cv;
	std::vector<uint8_t> trk_slot;
	int64_t trk_ts_boot{0};
	bool trk_have{false};
	std::thread trk_thread;
	std::atomic<bool> trk_quit{false};
	std::atomic<uint64_t> trk_in{0}, trk_fed{0};
	std::atomic<int64_t> trk_busy_ns{0};

	// Watchdog / lifecycle.
	std::atomic<bool> closing{false};
	int64_t opened_ns{0};
	int64_t down_since_ns{0};
	int64_t next_reopen_ns{0};
	int64_t backoff_ns{0};
	int reopens{0};
	//! Consecutive opens the device was lost within kEarlyFailNs of (a HAL that
	//! aborts on the open looks exactly like this).
	int early_fails{0};
	//! Terminal: wait_frame reports ERROR and nothing reopens.
	bool gave_up{false};
	int64_t stats_ns{0};
	uint64_t stats_pairs{0};
};

namespace {

//! Plane access for one YUV_420_888 image.
struct yuv_view
{
	const uint8_t *y{nullptr}, *u{nullptr}, *v{nullptr};
	int32_t y_stride{0}, uv_stride{0}, uv_pixel{0};
	int32_t w{0}, h{0};
};

bool
yuv_view_of(AImage *img, yuv_view *out)
{
	AImage_getWidth(img, &out->w);
	AImage_getHeight(img, &out->h);
	uint8_t *p = nullptr;
	int len = 0;
	if (AImage_getPlaneData(img, 0, &p, &len) != AMEDIA_OK) {
		return false;
	}
	out->y = p;
	AImage_getPlaneRowStride(img, 0, &out->y_stride);
	if (AImage_getPlaneData(img, 1, &p, &len) != AMEDIA_OK) {
		return false;
	}
	out->u = p;
	if (AImage_getPlaneData(img, 2, &p, &len) != AMEDIA_OK) {
		return false;
	}
	out->v = p;
	AImage_getPlaneRowStride(img, 1, &out->uv_stride);
	AImage_getPlanePixelStride(img, 1, &out->uv_pixel);
	return true;
}

/*!
 * One eye into the SBS NV12 buffer at column @p x0 (stride = 2*W): the centre
 * W x H crop of the captured image (crop offsets even, so chroma stays aligned).
 */
void
pack_eye(const yuv_view &v, uint8_t *dst, uint32_t W, uint32_t H, uint32_t x0)
{
	const uint32_t stride = 2 * W;
	const uint32_t w = std::min<uint32_t>((uint32_t)v.w, W), h = std::min<uint32_t>((uint32_t)v.h, H);
	const uint32_t cx = (((uint32_t)v.w - w) / 2) & ~1u, cy = (((uint32_t)v.h - h) / 2) & ~1u;
	for (uint32_t y = 0; y < h; y++) {
		memcpy(dst + (size_t)y * stride + x0, v.y + (size_t)(y + cy) * v.y_stride + cx, w);
	}
	uint8_t *uv = dst + (size_t)stride * H;
	const bool nv12 = v.uv_pixel == 2 && v.v == v.u + 1;
	const bool nv21 = v.uv_pixel == 2 && v.u == v.v + 1;
	const int32_t ps = v.uv_pixel > 0 ? v.uv_pixel : 1;
	for (uint32_t y = 0; y < h / 2; y++) {
		uint8_t *d = uv + (size_t)y * stride + x0;
		const uint8_t *u = v.u + (size_t)(y + cy / 2) * v.uv_stride + (size_t)(cx / 2) * ps;
		const uint8_t *vv = v.v + (size_t)(y + cy / 2) * v.uv_stride + (size_t)(cx / 2) * ps;
		if (nv12) {
			memcpy(d, u, w);
		} else if (nv21) {
			for (uint32_t x = 0; x < w / 2; x++) {
				d[2 * x] = vv[2 * x + 1];
				d[2 * x + 1] = vv[2 * x];
			}
		} else {
			for (uint32_t x = 0; x < w / 2; x++) {
				d[2 * x] = u[(size_t)x * ps];
				d[2 * x + 1] = vv[(size_t)x * ps];
			}
		}
	}
}

/*!
 * One eye into the SBS NV12 buffer at column @p x0 (stride = 2 * @p ew), turned
 * clockwise by @p rot (90, 180 or 270): the centre @p cw x @p ch crop of the
 * capture (even offsets), published as @p ew x @p eh (= ch x cw for 90 / 270).
 * Tiled 32 x 32 so the transposed reads stay in cache. Matches
 * leia_scam_stream_calib_from_factory's step 3: one 90 deg clockwise step
 * takes crop pixel (x, y) to (ch - 1 - y, x).
 */
void
pack_eye_turned(const yuv_view &v, uint8_t *dst, uint32_t cw, uint32_t ch, uint32_t rot, uint32_t ew, uint32_t eh,
                uint32_t x0)
{
	if ((uint32_t)v.w < cw || (uint32_t)v.h < ch) {
		return; // the capture is smaller than the crop: never configured that way
	}
	const uint32_t stride = 2 * ew;
	const uint32_t ox = (((uint32_t)v.w - cw) / 2) & ~1u, oy = (((uint32_t)v.h - ch) / 2) & ~1u;
	// Source crop pixel of published pixel (X, Y) in a W x H crop.
	auto src_of = [rot](uint32_t X, uint32_t Y, uint32_t W, uint32_t H, uint32_t *x, uint32_t *y) {
		if (rot == 90) {
			*x = Y;
			*y = H - 1 - X;
		} else if (rot == 270) {
			*x = W - 1 - Y;
			*y = X;
		} else {
			*x = W - 1 - X;
			*y = H - 1 - Y;
		}
	};
	constexpr uint32_t T = 32;
	for (uint32_t ty = 0; ty < eh; ty += T) {
		for (uint32_t tx = 0; tx < ew; tx += T) {
			const uint32_t ye = std::min(ty + T, eh), xe = std::min(tx + T, ew);
			for (uint32_t Y = ty; Y < ye; Y++) {
				uint8_t *d = dst + (size_t)Y * stride + x0;
				for (uint32_t X = tx; X < xe; X++) {
					uint32_t x, y;
					src_of(X, Y, cw, ch, &x, &y);
					d[X] = v.y[(size_t)(y + oy) * v.y_stride + x + ox];
				}
			}
		}
	}
	uint8_t *uv = dst + (size_t)stride * eh;
	const int32_t ps = v.uv_pixel > 0 ? v.uv_pixel : 1;
	const uint32_t cw2 = cw / 2, ch2 = ch / 2, ew2 = ew / 2, eh2 = eh / 2;
	for (uint32_t ty = 0; ty < eh2; ty += T) {
		for (uint32_t tx = 0; tx < ew2; tx += T) {
			const uint32_t ye = std::min(ty + T, eh2), xe = std::min(tx + T, ew2);
			for (uint32_t Y = ty; Y < ye; Y++) {
				uint8_t *d = uv + (size_t)Y * stride + x0;
				for (uint32_t X = tx; X < xe; X++) {
					uint32_t x, y;
					src_of(X, Y, cw2, ch2, &x, &y);
					const size_t o = (size_t)(y + oy / 2) * v.uv_stride + (size_t)(x + ox / 2) * ps;
					d[2 * X] = v.u[o];
					d[2 * X + 1] = v.v[o];
				}
			}
		}
	}
}

/*!
 * The tracking frame from the main lens's capture: its centre 4:3 region
 * scaled to 640x480 (2x2 box for an exact 1280x960 capture, which is the
 * whole sensor field of view; nearest otherwise).
 */
void
crop_track_from_eye(const yuv_view &v, uint8_t *dst)
{
	if (v.w == 2 * (int32_t)kTrackW && v.h == 2 * (int32_t)kTrackH) {
		for (uint32_t y = 0; y < kTrackH; y++) {
			const uint8_t *r0 = v.y + (size_t)(2 * y) * v.y_stride, *r1 = r0 + v.y_stride;
			uint8_t *d = dst + (size_t)y * kTrackW;
			for (uint32_t x = 0; x < kTrackW; x++) {
				d[x] = (uint8_t)((r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2);
			}
		}
		return;
	}
	const int32_t cw = std::min<int32_t>(v.w, v.h * 4 / 3), chh = std::min<int32_t>(v.h, cw * 3 / 4);
	const int32_t x0 = (v.w - cw) / 2, y0 = (v.h - chh) / 2;
	for (uint32_t y = 0; y < kTrackH; y++) {
		const uint8_t *row = v.y + (size_t)(y0 + (int32_t)(y * (uint32_t)chh / kTrackH)) * v.y_stride + x0;
		uint8_t *d = dst + (size_t)y * kTrackW;
		for (uint32_t x = 0; x < kTrackW; x++) {
			d[x] = row[x * (uint32_t)cw / kTrackW];
		}
	}
}

void
push_tracking(xrt_plugin_stereo_camera *c, const uint8_t *y, int32_t stride, int64_t ts_boot, bool already_tight)
{
	{
		std::lock_guard<std::mutex> lk(c->trk_mu);
		c->trk_slot.resize((size_t)kTrackW * kTrackH);
		if (already_tight) {
			memcpy(c->trk_slot.data(), y, c->trk_slot.size());
		} else {
			for (uint32_t r = 0; r < kTrackH; r++) {
				memcpy(c->trk_slot.data() + (size_t)r * kTrackW, y + (size_t)r * stride, kTrackW);
			}
		}
		c->trk_ts_boot = ts_boot;
		c->trk_have = true; // latest wins: an unconsumed older frame is overwritten
	}
	c->trk_in++;
	c->trk_cv.notify_one();
}

//! Under pair_mu: pack the matched pair (left, right) and publish it.
void
pack_pair_locked(xrt_plugin_stereo_camera *c, AImage *l, AImage *r, int64_t ts_boot)
{
	int slot = -1;
	for (int i = 0; i < 3; i++) {
		if (i != c->held && i != c->ready) {
			slot = i;
			break;
		}
	}
	if (slot < 0) {
		slot = c->ready; // runtime holds one, the other is stale: overwrite it
		c->ready = -1;
		c->drops++;
	}
	sbs_buf &b = c->bufs[slot];
	const size_t need = (size_t)2 * c->w * c->h * 3 / 2;
	if (b.data.size() != need) {
		b.data.assign(need, 0);
		memset(b.data.data() + (size_t)2 * c->w * c->h, 128, need - (size_t)2 * c->w * c->h);
	}
	yuv_view vl, vr;
	if (!yuv_view_of(l, &vl) || !yuv_view_of(r, &vr)) {
		c->drops++;
		return;
	}
	if (c->rot == 0) {
		pack_eye(vl, b.data.data(), c->w, c->h, 0);
		pack_eye(vr, b.data.data(), c->w, c->h, c->w);
	} else {
		pack_eye_turned(vl, b.data.data(), c->crop_w, c->crop_h, c->rot, c->w, c->h, 0);
		pack_eye_turned(vr, b.data.data(), c->crop_w, c->crop_h, c->rot, c->w, c->h, c->w);
	}
	b.seq = ++c->seq;
	b.time_mono_ns = ts_boot - boot_minus_mono_ns();
	if (c->ready >= 0) {
		c->drops++; // previous never fetched: latest wins
	}
	c->ready = slot;
	c->pairs++;
	const int64_t now = clock_ns(CLOCK_MONOTONIC);
	c->last_pair_ns.store(now);
	if (c->rate_n == 0) {
		c->rate_t0 = now;
	}
	if (++c->rate_n == kRateWindow + 1) {
		const float rate = (float)kRateWindow * 1e9f / (float)(now - c->rate_t0);
		if (c->desc.measured_rate <= 0.0f) {
			SCAM_LOG_W("leia stereo camera (android): measured pair rate %.2f Hz over %d pairs", rate,
			           kRateWindow);
		}
		c->desc.measured_rate = rate;
		{
			std::lock_guard<std::mutex> g(g_mu);
			for (auto &p : g_pairs) {
				if (strcmp(p.logical_id, c->desc.logical_id) == 0) {
					p.measured_rate = rate;
				}
			}
		}
		c->rate_n = 0;
	}
	c->cv.notify_all();
}

void
on_eye(xrt_plugin_stereo_camera *c, AImageReader *reader, int eye)
{
	for (;;) {
		AImage *img = nullptr;
		if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || img == nullptr) {
			return;
		}
		int64_t ts = 0;
		AImage_getTimestamp(img, &ts);
		// The tracking frame comes from EVERY main-lens image, before and
		// independent of pairing: CNSDK resets its predictor on a >70 ms gap,
		// so one lost pair must never become a lost tracking frame. Only this
		// reader's thread touches trk_scratch.
		if (c->track && eye == c->trk_eye && !c->closing.load()) {
			yuv_view vm;
			if (yuv_view_of(img, &vm)) {
				c->trk_scratch.resize((size_t)kTrackW * kTrackH);
				crop_track_from_eye(vm, c->trk_scratch.data());
				push_tracking(c, c->trk_scratch.data(), kTrackW, ts, true);
			}
		}
		std::lock_guard<std::mutex> lk(c->pair_mu);
		if (c->closing.load()) {
			AImage_delete(img);
			continue;
		}
		auto &mine = c->pend[eye];
		auto &other = c->pend[1 - eye];
		auto it = std::find_if(other.begin(), other.end(),
		                       [ts](const pending_img &p) { return std::llabs(p.ts - ts) <= kPairToleranceNs; });
		if (it != other.end()) {
			AImage *l = eye == 0 ? img : it->img;
			AImage *r = eye == 0 ? it->img : img;
			pack_pair_locked(c, l, r, ts);
			AImage_delete(img);
			// Drop the partner and everything older on both sides.
			const int64_t cut = ts;
			for (auto *q : {&mine, &other}) {
				while (!q->empty() && q->front().ts <= cut + kPairToleranceNs) {
					AImage_delete(q->front().img);
					q->pop_front();
				}
			}
			continue;
		}
		mine.push_back({img, ts});
		while (mine.size() > kPendingPerEye) {
			AImage_delete(mine.front().img);
			mine.pop_front();
			c->singles++;
		}
	}
}

void
on_eye_l(void *ctx, AImageReader *r)
{
	auto *c = (xrt_plugin_stereo_camera *)ctx;
	on_eye(c, r, 0);
}

void
on_eye_r(void *ctx, AImageReader *r)
{
	auto *c = (xrt_plugin_stereo_camera *)ctx;
	on_eye(c, r, 1);
}

/*!
 * Hands tracking frames to CNSDK through the C API (leia_cnsdk_ext_tracking_feed
 * -> leia_shared_camera_sink_on_image) with a tight 640-stride Y8 buffer and
 * no rotation. That path never had the Java/JNI SharedCameraSink bugs fixed by
 * CNSDK #762 (padded strides, non-zero rotation), so nothing here changes when
 * the plug-in pins a CNSDK >= 0.10.88.
 */
void
tracking_feeder(xrt_plugin_stereo_camera *c)
{
	std::vector<uint8_t> local;
	for (;;) {
		int64_t ts = 0;
		{
			std::unique_lock<std::mutex> lk(c->trk_mu);
			c->trk_cv.wait(lk, [c] { return c->trk_quit.load() || c->trk_have; });
			if (c->trk_quit.load()) {
				return;
			}
			local.swap(c->trk_slot);
			ts = c->trk_ts_boot;
			c->trk_have = false;
		}
		const int64_t t0 = clock_ns(CLOCK_MONOTONIC);
		leia_cnsdk_ext_tracking_feed(local.data(), (int32_t)kTrackW, (int32_t)kTrackH, ts);
		c->trk_busy_ns += clock_ns(CLOCK_MONOTONIC) - t0;
		c->trk_fed++;
	}
}

void
dev_disconnected(void *ctx, ACameraDevice *)
{
	auto *c = (xrt_plugin_stereo_camera *)ctx;
	c->device_failed = true;
	SCAM_LOG_W("leia stereo camera (android): camera %s DISCONNECTED", c->desc.logical_id);
	c->cv.notify_all();
}

void
dev_error(void *ctx, ACameraDevice *, int error)
{
	auto *c = (xrt_plugin_stereo_camera *)ctx;
	c->device_failed = true;
	c->device_error = error;
	SCAM_LOG_W("leia stereo camera (android): camera %s ERROR %d", c->desc.logical_id, error);
	c->cv.notify_all();
}

void
ses_noop(void *, ACameraCaptureSession *)
{}

//! Under cam_mu. Idempotent.
void
teardown_locked(xrt_plugin_stereo_camera *c)
{
	if (c->session != nullptr) {
		ACameraCaptureSession_stopRepeating(c->session);
		ACameraCaptureSession_close(c->session);
		c->session = nullptr;
	}
	if (c->req != nullptr) {
		ACaptureRequest_free(c->req);
		c->req = nullptr;
	}
	for (ACameraOutputTarget **t : {&c->tgt_l, &c->tgt_r}) {
		if (*t != nullptr) {
			ACameraOutputTarget_free(*t);
			*t = nullptr;
		}
	}
	if (c->dev != nullptr) {
		ACameraDevice_close(c->dev); // waits for in-flight callbacks
		c->dev = nullptr;
	}
	if (c->outputs != nullptr) {
		for (ACaptureSessionOutput **o : {&c->out_l, &c->out_r}) {
			if (*o != nullptr) {
				ACaptureSessionOutputContainer_remove(c->outputs, *o);
				ACaptureSessionOutput_free(*o);
				*o = nullptr;
			}
		}
		ACaptureSessionOutputContainer_free(c->outputs);
		c->outputs = nullptr;
	}
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		for (auto &q : c->pend) {
			for (auto &p : q) {
				AImage_delete(p.img);
			}
			q.clear();
		}
	}
	for (AImageReader **r : {&c->rd_l, &c->rd_r}) {
		if (*r != nullptr) {
			AImageReader_delete(*r);
			*r = nullptr;
		}
	}
}

bool
make_reader(xrt_plugin_stereo_camera *c, uint32_t w, uint32_t h, int max_images, AImageReader_ImageCallback cb,
            AImageReader **out)
{
	media_status_t s = AImageReader_new((int32_t)w, (int32_t)h, AIMAGE_FORMAT_YUV_420_888, max_images, out);
	if (s != AMEDIA_OK || *out == nullptr) {
		SCAM_LOG_W("leia stereo camera (android): AImageReader %ux%u YUV failed (%d)", w, h, (int)s);
		return false;
	}
	AImageReader_ImageListener l{c, cb};
	AImageReader_setImageListener(*out, &l);
	return true;
}

bool
add_output(xrt_plugin_stereo_camera *c, AImageReader *rd, const char *phys, ACaptureSessionOutput **out,
           ACameraOutputTarget **tgt)
{
	ANativeWindow *win = nullptr;
	if (AImageReader_getWindow(rd, &win) != AMEDIA_OK || win == nullptr) {
		return false;
	}
	if (ACaptureSessionPhysicalOutput_create(win, phys, out) != ACAMERA_OK) {
		return false;
	}
	if (ACaptureSessionOutputContainer_add(c->outputs, *out) != ACAMERA_OK) {
		return false;
	}
	if (ACameraOutputTarget_create(win, tgt) != ACAMERA_OK) {
		return false;
	}
	return ACaptureRequest_addTarget(c->req, *tgt) == ACAMERA_OK;
}

//! Under cam_mu. Hand head tracking to / back from this process's cores.
void
handover_locked(xrt_plugin_stereo_camera *c, bool on)
{
	if (!c->track || c->handed == on) {
		return;
	}
	c->handed = on;
	if (on) {
		const uint32_t cores = leia_cnsdk_ext_tracking_begin(&c->trk_intr);
		SCAM_LOG_W("leia stereo camera (android): tracking handed to %u in-process core(s) (in-app, fed from lens %s, "
		           "fx %.1f fy %.1f cx %.1f cy %.1f at 640x480)",
		           cores, c->trk_lens, c->trk_intr.fx, c->trk_intr.fy, c->trk_intr.cx, c->trk_intr.cy);
	} else {
		leia_cnsdk_ext_tracking_end();
		SCAM_LOG_W("leia stereo camera (android): tracking handed back to the tracking service");
	}
}

//! Under cam_mu: open the device and start the repeating request.
bool
start_locked(xrt_plugin_stereo_camera *c)
{
	teardown_locked(c);
	c->device_failed = false;
	c->device_error = 0;
	const char *left = c->lsub ? c->desc.sub_id : c->desc.main_id;
	const char *right = c->lsub ? c->desc.main_id : c->desc.sub_id;

	if (!make_reader(c, c->cap_w, c->cap_h, kEyeMaxImages, on_eye_l, &c->rd_l) ||
	    !make_reader(c, c->cap_w, c->cap_h, kEyeMaxImages, on_eye_r, &c->rd_r)) {
		teardown_locked(c);
		return false;
	}

	// Into in-app tracking first, so the tracking service lets go of its lens;
	// back out if the open does not get through.
	handover_locked(c, true);
	// Then wait for the camera service to report both lenses free: never open
	// the pair over a lens another client still streams (see g_in_use).
	// debug.dxr.leia.scam_wait_free=0 opens at once (the old eviction order),
	// for a vendor repro only.
	if (prop_bool("debug.dxr.leia.scam_wait_free", true)) {
		const int64_t t0 = clock_ns(CLOCK_MONOTONIC);
		const std::string busy = wait_lenses_free(c->desc, kLensFreeNs, c->closing);
		if (!busy.empty()) {
			SCAM_LOG_W("leia stereo camera (android): lens %s still in use %lld ms after the hand-over (the tracking "
			           "service or another app holds it); not opening the pair over it",
			           busy.c_str(), (long long)((clock_ns(CLOCK_MONOTONIC) - t0) / kNsPerMs));
			teardown_locked(c);
			handover_locked(c, false);
			return false;
		}
		SCAM_LOG_W("leia stereo camera (android): lenses %s + %s free after %.1f ms", c->desc.main_id,
		           c->desc.sub_id, (double)(clock_ns(CLOCK_MONOTONIC) - t0) / 1e6);
	}
	c->dev_cb = {c, dev_disconnected, dev_error};
	camera_status_t st = ACameraManager_openCamera(mgr(), c->desc.logical_id, &c->dev_cb, &c->dev);
	if (st != ACAMERA_OK || c->dev == nullptr) {
		SCAM_LOG_W("leia stereo camera (android): openCamera(%s) failed (%d)", c->desc.logical_id, (int)st);
		c->dev = nullptr;
		teardown_locked(c);
		handover_locked(c, false);
		return false;
	}
	if (ACaptureSessionOutputContainer_create(&c->outputs) != ACAMERA_OK ||
	    ACameraDevice_createCaptureRequest(c->dev, TEMPLATE_PREVIEW, &c->req) != ACAMERA_OK) {
		teardown_locked(c);
		handover_locked(c, false);
		return false;
	}
	const bool ok =
	    add_output(c, c->rd_l, left, &c->out_l, &c->tgt_l) && add_output(c, c->rd_r, right, &c->out_r, &c->tgt_r);
	if (!ok) {
		SCAM_LOG_W("leia stereo camera (android): could not build the outputs");
		teardown_locked(c);
		handover_locked(c, false);
		return false;
	}
	const int32_t fps[2] = {30, 30};
	ACaptureRequest_setEntry_i32(c->req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps);
	c->ses_cb = {c, ses_noop, ses_noop, ses_noop};
	st = ACameraDevice_createCaptureSessionWithSessionParameters(c->dev, c->outputs, c->req, &c->ses_cb,
	                                                             &c->session);
	if (st != ACAMERA_OK || c->session == nullptr) {
		SCAM_LOG_W("leia stereo camera (android): createCaptureSession failed (%d)", (int)st);
		c->session = nullptr;
		teardown_locked(c);
		handover_locked(c, false);
		return false;
	}
	st = ACameraCaptureSession_setRepeatingRequest(c->session, nullptr, 1, &c->req, nullptr);
	if (st != ACAMERA_OK) {
		SCAM_LOG_W("leia stereo camera (android): setRepeatingRequest failed (%d)", (int)st);
		teardown_locked(c);
		handover_locked(c, false);
		return false;
	}
	c->opened_ns = clock_ns(CLOCK_MONOTONIC);
	c->last_pair_ns.store(0);
	SCAM_LOG_W("leia stereo camera (android): %s pair streaming: logical %s, L=%s R=%s, %ux%u YUV per lens -> %ux%u "
	           "eyes (turned %u deg cw), tracking %s",
	           c->desc.front ? "front" : "rear", c->desc.logical_id, left, right, c->cap_w, c->cap_h, c->w, c->h,
	           c->rot, c->track ? "640x480 from the tracking lens (unturned)" : "not handed over");
	return true;
}

/*!
 * The watchdog, run from wait_frame (the runtime's camera thread for this
 * camera). Returns false while the source is down.
 */
bool
supervise(xrt_plugin_stereo_camera *c, int64_t now, bool *out_gave_up)
{
	*out_gave_up = c->gave_up;
	if (c->gave_up) {
		return false;
	}
	const bool streaming = c->session != nullptr && !c->device_failed.load();
	const int64_t last = c->last_pair_ns.load();
	const bool stalled = streaming && ((last != 0 && now - last > kStallNs) ||
	                                   (last == 0 && c->opened_ns != 0 && now - c->opened_ns > 3 * kStallNs));
	if (streaming && !stalled) {
		c->down_since_ns = 0;
		c->backoff_ns = 0;
		if (c->early_fails != 0 && now - c->opened_ns >= kEarlyFailNs) {
			c->early_fails = 0; // this open held
		}
		if (now - c->stats_ns >= 10000 * kNsPerMs) {
			const uint64_t p = c->pairs.load();
			const double fed = (double)c->trk_fed.load();
			SCAM_LOG_I("leia stereo camera (android): %.2f pairs/s, drops %llu, unpaired %llu, tracking in %llu fed "
			           "%llu (avg %.1f ms per feed)",
			           (double)(p - c->stats_pairs) * 1e9 / (double)(now - c->stats_ns),
			           (unsigned long long)c->drops.load(), (unsigned long long)c->singles.load(),
			           (unsigned long long)c->trk_in.load(), (unsigned long long)c->trk_fed.load(),
			           fed > 0 ? (double)c->trk_busy_ns.load() / fed / 1e6 : 0.0);
			c->stats_ns = now;
			c->stats_pairs = p;
		}
		return true;
	}
	if (c->down_since_ns == 0) {
		c->down_since_ns = now;
		c->next_reopen_ns = now + kReopenFirstNs;
		c->backoff_ns = kReopenFirstNs;
		SCAM_LOG_W("leia stereo camera (android): source down (%s, device error %d); reopening",
		           stalled ? "no frames" : "device failed", c->device_error.load());
		// Lost before the first pair or within kEarlyFailNs of the open: the
		// open itself is failing (a provider that aborts on it reports exactly
		// this). Reopening into it again only crashes the HAL again, and every
		// crash also drops the tracking camera.
		const bool early = c->opened_ns != 0 && (last == 0 || now - c->opened_ns < kEarlyFailNs);
		c->early_fails = early ? c->early_fails + 1 : 0;
		std::lock_guard<std::mutex> lk(c->cam_mu);
		teardown_locked(c);
		handover_locked(c, false);
		if (c->early_fails >= kEarlyFailMax) {
			SCAM_LOG_W("leia stereo camera (android): camera %s lost within %lld ms of opening %d times in a row "
			           "(device error %d); not reopening it",
			           c->desc.logical_id, (long long)(kEarlyFailNs / kNsPerMs), c->early_fails,
			           c->device_error.load());
			c->gave_up = true;
			*out_gave_up = true;
			return false;
		}
	}
	if (now - c->down_since_ns > kGiveUpNs) {
		SCAM_LOG_W("leia stereo camera (android): source gone for %lld ms; giving up",
		           (long long)(kGiveUpNs / kNsPerMs));
		c->gave_up = true;
		*out_gave_up = true;
		return false;
	}
	if (now >= c->next_reopen_ns) {
		std::lock_guard<std::mutex> lk(c->cam_mu);
		c->reopens++;
		if (start_locked(c)) {
			SCAM_LOG_W("leia stereo camera (android): reopened (attempt %d)", c->reopens);
			c->down_since_ns = 0;
			return false; // frames resume shortly
		}
		// Retry at most every second for the first 10 s: right after the
		// consumer's app comes to the foreground the camera service refuses the
		// runtime ("disabled by policy" / "access restricted") until the
		// satellite inherits the while-in-use camera capability, which takes a
		// few seconds (measured 1.8-8 s on a Lume Pad 2). Back off further after.
		const int64_t cap = now - c->down_since_ns < 10000 * kNsPerMs ? kReopenFastMaxNs : kReopenMaxNs;
		c->backoff_ns = std::min(c->backoff_ns * 2, cap);
		c->next_reopen_ns = now + c->backoff_ns;
	}
	return false;
}

} // namespace

/*
 *
 * Slots.
 *
 */

extern "C" uint32_t
leia_stereo_camera_android_enumerate(struct xrt_plugin_instance *inst,
                                     uint32_t capacity,
                                     struct xrt_plugin_stereo_camera_info *out)
{
	(void)inst;
	std::lock_guard<std::mutex> lk(g_mu);
	scan_locked();
	const uint32_t count = (uint32_t)g_pairs.size();
	if (out == nullptr || capacity == 0) {
		return count;
	}
	const uint32_t sz = out[0].struct_size;
	if (sz == 0) {
		return count;
	}
	const uint32_t n = std::min(count, capacity);
	for (uint32_t i = 0; i < n; i++) {
		const pair_desc &p = g_pairs[i];
		xrt_plugin_stereo_camera_info info;
		memset(&info, 0, sizeof(info));
		info.struct_size = (uint32_t)sizeof(info);
		snprintf(info.display_name, sizeof(info.display_name), "%s", p.front ? "3D Camera" : "3D Camera, rear");
		snprintf(info.device_identity, sizeof(info.device_identity), "leia-android:%s:%016llx",
		         p.front ? "front" : "rear", (unsigned long long)p.block_hash);
		info.platform_device_hint[0] = '\0'; // the logical ids are hidden from apps: nothing to de-duplicate
		info.flags = 0;
		if (p.front) {
			info.flags |= XRT_PLUGIN_STEREO_CAMERA_USER_FACING | XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING;
		}
		if (p.calibrated) {
			info.flags |= XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
		}
		info.eye_width = p.eye_w;
		info.eye_height = p.eye_h;
		info.max_frame_rate = p.measured_rate; // 0 until an open measured it
		info.native_format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
#ifdef XRT_PLUGIN_STEREO_CAMERA_INFO_HAS_FRAME_ORIENTATION
		// Runtime XR_DXR_stereo_camera v7: how the published frame is turned, so
		// a consumer does not add its own guess on top of our turn. Tablets keep
		// the sensor's 270 (front) / 90 (rear); a pair we turn upright reports 0.
		switch (leia_scam_frame_orientation_deg(p.have_sensor_orientation, p.sensor_orientation, p.rot)) {
		case 0: info.frame_orientation = XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_0; break;
		case 90: info.frame_orientation = XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_90; break;
		case 180: info.frame_orientation = XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_180; break;
		case 270: info.frame_orientation = XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_270; break;
		default: info.frame_orientation = XRT_PLUGIN_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN; break;
		}
#endif
		auto *dst = reinterpret_cast<xrt_plugin_stereo_camera_info *>(reinterpret_cast<uint8_t *>(out) + (size_t)i * sz);
		memcpy(dst, &info, std::min<size_t>(sz, sizeof(info)));
		dst->struct_size = sz;
	}
	return count;
}

extern "C" xrt_result_t
leia_stereo_camera_android_get_calibration(struct xrt_plugin_instance *inst,
                                           uint32_t index,
                                           struct xrt_plugin_stereo_camera_calibration *out)
{
	(void)inst;
	std::lock_guard<std::mutex> lk(g_mu);
	scan_locked();
	if (out == nullptr || index >= g_pairs.size() || !g_pairs[index].calibrated) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const pair_desc &p = g_pairs[index];
	leia_scam_stream_calib s = p.stream;
	if (left_is_sub(p) != s.left_is_sub) {
		// A forced swap: invert the pair so the halves and R/T still agree.
		leia_scam_factory_calib f = p.factory;
		for (int i = 0; i < 3; i++) {
			f.T[i] = -f.T[i];
		}
		leia_scam_stream_calib alt;
		leia_scam_stream_calib_from_factory(&f, p.array_w, p.array_h, p.cap_w, p.cap_h, p.crop_w, p.crop_h, p.rot,
		                                    &alt);
		s = alt;
	}
	const uint32_t sz = out->struct_size;
	xrt_plugin_stereo_camera_calibration c;
	memset(&c, 0, sizeof(c));
	c.struct_size = (uint32_t)sizeof(c);
	c.image_width = s.width;
	c.image_height = s.height;
	for (int e = 0; e < 2; e++) {
		for (int k = 0; k < 4; k++) {
			c.k[e][k] = s.k[e][k];
		}
		for (int k = 0; k < 8; k++) {
			c.distortion[e][k] = s.d[e][k];
		}
	}
	c.distortion_model = XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN8;
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			c.rotation_right_from_left[i][j] = s.R_rl[i][j];
		}
		c.translation_right_from_left_mm[i] = s.T_rl_mm[i];
	}
	memcpy(out, &c, std::min<size_t>(sz, sizeof(c)));
	out->struct_size = sz;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
leia_stereo_camera_android_open(struct xrt_plugin_instance *inst,
                                uint32_t index,
                                struct xrt_plugin_stereo_camera **out_cam)
{
	(void)inst;
	if (out_cam == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	auto *c = new xrt_plugin_stereo_camera();
	{
		std::lock_guard<std::mutex> lk(g_mu);
		scan_locked();
		if (index >= g_pairs.size()) {
			delete c;
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		c->desc = g_pairs[index];
	}
	c->lsub = left_is_sub(c->desc);
	c->w = c->desc.eye_w;
	c->h = c->desc.eye_h;
	c->crop_w = c->desc.crop_w;
	c->crop_h = c->desc.crop_h;
	c->rot = c->desc.rot;
	c->cap_w = c->desc.cap_w;
	c->cap_h = c->desc.cap_h;
	c->track = c->desc.front && prop_bool("debug.dxr.leia.scam_track", true);

	if (c->track) {
		// The intrinsics of the lens fed to this process's cores once tracking is
		// handed over (start_locked, right before the camera is taken from the
		// tracking service).
		// The main lens's capture, then crop_track_from_eye's centre 4:3 region
		// scaled to 640x480. With a 4:3 capture that is the whole array, i.e.
		// the tracking service's own 640x480 intrinsics.
		// Which lens feeds the tracker: the one the vendor device config names
		// as the tracking camera (its pose is what the in-app engine applies),
		// else the factory block's main lens. A Lume Pad 2's config names none
		// and its service tracks from the main lens; a nubia Pad 3D II's service
		// tracks from the sub lens. debug.dxr.leia.scam_track_lens=<id> forces it.
		char cfg_id[16] = {0};
		std::string forced = prop_str("debug.dxr.leia.scam_track_lens", "");
		const bool have_cfg = leia_cnsdk_get_tracking_camera_id(cfg_id, sizeof(cfg_id));
		const char *want = !forced.empty() ? forced.c_str() : (have_cfg ? cfg_id : c->desc.main_id);
		const bool use_sub = strcmp(want, c->desc.sub_id) == 0;
		snprintf(c->trk_lens, sizeof(c->trk_lens), "%s", use_sub ? c->desc.sub_id : c->desc.main_id);
		// SBS left eye = sub when lsub; the tracking eye is the one showing trk_lens.
		c->trk_eye = (use_sub == c->lsub) ? 0 : 1;
		SCAM_LOG_W("leia stereo camera (android): tracking lens %s (%s; device config names \"%s\")", c->trk_lens,
		           !forced.empty() ? "forced" : (have_cfg && strcmp(want, c->trk_lens) == 0 ? "device config" : "factory main"),
		           have_cfg ? cfg_id : "");
		leia_scam_lens tl{};
		leia_scam_lens_for_stream(use_sub ? &c->desc.factory.sub : &c->desc.factory.main, c->desc.array_w,
		                          c->desc.array_h, c->cap_w, c->cap_h, &tl);
		{
			const double cw = std::min<double>(c->cap_w, c->cap_h * 4.0 / 3.0);
			const double ch = std::min<double>(c->cap_h, cw * 0.75);
			const double s = (double)kTrackW / cw;
			tl.fx *= s;
			tl.fy *= s;
			tl.cx = (tl.cx - (c->cap_w - cw) / 2.0) * s;
			tl.cy = (tl.cy - (c->cap_h - ch) / 2.0) * s;
		}
		struct leia_cnsdk_ext_intrinsics ei;
		memset(&ei, 0, sizeof(ei));
		ei.width = (int32_t)kTrackW;
		ei.height = (int32_t)kTrackH;
		ei.fx = (float)tl.fx;
		ei.fy = (float)tl.fy;
		ei.cx = (float)tl.cx;
		ei.cy = (float)tl.cy;
		for (int i = 0; i < 8; i++) {
			ei.d[i] = tl.d[i];
		}
		ei.mirrored = true;
		ei.valid = c->desc.calibrated;
		c->trk_intr = ei; // handed over by start_locked, around the actual open
		c->trk_thread = std::thread(tracking_feeder, c);
	}

	bool ok;
	{
		std::lock_guard<std::mutex> lk(c->cam_mu);
		ok = start_locked(c);
	}
	if (!ok) {
		SCAM_LOG_W("leia stereo camera (android): open failed; the watchdog keeps retrying");
		c->down_since_ns = clock_ns(CLOCK_MONOTONIC);
		c->next_reopen_ns = c->down_since_ns + kReopenFirstNs;
		c->backoff_ns = kReopenFirstNs;
	}
	c->stats_ns = clock_ns(CLOCK_MONOTONIC);
	*out_cam = c;
	return XRT_SUCCESS;
}

extern "C" uint32_t
leia_stereo_camera_android_wait_frame(struct xrt_plugin_stereo_camera *c,
                                      int64_t timeout_ns,
                                      struct xrt_plugin_stereo_camera_frame *out)
{
	if (c == nullptr || out == nullptr) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR;
	}
	const int64_t deadline = clock_ns(CLOCK_MONOTONIC) + std::max<int64_t>(timeout_ns, 0);
	for (;;) {
		const int64_t now = clock_ns(CLOCK_MONOTONIC);
		bool gave_up = false;
		const bool up = supervise(c, now, &gave_up);
		if (gave_up) {
			return XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR; // supervise logged why, once
		}
		{
			std::unique_lock<std::mutex> lk(c->pair_mu);
			if (c->ready >= 0) {
				if (c->held >= 0) {
					c->held = -1; // the runtime always releases first; be safe
				}
				c->held = c->ready;
				c->ready = -1;
				const sbs_buf &b = c->bufs[c->held];
				memset(out, 0, sizeof(*out));
				out->sequence = b.seq;
				out->time_ns = b.time_mono_ns;
				out->time_is_exposure = true; // sensor start-of-exposure, BOOTTIME -> MONOTONIC
				out->width = 2 * c->w;
				out->height = c->h;
				out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
				out->planes[0] = b.data.data();
				out->planes[1] = b.data.data() + (size_t)2 * c->w * c->h;
				out->pitches[0] = 2 * c->w;
				out->pitches[1] = 2 * c->w;
				return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
			}
			if (now >= deadline) {
				return up ? XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT : XRT_PLUGIN_STEREO_CAMERA_WAIT_SUSPENDED;
			}
			const int64_t step = std::min<int64_t>(deadline - now, 100 * kNsPerMs);
			c->cv.wait_for(lk, std::chrono::nanoseconds(step));
		}
	}
}

extern "C" void
leia_stereo_camera_android_release_frame(struct xrt_plugin_stereo_camera *c)
{
	if (c == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lk(c->pair_mu);
	c->held = -1;
}

extern "C" void
leia_stereo_camera_android_close(struct xrt_plugin_stereo_camera *c)
{
	if (c == nullptr) {
		return;
	}
	c->closing = true;
	const int64_t t0 = clock_ns(CLOCK_MONOTONIC);
	{
		std::lock_guard<std::mutex> lk(c->cam_mu);
		teardown_locked(c);
	}
	const int64_t t1 = clock_ns(CLOCK_MONOTONIC);
	if (c->trk_thread.joinable()) {
		c->trk_quit = true;
		c->trk_cv.notify_all();
		c->trk_thread.join();
	}
	{
		std::lock_guard<std::mutex> lk(c->cam_mu);
		handover_locked(c, false);
	}
	const int64_t t2 = clock_ns(CLOCK_MONOTONIC);
	SCAM_LOG_W("leia stereo camera (android): closed %s pair after %llu pairs (camera close %.1f ms, tracking "
	           "hand-back %.1f ms)",
	           c->desc.front ? "front" : "rear", (unsigned long long)c->pairs.load(), (double)(t1 - t0) / 1e6,
	           (double)(t2 - t1) / 1e6);
	delete c;
}

#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA
