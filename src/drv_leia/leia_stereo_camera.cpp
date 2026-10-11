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
 * ## One camera per SR device
 *
 * SR runs one tracker process per device (the primary and a child per extra
 * calibrated device, SR ST-5481 D4). Each writes its camera to the same three
 * objects with "_<serial>" appended (`Global\SREyetrackerRawCamera_<serial>`,
 * ...`Mutex_<serial>`, ...`Event_<serial>`); the PRIMARY also keeps the bare
 * names above, a child never writes them. The live trackers are listed in
 * `Global\SREyetrackerStats2` (one row per tracker: FPC serial + pid + paused /
 * streaming). So enumerate advertises:
 *
 *  - the primary (`Global\sharedDeviceSerialMemory[0]`) at index 0, always —
 *    it is today's single camera and has the legacy sections to fall back on;
 *  - every other device with a LIVE tracker row whose own "_<serial>" section
 *    exists, sorted by serial. Anything else is not advertised: a paused
 *    tracker keeps its (zeroed) section, so a missing one means an SR whose
 *    children do not publish their camera — a camera that could never
 *    deliver. Indices are then kept stable for the instance's life (append-
 *    only registry; a device that goes away keeps its index and just waits).
 *
 * With no Stats2 table (an SR that predates per-device trackers) the list is
 * the primary alone, read from the legacy names: byte-for-byte today's
 * behaviour. A camera reads its suffixed section when it exists; the primary
 * falls back to the legacy section when it does not, and re-checks every
 * second that it is still the primary (the legacy names follow the primary)
 * and whether its suffixed section has appeared. The mapping, the section
 * names and the display names are pure functions in
 * leia_stereo_camera_parse.c, unit-tested on the host.
 *
 * Identity: device_identity "leia-sr:<serial>"; display_name names the screen
 * when the claims table (probe_displays) or SR's display enumeration maps the
 * serial to one, else the serial — always distinct per camera.
 *
 * ## Rules this file follows (ADR-043 §6, spec §6)
 *
 *  - **Never take the device.** We open the three named objects READ-ONLY
 *    (SYNCHRONIZE on the mutex, FILE_MAP_READ on the section) and never touch
 *    the camera; the tracker keeps it.
 *  - **Never wait on the event.** It is AUTO-RESET and shared by every reader:
 *    a wait here would steal the wake-up of another reader (the vendor's own
 *    apps). We do not even open it. We POLL the block every ~4 ms on a
 *    high-resolution waitable timer (~4x the source; the SR tracking camera
 *    runs ~60 Hz, NOT the 30 Hz first assumed) and detect a new frame by
 *    content fingerprint; the runtime then fans out with its own per-stream
 *    wake handles. Without the high-resolution timer (pre-1803 Windows) the
 *    poll falls back to Sleep, which lands on the ~15.6 ms tick (~64 Hz).
 *  - **Rate.** The channel carries no rate, so enumerate advertises the rate
 *    MEASURED on an earlier open in this process, else 0 = unknown (never a
 *    guess); the runtime measures independently.
 *  - **Hold the writer's mutex only for a memcpy** of the JPEG (~100-300 KB,
 *    tens of microseconds); decode OUTSIDE it, so the tracker never waits on
 *    our decoder.
 *  - **Calibration per device.** Each camera's serial names its folder: only
 *    `%ProgramData%\Simulated Reality\Devices\<serial>\` is read (the primary's
 *    serial is the FIRST entry of `Global\sharedDeviceSerialMemory` — SRService
 *    writes the primary device first, and the SR eyetracker keys its own
 *    calibration the same way); never "the
 *    first folder on the machine" (one field box carried eleven). Missing or
 *    unreadable => no CALIBRATED bit and a WARN naming the serial; never a
 *    fallback to another folder. The lens serial API (`srLensGetSerialNumber`)
 *    is deliberately NOT used: on the SRService side it force-enables and
 *    reconnects the display's controller board.
 *  - **Keep-alive.** Frames exist only while the tracker runs (a tracker
 *    pauses 2 s after its last client leaves, zeroing its section). While a
 *    camera is open we hold, PER DEVICE, an SR v2 instance with an eye tracker
 *    BOUND to that device's display (SrDisplayBindingInfo, the display id from
 *    the claims table / SR's enumeration; the primary falls back to an unbound
 *    tracker, today's behaviour) initialised (the
 *    same weaver-less pattern leia_sr_predict_trace.cpp uses, which the SR SDK
 *    owner confirmed legal as a second instance per process). No v2 runtime
 *    => no keep-alive: frames then flow only while some app weaves (WARN once).
 *    The eye-pair CALLBACK is an echo of srEyeTrackerPredict (the SDK only
 *    publishes to its streams at the end of predict), so a weaver-less tracker
 *    nobody predicts on reports zero pairs forever — the first field run said
 *    "released after 0 eye pairs" with a face in view. The keep-alive thread
 *    therefore predicts itself (~30 Hz) and counts DISTINCT tracker frames.
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
#include "leia_display_claims_win.h"

#ifdef DXR_LEIA_HAS_SR_V2
#include "leia_sr_v2_common.h"
#include <sr/sr_eye_tracker.h>
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
#include <sr/sr_display.h> // SrDisplayBindingInfo: a keep-alive tracker per device
#endif
#endif

#include "os/os_time.h"
#include "util/u_logging.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <windows.h>
#include <setupapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

//! Bases: the legacy (primary-only) name, or + "_<serial>" per device.
constexpr const char *k_cam_mutex = "Global\\SREyetrackerRawCameraMutex";
constexpr const char *k_cam_memory = "Global\\SREyetrackerRawCamera";
constexpr const char *k_serial_mutex = "Global\\sharedDeviceSerialMutex";
constexpr const char *k_serial_memory = "Global\\sharedDeviceSerialMemory";
//! SR eyetrackerstats.h (v2): one row per live tracker process.
constexpr const char *k_stats_mutex = "Global\\SREyetrackerStats2Mutex";
constexpr const char *k_stats_memory = "Global\\SREyetrackerStats2";
//! Object-name buffer: base + '_' + a 32-byte serial.
constexpr size_t k_name_max = 128;
//! How often a camera reading the LEGACY section re-checks it is still the primary.
constexpr int64_t k_legacy_recheck_ns = 1000ll * 1000 * 1000;

//! Poll period on the high-resolution timer (~4x the tracker's ~60 Hz).
constexpr DWORD k_poll_ms = 4;
//! CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+); older SDK headers lack it.
constexpr DWORD k_timer_high_res = 0x00000002;
//! Keep-alive predict poll period, and how often it logs the running count.
constexpr int64_t k_ka_poll_ns = 33ll * 1000 * 1000;
constexpr int64_t k_ka_log_ns = 30ll * 1000 * 1000 * 1000;
constexpr int64_t k_ka_silent_warn_ns = 5ll * 1000 * 1000 * 1000;
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
	bool calibrated = false;
	bool swap = false;
	//! The SR device: names its sections, its identity and (by default) its calibration folder.
	char serial[LEIA_SCAM_SERIAL_LEN + 1] = {0};
	//! The primary device: the legacy unsuffixed sections are its fallback.
	bool primary = false;
	//! SR display id for the keep-alive's SrDisplayBindingInfo; 0 = unknown.
	uint64_t display_id = 0;
	char display_name[LEIA_SCAM_NAME_MAX] = {0};
	//! The screen part of the name ("" = unknown), kept for a device that went away.
	char display_screen[LEIA_SCAM_NAME_MAX] = {0};
	uint32_t eye_w = k_default_eye_w, eye_h = k_default_eye_h;
	leia_scam_eye_calibration cal = {};
	//! platformDeviceHint: the OS camera the tracker holds, "" if not unambiguous.
	char platform_hint[LEIA_SCAM_HINT_MAX] = {0};
};

std::mutex g_resolve_mutex;
//! The last enumerate's cameras, in index order. Under g_resolve_mutex.
resolved_camera g_cams[LEIA_SCAM_MAX_CAMERAS];
uint32_t g_cam_count = 0;

//! Source rate measured on an open in this process, per device; what enumerate advertises.
std::mutex g_rate_mutex;
std::vector<std::pair<std::string, float>> g_rates;

float
rate_get(const char *serial)
{
	std::lock_guard<std::mutex> lk(g_rate_mutex);
	for (const auto &r : g_rates) {
		if (r.first == serial) {
			return r.second;
		}
	}
	return 0.0f;
}

//! @return the previous rate (0 = none).
float
rate_set(const char *serial, float rate)
{
	std::lock_guard<std::mutex> lk(g_rate_mutex);
	for (auto &r : g_rates) {
		if (r.first == serial) {
			float prev = r.second;
			r.second = rate;
			return prev;
		}
	}
	g_rates.emplace_back(serial, rate);
	return 0.0f;
}

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

/*!
 * Is @p pid a running process? A tracker runs as LocalSystem, so a caller
 * without the right may be refused even the limited query: ACCESS_DENIED means
 * the process exists. Only "no such process" (ERROR_INVALID_PARAMETER) or an
 * exit code makes the row stale.
 */
bool
pid_alive(uint32_t pid)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if (h == nullptr) {
		return GetLastError() != ERROR_INVALID_PARAMETER;
	}
	DWORD code = 0;
	bool alive = GetExitCodeProcess(h, &code) == 0 || code == STILL_ACTIVE;
	CloseHandle(h);
	return alive;
}

/*!
 * The live trackers from Global\SREyetrackerStats2. @return -1 when the table
 * does not exist (an SR that predates per-device trackers), else the count.
 */
int32_t
read_trackers(leia_scam_tracker *out, uint32_t cap)
{
	HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, k_stats_memory);
	if (map == nullptr) {
		return -1;
	}
	int32_t n = 0;
	HANDLE mtx = OpenMutexA(SYNCHRONIZE, FALSE, k_stats_mutex);
	const uint8_t *view = static_cast<const uint8_t *>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0));
	if (view != nullptr) {
		MEMORY_BASIC_INFORMATION mbi = {};
		SIZE_T region = VirtualQuery(view, &mbi, sizeof(mbi)) != 0 ? mbi.RegionSize : 0;
		size_t len = region < LEIA_SCAM_STATS_MEMORY_SIZE ? (size_t)region : LEIA_SCAM_STATS_MEMORY_SIZE;
		// SR's rule: nobody waits for it longer than 200 ms.
		bool locked = mtx != nullptr && WaitForSingleObject(mtx, 200) != WAIT_TIMEOUT;
		std::vector<uint8_t> copy(view, view + len);
		if (locked) {
			ReleaseMutex(mtx);
		}
		UnmapViewOfFile(view);
		// pid liveness outside the writer's mutex.
		n = (int32_t)leia_scam_parse_tracker_stats(copy.data(), copy.size(), pid_alive, out, cap);
	}
	if (mtx != nullptr) {
		CloseHandle(mtx);
	}
	CloseHandle(map);
	return n;
}

//! Open one device's raw-camera mutex + section (@p serial "" = the legacy names).
bool
open_named_pair(const char *serial, DWORD mutex_access, HANDLE *out_mtx, HANDLE *out_map)
{
	*out_mtx = nullptr;
	*out_map = nullptr;
	char mtx_name[k_name_max], mem_name[k_name_max];
	if (!leia_scam_section_name(k_cam_mutex, serial, mtx_name, sizeof(mtx_name)) ||
	    !leia_scam_section_name(k_cam_memory, serial, mem_name, sizeof(mem_name))) {
		return false;
	}
	*out_map = OpenFileMappingA(FILE_MAP_READ, FALSE, mem_name);
	if (*out_map == nullptr) {
		return false;
	}
	*out_mtx = OpenMutexA(mutex_access, FALSE, mtx_name);
	if (*out_mtx == nullptr && mutex_access != SYNCHRONIZE) {
		*out_mtx = OpenMutexA(SYNCHRONIZE, FALSE, mtx_name);
	}
	if (*out_mtx == nullptr) {
		CloseHandle(*out_map);
		*out_map = nullptr;
		return false;
	}
	return true;
}

//! Does the device's own "_<serial>" section exist?
bool
suffixed_section_exists(const char *serial)
{
	char mem_name[k_name_max];
	if (serial[0] == '\0' || !leia_scam_section_name(k_cam_memory, serial, mem_name, sizeof(mem_name))) {
		return false;
	}
	HANDLE map = OpenFileMappingA(FILE_MAP_READ, FALSE, mem_name);
	if (map == nullptr) {
		return false;
	}
	CloseHandle(map);
	return true;
}

//! Per-eye size from a frame already in the device's block, if any.
void
peek_eye_size(const char *serial, bool primary, uint32_t *eye_w, uint32_t *eye_h)
{
	HANDLE mtx = nullptr, map = nullptr;
	if (!open_named_pair(serial, SYNCHRONIZE, &mtx, &map) && primary) {
		open_named_pair("", SYNCHRONIZE, &mtx, &map);
	}
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

/*
 *
 * Platform device hint: which OS video-capture device is the tracker's camera.
 * The choice (known USB VID:PID, else a "tracking camera" friendly name; fill
 * only when unambiguous) lives in leia_scam_pick_platform_hint and is unit
 * tested; this is only the SetupAPI enumeration feeding it. SetupAPI rather
 * than MediaFoundation: it lists the device WITHOUT activating a media source
 * (which could contend with the tracker), and setupapi is a system library.
 *
 */

//! KSCATEGORY_VIDEO_CAMERA (MediaFoundation's enumeration) and KSCATEGORY_VIDEO
//! (DirectShow's); defined here so ks.h/ksmedia.h are not needed.
const GUID k_ksc_video_camera = {0xe5323777, 0xf976, 0x4f5b, {0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44}};
const GUID k_ksc_video = {0x6994ad05, 0x93ef, 0x11d0, {0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96}};
//! Re-enumerate at most this often while no hint was found (camera not yet attached).
constexpr int64_t k_hint_retry_ns = 5ll * 1000 * 1000 * 1000;

std::string
narrow(const wchar_t *w)
{
	if (w == nullptr || w[0] == L'\0') {
		return std::string();
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1) {
		return std::string();
	}
	std::string out((size_t)n - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
	return out;
}

void
enum_video_devices(const GUID &cls, std::vector<std::string> &paths, std::vector<std::string> &names)
{
	HDEVINFO set = SetupDiGetClassDevsW(&cls, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		return;
	}
	SP_DEVICE_INTERFACE_DATA ifd = {};
	ifd.cbSize = sizeof(ifd);
	for (DWORD i = 0; i < 64 && SetupDiEnumDeviceInterfaces(set, nullptr, &cls, i, &ifd); i++) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || need > 4096) {
			continue;
		}
		std::vector<uint8_t> buf(need);
		auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buf.data());
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		SP_DEVINFO_DATA dev = {};
		dev.cbSize = sizeof(dev);
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, &dev)) {
			continue;
		}
		// The interface's FriendlyName is what MediaFoundation/DirectShow show;
		// fall back to the device node's friendly name, then its description.
		wchar_t name[256] = {0};
		HKEY key = SetupDiOpenDeviceInterfaceRegKey(set, &ifd, 0, KEY_READ);
		if (key != (HKEY)INVALID_HANDLE_VALUE) {
			DWORD type = 0, bytes = sizeof(name) - sizeof(wchar_t);
			if (RegQueryValueExW(key, L"FriendlyName", nullptr, &type, (LPBYTE)name, &bytes) != ERROR_SUCCESS ||
			    type != REG_SZ) {
				name[0] = L'\0';
			}
			RegCloseKey(key);
		}
		if (name[0] == L'\0' &&
		    !SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_FRIENDLYNAME, nullptr, (PBYTE)name,
		                                       sizeof(name) - sizeof(wchar_t), nullptr)) {
			if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_DEVICEDESC, nullptr, (PBYTE)name,
			                                       sizeof(name) - sizeof(wchar_t), nullptr)) {
				name[0] = L'\0';
			}
		}
		name[255] = L'\0';
		paths.push_back(narrow(detail->DevicePath));
		names.push_back(narrow(name));
	}
	SetupDiDestroyDeviceInfoList(set);
}

//! Last result, kept across resolves: enumeration is not free and the camera
//! does not move. Re-tried every k_hint_retry_ns while empty. Under g_resolve_mutex.
std::string g_hint_cache;
bool g_hint_done = false;
int64_t g_hint_at_ns = 0;

void
resolve_platform_hint(char out[LEIA_SCAM_HINT_MAX])
{
	out[0] = '\0';
	const char *ovr = std::getenv("DXR_LEIA_STEREO_CAMERA_PLATFORM_HINT");
	if (ovr != nullptr) {
		// Field escape hatch: "-" = hide nothing, anything else is used verbatim.
		if (std::strcmp(ovr, "-") != 0) {
			std::snprintf(out, LEIA_SCAM_HINT_MAX, "%s", ovr);
		}
		return;
	}
	int64_t now = (int64_t)os_monotonic_get_ns();
	if (g_hint_done && (!g_hint_cache.empty() || now - g_hint_at_ns < k_hint_retry_ns)) {
		std::snprintf(out, LEIA_SCAM_HINT_MAX, "%s", g_hint_cache.c_str());
		return;
	}
	std::vector<std::string> paths, names;
	enum_video_devices(k_ksc_video_camera, paths, names);
	enum_video_devices(k_ksc_video, paths, names);
	std::vector<leia_scam_video_device> devs(paths.size());
	for (size_t i = 0; i < paths.size(); i++) {
		devs[i].path = paths[i].c_str();
		devs[i].friendly_name = names[i].c_str();
	}
	char pick[LEIA_SCAM_HINT_MAX] = {0};
	leia_scam_pick_platform_hint(devs.data(), devs.size(), pick, sizeof(pick));
	if (!g_hint_done || g_hint_cache != pick) {
		// One line per CHANGE of the answer, never per enumerate.
		U_LOG_W("leia stereo camera: platformDeviceHint = \"%s\" (%zu video-capture interfaces; %s)", pick,
		        devs.size(),
		        pick[0] != '\0' ? "the tracker's camera, hidden from capture stacks"
		                        : "no unambiguous SR tracking camera — nothing hidden");
	}
	g_hint_cache = pick;
	g_hint_done = true;
	g_hint_at_ns = now;
	std::snprintf(out, LEIA_SCAM_HINT_MAX, "%s", pick);
}

/*!
 * The OS's friendly name of the monitor driven by GDI source @p device_name
 * ("\\.\DISPLAY2"): DisplayConfig's monitorFriendlyDeviceName, which is the
 * EDID's monitor name. "" when it cannot be found.
 */
std::string
monitor_friendly_name(const char *device_name)
{
	if (device_name == nullptr || device_name[0] == '\0') {
		return std::string();
	}
	UINT32 npaths = 0, nmodes = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &npaths, &nmodes) != ERROR_SUCCESS || npaths == 0) {
		return std::string();
	}
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(npaths);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(nmodes);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &npaths, paths.data(), &nmodes, modes.data(), nullptr) !=
	    ERROR_SUCCESS) {
		return std::string();
	}
	for (UINT32 i = 0; i < npaths; i++) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME src = {};
		src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		src.header.size = sizeof(src);
		src.header.adapterId = paths[i].sourceInfo.adapterId;
		src.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS ||
		    narrow(src.viewGdiDeviceName) != device_name) {
			continue;
		}
		DISPLAYCONFIG_TARGET_DEVICE_NAME tgt = {};
		tgt.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tgt.header.size = sizeof(tgt);
		tgt.header.adapterId = paths[i].targetInfo.adapterId;
		tgt.header.id = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tgt.header) == ERROR_SUCCESS) {
			std::string name = narrow(tgt.monitorFriendlyDeviceName);
			// A laptop's own panel usually carries no EDID monitor name.
			const auto tech = tgt.outputTechnology;
			if (name.empty() && (tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
			                     tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
			                     tech == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED)) {
				name = "Built-in display";
			}
			return name;
		}
	}
	return std::string();
}

/*!
 * Which screen a device's camera belongs to: the claims table first (what
 * probe_displays stored — no SR call), then SR's own display enumeration
 * (cached, shared with probe_displays). Fills the SR display id the
 * keep-alive binds to and the screen's name ("" = unknown).
 */
void
resolve_screen(const char *serial, uint64_t *out_display_id, std::string *out_screen)
{
	*out_display_id = 0;
	out_screen->clear();
	char device_name[32] = {0};
	std::string product;
	leia_win_claim_binding claim = {};
	if (leia_win_claims_find_serial(serial, &claim)) {
		*out_display_id = claim.sr_display_id;
		std::snprintf(device_name, sizeof(device_name), "%s", claim.device_name);
	}
	if (*out_display_id == 0 || device_name[0] == '\0') {
		leia_win_sr_display d[LEIA_WIN_SR_MAX_DISPLAYS];
		int32_t n = leia_win_sr_enumerate_displays(d, LEIA_WIN_SR_MAX_DISPLAYS);
		for (int32_t i = 0; i < n && i < (int32_t)LEIA_WIN_SR_MAX_DISPLAYS; i++) {
			if (d[i].fpc_verified && std::strcmp(d[i].serial, serial) == 0) {
				if (*out_display_id == 0) {
					*out_display_id = d[i].display_id;
				}
				if (device_name[0] == '\0') {
					std::snprintf(device_name, sizeof(device_name), "%s", d[i].device_name);
				}
				product = d[i].product_code;
				break;
			}
		}
	}
	*out_screen = monitor_friendly_name(device_name);
	if (out_screen->empty() && !product.empty()) {
		*out_screen = product; // the panel's SR product code, when the OS has no name
	}
}

//! Calibration of one device, by its serial's folder (never another's).
void
resolve_calibration(resolved_camera &c, const char *calib_serial)
{
	const char *pd = std::getenv("ProgramData");
	std::string dir =
	    std::string(pd != nullptr ? pd : "C:\\ProgramData") + "\\Simulated Reality\\Devices\\" + calib_serial;
	std::string intr, extr;
	char why[160] = {0};
	leia_scam_calibration raw = {};
	if (!read_text_file(dir + "\\intrinsics.yml", intr) || !read_text_file(dir + "\\extrinsics.yml", extr)) {
		U_LOG_W(
		    "leia stereo camera: no calibration for device %s (%s\\{intrinsics,extrinsics}.yml "
		    "unreadable) — RAW only; NOT falling back to another device's folder",
		    calib_serial, dir.c_str());
	} else if (!leia_scam_parse_calibration(intr.c_str(), intr.size(), extr.c_str(), extr.size(), &raw, why,
	                                        sizeof(why))) {
		U_LOG_W("leia stereo camera: calibration of device %s rejected (%s) — RAW only", calib_serial, why);
	} else {
		c.swap = leia_scam_decide_swap(&raw, std::getenv("DXR_LEIA_STEREO_CAMERA_SWAP"));
		leia_scam_calibration_for_runtime(&raw, c.swap, &c.cal);
		c.calibrated = true;
		U_LOG_W(
		    "leia stereo camera: device %s calibrated from %s — baseline %.2f mm (T = %.2f %.2f %.2f), "
		    "left-eye fx %.2f, %s halves (camera %s = SBS left)",
		    calib_serial, dir.c_str(),
		    std::sqrt(raw.T[0] * raw.T[0] + raw.T[1] * raw.T[1] + raw.T[2] * raw.T[2]), raw.T[0], raw.T[1],
		    raw.T[2], c.cal.k[0][0], c.swap ? "SWAPPED" : "native", c.swap ? "2" : "1");
	}
}

//! One line per CHANGE of the advertised set, never per enumerate. Under g_resolve_mutex.
std::string g_logged_set;
//! Append-only serial -> index registry (stable indices for the instance's life).
leia_scam_camera_slot g_registry[LEIA_SCAM_MAX_CAMERAS];

void
resolve_locked()
{
	if (env_is("DXR_LEIA_STEREO_CAMERA", '0')) {
		g_cam_count = 0;
		return;
	}
	char primary[LEIA_SCAM_SERIAL_LEN + 1];
	read_primary_serial(primary);
	leia_scam_tracker trackers[LEIA_SCAM_STATS_ROWS];
	const int32_t ntr = read_trackers(trackers, LEIA_SCAM_STATS_ROWS);
	leia_scam_camera_slot cur[LEIA_SCAM_MAX_CAMERAS];
	const uint32_t ncur = leia_scam_build_camera_list(primary, trackers, ntr, suffixed_section_exists, cur,
	                                                  LEIA_SCAM_MAX_CAMERAS);
	bool present[LEIA_SCAM_MAX_CAMERAS] = {false};
	const uint32_t n =
	    leia_scam_merge_stable(g_registry, g_cam_count, LEIA_SCAM_MAX_CAMERAS, cur, ncur, present);
	if (n == 0) {
		if (g_logged_set != "-") {
			g_logged_set = "-";
			U_LOG_W(
			    "leia stereo camera: no SR device serial in %s and no live tracker in %s — no camera "
			    "advertised (is SRService running with a device attached?)",
			    k_serial_memory, k_stats_memory);
		}
		return;
	}

	// Debug override of the PRIMARY's calibration folder (not its sections or identity).
	const char *serial_override = std::getenv("DXR_LEIA_STEREO_CAMERA_SERIAL");
	const bool overridden = serial_override != nullptr && serial_override[0] != '\0';

	std::string screens[LEIA_SCAM_MAX_CAMERAS];
	const char *screen_ptrs[LEIA_SCAM_MAX_CAMERAS];
	resolved_camera cams[LEIA_SCAM_MAX_CAMERAS];
	for (uint32_t i = 0; i < n; i++) {
		resolved_camera &c = cams[i];
		if (!present[i] && i < g_cam_count) {
			// Gone since an earlier enumerate: keep its index and last answer
			// (its stream waits); never hand the index to another device.
			c = g_cams[i];
			c.primary = false;
			screens[i] = c.display_screen;
			screen_ptrs[i] = screens[i].c_str();
			continue;
		}
		std::snprintf(c.serial, sizeof(c.serial), "%s", g_registry[i].serial);
		c.primary = g_registry[i].primary;
		peek_eye_size(c.serial, c.primary, &c.eye_w, &c.eye_h);
		try {
			resolve_screen(c.serial, &c.display_id, &screens[i]);
		} catch (...) {
			c.display_id = 0;
			screens[i].clear();
		}
		std::snprintf(c.display_screen, sizeof(c.display_screen), "%s", screens[i].c_str());
		screen_ptrs[i] = screens[i].c_str();
		if (c.primary && overridden) {
			U_LOG_W("leia stereo camera: calibration serial OVERRIDDEN by DXR_LEIA_STEREO_CAMERA_SERIAL=%s",
			        serial_override);
		}
		resolve_calibration(c, c.primary && overridden ? serial_override : c.serial);
	}
	char names[LEIA_SCAM_MAX_CAMERAS][LEIA_SCAM_NAME_MAX];
	leia_scam_display_names(g_registry, screen_ptrs, n, names);

	// The platform hint names ONE OS camera; it can only be attributed when
	// exactly one device is advertised (several SR cameras make the pick
	// ambiguous anyway, and it then names none).
	char hint[LEIA_SCAM_HINT_MAX] = {0};
	if (n == 1) {
		try {
			resolve_platform_hint(hint);
		} catch (...) {
			hint[0] = '\0';
		}
	}

	std::string set;
	for (uint32_t i = 0; i < n; i++) {
		std::snprintf(cams[i].display_name, sizeof(cams[i].display_name), "%s", names[i]);
		std::snprintf(cams[i].platform_hint, sizeof(cams[i].platform_hint), "%s", hint);
		g_cams[i] = cams[i];
		char line[512];
		std::snprintf(line, sizeof(line), "[%u] \"%s\" leia-sr:%s%s%s display=0x%016llx %ux%u%s; ", i,
		              cams[i].display_name, cams[i].serial, cams[i].primary ? " (primary)" : "",
		              present[i] ? "" : " (GONE)", (unsigned long long)cams[i].display_id, cams[i].eye_w,
		              cams[i].eye_h, cams[i].calibrated ? " calibrated" : " RAW");
		set += line;
	}
	g_cam_count = n;
	if (set != g_logged_set) {
		g_logged_set = set;
		U_LOG_W("leia stereo camera: %u camera(s) (%s): %s", n,
		        ntr < 0 ? "no Stats2 table: primary only, legacy sections" : "per-device trackers",
		        set.c_str());
	}
}


/*
 *
 * Keep-alive: per DEVICE, an SR v2 eye tracker on our own instance, bound to
 * that device's display, while any camera of that device is open.
 *
 */

struct keepalive
{
	std::string serial;
	uint64_t display_id = 0;
	bool primary = false;
	int refs = 0;
#ifdef DXR_LEIA_HAS_SR_V2
	SrInstance instance = nullptr;
	SrEyeTracker tracker = nullptr;
	std::thread poller;
	std::atomic<bool> quit{false};
	//! Written by the poller only; read after join.
	leia_scam_pair_count count = {};
	uint64_t predict_failures = 0;
#endif
};

//! Guards the list and every entry's refs / SR handles (acquire/release are rare).
std::mutex g_ka_mutex;
std::vector<std::unique_ptr<keepalive>> g_kas;
bool g_ka_warned_unavailable = false;

#ifdef DXR_LEIA_HAS_SR_V2
/*!
 * Predict on the keep-alive's own tracker (nobody else does: that is what makes
 * the tracker's eye-pair stream flow) and account distinct tracker frames. One
 * WARN on the first frame with eyes, one if nothing arrives for 5 s, an INFO
 * running count every 30 s — never per frame.
 */
void
keepalive_poll(keepalive *ka)
{
	const int64_t t0 = os_monotonic_get_ns();
	int64_t next_log = t0 + k_ka_log_ns;
	bool warned_first = false, warned_silent = false;
	const char *sn = ka->serial.c_str();
	while (!ka->quit.load(std::memory_order_relaxed)) {
		SrEyePair pair{};
		SrResult r = SR_ERROR_VALIDATION_FAILURE;
		try {
			r = srEyeTrackerPredict(ka->tracker, 0, &pair);
		} catch (...) {
		}
		if (SR_SUCCEEDED(r)) {
			leia_scam_pair_count_push(&ka->count, pair.frameId, pair.leftEye.z, pair.rightEye.z);
			if (!warned_first && ka->count.with_eyes > 0) {
				warned_first = true;
				U_LOG_W(
				    "leia stereo camera %s: keep-alive tracker delivering eye pairs (first after %.1f s)",
				    sn, (double)(os_monotonic_get_ns() - t0) * 1e-9);
			}
		} else {
			ka->predict_failures++;
		}
		int64_t now = os_monotonic_get_ns();
		if (!warned_silent && ka->count.with_eyes == 0 && now - t0 > k_ka_silent_warn_ns) {
			warned_silent = true;
			U_LOG_W(
			    "leia stereo camera %s: keep-alive: no eye pair after 5 s (%llu predicts, %llu tracker "
			    "frames, %llu failed) — no face in view, or the tracker is not running for this instance",
			    sn, (unsigned long long)ka->count.polls, (unsigned long long)ka->count.new_frames,
			    (unsigned long long)ka->predict_failures);
		}
		if (now >= next_log) {
			next_log = now + k_ka_log_ns;
			U_LOG_I(
			    "leia stereo camera %s: keep-alive eye pairs so far: %llu with eyes / %llu tracker frames "
			    "(%llu predicts)",
			    sn, (unsigned long long)ka->count.with_eyes, (unsigned long long)ka->count.new_frames,
			    (unsigned long long)ka->count.polls);
		}
		Sleep((DWORD)(k_ka_poll_ns / 1000000));
	}
}

void
keepalive_teardown_locked(keepalive *ka)
{
	if (ka->poller.joinable()) {
		ka->quit = true;
		ka->poller.join();
	}
	if (ka->tracker != nullptr) {
		srDestroyEyeTracker(ka->tracker);
		ka->tracker = nullptr;
	}
	if (ka->instance != nullptr) {
		srDestroyInstance(ka->instance);
		ka->instance = nullptr;
	}
}

/*!
 * Own instance + an eye tracker bound to @p bind_display_id (0 = unbound: the
 * active display's, today's behaviour). @return false with everything released.
 */
bool
keepalive_try_start_locked(keepalive *ka, uint64_t bind_display_id)
{
	try {
		if (!leia_sr_v2_create_instance(3.0, &ka->instance) || ka->instance == nullptr) {
			ka->instance = nullptr;
			throw 1;
		}
		SrEyeTrackerCreateInfo tci{};
		tci.sType = SR_TYPE_EYE_TRACKER_CREATE_INFO;
		tci.pNext = nullptr;
		tci.enablePrediction = SR_TRUE;
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
		// SR D4: a tracker bound to a display tracks THAT device's camera, which
		// is what keeps that device's tracker process capturing.
		SrDisplayBindingInfo binding{};
		binding.sType = SR_TYPE_DISPLAY_BINDING_INFO;
		binding.pNext = nullptr;
		binding.displayId = bind_display_id;
		if (bind_display_id != 0) {
			tci.pNext = &binding;
		}
#else
		(void)bind_display_id;
#endif
		SrResult r = srCreateEyeTracker(ka->instance, &tci, &ka->tracker);
		if (!SR_SUCCEEDED(r) || ka->tracker == nullptr) {
			U_LOG_W("leia stereo camera %s: keep-alive srCreateEyeTracker (display 0x%016llx) failed (%s)",
			        ka->serial.c_str(), (unsigned long long)bind_display_id, leia_sr_v2_result_str(r));
			ka->tracker = nullptr;
			throw 2;
		}
		// Senses before srInitialize (sr_eye_tracker.h); this starts tracking.
		// No callback: it would only echo our own predicts (see the file comment).
		if (!leia_sr_v2_initialize(ka->instance)) {
			throw 4;
		}
		ka->count = {};
		ka->predict_failures = 0;
		ka->quit = false;
		ka->poller = std::thread(keepalive_poll, ka);
		U_LOG_W(
		    "leia stereo camera %s: keep-alive UP (own SR v2 instance + eye tracker %s, predicting at ~%d "
		    "Hz) — the device's tracker stays on while its camera is open",
		    ka->serial.c_str(), bind_display_id != 0 ? "BOUND to its display" : "unbound (active display)",
		    (int)(1000000000ll / k_ka_poll_ns));
		return true;
	} catch (...) {
		keepalive_teardown_locked(ka);
		return false;
	}
}
#endif

/*!
 * Hold device @p serial's tracker up. One keep-alive per device, shared by
 * every open camera of it. Never fails the open: without a keep-alive the
 * frames flow only while something else holds that device's tracker.
 */
keepalive *
keepalive_acquire(const char *serial, uint64_t display_id, bool primary)
{
	std::lock_guard<std::mutex> lk(g_ka_mutex);
	for (auto &k : g_kas) {
		if (k->serial == serial) {
			k->refs++;
			return k.get();
		}
	}
	g_kas.push_back(std::make_unique<keepalive>());
	keepalive *ka = g_kas.back().get();
	ka->serial = serial;
	ka->display_id = display_id;
	ka->primary = primary;
	ka->refs = 1;
	if (env_is("DXR_LEIA_STEREO_CAMERA_KEEPALIVE", '0')) {
		U_LOG_W(
		    "leia stereo camera %s: keep-alive disabled (DXR_LEIA_STEREO_CAMERA_KEEPALIVE=0) — frames only "
		    "while something else holds the device's tracker",
		    serial);
		return ka;
	}
#ifdef DXR_LEIA_HAS_SR_V2
	bool up = false;
#ifdef DXR_LEIA_HAS_SR_MULTI_WIN
	if (display_id != 0) {
		up = keepalive_try_start_locked(ka, display_id);
	}
#endif
	if (!up && primary) {
		// The primary's tracker is what an unbound tracker holds (today's path).
		up = keepalive_try_start_locked(ka, 0);
	}
	if (!up) {
		const char *why = primary ? "no SR v2 instance / tracker"
		                  : display_id == 0
		                      ? "no SR display id for this serial (claims / SR enumeration); an unbound "
		                        "tracker would hold the PRIMARY's"
		                      : "the bound tracker was refused (or this SDK cannot bind one)";
		U_LOG_W(
		    "leia stereo camera %s: keep-alive could not start (%s) — frames only while something else "
		    "holds the device's tracker",
		    serial, why);
	}
#else
	(void)display_id;
	(void)primary;
	if (!g_ka_warned_unavailable) {
		g_ka_warned_unavailable = true;
		U_LOG_W(
		    "leia stereo camera: built without the SR v2 SDK — no tracker keep-alive; frames only while "
		    "an app weaves");
	}
#endif
	return ka;
}

void
keepalive_release(keepalive *ka)
{
	if (ka == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lk(g_ka_mutex);
	if (--ka->refs > 0) {
		return;
	}
#ifdef DXR_LEIA_HAS_SR_V2
	try {
		const bool was_up = ka->instance != nullptr;
		keepalive_teardown_locked(ka);
		if (was_up) {
			U_LOG_W(
			    "leia stereo camera %s: keep-alive released after %llu eye pairs (%llu tracker frames, %llu "
			    "predicts, %llu failed)",
			    ka->serial.c_str(), (unsigned long long)ka->count.with_eyes,
			    (unsigned long long)ka->count.new_frames, (unsigned long long)ka->count.polls,
			    (unsigned long long)ka->predict_failures);
		}
	} catch (...) {
		ka->tracker = nullptr;
		ka->instance = nullptr;
	}
#endif
	for (auto it = g_kas.begin(); it != g_kas.end(); ++it) {
		if (it->get() == ka) {
			g_kas.erase(it);
			break;
		}
	}
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

	//! The device this camera streams (copied from the enumerate at open).
	char serial[LEIA_SCAM_SERIAL_LEN + 1] = {0};
	bool primary = false;
	//! Reading the LEGACY (unsuffixed) section, which follows the primary.
	bool legacy = false;
	int64_t next_legacy_check_ns = 0;
	keepalive *ka = nullptr;

	bool swap = false;
	uint32_t eye_w = 0, eye_h = 0;

	std::vector<uint8_t> jpeg;
	std::vector<uint8_t> gray;
	uint32_t gray_w = 0, gray_h = 0;

	HANDLE timer = nullptr; //!< high-resolution poll timer, or null (Sleep fallback)
	leia_scam_rate rate = {};

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
	c->legacy = false;
}

/*!
 * Open the device's named objects READ-ONLY (never the event): its own
 * "_<serial>" section, else — the primary only — the legacy unsuffixed one.
 */
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
	const DWORD access = SYNCHRONIZE | MUTEX_MODIFY_STATE;
	bool legacy = false;
	if (!open_named_pair(c->serial, access, &c->mutex, &c->mapping)) {
		if (!c->primary || !open_named_pair("", access, &c->mutex, &c->mapping)) {
			cam_close_channel(c); // the tracker has not created them yet
			return false;
		}
		legacy = true;
	}
	c->view = static_cast<const uint8_t *>(MapViewOfFile(c->mapping, FILE_MAP_READ, 0, 0, 0));
	MEMORY_BASIC_INFORMATION mbi = {};
	if (c->view == nullptr || VirtualQuery(c->view, &mbi, sizeof(mbi)) == 0) {
		cam_close_channel(c);
		return false;
	}
	c->view_size = mbi.RegionSize;
	c->legacy = legacy;
	c->next_legacy_check_ns = now + k_legacy_recheck_ns;
	char name[k_name_max];
	leia_scam_section_name(k_cam_memory, legacy ? "" : c->serial, name, sizeof(name));
	U_LOG_W(
	    "leia stereo camera %s: reading %s (%llu bytes, read-only%s; polling, never waiting on the "
	    "auto-reset event)",
	    c->serial, name, (unsigned long long)c->view_size,
	    legacy ? "; LEGACY primary-only section, no per-device one yet" : "");
	return true;
}

/*!
 * The legacy section follows the primary. Once a second, while reading it:
 * drop it if this device is no longer the primary (it would carry another
 * device's camera), and move to the device's own section once it exists.
 */
static void
cam_recheck_legacy(xrt_plugin_stereo_camera *c, int64_t now)
{
	if (!c->legacy || c->view == nullptr || now < c->next_legacy_check_ns) {
		return;
	}
	c->next_legacy_check_ns = now + k_legacy_recheck_ns;
	char primary[LEIA_SCAM_SERIAL_LEN + 1];
	read_primary_serial(primary);
	if (primary[0] != '\0' && std::strcmp(primary, c->serial) != 0) {
		U_LOG_W(
		    "leia stereo camera %s: no longer the primary device (now %s) — leaving the legacy section, "
		    "waiting for its own",
		    c->serial, primary);
		c->primary = false;
		cam_close_channel(c);
		c->next_open_try_ns = 0;
		return;
	}
	if (suffixed_section_exists(c->serial)) {
		U_LOG_W("leia stereo camera %s: per-device section appeared — switching to it", c->serial);
		cam_close_channel(c);
		c->next_open_try_ns = 0;
	}
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
		if (out == nullptr) {
			return g_cam_count;
		}
		// The runtime's stride: every entry is written only up to the
		// struct_size the runtime set on out[0] (ADR-020: never past it).
		const uint32_t sz = out[0].struct_size;
		if (sz == 0) {
			return g_cam_count;
		}
		const uint32_t n = g_cam_count < capacity ? g_cam_count : capacity;
		for (uint32_t i = 0; i < n; i++) {
			const resolved_camera &rc = g_cams[i];
			xrt_plugin_stereo_camera_info info;
			std::memset(&info, 0, sizeof(info));
			info.struct_size = (uint32_t)sizeof(info);
			std::snprintf(info.display_name, sizeof(info.display_name), "%s", rc.display_name);
			std::snprintf(info.device_identity, sizeof(info.device_identity), "leia-sr:%s", rc.serial);
			// The OS camera the tracker holds (USB "vvvv:pppp"), so a capture
			// stack can hide the busy duplicate; "" unless unambiguous.
			std::snprintf(info.platform_device_hint, sizeof(info.platform_device_hint), "%s",
			              rc.platform_hint);
			info.flags = XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING |
			             XRT_PLUGIN_STEREO_CAMERA_USER_FACING | XRT_PLUGIN_STEREO_CAMERA_MONOCHROME;
			if (rc.calibrated) {
				info.flags |= XRT_PLUGIN_STEREO_CAMERA_CALIBRATED;
			}
			info.eye_width = rc.eye_w;
			info.eye_height = rc.eye_h;
			// Measured on an earlier open, else 0 = unknown: never a guess (the
			// first field run advertised a hard-coded 30 Hz for a ~60 Hz camera).
			info.max_frame_rate = rate_get(rc.serial);
			info.native_format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8;
			auto *dst = reinterpret_cast<xrt_plugin_stereo_camera_info *>(reinterpret_cast<uint8_t *>(out) +
			                                                               (size_t)i * sz);
			std::memcpy(dst, &info, sz < sizeof(info) ? sz : sizeof(info));
			dst->struct_size = sz;
		}
		return g_cam_count;
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
		if (index >= g_cam_count || out == nullptr || !g_cams[index].calibrated) {
			return XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
		const resolved_camera &rc = g_cams[index];
		uint32_t sz = out->struct_size;
		xrt_plugin_stereo_camera_calibration c;
		std::memset(&c, 0, sizeof(c));
		c.struct_size = (uint32_t)sizeof(c);
		// The SR tracker calibrates at its native per-eye size (the frames').
		c.image_width = rc.eye_w;
		c.image_height = rc.eye_h;
		for (int e = 0; e < 2; e++) {
			for (int k = 0; k < 4; k++) {
				c.k[e][k] = rc.cal.k[e][k];
			}
			for (int k = 0; k < 8; k++) {
				c.distortion[e][k] = rc.cal.d[e][k];
			}
		}
		c.distortion_model = rc.cal.model == 2 ? XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN8
		                                       : XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN5;
		for (int i = 0; i < 3; i++) {
			for (int j = 0; j < 3; j++) {
				c.rotation_right_from_left[i][j] = rc.cal.R[i][j];
			}
			c.translation_right_from_left_mm[i] = rc.cal.T[i];
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
	if (out_cam == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	try {
		xrt_plugin_stereo_camera *c = new xrt_plugin_stereo_camera();
		uint64_t display_id = 0;
		{
			std::lock_guard<std::mutex> lk(g_resolve_mutex);
			if (index >= g_cam_count) {
				delete c;
				return XRT_ERROR_FEATURE_NOT_SUPPORTED;
			}
			const resolved_camera &rc = g_cams[index];
			std::snprintf(c->serial, sizeof(c->serial), "%s", rc.serial);
			c->primary = rc.primary;
			c->swap = rc.calibrated && rc.swap;
			c->eye_w = rc.eye_w;
			c->eye_h = rc.eye_h;
			display_id = rc.display_id;
		}
		c->jpeg.reserve(512 * 1024);
		c->timer = CreateWaitableTimerExW(nullptr, nullptr, k_timer_high_res, TIMER_ALL_ACCESS);
		if (c->timer == nullptr) {
			U_LOG_W(
			    "leia stereo camera: no high-resolution timer (pre-1803 Windows?) — polling on Sleep, "
			    "~64 Hz at best for a ~60 Hz source");
		}
		c->ka = keepalive_acquire(c->serial, display_id, c->primary);
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
			cam_recheck_legacy(c, now);
			if (cam_open_channel(c, now)) {
				leia_scam_header hdr;
				poll_result p = cam_poll(c, &hdr);
				if (p == POLL_NEW) {
					// Arrival time in the plug-in: the channel has no exposure stamp.
					int64_t arrival = os_monotonic_get_ns();
					if (cam_decode(c, hdr)) {
						c->block_empty = false;
						c->last_new_ns = arrival;
						if (leia_scam_rate_push(&c->rate, arrival)) {
							float prev = rate_set(c->serial, c->rate.rate);
							if (prev <= 0.0f) {
								U_LOG_W(
								    "leia stereo camera %s: measured source rate %.1f Hz "
								    "over %u frames (advertised from the next "
								    "enumerate)",
								    c->serial, c->rate.rate, LEIA_SCAM_RATE_WINDOW);
							}
						}
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
			int64_t left_ns = deadline - now;
			int64_t nap_ns =
			    left_ns < (int64_t)k_poll_ms * 1000000 ? left_ns : (int64_t)k_poll_ms * 1000000;
			if (nap_ns < 500000) {
				nap_ns = 500000;
			}
			LARGE_INTEGER due;
			due.QuadPart = -(nap_ns / 100); // relative, 100 ns units
			if (c->timer != nullptr && SetWaitableTimer(c->timer, &due, 0, nullptr, nullptr, FALSE)) {
				WaitForSingleObject(c->timer, INFINITE);
			} else {
				Sleep((DWORD)((nap_ns + 999999) / 1000000));
			}
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
		U_LOG_W("leia stereo camera %s: closed after %llu frames", c->serial, (unsigned long long)c->seq);
		cam_close_channel(c);
		if (c->timer != nullptr) {
			CloseHandle(c->timer);
			c->timer = nullptr;
		}
		keepalive_release(c->ka);
		c->ka = nullptr;
		delete c;
	} catch (...) {
	}
}

#endif // XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA
