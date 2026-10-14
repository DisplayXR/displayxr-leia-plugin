// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
/*!
 * @file
 * @brief  Off-render-thread start/stop for the Linux desktop capture (see the header).
 * @ingroup drv_leia_linux
 */

#include "leia_bg_capture_worker_linux.h"

#include "util/u_logging.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef DXR_LEIA_HAVE_PIPEWIRE
#include "leia_mutter_capture_linux.h" // LEIA_MUTTER_EXT_BUS
#include <dbus/dbus.h>
#endif

#define WORKER_QUEUE 8

//! While the unlock watch is on, the worker wakes this often to pump it.
#define WATCH_PUMP_MS 250
//! A failed watch connection is retried no sooner than this.
#define WATCH_REOPEN_MS 5000

enum job_type
{
	JOB_CREATE,
	JOB_DESTROY,
};

struct job
{
	enum job_type type;
	struct leia_bg_capture_linux *capture; //!< JOB_DESTROY
	uint32_t panel_w, panel_h;             //!< JOB_CREATE
	bool recreate;                         //!< JOB_CREATE: re-create after a lost capture (#364)
};

struct leia_bg_capture_worker
{
	struct vk_bundle *vk;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool quit;

	struct job queue[WORKER_QUEUE];
	uint32_t head, count;

	//! A finished start, waiting for poll_created.
	bool created_done;
	struct leia_bg_capture_linux *created;
	const char *created_fail_reason; //!< static string when created == NULL

	// Session-unlock watch (#364). Wanted/seen are shared (under lock); the
	// connection itself is touched by the worker thread only.
	bool watch_wanted;
	bool unlock_seen;
#ifdef DXR_LEIA_HAVE_PIPEWIRE
	DBusConnection *watch_conn;
	uint64_t watch_reopen_ms; //!< earliest retry after a failed open
#endif
};

static uint64_t
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

#ifdef DXR_LEIA_HAVE_PIPEWIRE
#define SCREENSAVER_RULE "type='signal',interface='org.gnome.ScreenSaver',member='ActiveChanged'"
#define EXT_BACK_RULE                                                                                                  \
	"type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',"      \
	"arg0='" LEIA_MUTTER_EXT_BUS "'"

static void
watch_close(struct leia_bg_capture_worker *w)
{
	if (w->watch_conn != NULL) {
		dbus_connection_close(w->watch_conn);
		dbus_connection_unref(w->watch_conn);
		w->watch_conn = NULL;
	}
}

/*!
 * Worker thread, lock NOT held: open/close the watch connection to match
 * @p wanted and drain it without blocking. Returns true if the session was
 * unlocked since the last pump.
 *
 * Two signals mean "unlocked": gnome-shell's org.gnome.ScreenSaver
 * ActiveChanged(false), and the DisplayXR extension's bus name coming back —
 * GNOME disables user extensions while the shield is up and re-enables them on
 * unlock, and the extension is what a re-created capture needs anyway.
 */
static bool
watch_service(struct leia_bg_capture_worker *w, bool wanted)
{
	if (!wanted) {
		watch_close(w);
		return false;
	}
	if (w->watch_conn == NULL) {
		if (mono_ms() < w->watch_reopen_ms) {
			return false;
		}
		DBusError err;
		dbus_error_init(&err);
		w->watch_conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
		if (w->watch_conn == NULL) {
			dbus_error_free(&err);
			w->watch_reopen_ms = mono_ms() + WATCH_REOPEN_MS;
			return false; // backoff still re-creates the capture; unlock just makes it prompt
		}
		dbus_connection_set_exit_on_disconnect(w->watch_conn, FALSE);
		dbus_bus_add_match(w->watch_conn, SCREENSAVER_RULE, &err);
		dbus_error_free(&err);
		dbus_bus_add_match(w->watch_conn, EXT_BACK_RULE, &err);
		dbus_error_free(&err);
	}
	if (!dbus_connection_read_write(w->watch_conn, 0)) {
		watch_close(w);
		w->watch_reopen_ms = mono_ms() + WATCH_REOPEN_MS;
		return false;
	}
	bool unlocked = false;
	DBusMessage *msg;
	while ((msg = dbus_connection_pop_message(w->watch_conn)) != NULL) {
		if (dbus_message_is_signal(msg, "org.gnome.ScreenSaver", "ActiveChanged")) {
			dbus_bool_t active = TRUE;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &active, DBUS_TYPE_INVALID) && !active) {
				unlocked = true;
			}
		} else if (dbus_message_is_signal(msg, "org.freedesktop.DBus", "NameOwnerChanged")) {
			const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old_owner,
			                          DBUS_TYPE_STRING, &new_owner, DBUS_TYPE_INVALID) &&
			    name != NULL && strcmp(name, LEIA_MUTTER_EXT_BUS) == 0 && new_owner != NULL &&
			    new_owner[0] != '\0') {
				unlocked = true;
			}
		}
		dbus_message_unref(msg);
	}
	return unlocked;
}
#else
static bool
watch_service(struct leia_bg_capture_worker *w, bool wanted)
{
	(void)w;
	(void)wanted;
	return false;
}
#endif

static bool
watch_open(const struct leia_bg_capture_worker *w)
{
#ifdef DXR_LEIA_HAVE_PIPEWIRE
	return w->watch_conn != NULL;
#else
	(void)w;
	return false;
#endif
}

static void *
worker_main(void *arg)
{
	struct leia_bg_capture_worker *w = arg;
	pthread_mutex_lock(&w->lock);
	for (;;) {
		while (w->count == 0 && !w->quit) {
			if (!w->watch_wanted && !watch_open(w)) {
				pthread_cond_wait(&w->cond, &w->lock);
				continue;
			}
			// Unlock watch on (a capture is being recovered): pump it, then
			// sleep until the next pump or a posted job, whichever is first.
			const bool wanted = w->watch_wanted;
			pthread_mutex_unlock(&w->lock);
			const bool unlocked = watch_service(w, wanted);
			pthread_mutex_lock(&w->lock);
			if (unlocked) {
				w->unlock_seen = true;
			}
			if (w->count != 0 || w->quit || (!w->watch_wanted && !watch_open(w))) {
				continue;
			}
			struct timespec until;
			timespec_get(&until, TIME_UTC);
			until.tv_nsec += (long)WATCH_PUMP_MS * 1000000L;
			if (until.tv_nsec >= 1000000000L) {
				until.tv_sec += 1;
				until.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&w->cond, &w->lock, &until);
		}
		if (w->count == 0 && w->quit) {
			break;
		}
		struct job j = w->queue[w->head];
		w->head = (w->head + 1) % WORKER_QUEUE;
		w->count--;
		pthread_mutex_unlock(&w->lock);

		if (j.type == JOB_DESTROY) {
			const uint64_t t0 = mono_ms();
			leia_bg_capture_linux_destroy(j.capture);
			U_LOG_W("leia_bg_capture_worker: desktop capture stopped (%llu ms, off the frame thread)",
			        (unsigned long long)(mono_ms() - t0));
			pthread_mutex_lock(&w->lock);
			continue;
		}

		const uint64_t t0 = mono_ms();
		const char *why = NULL;
		struct leia_bg_capture_linux *c =
		    leia_bg_capture_linux_create_ex(w->vk, j.panel_w, j.panel_h, j.recreate, &why);
		if (!j.recreate) {
			U_LOG_W("leia_bg_capture_worker: desktop capture start %s (%llu ms, off the frame thread)",
			        c != NULL ? "succeeded" : "declined", (unsigned long long)(mono_ms() - t0));
		} else {
			// The DP logs one line per re-create attempt (#364); this one is detail.
			U_LOG_D("leia_bg_capture_worker: desktop capture re-create %s (%llu ms)",
			        c != NULL ? "succeeded" : "declined", (unsigned long long)(mono_ms() - t0));
		}

		pthread_mutex_lock(&w->lock);
		if (w->created_done && w->created != NULL) {
			// Unclaimed older result (the caller keeps one start in flight,
			// so this is defensive): never leak a live ScreenCast session.
			struct leia_bg_capture_linux *stale = w->created;
			pthread_mutex_unlock(&w->lock);
			leia_bg_capture_linux_destroy(stale);
			pthread_mutex_lock(&w->lock);
		}
		w->created = c;
		w->created_fail_reason = c != NULL ? NULL : why;
		w->created_done = true;
	}
	// Quitting: a start nobody will claim must be stopped here.
	struct leia_bg_capture_linux *orphan = w->created_done ? w->created : NULL;
	w->created = NULL;
	w->created_done = false;
	pthread_mutex_unlock(&w->lock);
	if (orphan != NULL) {
		leia_bg_capture_linux_destroy(orphan);
	}
	watch_service(w, false);
	return NULL;
}

static void
post(struct leia_bg_capture_worker *w, const struct job *j)
{
	pthread_mutex_lock(&w->lock);
	if (w->count == WORKER_QUEUE) {
		// Cannot happen with one start in flight and a handful of retirees;
		// if it ever does, run a stop inline rather than leak a session.
		pthread_mutex_unlock(&w->lock);
		U_LOG_W("leia_bg_capture_worker: job queue full — running job on the caller's thread");
		if (j->type == JOB_DESTROY) {
			leia_bg_capture_linux_destroy(j->capture);
		}
		return;
	}
	w->queue[(w->head + w->count) % WORKER_QUEUE] = *j;
	w->count++;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);
}

struct leia_bg_capture_worker *
leia_bg_capture_worker_create(struct vk_bundle *vk)
{
	struct leia_bg_capture_worker *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		return NULL;
	}
	w->vk = vk;
	if (pthread_mutex_init(&w->lock, NULL) != 0) {
		free(w);
		return NULL;
	}
	if (pthread_cond_init(&w->cond, NULL) != 0) {
		pthread_mutex_destroy(&w->lock);
		free(w);
		return NULL;
	}
	if (pthread_create(&w->thread, NULL, worker_main, w) != 0) {
		pthread_cond_destroy(&w->cond);
		pthread_mutex_destroy(&w->lock);
		free(w);
		return NULL;
	}
	return w;
}

void
leia_bg_capture_worker_request_create(struct leia_bg_capture_worker *w,
                                      uint32_t panel_px_w,
                                      uint32_t panel_px_h,
                                      bool recreate)
{
	if (w == NULL) {
		return;
	}
	struct job j = {.type = JOB_CREATE, .panel_w = panel_px_w, .panel_h = panel_px_h, .recreate = recreate};
	post(w, &j);
}

bool
leia_bg_capture_worker_poll_created(struct leia_bg_capture_worker *w,
                                    struct leia_bg_capture_linux **out_capture,
                                    const char **out_fail_reason)
{
	if (w == NULL) {
		return false;
	}
	pthread_mutex_lock(&w->lock);
	const bool done = w->created_done;
	if (done) {
		*out_capture = w->created;
		if (out_fail_reason != NULL) {
			*out_fail_reason = w->created_fail_reason != NULL ? w->created_fail_reason : "declined";
		}
		w->created = NULL;
		w->created_fail_reason = NULL;
		w->created_done = false;
	}
	pthread_mutex_unlock(&w->lock);
	return done;
}

void
leia_bg_capture_worker_watch_unlock(struct leia_bg_capture_worker *w, bool on)
{
	if (w == NULL) {
		return;
	}
	pthread_mutex_lock(&w->lock);
	if (w->watch_wanted != on) {
		w->watch_wanted = on;
		w->unlock_seen = false;
		pthread_cond_signal(&w->cond);
	}
	pthread_mutex_unlock(&w->lock);
}

bool
leia_bg_capture_worker_take_unlock(struct leia_bg_capture_worker *w)
{
	if (w == NULL) {
		return false;
	}
	pthread_mutex_lock(&w->lock);
	const bool seen = w->unlock_seen;
	w->unlock_seen = false;
	pthread_mutex_unlock(&w->lock);
	return seen;
}

void
leia_bg_capture_worker_retire(struct leia_bg_capture_worker *w, struct leia_bg_capture_linux *capture)
{
	if (capture == NULL) {
		return;
	}
	if (w == NULL) {
		leia_bg_capture_linux_destroy(capture);
		return;
	}
	struct job j = {.type = JOB_DESTROY, .capture = capture};
	post(w, &j);
}

void
leia_bg_capture_worker_destroy(struct leia_bg_capture_worker **w_ptr)
{
	struct leia_bg_capture_worker *w = w_ptr != NULL ? *w_ptr : NULL;
	if (w == NULL) {
		return;
	}
	pthread_mutex_lock(&w->lock);
	w->quit = true;
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);
	pthread_join(w->thread, NULL);
	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->lock);
	free(w);
	*w_ptr = NULL;
}
