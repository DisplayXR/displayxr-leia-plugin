// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  The Leia conversion provider against the runtime's ABI contract,
 *         mirroring the runtime's tests_conversion_abi: negotiate, the
 *         describe_cap contract (count, well-formed capabilities, index past
 *         the end, writes only within struct_size, one generation per walk),
 *         get_state — and the vendor-module gate on fake modules:
 *
 *   missing  : no module            -> ABSENT(MODEL), provider PLATFORM_ABSENT
 *   old      : version below floor  -> FAILED(VERSION) at probe AND after warm,
 *                                      provider INCOMPATIBLE, the file never loaded
 *   noentry  : new enough, no entry -> INSTALLED, then FAILED(LOAD) after warm
 *   disabled : Enable=0             -> DISABLED
 *
 * One scenario per process (the module is process-wide, as NeurD is): ctest
 * runs `test_conv_leia_provider <scenario> <module path>`. The provider's
 * entry point is linked statically (leia_conversion_static) — no registry, no
 * runtime DLL. Configuration goes through the fake host's get_setting only.
 */

#include "xrt/xrt_conversion_plugin.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

extern "C" xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version);

namespace {

int g_fail = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		g_checks++;                                                                                            \
		if (!(cond)) {                                                                                         \
			g_fail++;                                                                                      \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                       \
		}                                                                                                      \
	} while (0)

/*
 * Fake host: settings from a table, notify_changed counted.
 */

std::string g_module_path;
std::string g_enable = "";
int g_notifies = 0;

bool
fake_get_setting(const char *key, char *buf, uint32_t size)
{
	const char *v = nullptr;
	if (_stricmp(key, "NeurDPath") == 0) {
		v = g_module_path.c_str();
	} else if (_stricmp(key, "Enable") == 0 && !g_enable.empty()) {
		v = g_enable.c_str();
	}
	if (v == nullptr || size == 0) {
		return false;
	}
	snprintf(buf, size, "%s", v);
	return true;
}

void
fake_notify(uint32_t cap_id)
{
	(void)cap_id;
	g_notifies++;
}

struct xrt_conversion_plugin_host_iface
make_host()
{
	struct xrt_conversion_plugin_host_iface h = {};
	h.struct_size = (uint32_t)sizeof(h);
	h.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	h.host_flags = 2u; // headless / CI
	h.get_setting = fake_get_setting;
	h.notify_changed = fake_notify;
	return h;
}

struct xrt_conversion_plugin_host_iface g_host;

bool
nul_terminated(const char *s, size_t n)
{
	return memchr(s, 0, n) != nullptr;
}

bool
never(void *ctx)
{
	(void)ctx;
	return false;
}

struct xrt_conversion_plugin_iface *
negotiate()
{
	g_host = make_host();
	struct xrt_conversion_plugin_iface *iface = nullptr;
	uint32_t v = 0;
	const xrt_result_t r = xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &g_host, &iface, &v);
	CHECK(r == XRT_SUCCESS);
	CHECK(v == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	CHECK(iface != nullptr);
	return iface;
}

struct xrt_conv_state
state_of(struct xrt_conversion_plugin_iface *iface, struct xrt_conversion_plugin_instance *inst, uint32_t cap)
{
	struct xrt_conv_state st;
	memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	CHECK(iface->get_state(inst, cap, &st) == XRT_SUCCESS);
	CHECK(st.struct_size == sizeof(st));
	CHECK(nul_terminated(st.version, sizeof(st.version)));
	CHECK(nul_terminated(st.detail, sizeof(st.detail)));
	return st;
}

uint32_t
provider_status(struct xrt_conversion_plugin_iface *iface)
{
	struct xrt_conv_provider_status ps;
	memset(&ps, 0, sizeof(ps));
	ps.struct_size = (uint32_t)sizeof(ps);
	CHECK(iface->get_provider_status(&ps));
	CHECK(nul_terminated(ps.hint, sizeof(ps.hint)));
	CHECK((ps.flags & XRT_CONV_PROVIDER_FLAG_FALLBACK) == 0); // a real provider, never the fallback
	return ps.state;
}

/*
 * The ABI contract (every scenario runs it).
 */

void
check_iface(struct xrt_conversion_plugin_iface *iface)
{
	CHECK(iface->struct_size == sizeof(struct xrt_conversion_plugin_iface));
	CHECK(iface->reserved_0 == 0u);
	CHECK(iface->id != nullptr && strcmp(iface->id, "leia-conversion") == 0);
	CHECK(iface->display_name != nullptr && iface->display_name[0] != '\0');
	CHECK(XRT_CONV_IFACE_HAS(iface, get_provider_status));
	CHECK(XRT_CONV_IFACE_HAS(iface, probe));
	CHECK(XRT_CONV_IFACE_HAS(iface, describe_cap));
	CHECK(XRT_CONV_IFACE_HAS(iface, get_generation));
	CHECK(XRT_CONV_IFACE_HAS(iface, get_state));
	CHECK(XRT_CONV_IFACE_HAS(iface, warm));
	CHECK(XRT_CONV_IFACE_HAS(iface, evict));
	CHECK(XRT_CONV_IFACE_HAS(iface, stream_create));
	CHECK(XRT_CONV_IFACE_HAS(iface, stream_get_output_desc));
	CHECK(XRT_CONV_IFACE_HAS(iface, stream_convert));
	CHECK(XRT_CONV_IFACE_HAS(iface, stream_destroy));
	CHECK(XRT_CONV_IFACE_HAS(iface, job_execute));
	CHECK(XRT_CONV_IFACE_HAS(iface, destroy));

	// A runtime of another major: the provider's version, no iface.
	struct xrt_conversion_plugin_iface *other = reinterpret_cast<struct xrt_conversion_plugin_iface *>(1);
	uint32_t v = 0;
	CHECK(xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT + 1, &g_host, &other, &v) !=
	      XRT_SUCCESS);
	CHECK(other == nullptr);
	CHECK(v == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	CHECK(xrtConversionPluginNegotiate(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &g_host, nullptr, &v) !=
	      XRT_SUCCESS);
}

void
check_describe(struct xrt_conversion_plugin_iface *iface, struct xrt_conversion_plugin_instance *inst)
{
	const uint64_t gen_before = iface->get_generation(inst);

	uint32_t count = 0xFFFFFFFFu;
	CHECK(iface->describe_cap(inst, 12345u, nullptr, &count) == XRT_SUCCESS);
	CHECK(count == 3u);
	CHECK(count <= XRT_CONV_MAX_CAPABILITIES);

	std::set<uint32_t> ids, tasks;
	for (uint32_t i = 0; i < count; i++) {
		struct xrt_conv_capability cap;
		memset(&cap, 0, sizeof(cap));
		cap.struct_size = (uint32_t)sizeof(cap);
		uint32_t n = 0;
		CHECK(iface->describe_cap(inst, i, &cap, &n) == XRT_SUCCESS);
		CHECK(n == count);
		CHECK(cap.struct_size == sizeof(cap));
		CHECK(cap.cap_id == i + 1u); // stable ids 1..3
		CHECK(ids.insert(cap.cap_id).second);
		tasks.insert(cap.task);
		CHECK(cap.task != XRT_CONV_TASK_NONE && cap.task < XRT_CONV_TASK_RESERVED_FIRST);
		CHECK(cap.lanes == XRT_CONV_LANE_STREAM);
		CHECK(cap.input_kinds == XRT_CONV_INPUT_MONO);
		CHECK(cap.outputs != 0);
		CHECK((cap.import_memory_kinds & XRT_CONV_MEM_CPU) != 0); // hosting rule
		CHECK((cap.import_memory_kinds & XRT_CONV_MEM_WIN32_NT_HANDLE) != 0);
		CHECK((cap.import_sync_kinds & (1u << XRT_CONV_SYNC_FENCE_NT)) != 0);
		CHECK((cap.input_formats & (1u << XRT_CONV_FMT_RGBA8_UNORM)) != 0);
		CHECK(cap.licence_class <= XRT_CONV_LICENCE_RESEARCH_ONLY);
		CHECK(cap.commercial_ok == (cap.licence_class == XRT_CONV_LICENCE_COMMERCIAL ? 1u : 0u));
		CHECK(cap.max_streams > 0u && cap.max_concurrent == 1u);
		if (cap.task == XRT_CONV_TASK_VIEWS_MONO) {
			CHECK((cap.flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) != 0);
			CHECK(cap.min_views == 2u && cap.max_views == 2u);
			CHECK((cap.outputs & XRT_CONV_OUT_VIEWS) != 0);
			CHECK((cap.flags & XRT_CONV_CAP_TEMPORAL) == 0);
		} else {
			CHECK(cap.outputs == XRT_CONV_OUT_DEPTH);
			CHECK(((cap.flags & XRT_CONV_CAP_TEMPORAL) != 0) == (cap.task == XRT_CONV_TASK_DEPTH_VIDEO));
		}
		if ((cap.outputs & XRT_CONV_OUT_DEPTH) != 0) {
			CHECK(cap.depth_kind != XRT_CONV_DEPTH_UNKNOWN);
			CHECK(cap.depth_output_format == XRT_CONV_FMT_R32_FLOAT);
		}
		CHECK(nul_terminated(cap.licence_spdx, sizeof(cap.licence_spdx)));
		CHECK(strncmp(cap.licence_spdx, "LicenseRef-", 11) == 0);
		CHECK(nul_terminated(cap.model_id, sizeof(cap.model_id)));
		CHECK(nul_terminated(cap.model_version, sizeof(cap.model_version)));
		CHECK(nul_terminated(cap.backend, sizeof(cap.backend)));
		CHECK(nul_terminated(cap.display_name, sizeof(cap.display_name)) && cap.display_name[0] != '\0');
		CHECK(cap.reserved_0 == 0u);
		const struct xrt_conv_state st = state_of(iface, inst, cap.cap_id);
		CHECK(st.state <= XRT_CONV_STATE_EVICTED);
	}
	CHECK(tasks.count(XRT_CONV_TASK_DEPTH_MONO) == 1 && tasks.count(XRT_CONV_TASK_DEPTH_VIDEO) == 1 &&
	      tasks.count(XRT_CONV_TASK_VIEWS_MONO) == 1);

	// index >= count: the documented error, *out untouched.
	struct xrt_conv_capability past;
	memset(&past, 0xA5, sizeof(past));
	past.struct_size = (uint32_t)sizeof(past);
	struct xrt_conv_capability before = past;
	CHECK(iface->describe_cap(inst, count, &past, nullptr) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(iface->describe_cap(inst, count + 7u, &past, nullptr) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(memcmp(&past, &before, sizeof(past)) == 0);

	// An older runtime's shorter capability: nothing written past its struct_size.
	const uint32_t older = (uint32_t)offsetof(struct xrt_conv_capability, reserved_0);
	struct xrt_conv_capability cap;
	memset(&cap, 0, sizeof(cap));
	cap.struct_size = older;
	CHECK(iface->describe_cap(inst, 2, &cap, nullptr) == XRT_SUCCESS);
	CHECK(cap.task == XRT_CONV_TASK_VIEWS_MONO);
	CHECK(cap.struct_size == older);
	const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&cap);
	size_t nonzero = 0;
	for (size_t i = older; i < sizeof(cap); i++) {
		nonzero += bytes[i] != 0 ? 1u : 0u;
	}
	CHECK(nonzero == 0u);

	// get_state into a short state struct: nothing past it either.
	struct xrt_conv_state st;
	memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)offsetof(struct xrt_conv_state, version);
	CHECK(iface->get_state(inst, 1, &st) == XRT_SUCCESS);
	for (size_t i = 0; i < sizeof(st.version); i++) {
		CHECK(st.version[i] == 0);
	}
	CHECK(iface->get_state(inst, 0, &st) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(iface->get_state(inst, 4, &st) == XRT_ERROR_FEATURE_NOT_SUPPORTED);

	// The walk saw one generation.
	CHECK(iface->get_generation(inst) == gen_before);
}

//! A stream can only be created on a READY capability; output descs need a live stream.
void
check_not_ready_streams(struct xrt_conversion_plugin_iface *iface, struct xrt_conversion_plugin_instance *inst)
{
	struct xrt_conv_stream_info si = {};
	si.struct_size = (uint32_t)sizeof(si);
	si.input_kind = XRT_CONV_INPUT_MONO;
	si.outputs = XRT_CONV_OUT_VIEWS;
	si.view_count = 2;
	uint64_t h = 0;
	CHECK(iface->stream_create(inst, 3, &si, &h) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	struct xrt_conv_output_desc od = {};
	od.struct_size = (uint32_t)sizeof(od);
	CHECK(iface->stream_get_output_desc(inst, 77, 1920, 1080, &od) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
	CHECK(iface->evict(inst, 1) == XRT_SUCCESS); // idempotent
}

void
all_caps(struct xrt_conversion_plugin_iface *iface,
         struct xrt_conversion_plugin_instance *inst,
         uint32_t state,
         uint32_t reason,
         uint32_t retryable)
{
	for (uint32_t c = 1; c <= 3; c++) {
		const struct xrt_conv_state st = state_of(iface, inst, c);
		CHECK(st.state == state);
		CHECK(st.failure_reason == reason);
		CHECK(st.retryable == retryable);
	}
}

uint32_t
licence_of(struct xrt_conversion_plugin_iface *iface, struct xrt_conversion_plugin_instance *inst)
{
	struct xrt_conv_capability cap;
	memset(&cap, 0, sizeof(cap));
	cap.struct_size = (uint32_t)sizeof(cap);
	CHECK(iface->describe_cap(inst, 0, &cap, nullptr) == XRT_SUCCESS);
	return cap.licence_class;
}

bool
module_mapped()
{
	return GetModuleHandleA(g_module_path.c_str()) != nullptr;
}

/*
 * Scenarios.
 */

int
scenario(const std::string &name)
{
	struct xrt_conversion_plugin_iface *iface = negotiate();
	if (iface == nullptr) {
		return 1;
	}
	check_iface(iface);
	struct xrt_conversion_plugin_instance *inst = nullptr;
	CHECK(iface->probe(&inst) == XRT_SUCCESS);
	CHECK(inst != nullptr);
	if (inst == nullptr) {
		return 1;
	}
	check_describe(iface, inst);
	check_not_ready_streams(iface, inst);

	if (name == "missing") {
		CHECK(provider_status(iface) == 2u); // PLATFORM_ABSENT
		all_caps(iface, inst, XRT_CONV_STATE_ABSENT, XRT_CONV_FAIL_MODEL, 1u);
		CHECK(iface->warm(inst, 3, never, nullptr) != XRT_SUCCESS);
		all_caps(iface, inst, XRT_CONV_STATE_ABSENT, XRT_CONV_FAIL_MODEL, 1u);
		CHECK(licence_of(iface, inst) == XRT_CONV_LICENCE_UNKNOWN); // no words, no class
	} else if (name == "old") {
		// The gate holds before any load, from the file's version resource...
		CHECK(provider_status(iface) == 5u); // INCOMPATIBLE
		all_caps(iface, inst, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_VERSION, 1u);
		CHECK(licence_of(iface, inst) == XRT_CONV_LICENCE_RESEARCH_ONLY); // the module's own product name
		// ...and after a warm, which refuses it WITHOUT mapping it.
		CHECK(iface->warm(inst, 1, never, nullptr) != XRT_SUCCESS);
		all_caps(iface, inst, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_VERSION, 1u);
		CHECK(!module_mapped());
		const struct xrt_conv_state st = state_of(iface, inst, 1);
		CHECK(strcmp(st.version, "0.4.1") == 0);
	} else if (name == "noentry") {
		CHECK(provider_status(iface) == 1u); // READY: the dependency is there
		all_caps(iface, inst, XRT_CONV_STATE_INSTALLED, XRT_CONV_FAIL_NONE, 0u);
		CHECK(licence_of(iface, inst) == XRT_CONV_LICENCE_COMMERCIAL);
		const int before = g_notifies;
		CHECK(iface->warm(inst, 3, never, nullptr) != XRT_SUCCESS);
		CHECK(module_mapped()); // it was loaded: the floor admitted it
		all_caps(iface, inst, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_LOAD, 1u);
		CHECK(g_notifies > before); // the readiness move was pushed
	} else if (name == "disabled") {
		all_caps(iface, inst, XRT_CONV_STATE_DISABLED, XRT_CONV_FAIL_NONE, 0u);
		CHECK(iface->warm(inst, 2, never, nullptr) != XRT_SUCCESS);
		all_caps(iface, inst, XRT_CONV_STATE_DISABLED, XRT_CONV_FAIL_NONE, 0u);
		CHECK(!module_mapped());
	} else {
		fprintf(stderr, "unknown scenario '%s'\n", name.c_str());
		return 2;
	}

	// A cancelled warm returns at once and changes nothing.
	struct xrt_conv_state before = state_of(iface, inst, 1);
	CHECK(iface->warm(inst, 1, [](void *) { return true; }, nullptr) != XRT_SUCCESS);
	struct xrt_conv_state after = state_of(iface, inst, 1);
	CHECK(before.state == after.state && before.failure_reason == after.failure_reason);

	iface->destroy(inst);
	return 0;
}

} // namespace

int
main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s missing|old|noentry|disabled <module path>\n", argv[0]);
		return 2;
	}
	const std::string name = argv[1];
	g_module_path = argv[2];
	if (name == "disabled") {
		g_enable = "0";
	}
	const int r = scenario(name);
	printf("test_conv_leia_provider %s: %d checks, %d failed\n", name.c_str(), g_checks, g_fail);
	return r != 0 ? r : (g_fail == 0 ? 0 : 1);
}
