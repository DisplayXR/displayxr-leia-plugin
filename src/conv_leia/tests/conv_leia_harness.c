// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Headless harness for the POSIX Leia conversion provider: loads the
 *         provider .so through its one export (dlopen + xrtConversionPluginNegotiate,
 *         as the runtime's loader does), drives probe -> describe -> warm ->
 *         stream -> convert on host memory, and writes the views as a PNG.
 *
 *   conv_leia_harness --provider <DisplayXR-LeiaConversion.so>
 *                     [--neurd-path <libNeurD.so | dir>] [--enable 0|1]
 *                     [--set KEY=VALUE]... [--expect absent|disabled|ready]
 *                     [--input <image>] [--out <views.png>] [--frames N]
 *                     [--convergence <rel 0..1 | -1 = auto>] [--strength S]
 *
 * Settings reach the provider ONLY through the fake host's get_setting, as in
 * the runtime. --expect absent / disabled check the gate without NeurD and exit
 * (ctest). With --input, prints one line per frame plus a summary (warm ms,
 * first / median / p90 / mean convert ms).
 */

#include "xrt/xrt_conversion_plugin.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_SETTINGS 32

static const char *g_keys[MAX_SETTINGS];
static const char *g_vals[MAX_SETTINGS];
static int g_nsettings = 0;
static int g_notifies = 0;

static void
add_setting(const char *k, const char *v)
{
	if (g_nsettings < MAX_SETTINGS) {
		g_keys[g_nsettings] = k;
		g_vals[g_nsettings] = v;
		g_nsettings++;
	}
}

static bool
fake_get_setting(const char *key, char *buf, uint32_t size)
{
	for (int i = g_nsettings - 1; i >= 0; i--) {
		if (strcasecmp(g_keys[i], key) == 0 && size > 0) {
			snprintf(buf, size, "%s", g_vals[i]);
			return true;
		}
	}
	return false;
}

static void
fake_notify(uint32_t cap_id)
{
	(void)cap_id;
	g_notifies++;
}

static bool
never(void *ctx)
{
	(void)ctx;
	return false;
}

static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int
cmp_double(const void *a, const void *b)
{
	const double x = *(const double *)a, y = *(const double *)b;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static const char *
state_str(uint32_t s)
{
	static const char *const n[] = {"UNKNOWN", "READY",    "WARMING",   "ABSENT",      "INCOMPATIBLE", "UNLICENSED",
	                                "FAILED",  "DISABLED", "INSTALLED", "DOWNLOADING", "EVICTED"};
	return s < sizeof(n) / sizeof(n[0]) ? n[s] : "?";
}

int
main(int argc, char **argv)
{
	const char *provider = NULL, *expect = NULL, *input = NULL, *out_png = "views.png";
	int frames = 10;
	float convergence = -1.0f, strength = 1.0f;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (strcmp(a, "--provider") == 0 && v) {
			provider = v, i++;
		} else if (strcmp(a, "--neurd-path") == 0 && v) {
			add_setting("NeurDPath", v), i++;
		} else if (strcmp(a, "--enable") == 0 && v) {
			add_setting("Enable", v), i++;
		} else if (strcmp(a, "--set") == 0 && v) {
			char *kv = strdup(v);
			char *eq = strchr(kv, '=');
			if (eq == NULL) {
				fprintf(stderr, "--set wants KEY=VALUE\n");
				return 2;
			}
			*eq = '\0';
			add_setting(kv, eq + 1), i++;
		} else if (strcmp(a, "--expect") == 0 && v) {
			expect = v, i++;
		} else if (strcmp(a, "--input") == 0 && v) {
			input = v, i++;
		} else if (strcmp(a, "--out") == 0 && v) {
			out_png = v, i++;
		} else if (strcmp(a, "--frames") == 0 && v) {
			frames = atoi(v), i++;
		} else if (strcmp(a, "--convergence") == 0 && v) {
			convergence = strtof(v, NULL), i++;
		} else if (strcmp(a, "--strength") == 0 && v) {
			strength = strtof(v, NULL), i++;
		} else {
			fprintf(stderr, "unknown argument %s\n", a);
			return 2;
		}
	}
	if (provider == NULL) {
		fprintf(stderr, "--provider is required\n");
		return 2;
	}

	void *so = dlopen(provider, RTLD_NOW | RTLD_LOCAL);
	if (so == NULL) {
		fprintf(stderr, "dlopen(%s): %s\n", provider, dlerror());
		return 1;
	}
	xrt_conversion_plugin_negotiate_fn_t neg =
	    (xrt_conversion_plugin_negotiate_fn_t)dlsym(so, XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
	if (neg == NULL) {
		fprintf(stderr, "no %s export\n", XRT_CONVERSION_PLUGIN_ENTRYPOINT_NAME);
		return 1;
	}
	struct xrt_conversion_plugin_host_iface host;
	memset(&host, 0, sizeof(host));
	host.struct_size = (uint32_t)sizeof(host);
	host.host_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	host.host_flags = 2u; // headless
	host.get_setting = fake_get_setting;
	host.notify_changed = fake_notify;
	struct xrt_conversion_plugin_iface *iface = NULL;
	uint32_t pv = 0;
	if (neg(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT, &host, &iface, &pv) != XRT_SUCCESS || iface == NULL) {
		fprintf(stderr, "negotiate failed (provider api %u)\n", pv);
		return 1;
	}
	printf("provider: id=%s name=\"%s\" vendor=\"%s\" version=%s api=%u\n", iface->id, iface->display_name,
	       iface->vendor, iface->version, pv);

	struct xrt_conv_provider_status ps;
	memset(&ps, 0, sizeof(ps));
	ps.struct_size = (uint32_t)sizeof(ps);
	iface->get_provider_status(&ps);
	printf("provider status: %u (%s)\n", ps.state, ps.hint);

	struct xrt_conversion_plugin_instance *inst = NULL;
	if (iface->probe(&inst) != XRT_SUCCESS || inst == NULL) {
		fprintf(stderr, "probe failed\n");
		return 1;
	}
	uint32_t count = 0;
	iface->describe_cap(inst, 0, NULL, &count);
	uint32_t views_cap = 0;
	for (uint32_t i = 0; i < count; i++) {
		struct xrt_conv_capability c;
		memset(&c, 0, sizeof(c));
		c.struct_size = (uint32_t)sizeof(c);
		iface->describe_cap(inst, i, &c, NULL);
		printf("cap %u: id=%u task=%u \"%s\" outputs=0x%x mem=0x%x sync=0x%x flags=0x%x licence=%u backend=%s "
		       "model=%s %s views=%u..%u est=%ums\n",
		       i, c.cap_id, c.task, c.display_name, c.outputs, c.import_memory_kinds, c.import_sync_kinds, c.flags,
		       c.licence_class, c.backend, c.model_id, c.model_version, c.min_views, c.max_views,
		       c.est_latency_ms);
		if (c.task == XRT_CONV_TASK_VIEWS_MONO) {
			views_cap = c.cap_id;
		}
	}
	struct xrt_conv_state st;
	memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	if (views_cap == 0) {
		fprintf(stderr, "no VIEWS_MONO capability\n");
		return 1;
	}
	iface->get_state(inst, views_cap, &st);
	printf("state before warm: %s (reason %u, retryable %u) version=\"%s\" detail=\"%s\"\n", state_str(st.state),
	       st.failure_reason, st.retryable, st.version, st.detail);

	if (expect != NULL && strcmp(expect, "absent") == 0) {
		const bool ok = st.state == XRT_CONV_STATE_ABSENT && st.failure_reason == XRT_CONV_FAIL_MODEL &&
		                ps.state == 2u && iface->warm(inst, views_cap, never, NULL) != XRT_SUCCESS;
		iface->get_state(inst, views_cap, &st);
		printf("after warm: %s\n", state_str(st.state));
		iface->destroy(inst);
		printf("%s\n", ok && st.state == XRT_CONV_STATE_ABSENT ? "PASS absent" : "FAIL absent");
		return ok && st.state == XRT_CONV_STATE_ABSENT ? 0 : 1;
	}
	if (expect != NULL && strcmp(expect, "disabled") == 0) {
		const bool ok = st.state == XRT_CONV_STATE_DISABLED && iface->warm(inst, views_cap, never, NULL) != XRT_SUCCESS;
		iface->destroy(inst);
		printf("%s\n", ok ? "PASS disabled" : "FAIL disabled");
		return ok ? 0 : 1;
	}

	const double tw0 = now_ms();
	const xrt_result_t wr = iface->warm(inst, views_cap, never, NULL);
	const double warm_ms = now_ms() - tw0;
	iface->get_state(inst, views_cap, &st);
	printf("warm: %s in %.0f ms -> %s (reason %u) version=\"%s\" detail=\"%s\"\n", wr == XRT_SUCCESS ? "OK" : "FAILED",
	       warm_ms, state_str(st.state), st.failure_reason, st.version, st.detail);
	for (uint32_t i = 0; i < count; i++) {
		struct xrt_conv_capability c;
		memset(&c, 0, sizeof(c));
		c.struct_size = (uint32_t)sizeof(c);
		iface->describe_cap(inst, i, &c, NULL);
		printf("cap %u after warm: flags=0x%x licence=%u commercial_ok=%u backend=%s version=%s outputs=0x%x\n", i,
		       c.flags, c.licence_class, c.commercial_ok, c.backend, c.model_version, c.outputs);
	}
	if (wr != XRT_SUCCESS) {
		iface->destroy(inst);
		return expect != NULL && strcmp(expect, "ready") != 0 ? 0 : 1;
	}
	if (input == NULL) {
		iface->destroy(inst);
		return 0;
	}

	int w = 0, h = 0, n = 0;
	uint8_t *rgba = stbi_load(input, &w, &h, &n, 4);
	if (rgba == NULL) {
		fprintf(stderr, "cannot read %s\n", input);
		return 1;
	}
	struct xrt_conv_stream_info si;
	memset(&si, 0, sizeof(si));
	si.struct_size = (uint32_t)sizeof(si);
	si.input_kind = XRT_CONV_INPUT_MONO;
	si.outputs = XRT_CONV_OUT_VIEWS;
	si.content_hint = XRT_CONV_CONTENT_VIDEO;
	si.view_count = 2;
	si.views_layout = XRT_CONV_VIEWS_ROW;
	si.max_input_width = (uint32_t)w;
	si.max_input_height = (uint32_t)h;
	uint64_t stream = 0;
	if (iface->stream_create(inst, views_cap, &si, &stream) != XRT_SUCCESS) {
		fprintf(stderr, "stream_create failed\n");
		return 1;
	}
	struct xrt_conv_output_desc od;
	memset(&od, 0, sizeof(od));
	od.struct_size = (uint32_t)sizeof(od);
	iface->stream_get_output_desc(inst, stream, (uint32_t)w, (uint32_t)h, &od);
	printf("input %dx%d -> views target %ux%u fmt %u (depth %ux%u)\n", w, h, od.views_width, od.views_height,
	       od.views_format, od.depth_width, od.depth_height);
	const uint32_t vstride = od.views_width * 4u;
	uint8_t *views = calloc((size_t)vstride * od.views_height, 1);

	struct xrt_conv_frame_in fin;
	memset(&fin, 0, sizeof(fin));
	fin.struct_size = (uint32_t)sizeof(fin);
	fin.image_count = 1;
	fin.image_role[0] = XRT_CONV_IMG_COLOR;
	struct xrt_conv_image *im = &fin.images[0];
	im->memory_kind = XRT_CONV_MEM_CPU;
	im->import_key = 1;
	im->cpu_ptr = rgba;
	im->byte_size = (uint64_t)w * h * 4u;
	im->width = (uint32_t)w;
	im->height = (uint32_t)h;
	im->format = XRT_CONV_FMT_RGBA8_UNORM;
	im->row_stride = (uint32_t)w * 4u;
	im->sync.kind = XRT_CONV_SYNC_NONE;

	double *ms = calloc((size_t)(frames > 0 ? frames : 1), sizeof(double));
	int ok_frames = 0;
	for (int f = 0; f < frames; f++) {
		struct xrt_conv_params pa;
		memset(&pa, 0, sizeof(pa));
		pa.struct_size = (uint32_t)sizeof(pa);
		pa.frame_id = (uint64_t)f + 1u;
		pa.strength = strength;
		pa.view_count = 2;
		pa.content_hint = XRT_CONV_CONTENT_VIDEO;
		pa.viewpoint_count = 2;
		pa.viewpoints_xyz[0][0] = -0.032f;
		pa.viewpoints_xyz[0][2] = 0.6f;
		pa.viewpoints_xyz[1][0] = 0.032f;
		pa.viewpoints_xyz[1][2] = 0.6f;
		pa.baseline_m = 0.064f;
		pa.nominal_z_m = 0.6f;
		if (convergence >= 0.0f) {
			// DECODED in the AFFINE_INVERSE units the provider maps from (1 - rel).
			pa.convergence_mode = XRT_CONV_CONVERGENCE_DECODED;
			pa.convergence = 1.0f - convergence;
		} else {
			pa.convergence_mode = XRT_CONV_CONVERGENCE_AUTO;
		}
		struct xrt_conv_frame_out fo;
		memset(&fo, 0, sizeof(fo));
		fo.struct_size = (uint32_t)sizeof(fo);
		fo.views.memory_kind = XRT_CONV_MEM_CPU;
		fo.views.import_key = 2;
		fo.views.cpu_ptr = views;
		fo.views.byte_size = (uint64_t)vstride * od.views_height;
		fo.views.width = od.views_width;
		fo.views.height = od.views_height;
		fo.views.format = od.views_format;
		fo.views.row_stride = vstride;
		fo.views.sync.kind = XRT_CONV_SYNC_NONE;
		const double t0 = now_ms();
		const xrt_result_t r = iface->stream_convert(inst, stream, &fin, &pa, &fo);
		ms[f] = now_ms() - t0;
		printf("frame %d: %s written=0x%x %.1f ms\n", f, r == XRT_SUCCESS ? "OK" : "FAILED", fo.written, ms[f]);
		ok_frames += r == XRT_SUCCESS && (fo.written & XRT_CONV_OUT_VIEWS) != 0;
	}
	if (ok_frames > 0) {
		stbi_write_png(out_png, (int)od.views_width, (int)od.views_height, 4, views, (int)vstride);
		printf("wrote %s\n", out_png);
	}
	if (frames > 0) {
		const double first = ms[0];
		double sum = 0.0;
		for (int f = 1; f < frames; f++) {
			sum += ms[f];
		}
		qsort(ms + 1, (size_t)(frames - 1), sizeof(double), cmp_double);
		const int steady = frames - 1;
		printf("SUMMARY warm_ms=%.0f first_ms=%.1f steady_n=%d median_ms=%.1f p90_ms=%.1f mean_ms=%.1f ok=%d/%d\n",
		       warm_ms, first, steady, steady > 0 ? ms[1 + steady / 2] : 0.0,
		       steady > 0 ? ms[1 + (int)floor(0.9 * (steady - 1))] : 0.0, steady > 0 ? sum / steady : 0.0, ok_frames,
		       frames);
	}
	{
		struct xrt_conv_capability c;
		memset(&c, 0, sizeof(c));
		c.struct_size = (uint32_t)sizeof(c);
		for (uint32_t i = 0; i < count; i++) {
			iface->describe_cap(inst, i, &c, NULL);
			if (c.cap_id == views_cap) {
				printf("provider-measured NeurD convert (EMA): %u ms\n", c.est_latency_ms);
			}
		}
	}
	iface->stream_destroy(inst, stream);
	iface->destroy(inst);
	stbi_image_free(rgba);
	free(views);
	free(ms);
	return ok_frames == frames ? 0 : 1;
}
