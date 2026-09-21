// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*! @file  @brief GNOME/mutter capture plumbing (DisplayConfig, CaptureExclusion1, ScreenCast). @ingroup drv_leia_linux */

#include "leia_mutter_capture_linux.h"

#include "../drv_leia/leia_edid_table.h"

#include "util/u_logging.h"

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Pure helpers (no D-Bus).
 *
 */

static void
edid_descriptor_text(const uint8_t *d, char *out, size_t out_len)
{
	// Display descriptor text: bytes 5..17, terminated by 0x0A, padded 0x20.
	size_t n = 0;
	for (size_t i = 5; i < 18 && n + 1 < out_len; i++) {
		if (d[i] == 0x0A || d[i] == 0x00) {
			break;
		}
		out[n++] = (char)d[i];
	}
	out[n] = '\0';
	while (n > 0 && isspace((unsigned char)out[n - 1])) {
		out[--n] = '\0';
	}
}

bool
leia_edid_identity_parse(const uint8_t edid[128], const char *drm_connector, struct leia_panel_identity *out)
{
	static const uint8_t magic[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
	memset(out, 0, sizeof(*out));
	if (memcmp(edid, magic, sizeof(magic)) != 0) {
		return false;
	}
	// PNP id: big-endian word, three 5-bit letters, 1 = 'A'.
	const uint16_t pnp = (uint16_t)((edid[8] << 8) | edid[9]);
	out->vendor[0] = (char)('A' - 1 + ((pnp >> 10) & 0x1F));
	out->vendor[1] = (char)('A' - 1 + ((pnp >> 5) & 0x1F));
	out->vendor[2] = (char)('A' - 1 + (pnp & 0x1F));
	out->vendor[3] = '\0';
	out->product_code = (uint16_t)(edid[10] | (edid[11] << 8));
	out->serial_number = (uint32_t)edid[12] | ((uint32_t)edid[13] << 8) | ((uint32_t)edid[14] << 16) |
	                     ((uint32_t)edid[15] << 24);
	for (int i = 0; i < 4; i++) {
		const uint8_t *d = edid + 54 + 18 * i;
		if (d[0] != 0 || d[1] != 0) {
			continue; // a detailed timing, not a display descriptor
		}
		if (d[3] == 0xFC) {
			edid_descriptor_text(d, out->name, sizeof(out->name));
		} else if (d[3] == 0xFF) {
			edid_descriptor_text(d, out->serial_str, sizeof(out->serial_str));
		}
	}
	if (drm_connector != NULL) {
		snprintf(out->drm_connector, sizeof(out->drm_connector), "%s", drm_connector);
	}
	out->valid = true;
	return true;
}

//! Compare two strings ignoring trailing whitespace (mutter keeps EDID padding).
static bool
streq_trim(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	while (la > 0 && isspace((unsigned char)a[la - 1])) {
		la--;
	}
	while (lb > 0 && isspace((unsigned char)b[lb - 1])) {
		lb--;
	}
	return la == lb && strncmp(a, b, la) == 0;
}

bool
leia_mutter_spec_matches(const struct leia_panel_identity *id,
                         const char *vendor,
                         const char *product,
                         const char *serial,
                         bool use_serial)
{
	if (id == NULL || !id->valid || vendor == NULL || product == NULL) {
		return false;
	}
	if (!streq_trim(vendor, id->vendor)) {
		return false;
	}
	char code[16];
	snprintf(code, sizeof(code), "0x%04x", id->product_code);
	const bool product_ok = (id->name[0] != '\0' && streq_trim(product, id->name)) || streq_trim(product, code);
	if (!product_ok) {
		return false;
	}
	if (!use_serial) {
		return true;
	}
	if (serial == NULL) {
		return false;
	}
	char num[16];
	snprintf(num, sizeof(num), "0x%08x", id->serial_number);
	return (id->serial_str[0] != '\0' && streq_trim(serial, id->serial_str)) || streq_trim(serial, num);
}

void
leia_mutter_connector_from_drm(const char *drm, char *out, size_t out_len)
{
	// Mutter's KMS backend names connectors with the short DRM type names:
	// "HDMI-A" → "HDMI", "HDMI-B" → "HDMI-B", DVI-I/D/A keep their suffix,
	// everything else ("DP", "eDP", "DSI", ...) is unchanged.
	if (out_len == 0) {
		return;
	}
	out[0] = '\0';
	if (drm == NULL) {
		return;
	}
	int n;
	if (strncmp(drm, "HDMI-A-", 7) == 0) {
		n = snprintf(out, out_len, "HDMI-%s", drm + 7);
	} else {
		n = snprintf(out, out_len, "%s", drm);
	}
	if (n < 0 || (size_t)n >= out_len) {
		out[0] = '\0';
	}
}

void
leia_mutter_logical_extent(uint32_t mode_w,
                           uint32_t mode_h,
                           double scale,
                           uint32_t transform,
                           uint32_t layout_mode,
                           uint32_t *out_logical_w,
                           uint32_t *out_logical_h,
                           uint32_t *out_device_w,
                           uint32_t *out_device_h)
{
	// Rotation by 90/270 (and their flipped variants) swaps the axes.
	const bool swap = (transform & 1u) != 0;
	const uint32_t dw = swap ? mode_h : mode_w;
	const uint32_t dh = swap ? mode_w : mode_h;
	uint32_t lw = dw, lh = dh;
	// layout_mode 1 = LOGICAL: the stage is in logical px, a monitor spans
	// mode / scale of it. 2 = PHYSICAL: the stage is in device px already.
	if (layout_mode != 2 && scale > 0.0) {
		lw = (uint32_t)lround((double)dw / scale);
		lh = (uint32_t)lround((double)dh / scale);
	}
	*out_logical_w = lw;
	*out_logical_h = lh;
	*out_device_w = dw;
	*out_device_h = dh;
}

bool
leia_mutter_panel_identity_from_sysfs(struct leia_panel_identity *out)
{
	memset(out, 0, sizeof(*out));
	DIR *drm = opendir("/sys/class/drm");
	if (drm == NULL) {
		return false;
	}
	struct leia_panel_identity table_hit = {0}, external = {0};
	struct dirent *entry;
	while ((entry = readdir(drm)) != NULL) {
		// card<N>-<CONNECTOR>; skip cardN and renderD nodes.
		const char *dash = strchr(entry->d_name, '-');
		if (strncmp(entry->d_name, "card", 4) != 0 || dash == NULL) {
			continue;
		}
		char path[512];
		snprintf(path, sizeof(path), "/sys/class/drm/%s/edid", entry->d_name);
		FILE *f = fopen(path, "rb");
		if (f == NULL) {
			continue;
		}
		uint8_t edid[128];
		size_t n = fread(edid, 1, sizeof(edid), f);
		fclose(f);
		struct leia_panel_identity id;
		if (n < sizeof(edid) || !leia_edid_identity_parse(edid, dash + 1, &id)) {
			continue;
		}
		const uint16_t man = (uint16_t)(edid[8] | (edid[9] << 8));
		for (size_t i = 0; i < LEIA_EDID_TABLE_LEN && !table_hit.valid; i++) {
			if (leia_edid_table[i][0] == man && leia_edid_table[i][1] == id.product_code) {
				table_hit = id;
			}
		}
		const bool internal = strncmp(dash + 1, "eDP", 3) == 0 || strncmp(dash + 1, "LVDS", 4) == 0 ||
		                      strncmp(dash + 1, "DSI", 3) == 0;
		if (!internal && !external.valid) {
			external = id;
		}
	}
	closedir(drm);
	if (table_hit.valid) {
		*out = table_hit;
	} else if (external.valid) {
		*out = external;
	}
	return out->valid;
}

const char *
leia_mutter_exclude_status_str(enum leia_mutter_exclude_status s)
{
	switch (s) {
	case LEIA_MUTTER_EXCLUDE_OK: return "ok";
	case LEIA_MUTTER_EXCLUDE_ABSENT: return "extension absent";
	case LEIA_MUTTER_EXCLUDE_OUTDATED: return "extension predates CaptureExclusion1";
	default: return "error";
	}
}


#ifdef DXR_LEIA_HAVE_PIPEWIRE // (== dbus-1 available)

#include <dbus/dbus.h>
#include <time.h>

#define DISPLAYCONFIG_BUS "org.gnome.Mutter.DisplayConfig"
#define DISPLAYCONFIG_PATH "/org/gnome/Mutter/DisplayConfig"
#define SCREENCAST_BUS "org.gnome.Mutter.ScreenCast"
#define SCREENCAST_PATH "/org/gnome/Mutter/ScreenCast"
#define SCREENCAST_IFACE "org.gnome.Mutter.ScreenCast"
#define SESSION_IFACE "org.gnome.Mutter.ScreenCast.Session"
#define STREAM_IFACE "org.gnome.Mutter.ScreenCast.Stream"

/*
 * DisplayConfig.GetCurrentState():
 *   (u serial,
 *    a((ssss) spec, a(s id, i w, i h, d refresh, d preferred_scale, ad scales, a{sv} props) modes, a{sv} props) monitors,
 *    a(i x, i y, d scale, u transform, b primary, a(ssss) monitors, a{sv} props) logical_monitors,
 *    a{sv} props)
 */

#define MAX_MONITORS 16

struct dc_monitor
{
	char connector[64], vendor[16], product[64], serial[64];
	uint32_t mode_w, mode_h; //!< current mode (0 = monitor not active)
	bool builtin;
};

struct dc_logical
{
	int32_t x, y;
	double scale;
	uint32_t transform;
	char connector[64]; //!< first monitor of the logical monitor
};

static void
iter_get_string(DBusMessageIter *it, char *out, size_t len)
{
	const char *s = "";
	if (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_STRING) {
		dbus_message_iter_get_basic(it, &s);
	}
	snprintf(out, len, "%s", s != NULL ? s : "");
}

//! Scan an a{sv} for a boolean key; @p def when absent.
static bool
dict_get_bool(DBusMessageIter dict, const char *key, bool def)
{
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter e, v;
		dbus_message_iter_recurse(&dict, &e);
		const char *k = NULL;
		dbus_message_iter_get_basic(&e, &k);
		if (k != NULL && strcmp(k, key) == 0) {
			dbus_message_iter_next(&e);
			dbus_message_iter_recurse(&e, &v);
			if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN) {
				dbus_bool_t b = FALSE;
				dbus_message_iter_get_basic(&v, &b);
				return b != FALSE;
			}
			return def;
		}
		dbus_message_iter_next(&dict);
	}
	return def;
}

static uint32_t
dict_get_u32(DBusMessageIter dict, const char *key, uint32_t def)
{
	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter e, v;
		dbus_message_iter_recurse(&dict, &e);
		const char *k = NULL;
		dbus_message_iter_get_basic(&e, &k);
		if (k != NULL && strcmp(k, key) == 0) {
			dbus_message_iter_next(&e);
			dbus_message_iter_recurse(&e, &v);
			if (dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_UINT32) {
				uint32_t u = 0;
				dbus_message_iter_get_basic(&v, &u);
				return u;
			}
			return def;
		}
		dbus_message_iter_next(&dict);
	}
	return def;
}

//! (ssss) → connector/vendor/product/serial.
static void
parse_spec(DBusMessageIter *spec_struct, char *conn, char *vendor, char *product, char *serial)
{
	DBusMessageIter s;
	dbus_message_iter_recurse(spec_struct, &s);
	iter_get_string(&s, conn, 64);
	dbus_message_iter_next(&s);
	iter_get_string(&s, vendor, 16);
	dbus_message_iter_next(&s);
	iter_get_string(&s, product, 64);
	dbus_message_iter_next(&s);
	iter_get_string(&s, serial, 64);
}

static bool
parse_current_state(DBusMessage *reply,
                    struct dc_monitor *mons,
                    uint32_t *mon_count,
                    struct dc_logical *lms,
                    uint32_t *lm_count,
                    uint32_t *layout_mode)
{
	*mon_count = 0;
	*lm_count = 0;
	*layout_mode = 1;

	DBusMessageIter it;
	if (!dbus_message_iter_init(reply, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) {
		return false;
	}
	dbus_message_iter_next(&it); // skip serial

	// --- monitors ---
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
		return false;
	}
	DBusMessageIter ma;
	dbus_message_iter_recurse(&it, &ma);
	while (dbus_message_iter_get_arg_type(&ma) == DBUS_TYPE_STRUCT && *mon_count < MAX_MONITORS) {
		struct dc_monitor *m = &mons[*mon_count];
		memset(m, 0, sizeof(*m));
		DBusMessageIter ms;
		dbus_message_iter_recurse(&ma, &ms);
		parse_spec(&ms, m->connector, m->vendor, m->product, m->serial);
		dbus_message_iter_next(&ms);
		// modes a(siiddada{sv})
		DBusMessageIter modes;
		dbus_message_iter_recurse(&ms, &modes);
		while (dbus_message_iter_get_arg_type(&modes) == DBUS_TYPE_STRUCT) {
			DBusMessageIter md;
			dbus_message_iter_recurse(&modes, &md);
			dbus_message_iter_next(&md); // id
			int32_t w = 0, h = 0;
			dbus_message_iter_get_basic(&md, &w);
			dbus_message_iter_next(&md);
			dbus_message_iter_get_basic(&md, &h);
			dbus_message_iter_next(&md); // refresh
			dbus_message_iter_next(&md); // preferred scale
			dbus_message_iter_next(&md); // supported scales
			dbus_message_iter_next(&md); // props
			DBusMessageIter props;
			dbus_message_iter_recurse(&md, &props);
			if (dict_get_bool(props, "is-current", false)) {
				m->mode_w = (uint32_t)w;
				m->mode_h = (uint32_t)h;
			}
			dbus_message_iter_next(&modes);
		}
		dbus_message_iter_next(&ms);
		DBusMessageIter mprops;
		dbus_message_iter_recurse(&ms, &mprops);
		m->builtin = dict_get_bool(mprops, "is-builtin", false);
		(*mon_count)++;
		dbus_message_iter_next(&ma);
	}
	dbus_message_iter_next(&it);

	// --- logical monitors ---
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
		return false;
	}
	DBusMessageIter la;
	dbus_message_iter_recurse(&it, &la);
	while (dbus_message_iter_get_arg_type(&la) == DBUS_TYPE_STRUCT && *lm_count < MAX_MONITORS) {
		struct dc_logical *l = &lms[*lm_count];
		memset(l, 0, sizeof(*l));
		DBusMessageIter ls;
		dbus_message_iter_recurse(&la, &ls);
		dbus_message_iter_get_basic(&ls, &l->x);
		dbus_message_iter_next(&ls);
		dbus_message_iter_get_basic(&ls, &l->y);
		dbus_message_iter_next(&ls);
		dbus_message_iter_get_basic(&ls, &l->scale);
		dbus_message_iter_next(&ls);
		dbus_message_iter_get_basic(&ls, &l->transform);
		dbus_message_iter_next(&ls); // primary
		dbus_message_iter_next(&ls);
		DBusMessageIter specs;
		dbus_message_iter_recurse(&ls, &specs);
		if (dbus_message_iter_get_arg_type(&specs) == DBUS_TYPE_STRUCT) {
			char v[16], p[64], s[64];
			parse_spec(&specs, l->connector, v, p, s);
		}
		(*lm_count)++;
		dbus_message_iter_next(&la);
	}
	dbus_message_iter_next(&it);

	// --- global properties ---
	if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		DBusMessageIter gp;
		dbus_message_iter_recurse(&it, &gp);
		*layout_mode = dict_get_u32(gp, "layout-mode", 1);
	}
	return true;
}

bool
leia_mutter_find_panel(struct DBusConnection *conn,
                       const struct leia_panel_identity *id,
                       uint32_t expect_w,
                       uint32_t expect_h,
                       struct leia_mutter_panel *out)
{
	memset(out, 0, sizeof(*out));
	DBusMessage *call = dbus_message_new_method_call(DISPLAYCONFIG_BUS, DISPLAYCONFIG_PATH, DISPLAYCONFIG_BUS,
	                                                 "GetCurrentState");
	if (call == NULL) {
		return false;
	}
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 2000, &err);
	dbus_message_unref(call);
	if (reply == NULL) {
		U_LOG_W("leia_mutter: DisplayConfig.GetCurrentState failed: %s — not a GNOME/mutter session?",
		        dbus_error_is_set(&err) ? err.message : "no reply");
		dbus_error_free(&err);
		return false;
	}
	const bool ok = leia_mutter_panel_from_reply(reply, id, expect_w, expect_h, out);
	dbus_message_unref(reply);
	return ok;
}

bool
leia_mutter_request_layout(struct DBusConnection *conn, uint32_t *out_serial)
{
	DBusMessage *call = dbus_message_new_method_call(DISPLAYCONFIG_BUS, DISPLAYCONFIG_PATH, DISPLAYCONFIG_BUS,
	                                                 "GetCurrentState");
	if (call == NULL) {
		return false;
	}
	dbus_uint32_t serial = 0;
	const bool ok = dbus_connection_send(conn, call, &serial) != FALSE;
	dbus_connection_flush(conn);
	dbus_message_unref(call);
	*out_serial = serial;
	return ok && serial != 0;
}

bool
leia_mutter_panel_from_reply(struct DBusMessage *reply,
                             const struct leia_panel_identity *id,
                             uint32_t expect_w,
                             uint32_t expect_h,
                             struct leia_mutter_panel *out)
{
	memset(out, 0, sizeof(*out));
	if (reply == NULL || dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
		U_LOG_W("leia_mutter: DisplayConfig.GetCurrentState returned an error — not a GNOME/mutter session?");
		return false;
	}
	struct dc_monitor mons[MAX_MONITORS];
	struct dc_logical lms[MAX_MONITORS];
	uint32_t nm = 0, nl = 0, layout_mode = 1;
	const bool parsed = parse_current_state(reply, mons, &nm, lms, &nl, &layout_mode);
	if (!parsed) {
		U_LOG_W("leia_mutter: could not parse DisplayConfig.GetCurrentState");
		return false;
	}

	// Pick the panel's monitor (see header for the order).
	int pick = -1;
	const char *how = "";
	const char *forced = getenv("DXR_LEIA_PANEL_CONNECTOR");
	if (forced != NULL && forced[0] != '\0') {
		for (uint32_t i = 0; i < nm && pick < 0; i++) {
			if (strcmp(mons[i].connector, forced) == 0) {
				pick = (int)i;
				how = "DXR_LEIA_PANEL_CONNECTOR";
			}
		}
		if (pick < 0) {
			U_LOG_W("leia_mutter: DXR_LEIA_PANEL_CONNECTOR=%s names no mutter monitor", forced);
		}
	}
	if (pick < 0 && id != NULL && id->valid) {
		int hits = 0, first = -1;
		for (uint32_t i = 0; i < nm; i++) {
			if (leia_mutter_spec_matches(id, mons[i].vendor, mons[i].product, mons[i].serial, false)) {
				hits++;
				if (first < 0) {
					first = (int)i;
				}
			}
		}
		if (hits == 1) {
			pick = first;
			how = "EDID vendor+product";
		} else if (hits > 1) {
			for (uint32_t i = 0; i < nm && pick < 0; i++) {
				if (leia_mutter_spec_matches(id, mons[i].vendor, mons[i].product, mons[i].serial, true)) {
					pick = (int)i;
					how = "EDID vendor+product+serial";
				}
			}
		}
		if (pick < 0 && id->drm_connector[0] != '\0') {
			char name[64];
			leia_mutter_connector_from_drm(id->drm_connector, name, sizeof(name));
			for (uint32_t i = 0; i < nm && pick < 0; i++) {
				if (strcmp(mons[i].connector, name) == 0) {
					pick = (int)i;
					how = "DRM connector name";
				}
			}
		}
	}
	if (pick < 0 && expect_w > 0 && expect_h > 0) {
		int hits = 0, first = -1;
		for (uint32_t i = 0; i < nm; i++) {
			if (!mons[i].builtin && mons[i].mode_w == expect_w && mons[i].mode_h == expect_h) {
				hits++;
				if (first < 0) {
					first = (int)i;
				}
			}
		}
		if (hits == 1) {
			pick = first;
			how = "only external monitor at the panel's resolution";
		}
	}
	if (pick < 0) {
		U_LOG_W("leia_mutter: the 3D panel is not among mutter's %u monitor(s)%s%s", nm,
		        id != NULL && id->valid ? " — EDID " : "", id != NULL && id->valid ? id->vendor : "");
		return false;
	}
	const struct dc_monitor *m = &mons[pick];
	if (m->mode_w == 0 || m->mode_h == 0) {
		U_LOG_W("leia_mutter: panel monitor %s has no current mode (disabled?)", m->connector);
		return false;
	}
	const struct dc_logical *l = NULL;
	for (uint32_t i = 0; i < nl && l == NULL; i++) {
		if (strcmp(lms[i].connector, m->connector) == 0) {
			l = &lms[i];
		}
	}
	if (l == NULL) {
		U_LOG_W("leia_mutter: panel monitor %s is not in any logical monitor", m->connector);
		return false;
	}

	snprintf(out->connector, sizeof(out->connector), "%s", m->connector);
	out->logical_x = l->x;
	out->logical_y = l->y;
	out->scale = l->scale;
	out->transform = l->transform;
	out->layout_mode = layout_mode;
	leia_mutter_logical_extent(m->mode_w, m->mode_h, l->scale, l->transform, layout_mode, &out->logical_w,
	                           &out->logical_h, &out->device_w, &out->device_h);

	U_LOG_W("leia_mutter: panel = %s (%s %s, matched by %s): LOGICAL origin (%d,%d) extent %ux%u at scale "
	        "%.4f, layout-mode %s, transform %u -> DEVICE %ux%u",
	        out->connector, m->vendor, m->product, how, out->logical_x, out->logical_y, out->logical_w,
	        out->logical_h, out->scale, layout_mode == 2 ? "physical" : "logical", out->transform, out->device_w,
	        out->device_h);
	if (expect_w > 0 && expect_h > 0 && (expect_w != out->device_w || expect_h != out->device_h)) {
		U_LOG_W("leia_mutter: panel's current mode %ux%u differs from the display processor's panel "
		        "resolution %ux%u — the capture follows mutter's mode",
		        out->device_w, out->device_h, expect_w, expect_h);
	}
	return true;
}

static DBusMessage *
exclude_call(void)
{
	DBusMessage *call = dbus_message_new_method_call(LEIA_MUTTER_EXT_BUS, LEIA_MUTTER_EXT_EXCLUDE_PATH,
	                                                 LEIA_MUTTER_EXT_EXCLUDE_IFACE, "Exclude");
	if (call != NULL) {
		uint32_t self = 0; // 0 = "the calling process" — the extension resolves it from the bus
		dbus_message_append_args(call, DBUS_TYPE_UINT32, &self, DBUS_TYPE_INVALID);
	}
	return call;
}

static enum leia_mutter_exclude_status
exclude_status_from_error(DBusError *err)
{
	if (dbus_error_has_name(err, DBUS_ERROR_UNKNOWN_METHOD) || dbus_error_has_name(err, DBUS_ERROR_UNKNOWN_OBJECT) ||
	    dbus_error_has_name(err, DBUS_ERROR_UNKNOWN_INTERFACE)) {
		return LEIA_MUTTER_EXCLUDE_OUTDATED;
	}
	if (dbus_error_has_name(err, DBUS_ERROR_SERVICE_UNKNOWN) || dbus_error_has_name(err, DBUS_ERROR_NAME_HAS_NO_OWNER)) {
		return LEIA_MUTTER_EXCLUDE_ABSENT;
	}
	U_LOG_W("leia_mutter: CaptureExclusion1.Exclude failed: %s: %s", err->name ? err->name : "?",
	        err->message ? err->message : "?");
	return LEIA_MUTTER_EXCLUDE_ERROR;
}

enum leia_mutter_exclude_status
leia_mutter_capture_exclude(struct DBusConnection *conn, int timeout_ms, uint32_t *out_windows)
{
	DBusError err;
	dbus_error_init(&err);
	if (!dbus_bus_name_has_owner(conn, LEIA_MUTTER_EXT_BUS, &err)) {
		dbus_error_free(&err);
		return LEIA_MUTTER_EXCLUDE_ABSENT;
	}

	DBusMessage *call = exclude_call();
	if (call == NULL) {
		return LEIA_MUTTER_EXCLUDE_ERROR;
	}
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, timeout_ms, &err);
	dbus_message_unref(call);
	if (reply == NULL) {
		const enum leia_mutter_exclude_status st = exclude_status_from_error(&err);
		dbus_error_free(&err);
		return st;
	}
	const enum leia_mutter_exclude_status st = leia_mutter_capture_exclude_from_reply(reply, out_windows);
	dbus_message_unref(reply);
	return st;
}

bool
leia_mutter_capture_exclude_send(struct DBusConnection *conn, uint32_t *out_serial)
{
	DBusMessage *call = exclude_call();
	if (call == NULL) {
		return false;
	}
	dbus_uint32_t serial = 0;
	const bool ok = dbus_connection_send(conn, call, &serial) != FALSE;
	dbus_connection_flush(conn);
	dbus_message_unref(call);
	*out_serial = serial;
	return ok && serial != 0;
}

enum leia_mutter_exclude_status
leia_mutter_capture_exclude_from_reply(struct DBusMessage *reply, uint32_t *out_windows)
{
	if (reply == NULL) {
		return LEIA_MUTTER_EXCLUDE_ERROR;
	}
	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		DBusError err;
		dbus_error_init(&err);
		dbus_set_error_from_message(&err, reply);
		const enum leia_mutter_exclude_status st = exclude_status_from_error(&err);
		dbus_error_free(&err);
		return st;
	}
	uint32_t windows = 0;
	dbus_message_get_args(reply, NULL, DBUS_TYPE_UINT32, &windows, DBUS_TYPE_INVALID);
	if (out_windows != NULL) {
		*out_windows = windows;
	}
	return LEIA_MUTTER_EXCLUDE_OK;
}

//! Call a method returning one object path; returns a malloc'd copy or NULL.
static char *
call_returning_path(DBusConnection *conn, DBusMessage *call, const char *what)
{
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 3000, &err);
	dbus_message_unref(call);
	if (reply == NULL) {
		U_LOG_W("leia_mutter: %s failed: %s", what, dbus_error_is_set(&err) ? err.message : "no reply");
		dbus_error_free(&err);
		return NULL;
	}
	const char *path = NULL;
	char *copy = NULL;
	if (dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID) && path != NULL) {
		copy = strdup(path);
	}
	dbus_message_unref(reply);
	return copy;
}

static void
append_dict_u32(DBusMessageIter *dict, const char *key, uint32_t value)
{
	DBusMessageIter entry, variant;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "u", &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(dict, &entry);
}

static uint64_t
now_ms(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

bool
leia_mutter_screencast_start(struct DBusConnection *conn,
                             const struct leia_mutter_panel *panel,
                             int timeout_ms,
                             char **out_session,
                             char **out_stream,
                             uint32_t *out_node)
{
	*out_session = NULL;
	*out_stream = NULL;
	*out_node = 0;

	// CreateSession(a{sv}) -> (o)
	DBusMessage *call = dbus_message_new_method_call(SCREENCAST_BUS, SCREENCAST_PATH, SCREENCAST_IFACE,
	                                                 "CreateSession");
	if (call == NULL) {
		return false;
	}
	{
		DBusMessageIter args, dict;
		dbus_message_iter_init_append(call, &args);
		dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
		dbus_message_iter_close_container(&args, &dict);
	}
	char *session = call_returning_path(conn, call, "ScreenCast.CreateSession");
	if (session == NULL) {
		return false;
	}

	// RecordArea(i x, i y, i w, i h, a{sv}) -> (o). LOGICAL coordinates: the
	// panel's FULL logical rectangle — the area is fixed at creation, so we
	// record the panel, not the window, and crop per frame.
	call = dbus_message_new_method_call(SCREENCAST_BUS, session, SESSION_IFACE, "RecordArea");
	if (call == NULL) {
		leia_mutter_screencast_stop(conn, session);
		free(session);
		return false;
	}
	{
		int32_t x = panel->logical_x, y = panel->logical_y;
		int32_t w = (int32_t)panel->logical_w, h = (int32_t)panel->logical_h;
		DBusMessageIter args, dict;
		dbus_message_iter_init_append(call, &args);
		dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &x);
		dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &y);
		dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &w);
		dbus_message_iter_append_basic(&args, DBUS_TYPE_INT32, &h);
		dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
		append_dict_u32(&dict, "cursor-mode", 0u); // hidden: a cursor is not desktop background
		dbus_message_iter_close_container(&args, &dict);
	}
	char *stream = call_returning_path(conn, call, "ScreenCast.Session.RecordArea");
	if (stream == NULL) {
		leia_mutter_screencast_stop(conn, session);
		free(session);
		return false;
	}

	// Match the stream's PipeWireStreamAdded and the session's Closed BEFORE
	// Start, so neither can be missed.
	char rule[512];
	DBusError err;
	dbus_error_init(&err);
	snprintf(rule, sizeof(rule), "type='signal',interface='" STREAM_IFACE "',member='PipeWireStreamAdded',path='%s'",
	         stream);
	dbus_bus_add_match(conn, rule, &err);
	dbus_error_free(&err);
	snprintf(rule, sizeof(rule), "type='signal',interface='" SESSION_IFACE "',member='Closed',path='%s'", session);
	dbus_bus_add_match(conn, rule, &err);
	dbus_error_free(&err);

	call = dbus_message_new_method_call(SCREENCAST_BUS, session, SESSION_IFACE, "Start");
	DBusMessage *reply = call ? dbus_connection_send_with_reply_and_block(conn, call, 3000, &err) : NULL;
	if (call != NULL) {
		dbus_message_unref(call);
	}
	if (reply == NULL) {
		U_LOG_W("leia_mutter: ScreenCast.Session.Start failed: %s", dbus_error_is_set(&err) ? err.message : "?");
		dbus_error_free(&err);
		leia_mutter_screencast_stop(conn, session);
		free(stream);
		free(session);
		return false;
	}
	dbus_message_unref(reply);

	// Wait (bounded) for PipeWireStreamAdded(u node_id) on our stream.
	const uint64_t deadline = now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 3000);
	bool got = false;
	while (!got && now_ms() < deadline) {
		if (!dbus_connection_read_write(conn, 20)) {
			break;
		}
		DBusMessage *msg;
		while (!got && (msg = dbus_connection_pop_message(conn)) != NULL) {
			if (dbus_message_is_signal(msg, STREAM_IFACE, "PipeWireStreamAdded") &&
			    dbus_message_has_path(msg, stream)) {
				uint32_t node = 0;
				if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &node, DBUS_TYPE_INVALID)) {
					*out_node = node;
					got = true;
				}
			}
			dbus_message_unref(msg);
		}
	}
	if (!got) {
		U_LOG_W("leia_mutter: no PipeWireStreamAdded for %s within %d ms", stream, timeout_ms);
		leia_mutter_screencast_stop(conn, session);
		free(stream);
		free(session);
		return false;
	}
	*out_session = session;
	*out_stream = stream;
	return true;
}

void
leia_mutter_screencast_stop(struct DBusConnection *conn, const char *session)
{
	if (conn == NULL || session == NULL) {
		return;
	}
	DBusMessage *call = dbus_message_new_method_call(SCREENCAST_BUS, session, SESSION_IFACE, "Stop");
	if (call == NULL) {
		return;
	}
	dbus_message_set_no_reply(call, TRUE);
	dbus_connection_send(conn, call, NULL);
	dbus_connection_flush(conn);
	dbus_message_unref(call);
}

#else // !DXR_LEIA_HAVE_PIPEWIRE

bool
leia_mutter_find_panel(struct DBusConnection *conn,
                       const struct leia_panel_identity *id,
                       uint32_t expect_w,
                       uint32_t expect_h,
                       struct leia_mutter_panel *out)
{
	(void)conn;
	(void)id;
	(void)expect_w;
	(void)expect_h;
	memset(out, 0, sizeof(*out));
	return false;
}

enum leia_mutter_exclude_status
leia_mutter_capture_exclude(struct DBusConnection *conn, int timeout_ms, uint32_t *out_windows)
{
	(void)conn;
	(void)timeout_ms;
	(void)out_windows;
	return LEIA_MUTTER_EXCLUDE_ABSENT;
}

bool
leia_mutter_screencast_start(struct DBusConnection *conn,
                             const struct leia_mutter_panel *panel,
                             int timeout_ms,
                             char **out_session,
                             char **out_stream,
                             uint32_t *out_node)
{
	(void)conn;
	(void)panel;
	(void)timeout_ms;
	*out_session = NULL;
	*out_stream = NULL;
	*out_node = 0;
	return false;
}

void
leia_mutter_screencast_stop(struct DBusConnection *conn, const char *session)
{
	(void)conn;
	(void)session;
}

bool
leia_mutter_capture_exclude_send(struct DBusConnection *conn, uint32_t *out_serial)
{
	(void)conn;
	*out_serial = 0;
	return false;
}

enum leia_mutter_exclude_status
leia_mutter_capture_exclude_from_reply(struct DBusMessage *reply, uint32_t *out_windows)
{
	(void)reply;
	(void)out_windows;
	return LEIA_MUTTER_EXCLUDE_ABSENT;
}

bool
leia_mutter_request_layout(struct DBusConnection *conn, uint32_t *out_serial)
{
	(void)conn;
	*out_serial = 0;
	return false;
}

bool
leia_mutter_panel_from_reply(struct DBusMessage *reply,
                             const struct leia_panel_identity *id,
                             uint32_t expect_w,
                             uint32_t expect_h,
                             struct leia_mutter_panel *out)
{
	(void)reply;
	(void)id;
	(void)expect_w;
	(void)expect_h;
	memset(out, 0, sizeof(*out));
	return false;
}

#endif // DXR_LEIA_HAVE_PIPEWIRE
