/*
OpenIO SDS event queue
Copyright (C) 2023 OVH SAS

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3.0 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.
*/

#include <glib.h>

#include <librdkafka/rdkafka.h>

#include <core/oio_core.h>
#include <events/events_variables.h>

#include "kafka.h"
#include "oio_events_queue.h"
#include "oio_events_queue_internals.h"
#include "oio_events_queue_kafka.h"
#include "oio_events_queue_buffer.h"
#include "oio_events_queue_shared.h"

static GError * _q_start (struct oio_events_queue_s *self);


static struct oio_events_queue_vtable_s vtable_KAFKA =
{
	.destroy = _q_destroy,
	.send = _q_send,
	.send_overwritable = _q_send_overwritable,
	.is_stalled = _q_is_stalled,
	.get_total_send_time = _q_get_total_send_time,
	.get_total_sent_events = _q_get_total_sent_events,
	.get_health = _q_get_health,
	.set_buffering = _q_set_buffering,
	.start = _q_start,
	.flush_overwritable = _q_flush_overwritable,
};

#ifdef HAVE_EXTRA_DEBUG
static _queue_BEANSTALKD_intercept_error_f intercept_errors = NULL;
#endif

GError *
oio_events_queue_factory__create_kafka(
		const char *endpoint, const char *topic,
		struct oio_events_queue_s **out)
{
	EXTRA_ASSERT(endpoint != NULL);
	EXTRA_ASSERT(out != NULL);
	*out = NULL;

	struct _queue_with_endpoint_s *self = g_malloc0(sizeof(*self));
	self->vtable = &vtable_KAFKA;
	self->queue = g_async_queue_new();
	self->endpoint = g_strdup(endpoint);
	self->queue_name = g_strdup(topic);
	self->running = FALSE;
	self->healthy = FALSE;

	oio_events_queue_buffer_init(&(self->buffer));
	self->event_send_count = grid_single_rrd_create(
			oio_ext_monotonic_seconds(), OIO_EVENTS_STATS_HISTORY_SECONDS);
	self->event_send_time = grid_single_rrd_create(
			oio_ext_monotonic_seconds(), OIO_EVENTS_STATS_HISTORY_SECONDS);

	*out = (struct oio_events_queue_s*) self;

	return NULL;
}

// --------------------------------------------------------

struct _running_ctx_s {
	gint64 last_flush;
	gint64 last_check;
	gint64 now;
	guint attempts_connect;
	guint attempts_check;
	guint attempts_put;
	struct kafka_s* kafka;
};


/**
 * Poll the next message and manage it.
 * Returns TRUE if the loop might continue or FALSE it the loop should
 * pause a bit.
 */
static gboolean
_q_manage_message(struct _queue_with_endpoint_s *q, struct _running_ctx_s *ctx)
{
	EXTRA_ASSERT(ctx->kafka != NULL);

	gboolean rc = TRUE;
	gchar* msg = g_async_queue_timeout_pop (q->queue, 200 * G_TIME_SPAN_MILLISECOND);
	if (!msg) goto exit;
	if (!*msg) goto exit;

	/* forward the event as a beanstalkd job */
	const size_t msglen = strlen(msg);
	gint64 start = oio_ext_monotonic_time();
	GError *err = kafka_publish_message(ctx->kafka, msg, msglen, q->queue_name);
	gint64 end = oio_ext_monotonic_time();
	time_t end_seconds = end / G_TIME_SPAN_SECOND;
	/* count the operation whether it's a success or a failure */
	grid_single_rrd_add(q->event_send_count, end_seconds, 1);
	grid_single_rrd_add(q->event_send_time, end_seconds, end - start);
#ifdef HAVE_EXTRA_DEBUG
	if (intercept_errors)
		(*intercept_errors) (err);
#endif
	if (!err) {
		ctx->attempts_put = 0;
	} else {
		if (CODE_IS_RETRY(err->code) || CODE_IS_NETWORK_ERROR(err->code)) {
			GRID_NOTICE("Kafka recoverable error with [%s]: (%d) %s",
					q->endpoint, err->code, err->message);
			g_async_queue_push_front(q->queue, msg);
			msg = NULL;
			ctx->attempts_put += 1;
			rc = FALSE;
		} else {
			GRID_WARN("Kafka unrecoverable error with [%s]: (%d) %s",
					q->endpoint, err->code, err->message);
			_event_dropped(msg, msglen);
			ctx->attempts_put = 0;
		}
		g_clear_error (&err);
	}

exit:
	oio_str_clean (&msg);
	return rc;
}


static GError *
_q_run(struct _queue_with_endpoint_s *q)
{
	GError *err = NULL;
	struct _running_ctx_s ctx = {0};
	err = kafka_create(
			q->endpoint, q->queue_name, &(ctx.kafka));

	if (err){
		return err;
	}

	/* Loop until the (asked) end or until there is no event */
	while (_q_is_running(q)) {
		ctx.now = oio_ext_monotonic_time();

		/* Maybe do a periodic flush of buffered/delayed events. */
		if (ctx.now - ctx.last_flush > q->buffer.delay / 10) {
			ctx.last_flush = ctx.now;
			_q_flush_buffered(q, FALSE);
		}
		if (!_q_manage_message(q, &ctx)) {
			EXPO_BACKOFF(100 * G_TIME_SPAN_MILLISECOND, ctx.attempts_put, 5);
		}
	}

	/* Exit phase */
	const gint64 deadline_exit = oio_ext_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
	while (!_q_is_empty(q)) {
		ctx.now = oio_ext_monotonic_time();
		GRID_WARN("exiting...");

		/* The exit phase doesn't last forever */
		if (ctx.now > deadline_exit)
			break;

		_q_flush_buffered(q, TRUE);

		if (!_q_manage_message(q, &ctx)) {
			g_usleep(100 * G_TIME_SPAN_MILLISECOND);
		}
	}

	_q_flush_pending(q);

	/* close the socket to the kafka broker */
	kafka_destroy(ctx.kafka);

	return NULL;
}

static gpointer
_q_worker(gpointer p)
{
	metautils_ignore_signals();
	GError *err = _q_run((struct _queue_with_endpoint_s*)p);
	if (err) {
		GRID_WARN("Events queue run error: (%d) %s", err->code, err->message);
		g_clear_error(&err);
	}
	return p;
}

static GError *
_q_start (struct oio_events_queue_s *self)
{
	struct _queue_with_endpoint_s *q = (struct _queue_with_endpoint_s*) self;
	g_assert_nonnull(q);
	g_assert(q->vtable == &vtable_KAFKA);
	g_assert_null(q->worker);

	GError *err = NULL;

	q->running = TRUE;
	q->healthy = TRUE;
	q->worker = g_thread_try_new("event|kafka", _q_worker, q, &err);
	if (!q->worker) {
		GRID_WARN("%s worker startup error: (%d) %s", __FUNCTION__,
				err ? err->code : 0, err ? err->message : "");
	}
	return err;
}
