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
 * Spec v8 (runtime #2803) adds, on the rear pair only: one further YUV
 * stream capture (1920x1440 by default; a mode switch to it is a session
 * reconfigure) and one JPEG still output per lens in every session (a still is
 * one request on the running session, decoded here). Still ONE YUV output per
 * lens. The front pair keeps exactly the shape above. Design and the stage 0
 * numbers: docs/android-stereo-camera.md, "Capture modes, controls and stills".
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
#include "leia_stereo_camera_android_modes.h"
#include "leia_stereo_camera_android_user_calib.h"
#include "leia_tracking_feed_stats.h"
#include "leia_cnsdk.h"

#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCaptureRequest.h>
#include <android/imagedecoder.h>
#include <dlfcn.h>
#include <jni.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <time.h>
#include <unistd.h>

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

/*
 * Spec v8 (capture modes, controls, stills): each slot group is compiled only
 * when the runtime headers announce it, like every earlier appended field, so
 * the plug-in still builds against the pinned (pre-v8) runtime and behaves
 * exactly as before there. LEIA_SCAM_V8 = any of them.
 */
#if defined(XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES) || defined(XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_CONTROLS) ||   \
    defined(XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_STILL)
#define LEIA_SCAM_V8 1
#endif

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
constexpr int64_t kReopenRefusedNs = 250 * kNsPerMs;  //!< retry period while the open is refused by policy
constexpr int64_t kGiveUpNs = 15000 * kNsPerMs;     //!< device gone this long -> ERROR
constexpr int64_t kEarlyFailNs = 3000 * kNsPerMs;   //!< a loss this soon after open counts as an open failure
constexpr int kEarlyFailMax = 2;                    //!< consecutive open failures -> ERROR, no more reopens
constexpr int64_t kLensFreeNs = 3000 * kNsPerMs;    //!< how long the tracking service gets to release its lens
constexpr int64_t kPairToleranceNs = 1 * kNsPerMs;  //!< measured skew is 0; be lenient
constexpr uint32_t kTrackW = 640, kTrackH = 480;    //!< the in-app detector accepts only this
constexpr int kEyeMaxImages = 4;
//! Readers of a large capture (kStreamMaxDefault raised by the knob): kPendingPerEye + 1.
constexpr int kEyeMaxImagesLarge = 3;
constexpr uint64_t kLargeCapturePixels = 4000000;
constexpr size_t kPendingPerEye = 2;
//! JPEG still readers: one still at a time, plus one spare.
constexpr int kJpegMaxImages = 2;
//! Spec v8 §5.7: a still that has not arrived within this fails (the stream never
//! pauses for it: the JPEG outputs share the running session, stage 0 §4b).
[[maybe_unused]] constexpr int64_t kStillJpegWaitNs = 3000 * kNsPerMs;
//! A still on the running capture is just the next pair: give it a second.
[[maybe_unused]] constexpr int64_t kStillSameCaptureNs = 1000 * kNsPerMs;
//! The longest frame a STREAM mode on its own capture may need (1/30 s + slack).
constexpr int64_t kStreamFrameNs = 33400000;
//! Default stream cap on a pair that may reconfigure (stage 0: lowest latency,
//! full 29.80 fps): debug.dxr.leia.scam_max_stream overrides it.
constexpr uint32_t kStreamMaxDefaultW = 1920, kStreamMaxDefaultH = 1440;
//! Default still width cap per eye (the lead's call: 4096x3072 on the rear pair).
constexpr uint32_t kStillMaxDefaultW = 4096;
//! Without the HAL's AF, the distance a cleared focus bit means: 0.5 D = 2 m.
//! Stage 0's rear lens bottoms out at 0.30-0.33 D (~hyperfocal, ~3.2 m), so at
//! 2 m the depth of field reaches ~1.2 m to ~5.3 m (H x d / (H +- d)): the
//! distances people and tabletop scenes are shot at, at the cost of sharpness
//! at infinity (0.31 D would cover ~1.6 m to infinity instead).
//! debug.dxr.leia.scam_default_focus_d overrides it.
constexpr float kDefaultFocusD = 0.5f;
//! Metering rectangle: this fraction of the logical array's shorter side.
constexpr double kMeteringFrac = 0.05;
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
 * Does this process hold android.permission.CAMERA (Context.checkSelfPermission)?
 * Without it the camera service strips the permission-gated vendor keys from
 * CameraCharacteristics, the factory block included, while getKeys() still
 * lists the rest (#505: measured after a fresh runtime install, whose CAMERA
 * grant is only requested at the first stream's consent). A failed check
 * counts as granted, so the read itself decides.
 */
bool
has_camera_permission(jni_env &j)
{
	JNIEnv *env = j.env;
	jclass ctx_cls = env->FindClass("android/content/Context");
	if (!j.ok() || ctx_cls == nullptr) {
		return true;
	}
	jmethodID check = env->GetMethodID(ctx_cls, "checkSelfPermission", "(Ljava/lang/String;)I");
	if (!j.ok() || check == nullptr) {
		env->DeleteLocalRef(ctx_cls);
		return true;
	}
	jstring perm = env->NewStringUTF("android.permission.CAMERA");
	const jint r = env->CallIntMethod(j.ctx, check, perm);
	const bool ok = j.ok();
	env->DeleteLocalRef(perm);
	env->DeleteLocalRef(ctx_cls);
	return !ok || r == 0; // PackageManager.PERMISSION_GRANTED
}

/*!
 * Bytes of the characteristics key of @p cam_id named @p full_name, else of
 * the first listed key whose name ends with @p suffix. Vendor keys are usually
 * NOT in getKeys() (measured on a Lume Pad 2), so the named lookup comes
 * first; it builds the Key exactly as an app would.
 *
 * #505: ABSENT only when the characteristics were read AND getKeys() listed
 * them without the key. A read without the host's JavaVM / Context (NO_JNI:
 * the runtime's camera manager can enumerate before the host published them)
 * or one whose characteristics call threw (FAILED, @p out_why says where)
 * says nothing about the block.
 */
leia_scam_block_read
read_vendor_block(const char *cam_id, const char *full_name, const char *suffix, std::vector<uint8_t> &out,
                  std::string *out_key, std::string *out_why)
{
	jni_env j;
	if (!j.acquire()) {
		return LEIA_SCAM_BLOCK_READ_NO_JNI;
	}
	JNIEnv *env = j.env;
	if (!has_camera_permission(j)) {
		return LEIA_SCAM_BLOCK_READ_NO_PERMISSION;
	}
	bool found = false;
	leia_scam_block_read res = LEIA_SCAM_BLOCK_READ_FAILED;
	const char *step = "?";
	env->PushLocalFrame(64);
	do {
		step = "Context.getSystemService(\"camera\")";
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
		step = "CameraManager.getCameraCharacteristics";
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
		step = "CameraCharacteristics key lookup";
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
		step = "CameraCharacteristics.getKeys()";
		jobject keys = env->CallObjectMethod(cc, get_keys);
		if (!j.ok() || keys == nullptr) {
			break;
		}
		const jint n = env->CallIntMethod(keys, l_size);
		if (!j.ok()) {
			break;
		}
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
		if (!found) {
			res = LEIA_SCAM_BLOCK_READ_ABSENT; // the keys were listed: the block is not there
		}
	} while (false);
	j.ok();
	env->PopLocalFrame(nullptr);
	if (found) {
		return LEIA_SCAM_BLOCK_READ_FOUND;
	}
	if (res == LEIA_SCAM_BLOCK_READ_FAILED && out_why != nullptr) {
		*out_why = std::string(step) + " failed for camera " + cam_id;
	}
	return res;
}

/*!
 * An int32[] characteristics key of @p cam_id by its full name (vendor keys
 * included), over JNI as read_vendor_block. False when absent.
 */
bool
read_vendor_i32(const char *cam_id, const char *full_name, std::vector<int32_t> &out)
{
	jni_env j;
	if (!j.acquire()) {
		return false;
	}
	JNIEnv *env = j.env;
	bool found = false;
	env->PushLocalFrame(32);
	do {
		jclass ctx_cls = env->FindClass("android/content/Context");
		jmethodID get_service = ctx_cls != nullptr ? env->GetMethodID(ctx_cls, "getSystemService",
		                                                              "(Ljava/lang/String;)Ljava/lang/Object;")
		                                           : nullptr;
		if (!j.ok() || get_service == nullptr) {
			break;
		}
		// Every call that can throw is checked before the next JNI call: with
		// an exception pending, CheckJNI aborts the whole process. A provider
		// that has just died throws from getCameraCharacteristics ("unknown
		// device"), which took the service down here once.
		jobject cm = env->CallObjectMethod(j.ctx, get_service, env->NewStringUTF("camera"));
		if (!j.ok() || cm == nullptr) {
			break;
		}
		jclass cm_cls = env->FindClass("android/hardware/camera2/CameraManager");
		if (!j.ok() || cm_cls == nullptr) {
			break;
		}
		jmethodID get_cc = env->GetMethodID(cm_cls, "getCameraCharacteristics",
		                                    "(Ljava/lang/String;)Landroid/hardware/camera2/CameraCharacteristics;");
		jobject cc = j.ok() ? env->CallObjectMethod(cm, get_cc, env->NewStringUTF(cam_id)) : nullptr;
		if (!j.ok() || cc == nullptr) {
			break;
		}
		jclass cc_cls = env->FindClass("android/hardware/camera2/CameraCharacteristics");
		jclass key_cls = env->FindClass("android/hardware/camera2/CameraCharacteristics$Key");
		jclass ints_cls = env->FindClass("[I");
		if (!j.ok() || cc == nullptr || cc_cls == nullptr || key_cls == nullptr || ints_cls == nullptr) {
			break;
		}
		jmethodID get = env->GetMethodID(cc_cls, "get",
		                                 "(Landroid/hardware/camera2/CameraCharacteristics$Key;)Ljava/lang/Object;");
		jmethodID key_ctor = env->GetMethodID(key_cls, "<init>", "(Ljava/lang/String;Ljava/lang/Class;)V");
		if (!j.ok() || get == nullptr || key_ctor == nullptr) {
			break;
		}
		jobject key = env->NewObject(key_cls, key_ctor, env->NewStringUTF(full_name), ints_cls);
		jobject v = j.ok() && key != nullptr ? env->CallObjectMethod(cc, get, key) : nullptr;
		if (j.ok() && v != nullptr && env->IsInstanceOf(v, ints_cls)) {
			jintArray arr = (jintArray)v;
			const jsize len = env->GetArrayLength(arr);
			out.resize((size_t)len);
			env->GetIntArrayRegion(arr, 0, len, (jint *)out.data());
			found = j.ok() && len > 0;
		}
	} while (false);
	j.ok();
	env->PopLocalFrame(nullptr);
	return found;
}

/*
 *
 * User stereo calibration: the record the Leia calibration app publishes for a
 * logical pair (leia_stereo_camera_android_user_calib.h). Read through its
 * exported, permission-free ContentProvider; the runtime APK declares the app
 * in <queries> for Android 11+ package visibility. The calibration app is
 * never started from here (no activity, no intent): the query is all.
 *
 */

constexpr const char *kUserCalibAuthority = "com.leiainc.calibrationapp.stereo";

enum class user_calib_query
{
	ROW,           //!< a record (json filled)
	NO_ROW,        //!< provider answered, nothing saved for this pair
	NOT_INSTALLED, //!< the provider does not resolve (app absent, or invisible: no <queries>)
	UNREACHABLE,   //!< resolves but query() returned null: the provider process could not be started
	FAILED,        //!< an exception, or a row without a "json" column
	NO_JNI,
};

//! Clear a pending exception, describing it in @p out when non-NULL.
bool
take_exception(JNIEnv *env, std::string *out)
{
	if (!env->ExceptionCheck()) {
		return false;
	}
	jthrowable t = env->ExceptionOccurred();
	env->ExceptionClear();
	if (out != nullptr && t != nullptr) {
		jclass tc = env->FindClass("java/lang/Throwable");
		jmethodID ts = tc != nullptr ? env->GetMethodID(tc, "toString", "()Ljava/lang/String;") : nullptr;
		jstring s = ts != nullptr ? (jstring)env->CallObjectMethod(t, ts) : nullptr;
		if (env->ExceptionCheck()) {
			env->ExceptionClear();
			s = nullptr;
		}
		if (s != nullptr) {
			const char *c = env->GetStringUTFChars(s, nullptr);
			if (c != nullptr) {
				*out = c;
				env->ReleaseStringUTFChars(s, c);
			}
		}
	}
	return true;
}

user_calib_query
query_user_calib(const char *logical_id, std::string &json, std::string &detail)
{
	jni_env j;
	if (!j.acquire()) {
		return user_calib_query::NO_JNI;
	}
	JNIEnv *env = j.env;
	user_calib_query res = user_calib_query::FAILED;
	env->PushLocalFrame(32);
	do {
		jclass ctx_cls = env->FindClass("android/content/Context");
		jclass pm_cls = env->FindClass("android/content/pm/PackageManager");
		jclass cr_cls = env->FindClass("android/content/ContentResolver");
		jclass uri_cls = env->FindClass("android/net/Uri");
		jclass cur_cls = env->FindClass("android/database/Cursor");
		if (take_exception(env, &detail)) {
			break;
		}
		jmethodID get_pm = env->GetMethodID(ctx_cls, "getPackageManager", "()Landroid/content/pm/PackageManager;");
		jmethodID resolve = env->GetMethodID(pm_cls, "resolveContentProvider",
		                                     "(Ljava/lang/String;I)Landroid/content/pm/ProviderInfo;");
		jmethodID get_cr = env->GetMethodID(ctx_cls, "getContentResolver", "()Landroid/content/ContentResolver;");
		jmethodID parse = env->GetStaticMethodID(uri_cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
		jmethodID query = env->GetMethodID(cr_cls, "query",
		                                   "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/"
		                                   "String;Ljava/lang/String;)Landroid/database/Cursor;");
		jmethodID move_first = env->GetMethodID(cur_cls, "moveToFirst", "()Z");
		jmethodID col_index = env->GetMethodID(cur_cls, "getColumnIndex", "(Ljava/lang/String;)I");
		jmethodID get_string = env->GetMethodID(cur_cls, "getString", "(I)Ljava/lang/String;");
		jmethodID close = env->GetMethodID(cur_cls, "close", "()V");
		if (take_exception(env, &detail)) {
			break;
		}
		jobject pm = env->CallObjectMethod(j.ctx, get_pm);
		if (take_exception(env, &detail) || pm == nullptr) {
			break;
		}
		jobject pi = env->CallObjectMethod(pm, resolve, env->NewStringUTF(kUserCalibAuthority), (jint)0);
		if (take_exception(env, &detail)) {
			break;
		}
		if (pi == nullptr) {
			res = user_calib_query::NOT_INSTALLED;
			break;
		}
		char uri[128];
		snprintf(uri, sizeof(uri), "content://%s/pairs/%s", kUserCalibAuthority, logical_id);
		jobject juri = env->CallStaticObjectMethod(uri_cls, parse, env->NewStringUTF(uri));
		jobject cr = env->CallObjectMethod(j.ctx, get_cr);
		if (take_exception(env, &detail) || juri == nullptr || cr == nullptr) {
			break;
		}
		jobject cur = env->CallObjectMethod(cr, query, juri, nullptr, nullptr, nullptr, nullptr);
		if (take_exception(env, &detail)) {
			break;
		}
		if (cur == nullptr) {
			res = user_calib_query::UNREACHABLE;
			break;
		}
		const jboolean any = env->CallBooleanMethod(cur, move_first);
		if (!take_exception(env, &detail)) {
			if (!any) {
				res = user_calib_query::NO_ROW;
			} else {
				const jint col = env->CallIntMethod(cur, col_index, env->NewStringUTF("json"));
				jstring s = col >= 0 && !env->ExceptionCheck() ? (jstring)env->CallObjectMethod(cur, get_string, col)
				                                               : nullptr;
				if (!take_exception(env, &detail) && s != nullptr) {
					const char *c = env->GetStringUTFChars(s, nullptr);
					if (c != nullptr) {
						json = c;
						env->ReleaseStringUTFChars(s, c);
						res = user_calib_query::ROW;
					}
				} else if (detail.empty()) {
					detail = "row has no \"json\" column";
				}
			}
		}
		env->CallVoidMethod(cur, close);
		take_exception(env, nullptr);
	} while (false);
	take_exception(env, nullptr);
	env->PopLocalFrame(nullptr);
	return res;
}

/*!
 * The host process's own files directory (Context.getFilesDir()), where the
 * last accepted user calibration is kept. Resolved once; empty when unknown.
 */
const std::string &
files_dir()
{
	static std::string dir;
	static bool tried = false;
	if (tried) {
		return dir;
	}
	tried = true;
	jni_env j;
	if (!j.acquire()) {
		return dir;
	}
	JNIEnv *env = j.env;
	env->PushLocalFrame(16);
	jclass ctx_cls = env->FindClass("android/content/Context");
	jclass file_cls = env->FindClass("java/io/File");
	jmethodID get_files = ctx_cls != nullptr ? env->GetMethodID(ctx_cls, "getFilesDir", "()Ljava/io/File;") : nullptr;
	jmethodID get_path =
	    file_cls != nullptr ? env->GetMethodID(file_cls, "getAbsolutePath", "()Ljava/lang/String;") : nullptr;
	jobject f = j.ok() && get_files != nullptr ? env->CallObjectMethod(j.ctx, get_files) : nullptr;
	jstring s = j.ok() && f != nullptr && get_path != nullptr ? (jstring)env->CallObjectMethod(f, get_path) : nullptr;
	if (j.ok() && s != nullptr) {
		const char *c = env->GetStringUTFChars(s, nullptr);
		if (c != nullptr) {
			dir = c;
			env->ReleaseStringUTFChars(s, c);
		}
	}
	j.ok();
	env->PopLocalFrame(nullptr);
	return dir;
}

/*!
 * The host process's external files directory
 * (Context.getExternalFilesDir(null), /sdcard/Android/data/<pkg>/files), which
 * adb can read without run-as: where the tracking-frame dump goes. Resolved
 * once; empty when unknown.
 */
const std::string &
ext_files_dir()
{
	static std::string dir;
	static bool tried = false;
	if (tried) {
		return dir;
	}
	tried = true;
	jni_env j;
	if (!j.acquire()) {
		return dir;
	}
	JNIEnv *env = j.env;
	env->PushLocalFrame(16);
	jclass ctx_cls = env->FindClass("android/content/Context");
	jclass file_cls = env->FindClass("java/io/File");
	jmethodID get_files = ctx_cls != nullptr
	                          ? env->GetMethodID(ctx_cls, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;")
	                          : nullptr;
	jmethodID get_path =
	    file_cls != nullptr ? env->GetMethodID(file_cls, "getAbsolutePath", "()Ljava/lang/String;") : nullptr;
	jobject f = j.ok() && get_files != nullptr ? env->CallObjectMethod(j.ctx, get_files, (jstring) nullptr) : nullptr;
	jstring s = j.ok() && f != nullptr && get_path != nullptr ? (jstring)env->CallObjectMethod(f, get_path) : nullptr;
	if (j.ok() && s != nullptr) {
		const char *c = env->GetStringUTFChars(s, nullptr);
		if (c != nullptr) {
			dir = c;
			env->ReleaseStringUTFChars(s, c);
		}
	}
	j.ok();
	env->PopLocalFrame(nullptr);
	return dir;
}

//! Cache file of one pair, keyed by the pair and the factory block's SHA-256.
std::string
user_calib_cache_path(const char *logical_id, const uint8_t block_sha256[32])
{
	const std::string &dir = files_dir();
	if (dir.empty()) {
		return std::string();
	}
	char hex[65];
	leia_scam_sha256_hex(block_sha256, hex);
	hex[16] = '\0';
	return dir + "/leia_scam_user_calib_" + logical_id + "_" + hex + ".json";
}

bool
read_small_file(const std::string &path, std::string &out, int64_t *out_mtime_s)
{
	FILE *f = path.empty() ? nullptr : fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return false;
	}
	struct stat st;
	if (fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > 64 * 1024) {
		fclose(f);
		return false;
	}
	out.resize((size_t)st.st_size);
	const bool ok = fread(&out[0], 1, out.size(), f) == out.size();
	fclose(f);
	if (out_mtime_s != nullptr) {
		*out_mtime_s = (int64_t)st.st_mtime;
	}
	return ok;
}

//! Write via a temporary file and rename, so a reader never sees half a record.
bool
write_small_file(const std::string &path, const std::string &data)
{
	if (path.empty()) {
		return false;
	}
	const std::string tmp = path + ".tmp";
	FILE *f = fopen(tmp.c_str(), "wb");
	if (f == nullptr) {
		return false;
	}
	const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
		remove(tmp.c_str());
		return false;
	}
	return true;
}

/*
 *
 * Discovery.
 *
 */

struct pair_desc
{
	char logical_id[16]{};
	char lens_a[16]{}, lens_b[16]{}; //!< the physical lenses, in the HAL's order
	//! #505: the factory block's read state. Until it is final (not PENDING)
	//! nothing below is derived and no pair is listed.
	leia_scam_block_retry block_retry{};
	//! The pending reason last logged (-1 = none), so each reason logs once.
	int block_deferred_reason{-1};
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
	//! What decided the eye order (leia_scam_factory_orient_by_pose): 1 = the
	//! HAL's lens poses disagreed with the block, which is then READ in the
	//! other convention (oriented_factory); 0 = they agreed; -1 = no usable
	//! poses (the block decides alone). `factory` itself stays as stored, so a
	//! later R from another source (same convention as the block) composes.
	int pose_orient{-1};
	double pose_main[3]{}, pose_sub[3]{}; //!< ACAMERA_LENS_POSE_TRANSLATION, metres
	bool have_pose{false};
	float measured_rate{0.0f};
	//! Spec v8 §5.5: the listed modes (mode 0 = the default eye), built once
	//! the pair is described (describe_v8_locked). Every one keeps the
	//! default's horizontal field of view (leia_scam_build_modes).
	leia_scam_mode modes[LEIA_SCAM_MODES_MAX]{};
	uint32_t n_modes{0};
	//! Spec v8 §5.6: the logical camera's control characteristics. hal.af_modes
	//! is NOT stored (a pair_desc is copied); hal_caps_of() points it at af_modes.
	uint8_t af_modes[8]{};
	size_t n_af_modes{0};
	leia_scam_hal_caps hal{};
	leia_scam_control_caps ctl_caps{};
	//! The lens cannot focus (the HAL lists a minimum focus distance of 0).
	bool fixed_focus{false};
	//! The HAL's AF may be used (debug.dxr.leia.scam_hal_af=1); else AF off + one distance.
	bool hal_af{false};
	float default_focus_d{0.5f};
	//! Index of the mode taken from the JPEG still output, -1 = none (no JPEG outputs).
	int still_out{-1};
	//! Measured pair rate of the own stream capture (the default's is measured_rate).
	float measured_own_rate{0.0f};
	//! The logical camera's active array: metering regions are relative to it.
	uint32_t log_aw{0}, log_ah{0};
	//! The user calibration (calibration app) is checked against these: the
	//! SHA-256 of the exact block bytes read, and the block's own R, which
	//! factory.R returns to whenever no valid record applies.
	bool have_block{false};
	uint8_t block_sha256[32]{};
	double factory_R[3][3]{};
	//! Outcome of the last user-calibration read, logged only when it changes.
	std::string user_calib_state;
	//! Provider back-off: after an unreachable provider, no query before this
	//! (CLOCK_MONOTONIC). Each blocked query costs the OEM policy log lines.
	int64_t user_calib_next_query_ns{0};
	bool user_calib_disabled{false};
	//! The record last written to the cache file (skip rewriting it).
	std::string user_calib_cached;
	//! The factory block at the array, as stored (factory returns to it before
	//! any user / served calibration is applied).
	leia_scam_factory_calib block_factory{};
	//! factory holds a served FULL record: its convention is explicit
	//! (x_sub = R x_main + T, the writer's), so the lens-pose swap
	//! (pose_orient) never applies to it.
	bool factory_explicit{false};
	//! Why the factory block is absent (for the source log line).
	std::string block_why;
};

std::mutex g_mu;
bool g_scanned = false;
std::vector<pair_desc> g_pairs; //!< index 0 = front when present
ACameraManager *g_mgr = nullptr;
int64_t g_user_calib_read_ns = 0; //!< CLOCK_MONOTONIC of the last user-calibration read
constexpr int64_t kUserCalibRereadNs = 1000 * kNsPerMs;
constexpr int64_t kUserCalibUnreachableRetryNs = 30000 * kNsPerMs;

leia_scam_factory_calib
oriented_factory(const pair_desc &p);

/*!
 * Put the calibration app's rotation for @p p into p.factory.R when a valid
 * record exists, else the block's own R, and recompute the stream
 * calibration. The record replaces R ONLY: intrinsics, T and the reference
 * size stay the plug-in's. R is in the sensors' native frame, so it goes in
 * before the eye turn (leia_scam_stream_calib_from_factory turns last).
 *
 * The provider is authoritative when it answers. When it does not (the
 * calibration app's process is not running and an OEM auto-start policy
 * refuses to start it for a provider query: measured on the phone-class
 * device, AutoLaunchManagerService "Provider RelatedStart BlockResult = true"
 * for the runtime's own uid), the last accepted record is used from a copy in
 * this process's files directory, keyed by the pair and the block's SHA-256,
 * through the same gate. After an unreachable answer the provider is not
 * asked again for kUserCalibUnreachableRetryNs.
 *
 * One WARN whenever the outcome changes.
 */
/*!
 * The served record (display config "stereoCalibrations", read by CNSDK) for
 * @p p, when it applies. Returns true and fills p.factory when it does; else
 * false with @p why (why it was skipped, for the log line).
 */
bool
apply_served_locked(pair_desc &p, std::string &why, std::string &state)
{
	if (!prop_bool("debug.dxr.leia.scam_served_calib", true)) {
		why = "disabled by debug.dxr.leia.scam_served_calib=0";
		return false;
	}
	leia_scam_served_stereo srv;
	memset(&srv, 0, sizeof(srv));
	const int r = leia_cnsdk_find_stereo_calibration(p.logical_id, &srv);
	if (r == LEIA_CNSDK_SERVED_UNKNOWN) {
		why = "no CNSDK core of this process has read its display config yet (checked again on enumerate)";
		return false;
	}
	if (r == LEIA_CNSDK_SERVED_UNSUPPORTED) {
		why = "the CNSDK on the device predates 0.10.91 (no stereoCalibrations accessor)";
		return false;
	}
	if (r == LEIA_CNSDK_SERVED_NONE) {
		why = std::string("the served display config has no stereoCalibrations entry for logical ") + p.logical_id;
		return false;
	}
	leia_scam_factory_calib conv{};
	double delta = 0.0;
	const leia_scam_served_verdict v =
	    leia_scam_served_to_factory(&srv, p.main_id, p.sub_id, p.array_w, p.array_h,
	                                p.have_block ? &p.block_factory : nullptr, p.have_block ? p.block_sha256 : nullptr,
	                                &conv, &delta);
	char buf[300];
	if (v == LEIA_SCAM_SERVED_FULL) {
		if (!p.have_block && leia_scam_factory_is_vertical(&conv)) {
			why = "a vertical (turned) pair without a factory block is not supported";
			return false;
		}
		// The record's eye order must agree with the HAL's lens poses (when
		// they decide): a record that puts the sub lens on the other side is
		// refused, never silently mirrored.
		if (p.have_pose && p.have_sensor_orientation && prop_bool("debug.dxr.leia.scam_pose", true)) {
			leia_scam_factory_calib probe = conv;
			if (leia_scam_factory_orient_by_pose(&probe, p.pose_main, p.pose_sub, p.front, p.sensor_orientation,
			                                     p.rot) == 1) {
				why = "its eye order contradicts the HAL's lens poses";
				return false;
			}
		}
		p.factory = conv;
		p.factory_explicit = true;
		snprintf(buf, sizeof(buf),
		         "source=served display config record (mirror: full lens model at %dx%d, lensIds [%s, %s], saved "
		         "%s, R %.3f deg from the factory R)",
		         srv.width, srv.height, srv.lens_ids[0], srv.lens_ids[1], srv.timestamp[0] ? srv.timestamp : "?",
		         delta);
		state = buf;
		return true;
	}
	if (v == LEIA_SCAM_SERVED_ROTATION) {
		memcpy(p.factory.R, conv.R, sizeof(p.factory.R));
		snprintf(buf, sizeof(buf),
		         "source=served display config record (user: R only on this factory block, saved %s, R %.3f deg "
		         "from the factory R)",
		         srv.timestamp[0] ? srv.timestamp : "?", delta);
		state = buf;
		return true;
	}
	why = std::string("the served record of logical ") + p.logical_id + " was refused: " +
	      leia_scam_served_verdict_str(v);
	return false;
}

/*!
 * Pick the calibration of @p p, in order (one WARN whenever the outcome
 * changes, naming the source that won and why the ones above it were skipped):
 *  1. the served display config record (leia_cnsdk_find_stereo_calibration):
 *     a mirror record replaces the whole lens model, a user record R only;
 *  2. the calibration app's provider (content://com.leiainc.calibrationapp.stereo,
 *     or its cached copy): R only, as before. Only a fallback: an OEM
 *     auto-launch policy blocks a cold start of that provider;
 *  3. the factory block.
 */
void
apply_user_calib_locked(pair_desc &p)
{
	const leia_scam_factory_calib prev = p.factory;
	p.factory_explicit = false;
	if (p.have_block) {
		p.factory = p.block_factory;
	}
	std::string served_why, served_state;
	if (apply_served_locked(p, served_why, served_state)) {
		const leia_scam_factory_calib of = oriented_factory(p);
		p.calibrated = leia_scam_stream_calib_from_factory(&of, p.array_w, p.array_h, p.cap_w, p.cap_h, p.crop_w,
		                                                   p.crop_h, p.rot, &p.stream);
		const std::string state = served_state + "; calibration app provider and factory block skipped (the served "
		                                         "record wins)";
		if (state != p.user_calib_state) {
			p.user_calib_state = state;
			SCAM_LOG_W("leia stereo camera (android): %s pair (logical %s) calibration %s", p.front ? "front" : "rear",
			           p.logical_id, state.c_str());
		}
		return;
	}
	if (!p.have_block || p.ref_w == 0) {
		// No factory block: only a served mirror record could calibrate it.
		if (p.calibrated) {
			p.calibrated = false;
		}
		const std::string state = "source=none (raw only, ideal parallel pair downstream): served display config "
		                          "record skipped (" +
		                          served_why +
		                          "); calibration app provider skipped (it refines the factory block's R); factory "
		                          "block skipped (" +
		                          p.block_why + ")";
		if (state != p.user_calib_state) {
			p.user_calib_state = state;
			SCAM_LOG_W("leia stereo camera (android): %s pair (logical %s) calibration %s", p.front ? "front" : "rear",
			           p.logical_id, state.c_str());
		}
		return;
	}
	const bool disabled = !prop_bool("debug.dxr.leia.scam_user_calib", true);
	const int64_t now = clock_ns(CLOCK_MONOTONIC);
	if (!disabled && !p.user_calib_disabled && now < p.user_calib_next_query_ns && !p.user_calib_state.empty() &&
	    p.user_calib_state.rfind("source=served", 0) != 0) {
		p.factory = prev; // backing off an unreachable provider: keep the current source
		return;
	}
	p.user_calib_disabled = disabled;
	memcpy(p.factory.R, p.factory_R, sizeof(p.factory.R));
	std::string state;
	std::string age;
	char buf[400];
	if (disabled) {
		state = "source=factory (user calibration disabled by debug.dxr.leia.scam_user_calib=0)";
	} else {
		std::string json, detail;
		const user_calib_query q = query_user_calib(p.logical_id, json, detail);
		std::string why; // the provider's answer, when it gave no record
		leia_scam_user_calib_provider prov = LEIA_SCAM_PROVIDER_UNAVAILABLE;
		switch (q) {
		case user_calib_query::ROW: prov = LEIA_SCAM_PROVIDER_RECORD; break;
		case user_calib_query::NO_ROW:
			prov = LEIA_SCAM_PROVIDER_NO_RECORD;
			why = "no user calibration saved for this pair";
			break;
		case user_calib_query::NOT_INSTALLED:
			prov = LEIA_SCAM_PROVIDER_NO_RECORD;
			why = std::string("calibration app not installed or not visible: content://") + kUserCalibAuthority +
			      " does not resolve";
			break;
		case user_calib_query::UNREACHABLE:
			why = std::string("content://") + kUserCalibAuthority +
			      " unreachable: query returned null; the calibration app's process is not running and an OEM "
			      "self-start policy may block starting it for a provider query; next try in 30 s";
			p.user_calib_next_query_ns = now + kUserCalibUnreachableRetryNs;
			break;
		case user_calib_query::NO_JNI: why = "no JNI context to query the calibration app"; break;
		case user_calib_query::FAILED: why = "user calibration query failed: " + (detail.empty() ? "?" : detail); break;
		}
		const std::string cache_path = user_calib_cache_path(p.logical_id, p.block_sha256);
		std::string cached;
		int64_t cached_mtime = 0;
		const bool have_cache = prov == LEIA_SCAM_PROVIDER_UNAVAILABLE &&
		                        read_small_file(cache_path, cached, &cached_mtime);
		leia_scam_user_calib_choice c;
		leia_scam_user_calib_choose(prov, json.data(), json.size(), have_cache ? cached.data() : nullptr,
		                            have_cache ? cached.size() : 0, p.logical_id, p.main_id, p.sub_id,
		                            p.block_sha256, p.factory_R, &c);
		if (c.cache_write && json != p.user_calib_cached) {
			if (write_small_file(cache_path, json)) {
				p.user_calib_cached = json;
			} else {
				SCAM_LOG_I("leia stereo camera (android): could not cache the user calibration at %s",
				           cache_path.empty() ? "(no files dir)" : cache_path.c_str());
			}
		}
		if (c.cache_drop && !cache_path.empty() && remove(cache_path.c_str()) == 0) {
			p.user_calib_cached.clear();
		}
		const char *ts = c.rec.timestamp[0] != '\0' ? c.rec.timestamp : "?";
		const char *method = c.rec.method[0] != '\0' ? c.rec.method : "?";
		switch (c.source) {
		case LEIA_SCAM_USER_CALIB_SRC_PROVIDER:
			memcpy(p.factory.R, c.rec.R, sizeof(p.factory.R));
			snprintf(buf, sizeof(buf),
			         "source=user-calibration (saved %s, method %s, R differs from the factory R by %.3f deg)", ts,
			         method, c.delta_deg);
			state = buf;
			break;
		case LEIA_SCAM_USER_CALIB_SRC_CACHE: {
			memcpy(p.factory.R, c.rec.R, sizeof(p.factory.R));
			snprintf(buf, sizeof(buf),
			         "source=user-calibration, cached copy (saved %s, method %s, R differs from the factory R by "
			         "%.3f deg; provider: %s)",
			         ts, method, c.delta_deg, why.c_str());
			state = buf;
			const int64_t age_s = (int64_t)time(nullptr) - cached_mtime;
			snprintf(buf, sizeof(buf), "; cache written %lld s ago", (long long)(age_s > 0 ? age_s : 0));
			age = buf;
		} break;
		case LEIA_SCAM_USER_CALIB_SRC_FACTORY:
			if (c.considered) {
				const char *what = prov == LEIA_SCAM_PROVIDER_RECORD ? "user calibration" : "cached user calibration";
				if (c.parse_failed) {
					snprintf(buf, sizeof(buf), "source=factory (%s record does not parse)", what);
				} else {
					snprintf(buf, sizeof(buf), "source=factory (%s of %s ignored: %s%s%s)", what, ts,
					         leia_scam_user_calib_verdict_str(c.verdict), why.empty() ? "" : "; provider: ",
					         why.c_str());
				}
				state = buf;
			} else {
				state = "source=factory (" + why + ")";
			}
			break;
		}
	}
	const leia_scam_factory_calib p_oriented_tmp = oriented_factory(p);
	p.calibrated = leia_scam_stream_calib_from_factory(&p_oriented_tmp, p.array_w, p.array_h, p.cap_w, p.cap_h, p.crop_w,
	                                                   p.crop_h, p.rot, &p.stream);
	state = "rotation " + state + "; served display config record skipped (" + served_why + ")";
	if (state != p.user_calib_state) {
		p.user_calib_state = state;
		SCAM_LOG_W("leia stereo camera (android): %s pair (logical %s) calibration %s%s", p.front ? "front" : "rear",
		           p.logical_id, state.c_str(), age.c_str());
	}
}

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

/*!
 * One physical lens's ACAMERA_LENS_POSE_TRANSLATION (Android sensor-coordinate
 * axes, metres). False when it is absent or its reference cannot be compared
 * (UNDEFINED / AUTOMOTIVE); @p ref gets the reference (0 = primary camera,
 * the pre-P meaning when the tag is missing).
 */
bool
read_lens_pose(const char *cam_id, double out[3], uint8_t *ref)
{
	ACameraMetadata *m = nullptr;
	if (ACameraManager_getCameraCharacteristics(mgr(), cam_id, &m) != ACAMERA_OK || m == nullptr) {
		return false;
	}
	*ref = ACAMERA_LENS_POSE_REFERENCE_PRIMARY_CAMERA;
	get_u8(m, ACAMERA_LENS_POSE_REFERENCE, ref);
	ACameraMetadata_const_entry e{};
	const bool ok = ACameraMetadata_getConstEntry(m, ACAMERA_LENS_POSE_TRANSLATION, &e) == ACAMERA_OK &&
	                e.count >= 3 &&
	                (*ref == ACAMERA_LENS_POSE_REFERENCE_PRIMARY_CAMERA || *ref == ACAMERA_LENS_POSE_REFERENCE_GYROSCOPE);
	if (ok) {
		for (int i = 0; i < 3; i++) {
			out[i] = (double)e.data.f[i];
		}
	}
	ACameraMetadata_free(m);
	return ok;
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

//! p.hal with its AF-mode list pointing into @p p itself.
leia_scam_hal_caps
hal_caps_of(const pair_desc &p)
{
	leia_scam_hal_caps h = p.hal;
	h.af_modes = p.af_modes;
	h.n_af_modes = p.n_af_modes;
	return h;
}

//! The default eye of @p p as the capture + crop + turn the source runs for it.
leia_scam_eye_geom
default_geom(const pair_desc &p)
{
	leia_scam_eye_geom g{};
	g.cap_w = p.cap_w;
	g.cap_h = p.cap_h;
	g.crop_w = p.crop_w;
	g.crop_h = p.crop_h;
	g.rot = p.rot;
	return g;
}

//! @p format output sizes of @p m with their minimum frame durations (0 = not listed).
[[maybe_unused]] std::vector<leia_scam_size>
output_sizes(const ACameraMetadata *m, int32_t format)
{
	std::vector<leia_scam_size> out;
	std::vector<int32_t> c;
	if (!get_i32(m, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, c)) {
		return out;
	}
	for (size_t i = 0; i + 3 < c.size(); i += 4) {
		if (c[i] == format && c[i + 3] == ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT &&
		    c[i + 1] > 0 && c[i + 2] > 0) {
			out.push_back({(uint32_t)c[i + 1], (uint32_t)c[i + 2], 0});
		}
	}
	ACameraMetadata_const_entry e{};
	if (ACameraMetadata_getConstEntry(m, ACAMERA_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, &e) == ACAMERA_OK) {
		for (uint32_t i = 0; i + 3 < e.count; i += 4) {
			if (e.data.i64[i] != format) {
				continue;
			}
			for (leia_scam_size &s : out) {
				if ((int64_t)s.w == e.data.i64[i + 1] && (int64_t)s.h == e.data.i64[i + 2]) {
					s.min_frame_duration_ns = e.data.i64[i + 3];
				}
			}
		}
	}
	return out;
}

/*!
 * Spec v8: the mode list and the control caps of @p p, from the lenses' and
 * the logical camera's characteristics (once the pair is described). One
 * WARN with the result. Knobs:
 *   debug.dxr.leia.scam_modes=0          list the default mode only (v7 behaviour)
 *   debug.dxr.leia.scam_max_stream=WxH   largest own-capture STREAM mode (rear default
 *                                        1920x1440; the front never reconfigures)
 *   debug.dxr.leia.scam_max_still=WxH    largest still from the JPEG output (default 4096 wide)
 *   debug.dxr.leia.scam_still=0          no JPEG still output in the session (rear)
 *   debug.dxr.leia.scam_front_controls=1 metering + exposure compensation on the front pair
 *   debug.dxr.leia.scam_hal_af=1         use the HAL's AF (CONTINUOUS / AUTO_ONCE) on a pair
 */
[[maybe_unused]] void
describe_v8_locked(ACameraManager *m, pair_desc &p)
{
	// Sizes both lenses list, with the larger of the two minimum frame durations.
	std::vector<leia_scam_size> sizes, jpegs;
	ACameraMetadata *mm = nullptr, *sm = nullptr;
	if (ACameraManager_getCameraCharacteristics(m, p.main_id, &mm) == ACAMERA_OK && mm != nullptr &&
	    ACameraManager_getCameraCharacteristics(m, p.sub_id, &sm) == ACAMERA_OK && sm != nullptr) {
		for (int32_t fmt : {(int32_t)AIMAGE_FORMAT_YUV_420_888, (int32_t)AIMAGE_FORMAT_JPEG}) {
			std::vector<leia_scam_size> &dst = fmt == AIMAGE_FORMAT_JPEG ? jpegs : sizes;
			const std::vector<leia_scam_size> a = output_sizes(mm, fmt), b = output_sizes(sm, fmt);
			for (const leia_scam_size &x : a) {
				for (const leia_scam_size &y : b) {
					if (x.w == y.w && x.h == y.h) {
						leia_scam_size s = x;
						s.min_frame_duration_ns =
						    (x.min_frame_duration_ns == 0 || y.min_frame_duration_ns == 0)
						        ? 0
						        : std::max(x.min_frame_duration_ns, y.min_frame_duration_ns);
						dst.push_back(s);
					}
				}
			}
		}
	}
	if (mm != nullptr) {
		ACameraMetadata_free(mm);
	}
	if (sm != nullptr) {
		ACameraMetadata_free(sm);
	}

	// The front pair is the head tracker's lens: no session change under it (no
	// reconfigure, no JPEG output on the tracking lens): its modes are cuts of
	// the default capture only, and a still there is the next stream pair.
	leia_scam_mode_policy pol{};
	pol.max_stream_w = kStreamMaxDefaultW;
	pol.max_stream_h = kStreamMaxDefaultH;
	parse_size(prop_str("debug.dxr.leia.scam_max_stream", ""), &pol.max_stream_w, &pol.max_stream_h);
	pol.max_still_w = kStillMaxDefaultW;
	pol.max_still_h = 0;
	parse_size(prop_str("debug.dxr.leia.scam_max_still", ""), &pol.max_still_w, &pol.max_still_h);
	pol.reconfigure_stream = !p.front;
	pol.still_output = !p.front && prop_bool("debug.dxr.leia.scam_still", true);
	pol.default_rate = 0.0f; // filled at enumerate time from the measured rates
	pol.own_rate = 0.0f;
	pol.max_stream_frame_ns = kStreamFrameNs;
	const leia_scam_eye_geom def = default_geom(p);
	if (prop_bool("debug.dxr.leia.scam_modes", true)) {
		p.n_modes = (uint32_t)leia_scam_build_modes(p.array_w, p.array_h, &def, sizes.data(), sizes.size(),
		                                            jpegs.data(), jpegs.size(), &pol, p.modes, LEIA_SCAM_MODES_MAX);
	} else {
		p.n_modes = (uint32_t)leia_scam_build_modes(p.array_w, p.array_h, &def, nullptr, 0, nullptr, 0, &pol,
		                                            p.modes, 1);
	}
	p.still_out = -1;
#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_STILL
	// The session carries the JPEG outputs only when a still can be asked for.
	for (uint32_t i = 0; i < p.n_modes; i++) {
		if (p.modes[i].source == LEIA_SCAM_SRC_STILL_OUTPUT) {
			p.still_out = (int)i;
		}
	}
#endif

	// Controls: the logical camera's characteristics (AF, AE and the lens are
	// controlled through it); the main lens stands in for a missing lens key.
	leia_scam_hal_caps h{};
	p.n_af_modes = 0;
	ACameraMetadata *lm = nullptr;
	if (ACameraManager_getCameraCharacteristics(m, p.logical_id, &lm) == ACAMERA_OK && lm != nullptr) {
		ACameraMetadata_const_entry e{};
		if (ACameraMetadata_getConstEntry(lm, ACAMERA_CONTROL_AF_AVAILABLE_MODES, &e) == ACAMERA_OK) {
			for (uint32_t i = 0; i < e.count && p.n_af_modes < sizeof(p.af_modes); i++) {
				p.af_modes[p.n_af_modes++] = e.data.u8[i];
			}
		}
		const char *lens_ids[2] = {p.logical_id, p.main_id};
		for (const char *id : lens_ids) {
			ACameraMetadata *x = nullptr;
			if (id == p.logical_id) {
				x = lm;
			} else if (ACameraManager_getCameraCharacteristics(m, id, &x) != ACAMERA_OK) {
				x = nullptr;
			}
			if (x == nullptr) {
				continue;
			}
			if (!h.have_min_focus_distance &&
			    ACameraMetadata_getConstEntry(x, ACAMERA_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &e) == ACAMERA_OK &&
			    e.count > 0) {
				h.min_focus_distance = e.data.f[0];
				h.have_min_focus_distance = true;
				uint8_t cal = 0;
				if (get_u8(x, ACAMERA_LENS_INFO_FOCUS_DISTANCE_CALIBRATION, &cal)) {
					h.focus_distance_calibration = cal;
				}
			}
			if (x != lm) {
				ACameraMetadata_free(x);
			}
		}
		std::vector<int32_t> v;
		if (get_i32(lm, ACAMERA_REQUEST_AVAILABLE_CAPABILITIES, v)) {
			h.manual_sensor = std::find(v.begin(), v.end(), (int32_t)ACAMERA_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR) !=
			                  v.end();
		}
		if (get_i32(lm, ACAMERA_CONTROL_MAX_REGIONS, v) && v.size() >= 3) {
			h.max_regions_ae = v[0];
			h.max_regions_af = v[2];
		}
		if (get_i32(lm, ACAMERA_CONTROL_AE_COMPENSATION_RANGE, v) && v.size() >= 2) {
			h.ae_comp_min = v[0];
			h.ae_comp_max = v[1];
		}
		if (ACameraMetadata_getConstEntry(lm, ACAMERA_CONTROL_AE_COMPENSATION_STEP, &e) == ACAMERA_OK && e.count > 0) {
			h.ae_comp_step_num = e.data.r[0].numerator;
			h.ae_comp_step_den = e.data.r[0].denominator;
		}
		if (get_i32(lm, ACAMERA_SENSOR_INFO_ACTIVE_ARRAY_SIZE, v) && v.size() >= 4) {
			p.log_aw = (uint32_t)v[2];
			p.log_ah = (uint32_t)v[3];
		}
		ACameraMetadata_free(lm);
	}
	if (p.log_aw == 0) {
		p.log_aw = p.array_w; // a logical camera without its own array uses its main lens's
		p.log_ah = p.array_h;
	}
	p.hal = h;
	p.hal.af_modes = nullptr;
	const leia_scam_hal_caps hc = hal_caps_of(p);
	p.hal_af = prop_bool("debug.dxr.leia.scam_hal_af", false);
	leia_scam_control_caps_from_hal(&hc, p.hal_af, p.front, prop_bool("debug.dxr.leia.scam_front_controls", false),
	                                &p.ctl_caps);
	p.default_focus_d = kDefaultFocusD;
	{
		const std::string d = prop_str("debug.dxr.leia.scam_default_focus_d", "");
		const double v = d.empty() ? -1.0 : atof(d.c_str());
		if (v >= 0.0 && v <= 50.0) {
			p.default_focus_d = (float)v;
		}
	}
	p.fixed_focus = h.have_min_focus_distance && h.min_focus_distance <= 0.0f;

	std::string ml;
	for (uint32_t i = 0; i < p.n_modes; i++) {
		const leia_scam_mode &md = p.modes[i];
		const bool same = md.geom.cap_w == p.cap_w && md.geom.cap_h == p.cap_h;
		char buf[160];
		(void)same;
		snprintf(buf, sizeof(buf), "%s%ux%u %s%s (%s %ux%u%s)", i == 0 ? "" : ", ", md.eye_w, md.eye_h,
		         (md.flags & LEIA_SCAM_MODE_STREAM) ? "stream" : "",
		         (md.flags & LEIA_SCAM_MODE_STILL) ? ((md.flags & LEIA_SCAM_MODE_STREAM) ? "+still" : "still") : "",
		         md.source == LEIA_SCAM_SRC_DEFAULT_CAPTURE ? "default capture"
		         : md.source == LEIA_SCAM_SRC_OWN_CAPTURE   ? "own capture"
		                                                    : "JPEG output",
		         md.geom.cap_w, md.geom.cap_h, md.geom.crop_w != md.geom.cap_w ? ", columns cropped" : "");
		ml += buf;
	}
	const leia_scam_control_caps &cc = p.ctl_caps;
	SCAM_LOG_W("leia stereo camera (android): %s pair v8: %u mode(s): %s; %zu YUV / %zu JPEG size(s) on both "
	           "lenses; controls 0x%x (focus modes 0x%x, %s, default %.2f D, min focus %.2f D %s%s, EV %.2f..%.2f "
	           "step %.3f)%s",
	           p.front ? "front" : "rear", p.n_modes, ml.c_str(), sizes.size(), jpegs.size(), cc.supported_controls,
	           cc.supported_focus_modes,
	           p.hal_af ? "HAL AF (debug.dxr.leia.scam_hal_af=1)"
	                    : "AF off, one logical distance drives both lenses",
	           p.default_focus_d, h.min_focus_distance,
	           h.focus_distance_calibration == 2   ? "calibrated"
	           : h.focus_distance_calibration == 1 ? "approximate"
	                                               : "uncalibrated",
	           p.fixed_focus ? ", fixed focus" : "", cc.exposure_compensation_min_ev, cc.exposure_compensation_max_ev,
	           cc.exposure_compensation_step_ev,
	           p.front ? "; tracking-shared: only cuts of the default capture (no reconfigure, no JPEG output on "
	                     "the tracking lens), exposure controls only with debug.dxr.leia.scam_front_controls=1"
	                   : "");
}

bool
left_is_sub(const pair_desc &p);

//! The factory block in the convention the HAL's lens poses chose (pose_orient).
leia_scam_factory_calib
oriented_factory(const pair_desc &p)
{
	leia_scam_factory_calib f = p.factory;
	if (p.pose_orient == 1 && !p.factory_explicit) {
		leia_scam_factory_swap_convention(&f);
	}
	return f;
}

/*!
 * Read the factory block of @p p (its logical id and both lenses) and derive
 * everything from it. While the block is not readable yet (#505) the read
 * is only recorded in p.block_retry and nothing else changes; once the
 * verdict is final the pair is described, logged and listed.
 */
void
describe_pair_locked(ACameraManager *m, pair_desc &p)
{
	uint32_t want_w = 1280, want_h = 720;
	parse_size(prop_str("debug.dxr.leia.scam_size", ""), &want_w, &want_h);
	const std::string swap = prop_str("debug.dxr.leia.scam_swap", "auto");
	std::vector<uint32_t> ref_sizes;

	// Factory block: on the logical id and on the main lens only.
	const char *suffix = p.front ? "eeprom3d_front" : "eeprom3d_rear";
	const std::string full = prop_str("debug.dxr.leia.scam_calib_key_prefix", kCalibKeyPrefix) + suffix;
	// The physical lenses are read explicitly by their own ids: the HAL
	// publishes the block on the main lens (tablets: front = lens 1, rear =
	// lens 0), and the logical id does not always carry it.
	const std::string a = p.lens_a, b = p.lens_b;
	std::vector<uint8_t> blk;
	std::string key, why;
	leia_scam_block_read reads[3];
	reads[0] = read_vendor_block(p.logical_id, full.c_str(), suffix, blk, &key, &why);
	bool have = reads[0] == LEIA_SCAM_BLOCK_READ_FOUND;
	std::vector<uint8_t> tmp;
	std::string lens_key;
	reads[1] = read_vendor_block(a.c_str(), full.c_str(), suffix, tmp, &lens_key, &why);
	const bool a_has = reads[1] == LEIA_SCAM_BLOCK_READ_FOUND;
	reads[2] = a_has ? LEIA_SCAM_BLOCK_READ_ABSENT
	                 : read_vendor_block(b.c_str(), full.c_str(), suffix, tmp, &lens_key, &why);
	const bool b_has = !a_has && reads[2] == LEIA_SCAM_BLOCK_READ_FOUND;
	const std::string main_id = b_has ? b : a, sub_id = b_has ? a : b;
	if (!have && (a_has || b_has)) {
		blk = tmp;
		key = lens_key;
		have = true;
	}
	const leia_scam_block_read combined = leia_scam_block_read_combine(reads, 3);
	const leia_scam_block_verdict verdict =
	    leia_scam_block_retry_step(&p.block_retry, combined, clock_ns(CLOCK_MONOTONIC));
	if (verdict == LEIA_SCAM_BLOCK_PENDING) {
		// #505: not readable yet. Nothing is derived (or listed) from an empty
		// read: the pair stays unlisted until the block is read, or the retry
		// gives up, and only then is it described.
		if (p.block_deferred_reason != (int)combined) {
			p.block_deferred_reason = (int)combined;
			if (combined == LEIA_SCAM_BLOCK_READ_NO_PERMISSION) {
				SCAM_LOG_W("leia stereo camera (android): %s pair (logical %s): factory block read deferred: "
				           "this process lacks android.permission.CAMERA, so the camera service hides the "
				           "vendor key %s from its characteristics; the pair is not listed until the "
				           "permission is granted (checked on every enumerate), then it appears CALIBRATED",
				           p.front ? "front" : "rear", p.logical_id, full.c_str());
			} else if (combined == LEIA_SCAM_BLOCK_READ_NO_JNI) {
				SCAM_LOG_W("leia stereo camera (android): %s pair (logical %s): factory block read deferred: "
				           "CNSDK host context not ready (leia_cnsdk_get_jni has no JavaVM / Context yet); "
				           "the pair is not listed until it is read, retrying every %lld ms for up to %lld s",
				           p.front ? "front" : "rear", p.logical_id,
				           (long long)(LEIA_SCAM_BLOCK_RETRY_PERIOD_NS / kNsPerMs),
				           (long long)(LEIA_SCAM_BLOCK_NO_JNI_LIMIT_NS / (1000 * kNsPerMs)));
			} else {
				SCAM_LOG_W("leia stereo camera (android): %s pair (logical %s): factory block read deferred: %s; "
				           "the pair is not listed until it is read, %u attempts at most",
				           p.front ? "front" : "rear", p.logical_id, why.c_str(), LEIA_SCAM_BLOCK_MAX_FAILED);
			}
		}
		return;
	}
	if (verdict == LEIA_SCAM_BLOCK_NONE) {
		why = "absent: the characteristics of logical " + std::string(p.logical_id) + ", lens " + a + " and lens " +
		      b + " were read and none has " + full;
	} else if (verdict == LEIA_SCAM_BLOCK_GAVE_UP) {
		char buf[200];
		snprintf(buf, sizeof(buf), "never readable after %u attempts over %.1f s: ", p.block_retry.attempts,
		         (double)(clock_ns(CLOCK_MONOTONIC) - p.block_retry.first_ns) / 1e9);
		why = buf + (combined == LEIA_SCAM_BLOCK_READ_NO_JNI ? std::string("no CNSDK host context (JavaVM / Context)")
		                                                     : why);
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
	// 3. Eye order: the HAL's own lens poses say which lens is on the camera's
	// left. A block written in the other convention (x_main = R x_sub + T, as
	// on a phone-class front module) disagrees with them and is swapped here,
	// before anything is derived from it. No poses: the block decides alone.
	{
		uint8_t ref_m = 0, ref_s = 0;
		p.have_pose = read_lens_pose(p.main_id, p.pose_main, &ref_m) &&
		              read_lens_pose(p.sub_id, p.pose_sub, &ref_s) && ref_m == ref_s;
	}
	if (p.ref_w != 0 && p.have_pose && p.have_sensor_orientation &&
	    prop_bool("debug.dxr.leia.scam_pose", true)) {
		leia_scam_factory_calib probe = p.factory;
		p.pose_orient = leia_scam_factory_orient_by_pose(&probe, p.pose_main, p.pose_sub, p.front,
		                                                 p.sensor_orientation, p.rot);
		if (p.pose_orient == 1) {
			SCAM_LOG_W("leia stereo camera (android): %s pair: the HAL's lens poses (main %s at "
			           "(%.1f, %.1f, %.1f) mm, sub %s at (%.1f, %.1f, %.1f) mm) disagree with the factory "
			           "block's eye order; reading the block as x_main = R x_sub + T",
			           p.front ? "front" : "rear", p.main_id, p.pose_main[0] * 1e3, p.pose_main[1] * 1e3,
			           p.pose_main[2] * 1e3, p.sub_id, p.pose_sub[0] * 1e3, p.pose_sub[1] * 1e3,
			           p.pose_sub[2] * 1e3);
		}
	}
	if (p.ref_w != 0) {
		const leia_scam_factory_calib of = oriented_factory(p);
		p.calibrated = leia_scam_stream_calib_from_factory(&of, p.array_w, p.array_h, p.cap_w, p.cap_h,
		                                                   p.crop_w, p.crop_h, p.rot, &p.stream);
		p.block_hash = leia_scam_block_hash(blk.data(), blk.size());
		p.have_block = true;
		leia_scam_sha256(blk.data(), blk.size(), p.block_sha256);
		memcpy(p.factory_R, p.factory.R, sizeof(p.factory_R));
		p.block_factory = p.factory;
	}
	if (swap == "0" || swap == "1") {
		p.swap_override = true;
		p.swap_value = swap == "1";
	}
	const bool eff_sub = left_is_sub(p);
	const char *order_src = p.swap_override ? "forced by debug.dxr.leia.scam_swap"
	                        : !p.calibrated  ? "assumed, no calibration"
	                        : p.pose_orient == 1 ? "HAL lens poses; block convention swapped"
	                        : p.pose_orient == 0 ? "HAL lens poses agree with the block"
	                                             : "factory block; no usable lens poses";
	SCAM_LOG_W(
	    "leia stereo camera (android): %s pair = logical %s, main lens %s, sub lens %s, array %ux%u, eye "
	    "%ux%u (captured %ux%u, cropped %ux%u, turned %u deg cw, sensor orientation %d), factory block %s "
	    "(%zu bytes, %s, expressed at %ux%u, principal points %.1f %% off its centre) -> %s, baseline %.2f mm, "
	    "SBS left = %s lens %s (%s)",
	    p.front ? "front" : "rear", p.logical_id, p.main_id, p.sub_id, p.array_w, p.array_h, p.eye_w, p.eye_h,
	    p.cap_w, p.cap_h, p.crop_w, p.crop_h, p.rot, p.sensor_orientation, have ? "found" : "MISSING",
	    blk.size(), have ? key.c_str() : suffix, p.ref_w, p.ref_h, ref_off * 100.0,
	    p.calibrated ? "CALIBRATED" : "raw only",
	    p.calibrated ? leia_scam_factory_baseline_mm(&p.factory) : 0.0,
	    eff_sub ? "sub" : "main", eff_sub ? p.sub_id : p.main_id, order_src);
#ifdef LEIA_SCAM_V8
	describe_v8_locked(m, p); // built against pre-v8 headers: one mode, no controls, as before
#endif
	p.block_why = have ? std::string() : why;
	// The served display config record, the calibration app's user rotation,
	// or the factory block (logs the source that won and why the higher ones
	// were skipped). Without a block only a served mirror record can calibrate.
	apply_user_calib_locked(p);
}

//! Is any pair still waiting for its factory block (#505)?
bool
any_pending_locked()
{
	return std::any_of(g_pairs.begin(), g_pairs.end(),
	                   [](const pair_desc &q) { return q.block_retry.verdict == LEIA_SCAM_BLOCK_PENDING; });
}

/*!
 * #505: read the factory block again for every pair still pending, when its
 * retry is due. Runs on every enumerate (the runtime re-enumerates at most
 * once a second on a client's enumerate and every ~2 s while one listens for
 * camera events), open, get_calibration and tracking-lens query.
 *
 * The runtime keeps a camera's FIRST description (flags included) until the
 * service restarts, and a camera's device_identity carries the block's hash,
 * so a pair is never listed raw and later upgraded: while any pair is pending
 * none is listed (indices stay stable), and once all are final they appear as
 * NEW cameras, which makes the runtime fetch their calibration and send
 * XrEventDataStereoCamerasChangedDXR to every listening client.
 */
void
retry_pending_locked()
{
	if (!any_pending_locked()) {
		return;
	}
	ACameraManager *m = mgr();
	const int64_t now = clock_ns(CLOCK_MONOTONIC);
	for (pair_desc &p : g_pairs) {
		if (m != nullptr && leia_scam_block_retry_due(&p.block_retry, now)) {
			describe_pair_locked(m, p);
		}
	}
	if (any_pending_locked()) {
		return;
	}
	std::string v;
	for (const pair_desc &q : g_pairs) {
		char buf[160];
		snprintf(buf, sizeof(buf), "%s%s pair %s after %u read(s)", v.empty() ? "" : ", ", q.front ? "front" : "rear",
		         q.block_retry.verdict == LEIA_SCAM_BLOCK_HAVE   ? "CALIBRATED from the factory block"
		         : q.block_retry.verdict == LEIA_SCAM_BLOCK_NONE ? "raw only (no factory block)"
		                                                         : "raw only (factory block never readable)",
		         q.block_retry.attempts);
		v += buf;
	}
	SCAM_LOG_W("leia stereo camera (android): factory block retry finished: %s; listing the cameras now",
	           v.c_str());
}

void
scan_locked()
{
	if (g_scanned) {
		retry_pending_locked();
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
		snprintf(p.lens_a, sizeof(p.lens_a), "%s", phys[0]);
		snprintf(p.lens_b, sizeof(p.lens_b), "%s", phys[1]);
		ACameraMetadata_free(meta);
		if ((p.front && !g_pairs.empty() && g_pairs[0].front) ||
		    (!p.front && std::any_of(g_pairs.begin(), g_pairs.end(), [](const pair_desc &q) { return !q.front; }))) {
			continue; // one pair per facing
		}
		if (!p.front && !prop_bool("debug.dxr.leia.scam_rear", true)) {
			continue;
		}
		describe_pair_locked(m, p);
		if (p.front) {
			g_pairs.insert(g_pairs.begin(), p);
		} else {
			g_pairs.push_back(p);
		}
	}
	if (g_pairs.empty()) {
		SCAM_LOG_W("leia stereo camera (android): no logical stereo pair found");
	}
	g_user_calib_read_ns = clock_ns(CLOCK_MONOTONIC);
}

/*!
 * Re-read the user calibration on enumerate (at most once a second), so a
 * record the calibration app saved after the scan takes effect without a
 * restart. A camera already open keeps the calibration it was opened with.
 */
void
refresh_user_calib_locked()
{
	const int64_t now = clock_ns(CLOCK_MONOTONIC);
	if (now - g_user_calib_read_ns < kUserCalibRereadNs) {
		return;
	}
	g_user_calib_read_ns = now;
	for (pair_desc &p : g_pairs) {
		apply_user_calib_locked(p);
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
	//! Per-eye size of THIS buffer (a mode switch changes it while the runtime
	//! may still hold an older buffer).
	uint32_t eye_w{0}, eye_h{0};
	//! The pair's sensor timestamp (BOOTTIME, the capture-result key) and each
	//! eye's own start of exposure on CLOCK_MONOTONIC.
	int64_t ts_boot{0};
	int64_t eye_mono_ns[2]{};
};

struct pending_img
{
	AImage *img{nullptr};
	int64_t ts{0};
};

//! One capture's metadata (spec v8 capture result), keyed by sensor timestamp.
struct hal_rec
{
	int64_t ts{0}; //!< logical result's SENSOR_TIMESTAMP (BOOTTIME), 0 = empty slot
	bool have_logical{false}, have_left{false}, have_right{false};
	leia_scam_hal_result logical{}, left{}, right{};
};
constexpr size_t kResultRing = 16;

/*!
 * The Camera2 request template's own values for every tag the controls touch,
 * so a cleared control bit puts back exactly the source's default (spec v8
 * §9b: "a field whose bit is clear means the source's own default").
 */
struct req_defaults
{
	bool have_af_mode{false};
	uint8_t af_mode{0};
	bool have_ae_comp{false};
	int32_t ae_comp{0};
	bool have_focus_distance{false};
	float focus_distance{0.0f};
	std::vector<int32_t> af_regions, ae_regions; //!< empty = the template had none
};

//! Still state machine (spec v8 §5.7). Written under pair_mu.
enum class still_state
{
	IDLE,      //!< nothing requested (or released)
	BUSY,      //!< the worker is getting it
	GRAB,      //!< armed: the next stream pair is packed as the still (default capture)
	WAIT_JPEG, //!< a still request went out; collecting both lenses' JPEGs
	CAPTURED,  //!< the pair is here; the worker finishes it
	READY,     //!< poll_still hands it over
	FAILED,    //!< poll_still reports FAILED once
	DELIVERED, //!< handed over, until release_still
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
	//! The last openCamera was refused by policy (ACAMERA_ERROR_CAMERA_DISABLED):
	//! the next start opens the device first and hands tracking over after.
	bool open_refused{false};
	//! SBS eye (0 = left, 1 = right) whose lens feeds the tracker, and its id.
	//! Which SBS eye carries trk_lens. Atomic: the feeder can re-pick the lens
	//! (pick_tracking_lens) while the reader threads read it.
	std::atomic<int> trk_eye{1};
	//! The lens was chosen without knowing the cores' tracking camera (no core
	//! had read its config yet): the feeder re-asks until one knows.
	bool trk_lens_guessed{false};
	char trk_lens[16]{};
	struct leia_cnsdk_ext_intrinsics trk_intr{};
	// The RUNNING stream mode's geometry (spec v8: set_mode changes it; the
	// readers read it under pair_mu, so it is written under pair_mu too).
	uint32_t w{0}, h{0};           //!< per-eye output (after the turn)
	uint32_t crop_w{0}, crop_h{0}; //!< per-eye crop of the capture (before the turn)
	uint32_t rot{0};               //!< clockwise turn of each eye
	uint32_t cap_w{0}, cap_h{0};   //!< per-lens capture of the stream mode
	uint32_t mode_index{0};
	//! What the capture session streams right now (= cap_w x cap_h except while
	//! a still runs on its own session). Under pair_mu.
	uint32_t sess_cap_w{0}, sess_cap_h{0};
	//! The capture session is configured lazily (spec v8 §9b): on the first
	//! wait_frame or set_mode, so a set_mode right after open costs nothing.
	bool started{false};

	// Spec v8 controls (camera thread; applied under cam_mu).
	leia_scam_controls ctl{};
	uint32_t trigger_seen{0};
	//! Without HAL AF: the distance the request carries now (LOCKED keeps it).
	float cmd_focus_d{0.0f};
	leia_scam_eye_times_tracker eye_times{};
	// The JPEG still outputs (rear) and the still request, beside the stream's.
	AImageReader *rd_jl{nullptr}, *rd_jr{nullptr};
	ACaptureSessionOutput *out_jl{nullptr}, *out_jr{nullptr};
	ACameraOutputTarget *stgt_l{nullptr}, *stgt_r{nullptr}, *stgt_jl{nullptr}, *stgt_jr{nullptr};
	ACaptureRequest *still_req{nullptr};
	req_defaults still_tmpl;
	req_defaults tmpl;
	//! The metering rectangle last applied (valid when ctl asks for a point).
	bool region_ok{false};
	int32_t region[5]{};
	ACameraCaptureSession_logicalCamera_captureCallbacks cap_cb{};
	std::mutex res_mu;
	hal_rec res_ring[kResultRing];
	size_t res_next{0};
	char left_id[16]{}; //!< physical id of the SBS left eye's lens

	// Spec v8 still (state under pair_mu).
	still_state still{still_state::IDLE};
	std::thread still_thread;
	std::condition_variable still_cv;
	leia_scam_eye_geom still_geom{};
	std::vector<uint8_t> jpeg[2]; //!< the still's JPEG per eye (WAIT_JPEG -> CAPTURED)
	int64_t jpeg_ts[2]{};
	std::vector<uint8_t> still_buf;
	uint32_t still_w{0}, still_h{0}; //!< per eye
	int64_t still_ts_boot{0};
	int64_t still_eye_mono[2]{};
	uint64_t still_request{0};
	bool still_lossy{false}; //!< the still came through the JPEG outputs
	leia_scam_capture_state still_res{};
	bool still_have_res{false};

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
	//! Per eye: each reader thread uses only its own, so a lens re-pick that
	//! moves the tracking frame to the other reader never shares a buffer.
	std::vector<uint8_t> trk_scratch[2];

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

//! One eye of @p g's geometry (crop, then turn) into an SBS NV12 buffer of eye width @p ew at column @p x0.
void
pack_eye_geom(const yuv_view &v, uint8_t *dst, const leia_scam_eye_geom &g, uint32_t ew, uint32_t eh, uint32_t x0)
{
	if (g.rot == 0) {
		pack_eye(v, dst, ew, eh, x0);
	} else {
		pack_eye_turned(v, dst, g.crop_w, g.crop_h, g.rot, ew, eh, x0);
	}
}

/*!
 * Under pair_mu: the armed still (spec v8 §5.7) from this pair, when the pair
 * is of the still's capture (a still of the default capture is the next
 * stream pair, cropped as the still asks): pack it into still_buf and wake
 * the worker.
 */
void
grab_still_locked(xrt_plugin_stereo_camera *c, const yuv_view &vl, const yuv_view &vr, int64_t ts_l, int64_t ts_r)
{
	if (c->still != still_state::GRAB || (uint32_t)vl.w != c->still_geom.cap_w ||
	    (uint32_t)vl.h != c->still_geom.cap_h || (uint32_t)vr.w != c->still_geom.cap_w ||
	    (uint32_t)vr.h != c->still_geom.cap_h) {
		return;
	}
	uint32_t ew, eh;
	leia_scam_eye_size(&c->still_geom, &ew, &eh);
	const size_t need = (size_t)2 * ew * eh * 3 / 2;
	if (c->still_buf.size() != need) {
		c->still_buf.assign(need, 128);
	}
	pack_eye_geom(vl, c->still_buf.data(), c->still_geom, ew, eh, 0);
	pack_eye_geom(vr, c->still_buf.data(), c->still_geom, ew, eh, ew);
	const int64_t off = boot_minus_mono_ns();
	c->still_w = ew;
	c->still_h = eh;
	c->still_ts_boot = ts_l;
	c->still_eye_mono[0] = ts_l - off;
	c->still_eye_mono[1] = ts_r - off;
	c->still = still_state::CAPTURED;
	c->still_cv.notify_all();
}

//! Under pair_mu: pack the matched pair (left, right) and publish it.
void
pack_pair_locked(xrt_plugin_stereo_camera *c, AImage *l, AImage *r, int64_t ts_l, int64_t ts_r)
{
	const int64_t ts_boot = ts_l;
	yuv_view vl, vr;
	if (!yuv_view_of(l, &vl) || !yuv_view_of(r, &vr)) {
		c->drops++;
		return;
	}
	grab_still_locked(c, vl, vr, ts_l, ts_r);
	if ((uint32_t)vl.w != c->cap_w || (uint32_t)vl.h != c->cap_h || (uint32_t)vr.w != c->cap_w ||
	    (uint32_t)vr.h != c->cap_h) {
		// The tail of a session switch (an image of the previous capture size):
		// not a frame of the running stream mode.
		return;
	}
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
	leia_scam_eye_geom g{c->cap_w, c->cap_h, c->crop_w, c->crop_h, c->rot};
	pack_eye_geom(vl, b.data.data(), g, c->w, c->h, 0);
	pack_eye_geom(vr, b.data.data(), g, c->w, c->h, c->w);
	b.seq = ++c->seq;
	const int64_t off = boot_minus_mono_ns();
	b.time_mono_ns = ts_boot - off;
	b.eye_w = c->w;
	b.eye_h = c->h;
	b.ts_boot = ts_boot;
	b.eye_mono_ns[0] = ts_l - off;
	b.eye_mono_ns[1] = ts_r - off;
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
		// Per capture (spec v8 modes report the rate of THEIR capture): the
		// default's, or the one further stream capture's.
		const bool def_cap = c->cap_w == c->desc.cap_w && c->cap_h == c->desc.cap_h;
		float &mine = def_cap ? c->desc.measured_rate : c->desc.measured_own_rate;
		if (mine <= 0.0f) {
			SCAM_LOG_W("leia stereo camera (android): measured pair rate %.2f Hz over %d pairs (%ux%u per lens)",
			           rate, kRateWindow, c->cap_w, c->cap_h);
		}
		mine = rate;
		{
			std::lock_guard<std::mutex> g(g_mu);
			for (auto &p : g_pairs) {
				if (strcmp(p.logical_id, c->desc.logical_id) == 0) {
					(def_cap ? p.measured_rate : p.measured_own_rate) = rate;
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
		// reader's thread touches trk_scratch[eye].
		// The tracker's intrinsics are those of the default capture's 4:3 cut;
		// a capture of another aspect (never on the front pair today, which
		// only runs its default capture) would feed it a different field.
		if (c->track && eye == c->trk_eye.load(std::memory_order_relaxed) && !c->closing.load()) {
			yuv_view vm;
			if (yuv_view_of(img, &vm) && (uint64_t)vm.w * c->desc.cap_h == (uint64_t)vm.h * c->desc.cap_w) {
				std::vector<uint8_t> &scratch = c->trk_scratch[eye];
				scratch.resize((size_t)kTrackW * kTrackH);
				crop_track_from_eye(vm, scratch.data());
				push_tracking(c, scratch.data(), kTrackW, ts, true);
			}
		}
		std::lock_guard<std::mutex> lk(c->pair_mu);
		// A reader of a session that was just replaced (spec v8 mode switch or
		// still): its images are no longer this source's.
		if (c->closing.load() || reader != (eye == 0 ? c->rd_l : c->rd_r)) {
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
			pack_pair_locked(c, l, r, eye == 0 ? ts : it->ts, eye == 0 ? it->ts : ts);
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
 * Chooses the lens that feeds the in-app tracker and the intrinsics handed to
 * the cores with it (c->trk_lens, trk_eye, trk_intr, trk_lens_guessed). Run at
 * open, and again from the feeder once a core knows its tracking camera when
 * open had to guess (tracking_feeder). Returns true when the choice changed.
 */
bool
pick_tracking_lens(xrt_plugin_stereo_camera *c)
{
	const std::string prev = c->trk_lens;
	// Which lens feeds the tracker: the one the vendor device config names
	// as the tracking camera (its pose is what the in-app engine applies),
	// else the factory block's main lens. A Lume Pad 2's config names none
	// and its service tracks from the main lens; a nubia Pad 3D II's service
	// tracks from the sub lens. debug.dxr.leia.scam_track_lens=<id> forces it.
	char cfg_id[16] = {0};
	std::string forced = prop_str("debug.dxr.leia.scam_track_lens", "");
	const bool have_cfg = leia_cnsdk_get_tracking_camera_id(cfg_id, sizeof(cfg_id));
	// The cores' tracking camera when they know it for sure (CNSDK's own
	// accessor, or the factory main lens matched in the device config;
	// face_tracking_worker), else the factory main lens. Never the device
	// config's cameras[0]: a phone-class config lists camera 0 first while
	// its trackingCameras[0] is camera 1.
	const char *want = !forced.empty() ? forced.c_str() : (have_cfg ? cfg_id : c->desc.main_id);
	const bool use_sub = strcmp(want, c->desc.sub_id) == 0;
	snprintf(c->trk_lens, sizeof(c->trk_lens), "%s", use_sub ? c->desc.sub_id : c->desc.main_id);
	// SBS left eye = sub when lsub; the tracking eye is the one showing trk_lens.
	c->trk_eye.store((use_sub == c->lsub) ? 0 : 1);
	SCAM_LOG_W("leia stereo camera (android): tracking lens %s (%s)", c->trk_lens,
	           !forced.empty() ? "forced by debug.dxr.leia.scam_track_lens"
	           : have_cfg      ? "the cores' tracking camera"
	                           : "factory main lens; no core knows the tracking camera yet");
	leia_scam_lens tl{};
	leia_scam_lens_for_stream(use_sub ? &c->desc.factory.sub : &c->desc.factory.main, c->desc.array_w,
	                          c->desc.array_h, c->desc.cap_w, c->desc.cap_h, &tl);
	{
		const double cw = std::min<double>(c->desc.cap_w, c->desc.cap_h * 4.0 / 3.0);
		const double ch = std::min<double>(c->desc.cap_h, cw * 0.75);
		const double s = (double)kTrackW / cw;
		tl.fx *= s;
		tl.fy *= s;
		tl.cx = (tl.cx - (c->desc.cap_w - cw) / 2.0) * s;
		tl.cy = (tl.cy - (c->desc.cap_h - ch) / 2.0) * s;
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
	// The head tracker's configured intrinsics describe ITS capture (e.g.
	// 640x480 at 90 fps), which on some modules is a narrower cut of the
	// sensor than our 640x480: the cores prefer those values, re-expressed
	// for our field, over the factory block's (leia_cnsdk_ext_tracking_feed).
	// The field ratio comes from the HAL's sensor-mode table when it has one
	// (leia_scam_track_fov_ratio), else 1 (Camera2: every stream sees the
	// same field); debug.dxr.leia.scam_track_fov_ratio overrides it.
	{
		const uint32_t trk_fps = (uint32_t)std::max(1, atoi(prop_str("debug.dxr.leia.scam_track_fps", "90").c_str()));
		std::vector<int32_t> tbl;
		leia_scam_sensor_mode modes[8];
		size_t nm = 0;
		if (read_vendor_i32(c->trk_lens, "org.quic.camera2.sensormode.info.SensorModeTable", tbl)) {
			nm = leia_scam_parse_sensor_modes(tbl.data(), tbl.size(), modes, 8);
		}
		double r = leia_scam_track_fov_ratio(c->desc.array_w, c->desc.array_h, modes, nm, c->desc.cap_w, c->desc.cap_h,
		                                     30, kTrackW, kTrackH, trk_fps);
		const char *src = "sensor-mode table";
		if (r > 0.0 && c->desc.cap_w * 3 > c->desc.cap_h * 4) {
			r *= (double)c->desc.cap_h * 4.0 / (3.0 * (double)c->desc.cap_w); // our 4:3 cut of a wider capture
		}
		if (!(r > 0.0)) {
			r = 1.0;
			src = "no sensor-mode table: same field assumed";
		}
		const std::string ov = prop_str("debug.dxr.leia.scam_track_fov_ratio", "");
		const double ovr = ov.empty() ? 0.0 : atof(ov.c_str());
		if (ovr > 0.1 && ovr < 10.0) {
			r = ovr;
			src = "forced by debug.dxr.leia.scam_track_fov_ratio";
		}
		ei.fov_ratio = r;
		snprintf(ei.lens, sizeof(ei.lens), "%s", c->trk_lens);
		SCAM_LOG_W("leia stereo camera (android): tracking frame field = %.4f x the tracker's own %ux%u@%u "
		           "capture (%s; %zu sensor mode(s); our %ux%u@30 cut to 640x480); block-derived fx %.1f",
		           r, kTrackW, kTrackH, trk_fps, src, nm, c->desc.cap_w, c->desc.cap_h, tl.fx);
	}
	c->trk_intr = ei; // handed over by start_locked, around the actual open
	c->trk_lens_guessed = forced.empty() && !have_cfg;
	return prev != c->trk_lens;
}

//! Writes one fed tracking frame (exactly the bytes the sink got) as a binary
//! PGM, for checking orientation / aspect / luma against what the tracker expects.
void
dump_tracking_frame(xrt_plugin_stereo_camera *c, const std::string &path, const uint8_t *y)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr) {
		SCAM_LOG_W("leia stereo camera (android): tracking frame dump: cannot write %s", path.c_str());
		return;
	}
	fprintf(f, "P5\n%u %u\n255\n", kTrackW, kTrackH);
	const size_t n = fwrite(y, 1, (size_t)kTrackW * kTrackH, f);
	fclose(f);
	SCAM_LOG_W("leia stereo camera (android): tracking frame dump: wrote %s (%zu bytes Y8 %ux%u, lens %s, "
	           "unturned sensor-native, as fed to the shared-camera sink)",
	           path.c_str(), n, kTrackW, kTrackH, c->trk_lens);
}

/*!
 * Hands tracking frames to CNSDK through the C API (leia_cnsdk_ext_tracking_feed
 * -> leia_shared_camera_sink_on_image) with a tight 640-stride Y8 buffer and
 * no rotation. That path never had the Java/JNI SharedCameraSink bugs fixed by
 * CNSDK #762 (padded strides, non-zero rotation), so nothing here changes when
 * the plug-in pins a CNSDK >= 0.10.88.
 *
 * This thread exists so that nothing the engine does can reach the camera:
 * the engine runs detection on the caller's thread, and its idle throttle
 * SLEEPS the caller 500 ms per skipped frame (CNSDK engine FrameThrottle,
 * active 5 s after start once no face was seen for 2 s). The capture callback
 * only overwrites the latest-frame slot (push_tracking), so a sleeping engine
 * costs tracking frames, never camera pairs. The 10 s rate WARN below is where
 * that shows: fed/s far under captured/s with stalls > 0 is the throttle.
 *
 * Dump: touch <external files dir>/dxr_track_dump_trigger (the host process's
 * /sdcard/Android/data/<pkg>/files) and the next fed frame is written beside it
 * as dxr_track_frame.pgm.
 */
void
tracking_feeder(xrt_plugin_stereo_camera *c)
{
	std::vector<uint8_t> local;
	leia_tfs stats;
	leia_tfs_init(&stats, clock_ns(CLOCK_MONOTONIC));
	{
		uint64_t polls = 0, faces = 0;
		leia_cnsdk_face_poll_counters(&polls, &faces);
		stats.captured_at = c->trk_in.load();
		stats.faces_at = faces;
		stats.polls_at = polls;
	}
	const std::string &dir = ext_files_dir();
	const std::string trigger = dir.empty() ? std::string() : dir + "/dxr_track_dump_trigger";
	int64_t next_trigger_check_ns = 0;
	int64_t next_lens_check_ns = 0;
	for (;;) {
		int64_t ts = 0;
		bool have = false;
		{
			std::unique_lock<std::mutex> lk(c->trk_mu);
			// Bounded: the rate report must also come out when no frame does.
			c->trk_cv.wait_for(lk, std::chrono::milliseconds(1000),
			                   [c] { return c->trk_quit.load() || c->trk_have; });
			if (c->trk_quit.load()) {
				return;
			}
			if (c->trk_have) {
				local.swap(c->trk_slot);
				ts = c->trk_ts_boot;
				c->trk_have = false;
				have = true;
			}
		}
		if (have) {
			const int64_t t0 = clock_ns(CLOCK_MONOTONIC);
			leia_cnsdk_ext_tracking_feed(local.data(), (int32_t)kTrackW, (int32_t)kTrackH, ts);
			const int64_t t1 = clock_ns(CLOCK_MONOTONIC);
			leia_tfs_on_feed(&stats, t0, t1);
			c->trk_busy_ns += t1 - t0;
			c->trk_fed++;
			if (!trigger.empty() && t1 >= next_trigger_check_ns) {
				next_trigger_check_ns = t1 + 1000 * kNsPerMs;
				struct stat st;
				if (stat(trigger.c_str(), &st) == 0) {
					unlink(trigger.c_str());
					dump_tracking_frame(c, dir + "/dxr_track_frame.pgm", local.data());
				}
			}
		}
		// Open had to guess the tracking lens (the stream can open before this
		// process's core has read its device config): feed the lens the engine
		// is configured for (its pose and intrinsics) as soon as a core knows.
		const int64_t now = clock_ns(CLOCK_MONOTONIC);
		if (c->trk_lens_guessed && now >= next_lens_check_ns) {
			next_lens_check_ns = now + 1000 * kNsPerMs;
			char known[16] = {0};
			if (leia_cnsdk_get_tracking_camera_id(known, sizeof(known))) {
				std::lock_guard<std::mutex> lk(c->cam_mu);
				const std::string was = c->trk_lens;
				if (pick_tracking_lens(c)) {
					leia_cnsdk_ext_tracking_set_intrinsics(&c->trk_intr);
					SCAM_LOG_W("leia stereo camera (android): tracking lens %s -> %s once the cores knew their "
					           "tracking camera (\"%s\"); fed frames now come from lens %s",
					           was.c_str(), c->trk_lens, known, c->trk_lens);
				}
			}
		}
		uint64_t polls = 0, faces = 0;
		leia_cnsdk_face_poll_counters(&polls, &faces);
		leia_tfs_report r;
		if (leia_tfs_report_due(&stats, clock_ns(CLOCK_MONOTONIC), c->trk_in.load(), faces, polls, &r)) {
			SCAM_LOG_W("leia stereo camera (android): tracking feed %.1f captured/s -> %.1f fed/s to the sink "
			           "(call avg %.1f ms max %.0f ms, longest gap %.0f ms, %llu engine stall(s) >= 400 ms), "
			           "faces %.1f/s of %.1f polls/s, lens %s",
			           r.captured_per_s, r.fed_per_s, r.avg_call_ms, r.max_call_ms, r.max_gap_ms,
			           (unsigned long long)r.stalls, r.faces_per_s, r.polls_per_s, c->trk_lens);
		}
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

/*
 *
 * Capture results (spec v8 §5.6): the logical camera's per-frame metadata and
 * the SBS left lens's physical result, kept in a small ring keyed by the
 * sensor timestamp; get_capture_result and the still look a pair up by it.
 *
 */

//! What @p m reports; @p ts gets its SENSOR_TIMESTAMP when non-NULL.
[[maybe_unused]] void
read_hal_result(const ACameraMetadata *m, leia_scam_hal_result *r, int64_t *ts)
{
	ACameraMetadata_const_entry e{};
	if (ts != nullptr && ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_TIMESTAMP, &e) == ACAMERA_OK && e.count > 0) {
		*ts = e.data.i64[0];
	}
	if (ACameraMetadata_getConstEntry(m, ACAMERA_CONTROL_AF_STATE, &e) == ACAMERA_OK && e.count > 0) {
		r->have_af_state = true;
		r->af_state = e.data.u8[0];
	}
	if (ACameraMetadata_getConstEntry(m, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count > 0) {
		r->have_focus_distance = true;
		r->focus_distance = e.data.f[0];
	}
	if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_EXPOSURE_TIME, &e) == ACAMERA_OK && e.count > 0) {
		r->have_exposure = true;
		r->exposure_ns = e.data.i64[0];
	}
	if (ACameraMetadata_getConstEntry(m, ACAMERA_SENSOR_SENSITIVITY, &e) == ACAMERA_OK && e.count > 0) {
		r->have_iso = true;
		r->iso = e.data.i32[0];
	}
	if (ACameraMetadata_getConstEntry(m, ACAMERA_LENS_STATE, &e) == ACAMERA_OK && e.count > 0) {
		r->have_lens_state = true;
		r->lens_state = e.data.u8[0];
	}
	if (ts != nullptr && *ts != 0) {
		r->have_timestamp = true;
		r->timestamp = *ts;
	}
}

#ifdef LEIA_SCAM_V8
/*!
 * ACaptureSessionLogicalCamera completion (HAL thread): the logical result
 * (focus distance; AF state with HAL AF) and both lenses' physical results
 * (exposure, ISO, LENS_STATE, SENSOR_TIMESTAMP). Also counts whether the two
 * lenses' stamps are ever different (leia_scam_eye_times_tracker).
 */
void
on_capture_completed(void *ctx,
                     ACameraCaptureSession *,
                     ACaptureRequest *,
                     const ACameraMetadata *result,
                     size_t n,
                     const char **ids,
                     const ACameraMetadata **phys)
{
	auto *c = (xrt_plugin_stereo_camera *)ctx;
	hal_rec rec;
	int64_t ts = 0;
	if (result != nullptr) {
		rec.have_logical = true;
		read_hal_result(result, &rec.logical, &ts);
	}
	for (size_t i = 0; i < n && ids != nullptr && phys != nullptr; i++) {
		if (ids[i] == nullptr || phys[i] == nullptr) {
			continue;
		}
		const bool is_left = strcmp(ids[i], c->left_id) == 0;
		const bool is_right = !is_left && (strcmp(ids[i], c->desc.main_id) == 0 || strcmp(ids[i], c->desc.sub_id) == 0);
		if (!is_left && !is_right) {
			continue;
		}
		int64_t pts = 0;
		read_hal_result(phys[i], is_left ? &rec.left : &rec.right, &pts);
		(is_left ? rec.have_left : rec.have_right) = true;
		if (ts == 0) {
			ts = pts;
		}
	}
	if (ts == 0) {
		return;
	}
	rec.ts = ts;
	std::lock_guard<std::mutex> lk(c->res_mu);
	if (rec.have_left && rec.have_right) {
		leia_scam_eye_times_observe(&c->eye_times, rec.left.timestamp, rec.right.timestamp);
	}
	c->res_ring[c->res_next] = rec;
	c->res_next = (c->res_next + 1) % kResultRing;
}

void
on_capture_failed(void *, ACameraCaptureSession *, ACaptureRequest *, ALogicalCameraCaptureFailure *)
{}
#endif

[[maybe_unused]] bool
lookup_result(xrt_plugin_stereo_camera *c, int64_t ts, hal_rec *out)
{
	std::lock_guard<std::mutex> lk(c->res_mu);
	for (const hal_rec &r : c->res_ring) {
		if (r.ts != 0 && std::llabs(r.ts - ts) <= kPairToleranceNs) {
			*out = r;
			return true;
		}
	}
	return false;
}

//! The source holds the lens itself (MANUAL / LOCKED in force).
[[maybe_unused]] bool
holding_focus(const xrt_plugin_stereo_camera *c)
{
	return (c->ctl.valid_controls & c->desc.ctl_caps.supported_controls & LEIA_SCAM_CTL_FOCUS_MODE) != 0 &&
	       (c->ctl.focus_mode == LEIA_SCAM_FOCUS_MANUAL || c->ctl.focus_mode == LEIA_SCAM_FOCUS_LOCKED);
}

/*!
 * The capture state of the pair at @p ts_boot (only what the HAL reported).
 * Per-eye times come from the two physical results, and only once they have
 * proved independent (stage 0: this HAL stamps both lenses with one
 * synthesized time, so they never do there).
 */
[[maybe_unused]] void
capture_state_of(xrt_plugin_stereo_camera *c, int64_t ts_boot, leia_scam_capture_state *out)
{
	hal_rec r;
	const bool have = lookup_result(c, ts_boot, &r);
	bool indep;
	{
		std::lock_guard<std::mutex> lk(c->res_mu);
		indep = leia_scam_eye_times_independent(&c->eye_times);
	}
	leia_scam_capture_state_from_hal(have && r.have_left ? &r.left : nullptr, have && r.have_right ? &r.right : nullptr,
	                                 have && r.have_logical ? &r.logical : nullptr, c->desc.fixed_focus,
	                                 c->desc.hal_af, holding_focus(c), indep, boot_minus_mono_ns(), out);
}

/*
 *
 * Requests and controls (spec v8 §5.6).
 *
 */

void
read_defaults(const ACaptureRequest *req, req_defaults *d)
{
	*d = req_defaults{};
	ACameraMetadata_const_entry e{};
	if (ACaptureRequest_getConstEntry(req, ACAMERA_CONTROL_AF_MODE, &e) == ACAMERA_OK && e.count > 0) {
		d->have_af_mode = true;
		d->af_mode = e.data.u8[0];
	}
	if (ACaptureRequest_getConstEntry(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, &e) == ACAMERA_OK && e.count > 0) {
		d->have_ae_comp = true;
		d->ae_comp = e.data.i32[0];
	}
	if (ACaptureRequest_getConstEntry(req, ACAMERA_LENS_FOCUS_DISTANCE, &e) == ACAMERA_OK && e.count > 0) {
		d->have_focus_distance = true;
		d->focus_distance = e.data.f[0];
	}
	if (ACaptureRequest_getConstEntry(req, ACAMERA_CONTROL_AF_REGIONS, &e) == ACAMERA_OK && e.count > 0) {
		d->af_regions.assign(e.data.i32, e.data.i32 + e.count);
	}
	if (ACaptureRequest_getConstEntry(req, ACAMERA_CONTROL_AE_REGIONS, &e) == ACAMERA_OK && e.count > 0) {
		d->ae_regions.assign(e.data.i32, e.data.i32 + e.count);
	}
}

//! Under cam_mu: the metering rectangle of c->ctl's point, in the running mode's frame.
[[maybe_unused]] void
update_region_locked(xrt_plugin_stereo_camera *c)
{
	c->region_ok = false;
	if ((c->ctl.valid_controls & LEIA_SCAM_CTL_METERING_POINT) == 0) {
		return;
	}
	const leia_scam_eye_geom g{c->cap_w, c->cap_h, c->crop_w, c->crop_h, c->rot};
	double nx = 0.0, ny = 0.0;
	if (leia_scam_metering_to_array(c->ctl.metering_x, c->ctl.metering_y, c->desc.array_w, c->desc.array_h, &g, &nx,
	                                &ny)) {
		leia_scam_metering_region(nx, ny, c->desc.log_aw, c->desc.log_ah, kMeteringFrac, c->region);
		c->region_ok = true;
	}
}

//! Write @p p into @p req (a default = @p d's value back, or the tag removed).
[[maybe_unused]] void
write_plan(ACaptureRequest *req, const req_defaults &d, const leia_scam_request_plan &p)
{
	auto put_u8 = [req](uint32_t tag, bool have, uint8_t v) {
		ACaptureRequest_setEntry_u8(req, tag, have ? 1 : 0, have ? &v : nullptr);
	};
	auto put_regions = [req](uint32_t tag, const int32_t *v, uint32_t n) {
		ACaptureRequest_setEntry_i32(req, tag, n, n > 0 ? v : nullptr);
	};
	if (p.af_mode_default) {
		put_u8(ACAMERA_CONTROL_AF_MODE, d.have_af_mode, d.af_mode);
	} else {
		put_u8(ACAMERA_CONTROL_AF_MODE, true, p.af_mode);
	}
	if (p.set_focus_distance) {
		ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, 1, &p.focus_distance);
	} else {
		ACaptureRequest_setEntry_float(req, ACAMERA_LENS_FOCUS_DISTANCE, d.have_focus_distance ? 1 : 0,
		                               d.have_focus_distance ? &d.focus_distance : nullptr);
	}
	if (!p.regions_default && p.set_af_regions) {
		put_regions(ACAMERA_CONTROL_AF_REGIONS, p.region, 5);
	} else {
		put_regions(ACAMERA_CONTROL_AF_REGIONS, d.af_regions.data(), (uint32_t)d.af_regions.size());
	}
	if (!p.regions_default && p.set_ae_regions) {
		put_regions(ACAMERA_CONTROL_AE_REGIONS, p.region, 5);
	} else {
		put_regions(ACAMERA_CONTROL_AE_REGIONS, d.ae_regions.data(), (uint32_t)d.ae_regions.size());
	}
	if (p.ae_comp_default) {
		ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, d.have_ae_comp ? 1 : 0,
		                             d.have_ae_comp ? &d.ae_comp : nullptr);
	} else {
		ACaptureRequest_setEntry_i32(req, ACAMERA_CONTROL_AE_EXPOSURE_COMPENSATION, 1, &p.ae_comp_index);
	}
	const uint8_t idle = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
	ACaptureRequest_setEntry_u8(req, ACAMERA_CONTROL_AF_TRIGGER, 1, &idle);
}

/*!
 * Under cam_mu: write the plan for c->ctl into the stream request (and the
 * still request, so a still carries the same focus and exposure). Without
 * HAL AF (the default for a pair), a lens that focuses always runs AF OFF at
 * one logical distance: the asked one, the current one (LOCKED), or
 * desc.default_focus_d when no focus control is set. Returns whether one AF
 * trigger is due (HAL AF only: AUTO_ONCE whose trigger moved, or after a
 * re-open with @p force_trigger).
 *
 * Built against pre-v8 runtime headers nothing is written: the request stays
 * the template, exactly as before v8.
 */
bool
apply_controls_to_request_locked(xrt_plugin_stereo_camera *c, bool force_trigger)
{
#ifdef LEIA_SCAM_V8
	if (c->req == nullptr) {
		return false;
	}
	const leia_scam_hal_caps hal = hal_caps_of(c->desc);
	leia_scam_focus_policy fp{};
	fp.hal_af = c->desc.hal_af;
	fp.default_d = c->desc.default_focus_d;
	fp.current_d = c->cmd_focus_d;
	leia_scam_request_plan p;
	leia_scam_plan_controls(&c->ctl, &c->desc.ctl_caps, &hal, &fp, c->region_ok, c->region, &c->trigger_seen, &p);
	if (p.set_focus_distance) {
		c->cmd_focus_d = p.focus_distance;
	}
	write_plan(c->req, c->tmpl, p);
	if (c->still_req != nullptr) {
		leia_scam_request_plan ps = p;
		ps.af_trigger_start = false;
		write_plan(c->still_req, c->still_tmpl, ps);
	}
	const bool once = !p.af_mode_default && p.af_mode == LEIA_SCAM_AF_AUTO &&
	                  c->ctl.focus_mode == LEIA_SCAM_FOCUS_AUTO_ONCE;
	return p.af_trigger_start || (force_trigger && once);
#else
	(void)c;
	(void)force_trigger;
	return false;
#endif
}

/*!
 * Under cam_mu, session running: (re)issue the repeating request, with ONE
 * AF_TRIGGER_START capture first when @p trigger (the trigger is a one-shot
 * in Camera2: the repeating request carries IDLE).
 */
camera_status_t
repeat_locked(xrt_plugin_stereo_camera *c, bool trigger)
{
#ifdef LEIA_SCAM_V8
	if (trigger) {
		const uint8_t start = ACAMERA_CONTROL_AF_TRIGGER_START;
		ACaptureRequest_setEntry_u8(c->req, ACAMERA_CONTROL_AF_TRIGGER, 1, &start);
		const camera_status_t st = ACameraCaptureSession_logicalCamera_capture(c->session, &c->cap_cb, 1, &c->req,
		                                                                       nullptr);
		const uint8_t idle = ACAMERA_CONTROL_AF_TRIGGER_IDLE;
		ACaptureRequest_setEntry_u8(c->req, ACAMERA_CONTROL_AF_TRIGGER, 1, &idle);
		if (st != ACAMERA_OK) {
			SCAM_LOG_W("leia stereo camera (android): AF trigger capture failed (%d)", (int)st);
		}
	}
	return ACameraCaptureSession_logicalCamera_setRepeatingRequest(c->session, &c->cap_cb, 1, &c->req, nullptr);
#else
	(void)trigger;
	return ACameraCaptureSession_setRepeatingRequest(c->session, nullptr, 1, &c->req, nullptr);
#endif
}

/*
 *
 * Stills from the JPEG outputs (spec v8 §5.7, rear pair).
 *
 */

//! JPEG reader callback: keep this lens's JPEG of the still in flight.
void
on_jpeg(xrt_plugin_stereo_camera *c, AImageReader *reader, int eye)
{
	for (;;) {
		AImage *img = nullptr;
		if (AImageReader_acquireNextImage(reader, &img) != AMEDIA_OK || img == nullptr) {
			return;
		}
		int64_t ts = 0;
		AImage_getTimestamp(img, &ts);
		uint8_t *data = nullptr;
		int len = 0;
		const bool ok = AImage_getPlaneData(img, 0, &data, &len) == AMEDIA_OK && data != nullptr && len > 0;
		{
			std::lock_guard<std::mutex> lk(c->pair_mu);
			if (ok && !c->closing.load() && reader == (eye == 0 ? c->rd_jl : c->rd_jr) &&
			    c->still == still_state::WAIT_JPEG && c->jpeg[eye].empty()) {
				c->jpeg[eye].assign(data, data + len);
				c->jpeg_ts[eye] = ts;
				if (!c->jpeg[1 - eye].empty()) {
					c->still = still_state::CAPTURED;
					c->still_cv.notify_all();
				}
			}
		}
		AImage_delete(img);
	}
}

void
on_jpeg_l(void *ctx, AImageReader *r)
{
	on_jpeg((xrt_plugin_stereo_camera *)ctx, r, 0);
}

void
on_jpeg_r(void *ctx, AImageReader *r)
{
	on_jpeg((xrt_plugin_stereo_camera *)ctx, r, 1);
}

/*
 *
 * Capture sessions.
 *
 */

//! The objects of one capture session (and its two readers).
struct session_objs
{
	ACameraCaptureSession *session{nullptr};
	ACaptureRequest *req{nullptr};
	ACaptureSessionOutputContainer *outputs{nullptr};
	ACaptureSessionOutput *out_l{nullptr}, *out_r{nullptr};
	ACameraOutputTarget *tgt_l{nullptr}, *tgt_r{nullptr};
	AImageReader *rd_l{nullptr}, *rd_r{nullptr};
	// The still outputs (rear): one JPEG reader per lens + the still request.
	ACaptureRequest *still_req{nullptr};
	ACaptureSessionOutput *out_jl{nullptr}, *out_jr{nullptr};
	ACameraOutputTarget *stgt_l{nullptr}, *stgt_r{nullptr}, *stgt_jl{nullptr}, *stgt_jr{nullptr};
	AImageReader *rd_jl{nullptr}, *rd_jr{nullptr};
};

//! Under cam_mu: move the session objects out of @p c (the readers stay
//! installed until the caller swaps them under pair_mu).
session_objs
take_session_locked(xrt_plugin_stereo_camera *c)
{
	session_objs o;
	o.session = c->session;
	o.req = c->req;
	o.outputs = c->outputs;
	o.out_l = c->out_l;
	o.out_r = c->out_r;
	o.tgt_l = c->tgt_l;
	o.tgt_r = c->tgt_r;
	o.rd_l = c->rd_l;
	o.rd_r = c->rd_r;
	o.still_req = c->still_req;
	o.out_jl = c->out_jl;
	o.out_jr = c->out_jr;
	o.stgt_l = c->stgt_l;
	o.stgt_r = c->stgt_r;
	o.stgt_jl = c->stgt_jl;
	o.stgt_jr = c->stgt_jr;
	o.rd_jl = c->rd_jl;
	o.rd_jr = c->rd_jr;
	c->session = nullptr;
	c->req = nullptr;
	c->still_req = nullptr;
	c->outputs = nullptr;
	c->out_l = c->out_r = c->out_jl = c->out_jr = nullptr;
	c->tgt_l = c->tgt_r = nullptr;
	c->stgt_l = c->stgt_r = c->stgt_jl = c->stgt_jr = nullptr;
	return o;
}

//! Free @p o. Its session must be stopped (or replaced) and its readers no
//! longer installed in the camera (on_eye drops what they still deliver).
void
free_session_objs(session_objs &o)
{
	if (o.session != nullptr) {
		ACameraCaptureSession_close(o.session);
		o.session = nullptr;
	}
	for (ACaptureRequest **q : {&o.req, &o.still_req}) {
		if (*q != nullptr) {
			ACaptureRequest_free(*q);
			*q = nullptr;
		}
	}
	for (ACameraOutputTarget **t : {&o.tgt_l, &o.tgt_r, &o.stgt_l, &o.stgt_r, &o.stgt_jl, &o.stgt_jr}) {
		if (*t != nullptr) {
			ACameraOutputTarget_free(*t);
			*t = nullptr;
		}
	}
	if (o.outputs != nullptr) {
		for (ACaptureSessionOutput **x : {&o.out_l, &o.out_r, &o.out_jl, &o.out_jr}) {
			if (*x != nullptr) {
				ACaptureSessionOutputContainer_remove(o.outputs, *x);
				ACaptureSessionOutput_free(*x);
				*x = nullptr;
			}
		}
		ACaptureSessionOutputContainer_free(o.outputs);
		o.outputs = nullptr;
	}
	for (AImageReader **r : {&o.rd_l, &o.rd_r, &o.rd_jl, &o.rd_jr}) {
		if (*r != nullptr) {
			AImageReader_delete(*r); // joins its callback thread
			*r = nullptr;
		}
	}
}

//! Under cam_mu, then pair_mu: drop the pending images and install the readers.
void
swap_readers_locked(xrt_plugin_stereo_camera *c,
                    AImageReader *l,
                    AImageReader *r,
                    AImageReader *jl,
                    AImageReader *jr,
                    uint32_t cap_w,
                    uint32_t cap_h)
{
	std::lock_guard<std::mutex> lk(c->pair_mu);
	c->rd_jl = jl;
	c->rd_jr = jr;
	for (auto &q : c->pend) {
		for (auto &p : q) {
			AImage_delete(p.img);
		}
		q.clear();
	}
	c->rd_l = l;
	c->rd_r = r;
	c->sess_cap_w = cap_w;
	c->sess_cap_h = cap_h;
}

//! Under cam_mu. Idempotent.
void
teardown_locked(xrt_plugin_stereo_camera *c)
{
	session_objs o = take_session_locked(c);
	if (o.session != nullptr) {
		ACameraCaptureSession_stopRepeating(o.session);
		ACameraCaptureSession_close(o.session);
		o.session = nullptr;
	}
	if (c->dev != nullptr) {
		ACameraDevice_close(c->dev); // waits for in-flight callbacks
		c->dev = nullptr;
	}
	swap_readers_locked(c, nullptr, nullptr, nullptr, nullptr, 0, 0);
	free_session_objs(o);
}

bool
make_reader(xrt_plugin_stereo_camera *c,
            uint32_t w,
            uint32_t h,
            int max_images,
            AImageReader_ImageCallback cb,
            AImageReader **out,
            int32_t format = AIMAGE_FORMAT_YUV_420_888)
{
	media_status_t s = AImageReader_new((int32_t)w, (int32_t)h, format, max_images, out);
	if (s != AMEDIA_OK || *out == nullptr) {
		SCAM_LOG_W("leia stereo camera (android): AImageReader %ux%u format 0x%x failed (%d)", w, h, (unsigned)format,
		           (int)s);
		*out = nullptr;
		return false;
	}
	AImageReader_ImageListener l{c, cb};
	AImageReader_setImageListener(*out, &l);
	return true;
}

/*!
 * One physical output for @p rd on lens @p phys, a target of it in @p req (the
 * stream request for the stream outputs, NULL for the JPEG outputs, which only
 * the still request targets) and, when @p still_tgt is non-NULL, a second
 * target of it in the still request (the still request also targets the
 * stream outputs, so the stream keeps every frame, stage 0 §4b).
 */
bool
add_output(xrt_plugin_stereo_camera *c,
           AImageReader *rd,
           const char *phys,
           ACaptureSessionOutput **out,
           ACaptureRequest *req,
           ACameraOutputTarget **tgt,
           ACameraOutputTarget **still_tgt)
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
	if (req != nullptr && (ACameraOutputTarget_create(win, tgt) != ACAMERA_OK ||
	                       ACaptureRequest_addTarget(req, *tgt) != ACAMERA_OK)) {
		return false;
	}
	if (still_tgt != nullptr && c->still_req != nullptr &&
	    (ACameraOutputTarget_create(win, still_tgt) != ACAMERA_OK ||
	     ACaptureRequest_addTarget(c->still_req, *still_tgt) != ACAMERA_OK)) {
		return false;
	}
	return true;
}

/*!
 * Under cam_mu, device open: build a capture session of ONE YUV_420_888
 * output per physical lens at @p cap_w x @p cap_h (the HAL aborts on a second
 * YUV output on a lens, so a different size is always a new session, never an
 * added stream), plus, when the camera lists a still from the JPEG output
 * (desc.still_out, the rear pair), ONE JPEG output per lens at that still's
 * size (stage 0 §4b: YUV stream + JPEG still per lens is accepted by the HAL
 * and a still request on it loses no stream frame). The stream request
 * targets the YUV outputs only; the still request (TEMPLATE_ZERO_SHUTTER_LAG,
 * or TEMPLATE_PREVIEW with debug.dxr.leia.scam_still_template=preview; never
 * STILL_CAPTURE, which freezes the stream ~400 ms) targets all four. Then the
 * repeating request starts with the controls in force.
 *
 * When a session already runs this is a RECONFIGURE (spec v8 set_mode): the
 * device stays open, the old session's repeating request stops, the new
 * readers are installed (the old ones' images are dropped from then on), and
 * creating the new session makes the camera device close the old one and
 * reconfigure its streams (stage 0: 86-124 ms); the old objects are freed
 * after. @p force_trigger: see apply_controls_to_request_locked. False = no
 * session runs now (everything freed); the caller decides what to rebuild.
 */
bool
build_session_locked(xrt_plugin_stereo_camera *c, uint32_t cap_w, uint32_t cap_h, bool force_trigger)
{
	const char *left = c->lsub ? c->desc.sub_id : c->desc.main_id;
	const char *right = c->lsub ? c->desc.main_id : c->desc.sub_id;
	const int max_images = (uint64_t)cap_w * cap_h >= kLargeCapturePixels ? kEyeMaxImagesLarge : kEyeMaxImages;
	const leia_scam_mode *sm = c->desc.still_out >= 0 ? &c->desc.modes[c->desc.still_out] : nullptr;
	session_objs old = take_session_locked(c);
	AImageReader *nl = nullptr, *nr = nullptr, *njl = nullptr, *njr = nullptr;
	bool ok = c->dev != nullptr && make_reader(c, cap_w, cap_h, max_images, on_eye_l, &nl) &&
	          make_reader(c, cap_w, cap_h, max_images, on_eye_r, &nr);
	if (ok && sm != nullptr) {
		ok = make_reader(c, sm->geom.cap_w, sm->geom.cap_h, kJpegMaxImages, on_jpeg_l, &njl, AIMAGE_FORMAT_JPEG) &&
		     make_reader(c, sm->geom.cap_w, sm->geom.cap_h, kJpegMaxImages, on_jpeg_r, &njr, AIMAGE_FORMAT_JPEG);
	}
	ok = ok && ACaptureSessionOutputContainer_create(&c->outputs) == ACAMERA_OK &&
	     ACameraDevice_createCaptureRequest(c->dev, TEMPLATE_PREVIEW, &c->req) == ACAMERA_OK;
	if (ok && sm != nullptr) {
		const bool preview = prop_str("debug.dxr.leia.scam_still_template", "zsl") == "preview";
		ok = ACameraDevice_createCaptureRequest(c->dev, preview ? TEMPLATE_PREVIEW : TEMPLATE_ZERO_SHUTTER_LAG,
		                                        &c->still_req) == ACAMERA_OK;
	}
	if (ok) {
		read_defaults(c->req, &c->tmpl);
		if (c->still_req != nullptr) {
			read_defaults(c->still_req, &c->still_tmpl);
		}
		ok = add_output(c, nl, left, &c->out_l, c->req, &c->tgt_l, &c->stgt_l) &&
		     add_output(c, nr, right, &c->out_r, c->req, &c->tgt_r, &c->stgt_r);
		if (ok && sm != nullptr) {
			ok = add_output(c, njl, left, &c->out_jl, nullptr, nullptr, &c->stgt_jl) &&
			     add_output(c, njr, right, &c->out_jr, nullptr, nullptr, &c->stgt_jr);
		}
		if (!ok) {
			SCAM_LOG_W("leia stereo camera (android): could not build the outputs");
		}
	}
	if (ok) {
		const int32_t fps[2] = {30, 30};
		ACaptureRequest_setEntry_i32(c->req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps);
		if (c->still_req != nullptr) {
			// The still request runs inside the stream: same rate, upright-agnostic
			// JPEG (no EXIF turn: the eyes are published unturned), quality 95.
			ACaptureRequest_setEntry_i32(c->still_req, ACAMERA_CONTROL_AE_TARGET_FPS_RANGE, 2, fps);
			const int32_t orient = 0;
			ACaptureRequest_setEntry_i32(c->still_req, ACAMERA_JPEG_ORIENTATION, 1, &orient);
			uint8_t q = 95;
			const int qp = atoi(prop_str("debug.dxr.leia.scam_still_quality", "95").c_str());
			if (qp >= 50 && qp <= 100) {
				q = (uint8_t)qp;
			}
			ACaptureRequest_setEntry_u8(c->still_req, ACAMERA_JPEG_QUALITY, 1, &q);
		}
	}
	const bool trigger = ok && apply_controls_to_request_locked(c, force_trigger);
	if (old.session != nullptr) {
		ACameraCaptureSession_stopRepeating(old.session);
	}
	swap_readers_locked(c, ok ? nl : nullptr, ok ? nr : nullptr, ok ? njl : nullptr, ok ? njr : nullptr,
	                    ok ? cap_w : 0, ok ? cap_h : 0);
	if (ok) {
		c->ses_cb = {c, ses_noop, ses_noop, ses_noop};
		const camera_status_t st = ACameraDevice_createCaptureSessionWithSessionParameters(
		    c->dev, c->outputs, c->req, &c->ses_cb, &c->session);
		if (st != ACAMERA_OK || c->session == nullptr) {
			SCAM_LOG_W("leia stereo camera (android): createCaptureSession (%ux%u YUV per lens%s) failed (%d)",
			           cap_w, cap_h, sm != nullptr ? " + JPEG still output" : "", (int)st);
			c->session = nullptr;
			ok = false;
		}
	}
	if (ok) {
		const camera_status_t st = repeat_locked(c, trigger);
		if (st != ACAMERA_OK) {
			SCAM_LOG_W("leia stereo camera (android): setRepeatingRequest failed (%d)", (int)st);
			ok = false;
		}
	}
	free_session_objs(old); // the device already closed the old session when the new one was created
	if (!ok) {
		session_objs mine = take_session_locked(c);
		if (mine.session != nullptr) {
			ACameraCaptureSession_stopRepeating(mine.session);
		}
		swap_readers_locked(c, nullptr, nullptr, nullptr, nullptr, 0, 0);
		mine.rd_l = nl;
		mine.rd_r = nr;
		mine.rd_jl = njl;
		mine.rd_jr = njr;
		free_session_objs(mine);
	}
	return ok;
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

//! Under cam_mu: open the device and start the repeating request of the running mode.
bool
start_locked(xrt_plugin_stereo_camera *c)
{
	teardown_locked(c);
	c->device_failed = false;
	c->device_error = 0;
	const char *left = c->lsub ? c->desc.sub_id : c->desc.main_id;
	const char *right = c->lsub ? c->desc.main_id : c->desc.sub_id;

	// Into in-app tracking first, so the tracking service lets go of its lens;
	// back out if the open does not get through. Not after a refused open while
	// both lenses are free: while the camera service refuses this process
	// ("disabled by policy", the first seconds after the consumer's app comes
	// forward) it refuses the tracking service's camera too, so there is
	// nothing to release, and handing over on every retry only churns tracking
	// (an engine switch and a restart of the tracking service each way). Once
	// the tracking service holds a lens again, hand over first as usual.
	const bool hand_first = !c->open_refused || !wait_lenses_free(c->desc, 0, c->closing).empty();
	if (hand_first) {
		handover_locked(c, true);
	}
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
		if (hand_first) {
			SCAM_LOG_W("leia stereo camera (android): lenses %s + %s free after %.1f ms", c->desc.main_id,
			           c->desc.sub_id, (double)(clock_ns(CLOCK_MONOTONIC) - t0) / 1e6);
		}
	}
	c->dev_cb = {c, dev_disconnected, dev_error};
	camera_status_t st = ACameraManager_openCamera(mgr(), c->desc.logical_id, &c->dev_cb, &c->dev);
	if (st != ACAMERA_OK || c->dev == nullptr) {
		const bool refused = st == ACAMERA_ERROR_CAMERA_DISABLED;
		if (refused && !c->open_refused) {
			SCAM_LOG_W("leia stereo camera (android): openCamera(%s) refused by policy (%d); tracking stays with "
			           "the tracking service until an open gets through",
			           c->desc.logical_id, (int)st);
		} else if (!refused) {
			SCAM_LOG_W("leia stereo camera (android): openCamera(%s) failed (%d)", c->desc.logical_id, (int)st);
		}
		c->open_refused = refused;
		c->dev = nullptr;
		teardown_locked(c);
		handover_locked(c, false);
		return false;
	}
	if (c->open_refused) {
		SCAM_LOG_W("leia stereo camera (android): openCamera(%s) let through after refusals", c->desc.logical_id);
		c->open_refused = false;
	}
	if (!hand_first) {
		handover_locked(c, true);
	}
	// The device was (re)opened: its lens position is gone, so an AUTO_ONCE in
	// force searches again (force_trigger).
	if (!build_session_locked(c, c->cap_w, c->cap_h, true)) {
		teardown_locked(c);
		handover_locked(c, false);
		return false;
	}
	c->opened_ns = clock_ns(CLOCK_MONOTONIC);
	c->last_pair_ns.store(0);
	SCAM_LOG_W("leia stereo camera (android): %s pair streaming: logical %s, L=%s R=%s, %ux%u YUV per lens -> %ux%u "
	           "eyes (mode %u, turned %u deg cw), tracking %s",
	           c->desc.front ? "front" : "rear", c->desc.logical_id, left, right, c->cap_w, c->cap_h, c->w, c->h,
	           c->mode_index, c->rot, c->track ? "640x480 from the tracking lens (unturned)" : "not handed over");
	return true;
}

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_STILL
/*
 * AImageDecoder (NDK, API 30+) decodes the still's JPEGs. The plug-in is built
 * for API 29 (the runtime's minSdk), so it is looked up at run time in
 * libjnigraphics.so; on an API 29 system the JPEG still fails (FAILED) and
 * the stills of the default capture still work.
 */
struct jpeg_decoder_api
{
	bool ok{false};
	int (*create)(const void *, size_t, AImageDecoder **){nullptr};
	int (*set_format)(AImageDecoder *, int32_t){nullptr};
	const AImageDecoderHeaderInfo *(*header)(const AImageDecoder *){nullptr};
	int32_t (*width)(const AImageDecoderHeaderInfo *){nullptr};
	int32_t (*height)(const AImageDecoderHeaderInfo *){nullptr};
	size_t (*stride)(AImageDecoder *){nullptr};
	int (*decode)(AImageDecoder *, void *, size_t, size_t){nullptr};
	void (*destroy)(AImageDecoder *){nullptr};
};

const jpeg_decoder_api &
jpeg_decoder()
{
	static jpeg_decoder_api api;
	static std::once_flag once;
	std::call_once(once, [] {
		void *h = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
		if (h == nullptr) {
			return;
		}
		api.create = (decltype(api.create))dlsym(h, "AImageDecoder_createFromBuffer");
		api.set_format = (decltype(api.set_format))dlsym(h, "AImageDecoder_setAndroidBitmapFormat");
		api.header = (decltype(api.header))dlsym(h, "AImageDecoder_getHeaderInfo");
		api.width = (decltype(api.width))dlsym(h, "AImageDecoderHeaderInfo_getWidth");
		api.height = (decltype(api.height))dlsym(h, "AImageDecoderHeaderInfo_getHeight");
		api.stride = (decltype(api.stride))dlsym(h, "AImageDecoder_getMinimumStride");
		api.decode = (decltype(api.decode))dlsym(h, "AImageDecoder_decodeImage");
		api.destroy = (decltype(api.destroy))dlsym(h, "AImageDecoder_delete");
		api.ok = api.create && api.set_format && api.header && api.width && api.height && api.stride &&
		         api.decode && api.destroy;
	});
	return api;
}

/*!
 * One eye's JPEG (@p cap_w x @p cap_h, unturned: JPEG_ORIENTATION 0) into
 * the SBS NV12 still at column @p x0 (stride 2 * @p ew): decode to RGBA, keep
 * the centred @p ew x @p eh, convert to full-range BT.601 YCbCr (what the
 * camera's own YUV_420_888 is), chroma from each 2x2 block. @p rgba is
 * scratch, reused across eyes. False (with @p why) on any failure.
 */
bool
jpeg_eye_to_nv12(const std::vector<uint8_t> &jpeg,
                 uint32_t cap_w,
                 uint32_t cap_h,
                 uint32_t ew,
                 uint32_t eh,
                 uint8_t *dst,
                 uint32_t x0,
                 std::vector<uint8_t> &rgba,
                 std::string &why)
{
	const jpeg_decoder_api &api = jpeg_decoder();
	if (!api.ok) {
		why = "no AImageDecoder (libjnigraphics, API 30+)";
		return false;
	}
	AImageDecoder *d = nullptr;
	if (api.create(jpeg.data(), jpeg.size(), &d) != ANDROID_IMAGE_DECODER_SUCCESS || d == nullptr) {
		why = "AImageDecoder_createFromBuffer failed";
		return false;
	}
	bool ok = false;
	do {
		if (api.set_format(d, ANDROID_BITMAP_FORMAT_RGBA_8888) != ANDROID_IMAGE_DECODER_SUCCESS) {
			why = "RGBA_8888 refused";
			break;
		}
		const AImageDecoderHeaderInfo *hi = api.header(d);
		const int32_t w = hi != nullptr ? api.width(hi) : 0, h = hi != nullptr ? api.height(hi) : 0;
		if ((uint32_t)w != cap_w || (uint32_t)h != cap_h) {
			char b[128];
			snprintf(b, sizeof(b), "JPEG is %dx%d, the output is %ux%u (EXIF turn?)", w, h, cap_w, cap_h);
			why = b;
			break;
		}
		const size_t stride = api.stride(d);
		rgba.resize(stride * (size_t)h);
		if (api.decode(d, rgba.data(), stride, rgba.size()) != ANDROID_IMAGE_DECODER_SUCCESS) {
			why = "AImageDecoder_decodeImage failed";
			break;
		}
		const uint32_t ox = ((cap_w - ew) / 2u) & ~1u, oy = ((cap_h - eh) / 2u) & ~1u;
		const size_t ds = (size_t)2 * ew;
		uint8_t *uv = dst + ds * eh;
		for (uint32_t y = 0; y < eh; y += 2) {
			const uint8_t *r0 = rgba.data() + (size_t)(y + oy) * stride + (size_t)ox * 4;
			const uint8_t *r1 = r0 + stride;
			uint8_t *y0 = dst + (size_t)y * ds + x0, *y1 = y0 + ds;
			uint8_t *c = uv + (size_t)(y / 2) * ds + x0;
			for (uint32_t x = 0; x < ew; x += 2) {
				int sr = 0, sg = 0, sb = 0;
				const uint8_t *px[4] = {r0 + 4 * x, r0 + 4 * x + 4, r1 + 4 * x, r1 + 4 * x + 4};
				uint8_t *py[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
				for (int k = 0; k < 4; k++) {
					const int R = px[k][0], G = px[k][1], B = px[k][2];
					*py[k] = (uint8_t)((77 * R + 150 * G + 29 * B + 128) >> 8);
					sr += R;
					sg += G;
					sb += B;
				}
				// Averages, then Cb / Cr with a +128 offset folded in (no negative shifts).
				const int cb = (-43 * sr - 85 * sg + 128 * sb + 4 * 32768 + 512) >> 10;
				const int cr = (128 * sr - 107 * sg - 21 * sb + 4 * 32768 + 512) >> 10;
				c[x] = (uint8_t)std::min(255, std::max(0, cb));
				c[x + 1] = (uint8_t)std::min(255, std::max(0, cr));
			}
		}
		ok = true;
	} while (false);
	api.destroy(d);
	return ok;
}

/*!
 * The still worker (one per request; the camera thread never waits for it).
 *
 * - A still of the default capture (LEIA_SCAM_SRC_DEFAULT_CAPTURE): the next
 *   stream pair is packed at the still's crop (grab_still_locked). No session
 *   change, no frame lost, ~1 frame.
 * - A still from the JPEG outputs (rear): ONE still request (ZSL / preview
 *   template, targeting the stream outputs too) on the RUNNING session; stage
 *   0 measured both JPEGs ~320 ms later with no stream frame lost. Each eye is
 *   then decoded and converted to NV12 here, one after the other (peak memory:
 *   one RGBA eye, ~50 MB at 4096x3072, beside the 38 MB NV12 pair and the two
 *   JPEGs).
 */
void
still_worker(xrt_plugin_stereo_camera *c, leia_scam_mode m, uint64_t request_id)
{
	const int64_t t0 = clock_ns(CLOCK_MONOTONIC);
	const bool jpeg = m.source == LEIA_SCAM_SRC_STILL_OUTPUT;
	std::string why;
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		c->still_geom = m.geom;
		c->jpeg[0].clear();
		c->jpeg[1].clear();
		c->still = jpeg ? still_state::WAIT_JPEG : still_state::GRAB;
		c->still_lossy = jpeg;
	}
	if (jpeg) {
		std::lock_guard<std::mutex> lk(c->cam_mu);
		if (c->session == nullptr || c->still_req == nullptr) {
			why = "the source is not streaming";
		} else {
			const camera_status_t st =
			    ACameraCaptureSession_logicalCamera_capture(c->session, &c->cap_cb, 1, &c->still_req, nullptr);
			if (st != ACAMERA_OK) {
				why = "the still request was refused (" + std::to_string((int)st) + ")";
			}
		}
	}
	int64_t t_pair = t0;
	if (why.empty()) {
		std::unique_lock<std::mutex> lk(c->pair_mu);
		const int64_t deadline = t0 + (jpeg ? kStillJpegWaitNs : kStillSameCaptureNs);
		for (;;) {
			const int64_t now = clock_ns(CLOCK_MONOTONIC);
			if (c->still == still_state::CAPTURED || c->closing.load() || now >= deadline) {
				break;
			}
			c->still_cv.wait_for(lk, std::chrono::nanoseconds(std::min<int64_t>(deadline - now, 100 * kNsPerMs)));
		}
		if (c->still != still_state::CAPTURED) {
			why = c->closing.load() ? "the camera closed"
			      : jpeg            ? "no JPEG pair from the HAL within 3 s"
			                        : "no stream pair within 1 s";
		}
		t_pair = clock_ns(CLOCK_MONOTONIC);
	}
	if (why.empty() && jpeg) {
		std::vector<uint8_t> jl, jr;
		int64_t tl = 0, tr = 0;
		{
			std::lock_guard<std::mutex> lk(c->pair_mu);
			jl.swap(c->jpeg[0]);
			jr.swap(c->jpeg[1]);
			tl = c->jpeg_ts[0];
			tr = c->jpeg_ts[1];
		}
		if (std::llabs(tl - tr) > kPairToleranceNs) {
			SCAM_LOG_W("leia stereo camera (android): still %llu: the two JPEGs' timestamps differ by %.1f ms",
			           (unsigned long long)request_id, (double)(tl - tr) / 1e6);
		}
		uint32_t ew, eh;
		leia_scam_eye_size(&m.geom, &ew, &eh);
		// still_buf is the worker's alone in CAPTURED (nothing packs into it).
		c->still_buf.assign((size_t)2 * ew * eh * 3 / 2, 128);
		std::vector<uint8_t> rgba;
		if (!jpeg_eye_to_nv12(jl, m.geom.cap_w, m.geom.cap_h, ew, eh, c->still_buf.data(), 0, rgba, why) ||
		    !jpeg_eye_to_nv12(jr, m.geom.cap_w, m.geom.cap_h, ew, eh, c->still_buf.data(), ew, rgba, why)) {
			c->still_buf.clear();
			c->still_buf.shrink_to_fit();
		} else {
			const int64_t off = boot_minus_mono_ns();
			c->still_w = ew;
			c->still_h = eh;
			c->still_ts_boot = tl;
			c->still_eye_mono[0] = tl - off;
			c->still_eye_mono[1] = tr - off;
		}
	}
	if (why.empty()) {
		std::lock_guard<std::mutex> lk(c->cam_mu); // c->ctl
		capture_state_of(c, c->still_ts_boot, &c->still_res);
	}
	const int64_t t_end = clock_ns(CLOCK_MONOTONIC);
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		c->still = why.empty() ? still_state::READY : still_state::FAILED;
	}
	if (why.empty()) {
		SCAM_LOG_W("leia stereo camera (android): still %llu: %ux%u per eye from %s in %.0f ms (pair after %.0f ms, "
		           "decode + NV12 %.0f ms); the stream lost no frame for it",
		           (unsigned long long)request_id, c->still_w, c->still_h,
		           jpeg ? "the JPEG outputs" : "the next stream pair", (double)(t_end - t0) / 1e6,
		           (double)(t_pair - t0) / 1e6, (double)(t_end - t_pair) / 1e6);
	} else {
		SCAM_LOG_W("leia stereo camera (android): still %llu (%ux%u per eye) FAILED after %.0f ms: %s",
		           (unsigned long long)request_id, m.eye_w, m.eye_h, (double)(t_end - t0) / 1e6, why.c_str());
	}
}
#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_STILL

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
		// A refused open no longer hands tracking over (start_locked), so it
		// costs a few ms: poll the refusal fast to catch the moment it lifts.
		const bool fast = now - c->down_since_ns < 10000 * kNsPerMs;
		const int64_t cap = fast ? kReopenFastMaxNs : kReopenMaxNs;
		c->backoff_ns = fast && c->open_refused ? kReopenRefusedNs : std::min(c->backoff_ns * 2, cap);
		c->next_reopen_ns = now + c->backoff_ns;
	}
	return false;
}


/*!
 * Spec v8 §9b: the capture session starts on the first wait_frame or
 * set_mode, not at open (a set_mode right after open then costs nothing).
 * A failed start hands over to the watchdog, exactly as a failed open did.
 */
void
ensure_started(xrt_plugin_stereo_camera *c)
{
	if (c->started) {
		return;
	}
	c->started = true;
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
	refresh_user_calib_locked();
	// #505: nothing is listed while a factory block is still being read.
	const uint32_t count = any_pending_locked() ? 0u : (uint32_t)g_pairs.size();
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
	if (out == nullptr || index >= g_pairs.size() || any_pending_locked() || !g_pairs[index].calibrated) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const pair_desc &p = g_pairs[index];
	leia_scam_stream_calib s = p.stream;
	if (left_is_sub(p) != s.left_is_sub) {
		// A forced swap: read the block in the other convention, so the halves
		// and R/T still agree exactly.
		leia_scam_factory_calib f = oriented_factory(p);
		leia_scam_factory_swap_convention(&f);
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
		if (index >= g_pairs.size() || any_pending_locked()) {
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
	c->mode_index = 0;
	c->cmd_focus_d = c->desc.default_focus_d;
	snprintf(c->left_id, sizeof(c->left_id), "%s", c->lsub ? c->desc.sub_id : c->desc.main_id);
#ifdef LEIA_SCAM_V8
	c->cap_cb.context = c;
	c->cap_cb.onLogicalCameraCaptureCompleted = on_capture_completed;
	c->cap_cb.onLogicalCameraCaptureFailed = on_capture_failed;
#endif
	c->track = c->desc.front && prop_bool("debug.dxr.leia.scam_track", true);

	if (c->track) {
		// The intrinsics of the lens fed to this process's cores once tracking is
		// handed over (start_locked, right before the camera is taken from the
		// tracking service).
		// The main lens's capture, then crop_track_from_eye's centre 4:3 region
		// scaled to 640x480. With a 4:3 capture that is the whole array, i.e.
		// the tracking service's own 640x480 intrinsics.
		pick_tracking_lens(c);
		c->trk_thread = std::thread(tracking_feeder, c);
	}

	// Spec v8 §9b: the capture session is configured lazily, on the first
	// wait_frame or set_mode (ensure_started), so a set_mode right after open
	// opens straight into the wanted mode.
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
	ensure_started(c);
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
				// The buffer's own size: after a mode switch the next buffer may
				// still be of the previous mode (the runtime sizes from the frame).
				out->width = 2 * b.eye_w;
				out->height = b.eye_h;
				out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
				out->planes[0] = b.data.data();
				out->planes[1] = b.data.data() + (size_t)2 * b.eye_w * b.eye_h;
				out->pitches[0] = 2 * b.eye_w;
				out->pitches[1] = 2 * b.eye_w;
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
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		c->still_cv.notify_all();
	}
	if (c->still_thread.joinable()) {
		c->still_thread.join(); // bounded: it sees closing within 100 ms (or finishes a decode)
	}
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

/*
 *
 * Spec v8 slots (XR_DXR_stereo_camera §5.5-§5.7, §9b). Each group only when
 * the runtime headers announce it.
 *
 */

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES
static_assert(LEIA_SCAM_MODE_STREAM == XRT_PLUGIN_STEREO_CAMERA_MODE_STREAM, "mode bits");
static_assert(LEIA_SCAM_MODE_STILL == XRT_PLUGIN_STEREO_CAMERA_MODE_STILL, "mode bits");

namespace {

//! The listed rate of @p m: the measured rate of its capture, else 0 (never the HAL's claimed 30).
float
mode_rate(const pair_desc &p, const leia_scam_mode &m)
{
	switch (m.source) {
	case LEIA_SCAM_SRC_DEFAULT_CAPTURE: return p.measured_rate;
	case LEIA_SCAM_SRC_OWN_CAPTURE: return p.measured_own_rate;
	default: return 0.0f;
	}
}

} // namespace

extern "C" uint32_t
leia_stereo_camera_android_enumerate_modes(struct xrt_plugin_instance *inst,
                                           uint32_t index,
                                           uint32_t capacity,
                                           struct xrt_plugin_stereo_camera_mode *out)
{
	(void)inst;
	std::lock_guard<std::mutex> lk(g_mu);
	scan_locked();
	if (any_pending_locked() || index >= g_pairs.size()) {
		return 0;
	}
	const pair_desc &p = g_pairs[index];
	for (uint32_t i = 0; out != nullptr && i < p.n_modes && i < capacity; i++) {
		xrt_plugin_stereo_camera_mode m;
		memset(&m, 0, sizeof(m));
		m.struct_size = (uint32_t)sizeof(m);
		m.eye_width = p.modes[i].eye_w;
		m.eye_height = p.modes[i].eye_h;
		m.max_frame_rate = mode_rate(p, p.modes[i]);
		m.flags = p.modes[i].flags;
		const uint32_t sz = out[i].struct_size;
		memcpy(&out[i], &m, std::min<size_t>(sz, sizeof(m))); // never past struct_size
		out[i].struct_size = sz;
	}
	return p.n_modes;
}

/*!
 * Reconfigure the open camera to mode @p mode_index. A cut of the running
 * capture (mode 0 <-> 1280x960, 1920x1080 <-> 1920x1440) only changes the crop:
 * no frame and no tracking frame is lost. Another capture (rear only) is a
 * session reconfigure on the open device: stage 0 measured 86-124 ms to
 * configure plus ~85 ms to the first frame, inside the contract's 500 ms. The
 * front pair lists no mode that needs one. A failure rebuilds the previous
 * capture; if even that fails the watchdog re-opens it.
 */
extern "C" xrt_result_t
leia_stereo_camera_android_set_mode(struct xrt_plugin_stereo_camera *c, uint32_t mode_index)
{
	if (c == nullptr || mode_index >= c->desc.n_modes ||
	    (c->desc.modes[mode_index].flags & LEIA_SCAM_MODE_STREAM) == 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const leia_scam_mode &m = c->desc.modes[mode_index];
	if (mode_index == c->mode_index) {
		return XRT_SUCCESS;
	}
	auto set_geom = [c](const leia_scam_eye_geom &g, uint32_t idx) {
		std::lock_guard<std::mutex> lk(c->pair_mu);
		c->cap_w = g.cap_w;
		c->cap_h = g.cap_h;
		c->crop_w = g.crop_w;
		c->crop_h = g.crop_h;
		c->rot = g.rot;
		leia_scam_eye_size(&g, &c->w, &c->h);
		c->mode_index = idx;
		c->rate_n = 0; // the rate window restarts with the capture
	};
	const leia_scam_eye_geom old{c->cap_w, c->cap_h, c->crop_w, c->crop_h, c->rot};
	const uint32_t old_index = c->mode_index;
	const bool same_capture = m.geom.cap_w == c->cap_w && m.geom.cap_h == c->cap_h;
	std::lock_guard<std::mutex> lk(c->cam_mu);
	if (same_capture || !c->started || c->session == nullptr) {
		// A new crop of the running capture, or not streaming yet: the next
		// start (lazy, or the watchdog's) builds the session in this mode.
		set_geom(m.geom, mode_index);
		SCAM_LOG_W("leia stereo camera (android): %s pair mode %u = %ux%u per eye (%s)",
		           c->desc.front ? "front" : "rear", mode_index, m.eye_w, m.eye_h,
		           same_capture ? "same capture, new crop: no frame lost" : "before the session starts");
		return XRT_SUCCESS;
	}
	const int64_t t0 = clock_ns(CLOCK_MONOTONIC);
	set_geom(m.geom, mode_index);
	if (build_session_locked(c, m.geom.cap_w, m.geom.cap_h, false)) {
		c->last_pair_ns.store(clock_ns(CLOCK_MONOTONIC)); // the watchdog counts from the switch
		SCAM_LOG_W("leia stereo camera (android): %s pair mode %u = %ux%u per eye: session reconfigured to %ux%u "
		           "YUV per lens in %.1f ms (frames resume after the HAL's first-frame latency)",
		           c->desc.front ? "front" : "rear", mode_index, m.eye_w, m.eye_h, m.geom.cap_w, m.geom.cap_h,
		           (double)(clock_ns(CLOCK_MONOTONIC) - t0) / 1e6);
		return XRT_SUCCESS;
	}
	set_geom(old, old_index);
	if (!build_session_locked(c, old.cap_w, old.cap_h, false)) {
		c->device_failed = true; // the watchdog re-opens it in the old mode
	}
	c->last_pair_ns.store(clock_ns(CLOCK_MONOTONIC));
	SCAM_LOG_W("leia stereo camera (android): %s pair mode %u (%ux%u per lens) failed; staying in mode %u",
	           c->desc.front ? "front" : "rear", mode_index, m.geom.cap_w, m.geom.cap_h, old_index);
	return XRT_ERROR_STEREO_CAMERA_BUSY;
}
#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES

#ifdef LEIA_SCAM_V8
namespace {

//! Our capture state as the runtime's struct, within @p out's struct_size.
void
put_capture_result(const leia_scam_capture_state &s, struct xrt_plugin_stereo_camera_capture_result *out)
{
	xrt_plugin_stereo_camera_capture_result r;
	memset(&r, 0, sizeof(r));
	r.struct_size = (uint32_t)sizeof(r);
	r.valid_fields = s.valid_fields;
	r.focus_distance_diopters = s.focus_distance_diopters;
	r.focus_state = s.focus_state;
	r.exposure_time_ns = s.exposure_time_ns;
	r.sensitivity_iso = s.sensitivity_iso;
	r.eye_time_ns[0] = s.eye_time_ns[0];
	r.eye_time_ns[1] = s.eye_time_ns[1];
	const uint32_t sz = out->struct_size;
	memcpy(out, &r, std::min<size_t>(sz, sizeof(r)));
	out->struct_size = sz;
}

} // namespace
#endif

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_CONTROLS
static_assert(LEIA_SCAM_CTL_FOCUS_MODE == XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_MODE, "control bits");
static_assert(LEIA_SCAM_CTL_FOCUS_DISTANCE == XRT_PLUGIN_STEREO_CAMERA_CONTROL_FOCUS_DISTANCE, "control bits");
static_assert(LEIA_SCAM_CTL_METERING_POINT == XRT_PLUGIN_STEREO_CAMERA_CONTROL_METERING_POINT, "control bits");
static_assert(LEIA_SCAM_CTL_EXPOSURE_COMPENSATION == XRT_PLUGIN_STEREO_CAMERA_CONTROL_EXPOSURE_COMPENSATION,
              "control bits");
static_assert((int)LEIA_SCAM_FOCUS_CONTINUOUS_AUTO == (int)XRT_PLUGIN_STEREO_CAMERA_FOCUS_CONTINUOUS_AUTO, "focus modes");
static_assert((int)LEIA_SCAM_FOCUS_AUTO_ONCE == (int)XRT_PLUGIN_STEREO_CAMERA_FOCUS_AUTO_ONCE, "focus modes");
static_assert((int)LEIA_SCAM_FOCUS_MANUAL == (int)XRT_PLUGIN_STEREO_CAMERA_FOCUS_MANUAL, "focus modes");
static_assert((int)LEIA_SCAM_FOCUS_LOCKED == (int)XRT_PLUGIN_STEREO_CAMERA_FOCUS_LOCKED, "focus modes");
static_assert(LEIA_SCAM_RES_FOCUS_DISTANCE == XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_DISTANCE, "result bits");
static_assert(LEIA_SCAM_RES_FOCUS_STATE == XRT_PLUGIN_STEREO_CAMERA_RESULT_FOCUS_STATE, "result bits");
static_assert(LEIA_SCAM_RES_EXPOSURE_TIME == XRT_PLUGIN_STEREO_CAMERA_RESULT_EXPOSURE_TIME, "result bits");
static_assert(LEIA_SCAM_RES_SENSITIVITY == XRT_PLUGIN_STEREO_CAMERA_RESULT_SENSITIVITY, "result bits");
static_assert(LEIA_SCAM_RES_EYE_TIMES == XRT_PLUGIN_STEREO_CAMERA_RESULT_EYE_TIMES, "result bits");
static_assert((int)LEIA_SCAM_FOCUS_STATE_FIXED == (int)XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_FIXED, "focus states");
static_assert((int)LEIA_SCAM_FOCUS_STATE_FAILED == (int)XRT_PLUGIN_STEREO_CAMERA_FOCUS_STATE_FAILED, "focus states");


extern "C" xrt_result_t
leia_stereo_camera_android_get_control_caps(struct xrt_plugin_instance *inst,
                                            uint32_t index,
                                            struct xrt_plugin_stereo_camera_control_caps *out)
{
	(void)inst;
	std::lock_guard<std::mutex> lk(g_mu);
	scan_locked();
	if (out == nullptr || any_pending_locked() || index >= g_pairs.size()) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const leia_scam_control_caps &cc = g_pairs[index].ctl_caps;
	xrt_plugin_stereo_camera_control_caps c;
	memset(&c, 0, sizeof(c));
	c.struct_size = (uint32_t)sizeof(c);
	c.supported_controls = cc.supported_controls;
	c.supported_focus_modes = cc.supported_focus_modes;
	c.min_focus_distance_diopters = cc.min_focus_distance_diopters;
	c.focus_distance_calibration = cc.focus_distance_calibration;
	c.exposure_compensation_min_ev = cc.exposure_compensation_min_ev;
	c.exposure_compensation_max_ev = cc.exposure_compensation_max_ev;
	c.exposure_compensation_step_ev = cc.exposure_compensation_step_ev;
#ifdef XRT_PLUGIN_STEREO_CAMERA_CONTROL_CAPS_HAS_DEFAULT_FOCUS
	// What the source holds with no focus control set (written only within
	// the runtime's struct_size, by the memcpy below). A pair whose lens
	// focuses: AF off at desc.default_focus_d (FIXED), or the HAL's own AF with
	// debug.dxr.leia.scam_hal_af=1 (AUTO). A fixed-focus lens (the front pair)
	// has no focus control and min_focus_distance 0 already says so: UNKNOWN.
	const pair_desc &pd = g_pairs[index];
	if ((cc.supported_controls & LEIA_SCAM_CTL_FOCUS_MODE) != 0) {
		c.default_focus = pd.hal_af ? XRT_PLUGIN_STEREO_CAMERA_DEFAULT_FOCUS_AUTO
		                            : XRT_PLUGIN_STEREO_CAMERA_DEFAULT_FOCUS_FIXED;
		c.default_focus_distance_diopters = pd.hal_af ? 0.0f : pd.default_focus_d;
	} else {
		c.default_focus = XRT_PLUGIN_STEREO_CAMERA_DEFAULT_FOCUS_UNKNOWN;
	}
#endif
	const uint32_t sz = out->struct_size;
	memcpy(out, &c, std::min<size_t>(sz, sizeof(c)));
	out->struct_size = sz;
	return XRT_SUCCESS;
}

/*!
 * The WHOLE wanted control state (valid_controls == 0 = the defaults, at
 * once). Applied to the running repeating request (and the still request)
 * right away; kept for every session the camera builds after (a mode switch,
 * the watchdog's re-open). The metering point is mapped through the running
 * mode's crop and turn into the logical camera's active array now, so a later
 * crop change keeps pointing at the same scene spot.
 */
extern "C" xrt_result_t
leia_stereo_camera_android_set_controls(struct xrt_plugin_stereo_camera *c,
                                        const struct xrt_plugin_stereo_camera_controls *controls)
{
	if (c == nullptr || controls == nullptr) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	xrt_plugin_stereo_camera_controls in;
	memset(&in, 0, sizeof(in));
	memcpy(&in, controls, std::min<size_t>(controls->struct_size, sizeof(in)));
	leia_scam_controls k{};
	k.valid_controls = in.valid_controls;
	k.focus_mode = in.focus_mode;
	k.focus_trigger = in.focus_trigger;
	k.focus_distance_diopters = in.focus_distance_diopters;
	k.metering_x = in.metering_x;
	k.metering_y = in.metering_y;
	k.exposure_compensation_ev = in.exposure_compensation_ev;
	std::lock_guard<std::mutex> lk(c->cam_mu);
	c->ctl = k;
	update_region_locked(c);
	const bool trigger = apply_controls_to_request_locked(c, false);
	if (c->session != nullptr) {
		const camera_status_t st = repeat_locked(c, trigger);
		if (st != ACAMERA_OK) {
			SCAM_LOG_W("leia stereo camera (android): controls not applied (setRepeatingRequest %d)", (int)st);
			return XRT_ERROR_STEREO_CAMERA_BUSY;
		}
	}
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
leia_stereo_camera_android_get_capture_result(struct xrt_plugin_stereo_camera *c,
                                              struct xrt_plugin_stereo_camera_capture_result *out)
{
	if (c == nullptr || out == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	int64_t ts = 0;
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		if (c->held < 0) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		ts = c->bufs[c->held].ts_boot;
	}
	leia_scam_capture_state s;
	capture_state_of(c, ts, &s); // camera thread: c->ctl is stable here
	put_capture_result(s, out);
	return XRT_SUCCESS;
}
#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_CONTROLS

#ifdef XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_STILL
static_assert(XRT_PLUGIN_STEREO_CAMERA_STILL_READY == 1, "still poll");

/*!
 * Asynchronous: hands the request to a worker and returns. A still of the
 * running capture (the default's cuts, or the stream's own crop) is the next
 * pair; the rear pair's large still comes from its JPEG outputs on the
 * running session (still_worker). Neither pauses the stream.
 */
extern "C" xrt_result_t
leia_stereo_camera_android_request_still(struct xrt_plugin_stereo_camera *c,
                                         const struct xrt_plugin_stereo_camera_still_request *req)
{
	if (c == nullptr || req == nullptr) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const int i = leia_scam_find_still_mode(c->desc.modes, c->desc.n_modes, req->eye_width, req->eye_height);
	if (i < 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	leia_scam_mode m = c->desc.modes[i];
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		if (c->still != still_state::IDLE) {
			return XRT_ERROR_STEREO_CAMERA_BUSY; // one at a time
		}
		if (m.source == LEIA_SCAM_SRC_DEFAULT_CAPTURE && (m.geom.cap_w != c->cap_w || m.geom.cap_h != c->cap_h)) {
			// The stream runs its own (larger) capture right now: a cut of the
			// default capture cannot be taken from it without scaling.
			SCAM_LOG_W("leia stereo camera (android): still %llu at %ux%u refused: the stream runs %ux%u per lens, "
			           "not the %ux%u capture that still is cut from",
			           (unsigned long long)req->request_id, m.eye_w, m.eye_h, c->cap_w, c->cap_h, m.geom.cap_w,
			           m.geom.cap_h);
			return XRT_ERROR_STEREO_CAMERA_BUSY;
		}
		c->still = still_state::BUSY;
		c->still_request = req->request_id;
	}
	if (c->still_thread.joinable()) {
		c->still_thread.join(); // the previous one finished (the state was IDLE)
	}
	c->still_thread = std::thread(still_worker, c, m, req->request_id);
	return XRT_SUCCESS;
}

extern "C" uint32_t
leia_stereo_camera_android_poll_still(struct xrt_plugin_stereo_camera *c,
                                      struct xrt_plugin_stereo_camera_still *out,
                                      struct xrt_plugin_stereo_camera_capture_result *out_result)
{
	if (c == nullptr || out == nullptr) {
		return XRT_PLUGIN_STEREO_CAMERA_STILL_FAILED;
	}
	still_state st;
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		st = c->still;
		if (st == still_state::FAILED) {
			c->still = still_state::IDLE;
		} else if (st == still_state::READY) {
			c->still = still_state::DELIVERED;
		}
	}
	switch (st) {
	case still_state::READY:
	case still_state::DELIVERED: break;
	case still_state::IDLE:
	case still_state::FAILED:
		if (c->still_thread.joinable()) {
			c->still_thread.join();
		}
		return XRT_PLUGIN_STEREO_CAMERA_STILL_FAILED;
	default: return XRT_PLUGIN_STEREO_CAMERA_STILL_NONE;
	}
	if (c->still_thread.joinable()) {
		c->still_thread.join(); // it set READY as its last step
	}
	xrt_plugin_stereo_camera_still s;
	memset(&s, 0, sizeof(s));
	s.width = 2 * c->still_w;
	s.height = c->still_h;
	s.format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
	s.planes[0] = c->still_buf.data();
	s.planes[1] = c->still_buf.data() + (size_t)2 * c->still_w * c->still_h;
	s.pitches[0] = s.pitches[1] = 2 * c->still_w;
	s.time_ns = c->still_eye_mono[0];
	s.time_is_exposure = 1;
#ifdef XRT_PLUGIN_STEREO_CAMERA_STILL_HAS_FLAGS
	// The 4096x3072 still went through one JPEG per lens (quality 95); a still
	// from stream pairs did not.
	s.flags = c->still_lossy ? XRT_PLUGIN_STEREO_CAMERA_STILL_LOSSY_SOURCE : 0u;
#endif
	const uint32_t sz = out->struct_size;
	memcpy(out, &s, std::min<size_t>(sz, sizeof(s)));
	out->struct_size = sz;
	if (out_result != nullptr) {
		put_capture_result(c->still_res, out_result);
	}
	return XRT_PLUGIN_STEREO_CAMERA_STILL_READY;
}

extern "C" void
leia_stereo_camera_android_release_still(struct xrt_plugin_stereo_camera *c)
{
	if (c == nullptr) {
		return;
	}
	bool drop = false;
	{
		std::lock_guard<std::mutex> lk(c->pair_mu);
		if (c->still == still_state::DELIVERED || c->still == still_state::READY ||
		    c->still == still_state::FAILED) {
			c->still = still_state::IDLE;
			drop = true;
		}
	}
	if (drop && c->still_buf.size() > ((size_t)8 << 20)) {
		// A full-resolution pair is ~38 MB: give it back between stills.
		c->still_buf.clear();
		c->still_buf.shrink_to_fit();
	}
	if (drop && c->still_thread.joinable()) {
		c->still_thread.join();
	}
}
#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_STILL

extern "C" bool
leia_stereo_camera_android_tracking_lens(char *out, size_t cap)
{
	if (out == nullptr || cap == 0) {
		return false;
	}
	std::lock_guard<std::mutex> lk(g_mu);
	scan_locked();
	if (g_pairs.empty() || !g_pairs[0].front || g_pairs[0].block_retry.verdict == LEIA_SCAM_BLOCK_PENDING) {
		return false; // #505: the main lens is not known before the block is read
	}
	snprintf(out, cap, "%s", g_pairs[0].main_id);
	return true;
}

#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA
