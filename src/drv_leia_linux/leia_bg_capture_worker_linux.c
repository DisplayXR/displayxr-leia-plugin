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

#define WORKER_QUEUE 8

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
};

static uint64_t
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void *
worker_main(void *arg)
{
	struct leia_bg_capture_worker *w = arg;
	pthread_mutex_lock(&w->lock);
	for (;;) {
		while (w->count == 0 && !w->quit) {
			pthread_cond_wait(&w->cond, &w->lock);
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
		struct leia_bg_capture_linux *c = leia_bg_capture_linux_create(w->vk, j.panel_w, j.panel_h);
		U_LOG_W("leia_bg_capture_worker: desktop capture start %s (%llu ms, off the frame thread)",
		        c != NULL ? "succeeded" : "declined", (unsigned long long)(mono_ms() - t0));

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
leia_bg_capture_worker_request_create(struct leia_bg_capture_worker *w, uint32_t panel_px_w, uint32_t panel_px_h)
{
	if (w == NULL) {
		return;
	}
	struct job j = {.type = JOB_CREATE, .panel_w = panel_px_w, .panel_h = panel_px_h};
	post(w, &j);
}

bool
leia_bg_capture_worker_poll_created(struct leia_bg_capture_worker *w, struct leia_bg_capture_linux **out_capture)
{
	if (w == NULL) {
		return false;
	}
	pthread_mutex_lock(&w->lock);
	const bool done = w->created_done;
	if (done) {
		*out_capture = w->created;
		w->created = NULL;
		w->created_done = false;
	}
	pthread_mutex_unlock(&w->lock);
	return done;
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
