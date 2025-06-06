/*
OpenIO SDS sqliterepo
Copyright (C) 2014 Worldline, as part of Redcurrant
Copyright (C) 2015-2020 OpenIO SAS, as part of OpenIO SDS
Copyright (C) 2021-2025 OVH SAS

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

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <errno.h>


#include <metautils/lib/metautils.h>
#include <sqliterepo/sqliterepo_variables.h>
#include <metautils/lib/common_variables.h>

#include "sqliterepo.h"
#include "cache.h"
#include "internals.h"

#define GET(R,I) ((R)->bases + (I))

#define EXCESSIVE_LOAD(AVG_WAITING_TIME, DEADLINE_REACHED) NEWERROR( \
		CODE_EXCESSIVE_LOAD, \
		"Load too high (waiting_requests=%"G_GUINT32_FORMAT", " \
		"avg_waiting_time=%.6lf, deadline_reached=%s)", \
		base->count_waiting, AVG_WAITING_TIME, DEADLINE_REACHED)

#define BEACON_RESET(B) do { (B)->first = (B)->last = -1; } while (0)

struct beacon_s
{
	gint first;
	gint last;
};

enum sqlx_base_status_e
{
	SQLX_BASE_FREE=1,
	SQLX_BASE_IDLE,	  /*!< without user */
	SQLX_BASE_IDLE_HOT,	  /*!< without user */
	SQLX_BASE_USED,	  /*!< with users. count_open then
						 * tells how many threads have marked the base
						 * to be kept open, and owner tells if the lock
						 * os currently owned by a thread. */
	SQLX_BASE_CLOSING, // base being closed, wait for notification and retry on it
	SQLX_BASE_CLOSING_FOR_DELETION, // base being deleted
};

struct sqlx_base_s
{
	hashstr_t *name; /*!< This is registered in the DB */

	GThread *owner; /*!< The current owner of the database. Changed under the
					  global lock */
	GCond cond;
	GCond cond_prio;

	gpointer handle;

	gint64 last_update; /*!< Changed under the global lock */

	struct {
		gint prev;
		gint next;
	} link; /*< Used to build a doubly-linked list */

	guint32 heat;

	guint32 count_open; /*!< Counts the number of times this base has been
						  explicitly opened and locked by the user. */

	guint32 count_waiting; /*!< Counts the number of threads waiting for the
							base to become available. */

	gint index; /*!< self reference */

	enum sqlx_base_status_e status; /*!< Changed under the global lock */

	struct grid_single_rrd_s *open_attempts;
	struct grid_single_rrd_s *open_wait_time;
};

typedef struct sqlx_base_s sqlx_base_t;

struct sqlx_cache_s
{
	GMutex lock;
	GTree *bases_by_name;
	sqlx_base_t *bases;
	guint bases_max_soft;
	guint bases_max_hard;
	guint bases_used;

	gboolean is_running;
	gint64 last_memory_usage;

	/* Doubly linked lists of tables, one by status */
	struct beacon_s beacon_free;
	struct beacon_s beacon_idle;
	struct beacon_s beacon_idle_hot;
	struct beacon_s beacon_used;

	sqlx_cache_unlock_hook unlock_hook;
	sqlx_cache_close_hook close_hook;
};

/* ------------------------------------------------------------------------- */

static gboolean
_ram_exhausted(sqlx_cache_t *cache)
{
	if (sqliterepo_max_rss) {
		gint64 total = cache->last_memory_usage;
		if (total > sqliterepo_max_rss) {
			GRID_DEBUG("RAM [MiB] used %" G_GINT64_FORMAT" max %" G_GINT64_FORMAT,
					total / (1024 * 1024),
					sqliterepo_max_rss / (1024 * 1024));
			return TRUE;
		} else {
			GRID_TRACE2("RAM [MiB] used %" G_GINT64_FORMAT" max %" G_GINT64_FORMAT,
					total / (1024 * 1024),
					sqliterepo_max_rss / (1024 * 1024));
			return FALSE;
		}
	}
	return FALSE;
}

static gboolean
base_id_out(sqlx_cache_t *cache, gint bd)
{
	return (bd < 0) || ((guint)bd) >= cache->bases_max_hard;
}

#ifdef HAVE_EXTRA_DEBUG
static const gchar *
sqlx_status_to_str(enum sqlx_base_status_e status)
{
	switch (status) {
		case SQLX_BASE_FREE:
			return "FREE";
		case SQLX_BASE_IDLE:
			return "IDLE";
		case SQLX_BASE_IDLE_HOT:
			return "IDLE_HOT";
		case SQLX_BASE_USED:
			return "USED";
		case SQLX_BASE_CLOSING:
			return "CLOSING";
		case SQLX_BASE_CLOSING_FOR_DELETION:
			return "CLOSING_FOR_DELETION";
		default:
			return "?";
	}
}

static void
sqlx_base_debug_func(const gchar *from, sqlx_base_t *base)
{
	(void) from;
	(void) base;

	EXTRA_ASSERT(base);
	GRID_TRACE2("BASE [%d/%s]"
			" %"G_GUINT32_FORMAT
			" LIST=%s [%d,%d]"
			" (%s)",
			base->index, (base->name ? hashstr_str(base->name) : ""),
			base->count_open,
			sqlx_status_to_str(base->status),
			base->link.prev, base->link.next,
			from);
}

# define sqlx_base_debug(From,Base) do { sqlx_base_debug_func(From,Base); } while (0)
#else
# define sqlx_base_debug(From,Base) do {  } while (0)
#endif

static sqlx_base_t *
sqlx_get_by_id(sqlx_cache_t *cache, gint i)
{
	return base_id_out(cache, i) ? NULL : cache->bases + i;
}

static sqlx_base_t *
sqlx_next_by_id(sqlx_cache_t *cache, gint i)
{
	sqlx_base_t *current;

	return (current = sqlx_get_by_id(cache, i))
		? sqlx_get_by_id(cache, current->link.next)
		: NULL;
}

static sqlx_base_t *
sqlx_prev_by_id(sqlx_cache_t *cache, gint i)
{
	sqlx_base_t *current;

	return (current = sqlx_get_by_id(cache, i))
		? sqlx_get_by_id(cache, current->link.prev)
		: NULL;
}

static gint
sqlx_base_get_id(sqlx_base_t *base)
{
	return base ? base->index : -1;
}

static void
SQLX_REMOVE(sqlx_cache_t *cache, sqlx_base_t *base,
		struct beacon_s *beacon)
{
	sqlx_base_t *next, *prev;

	/* Update the beacon */
	if (beacon->first == base->index)
		beacon->first = sqlx_base_get_id(sqlx_next_by_id(cache, beacon->first));
	if (beacon->last == base->index)
		beacon->last = sqlx_base_get_id(sqlx_prev_by_id(cache, beacon->last));

	/* Update the previous and next */
	next = sqlx_get_by_id(cache, base->link.next);
	prev = sqlx_get_by_id(cache, base->link.prev);

	if (prev)
		prev->link.next = sqlx_base_get_id(next);
	if (next)
		next->link.prev = sqlx_base_get_id(prev);

	/* Update the base itself */
	base->status = 0;
	base->link.prev = -1;
	base->link.next = -1;
}

static void
SQLX_UNSHIFT(sqlx_cache_t *cache, sqlx_base_t *base,
		struct beacon_s *beacon, enum sqlx_base_status_e status)
{
	sqlx_base_t *first;

	base->link.prev = base->link.next = -1;
	base->link.next = beacon->first;

	first = sqlx_get_by_id(cache, beacon->first);
	if (first)
		first->link.prev = base->index;
	beacon->first = base->index;

	if (beacon->last < 0)
		beacon->last = base->index;

	base->status = status;
	base->last_update = oio_ext_monotonic_time ();
}

static void
sqlx_save_id(sqlx_cache_t *cache, sqlx_base_t *base)
{
	gpointer pointer_index = GINT_TO_POINTER(base->index + 1);
	g_tree_replace(cache->bases_by_name, base->name, pointer_index);
}

static gint
sqlx_lookup_id(sqlx_cache_t *cache, const hashstr_t *hs)
{
	gpointer lookup_result = g_tree_lookup(cache->bases_by_name, hs);
	return !lookup_result ? -1 : (GPOINTER_TO_INT(lookup_result) - 1);
}

static void
sqlx_base_remove_from_list(sqlx_cache_t *cache, sqlx_base_t *base)
{
	switch (base->status) {
		case SQLX_BASE_FREE:
			SQLX_REMOVE(cache, base, &(cache->beacon_free));
			return;
		case SQLX_BASE_IDLE:
			SQLX_REMOVE(cache, base, &(cache->beacon_idle));
			return;
		case SQLX_BASE_IDLE_HOT:
			SQLX_REMOVE(cache, base, &(cache->beacon_idle_hot));
			return;
		case SQLX_BASE_USED:
			SQLX_REMOVE(cache, base, &(cache->beacon_used));
			return;
		case SQLX_BASE_CLOSING:
		case SQLX_BASE_CLOSING_FOR_DELETION:
			EXTRA_ASSERT(base->link.prev < 0);
			EXTRA_ASSERT(base->link.next < 0);
			return;
	}
}

static void
sqlx_base_add_to_list(sqlx_cache_t *cache, sqlx_base_t *base,
		enum sqlx_base_status_e status)
{
	EXTRA_ASSERT(base->link.prev < 0);
	EXTRA_ASSERT(base->link.next < 0);

	switch (status) {
		case SQLX_BASE_FREE:
			EXTRA_ASSERT(cache->bases_used > 0);
			cache->bases_used --;
			SQLX_UNSHIFT(cache, base, &(cache->beacon_free), SQLX_BASE_FREE);
			return;
		case SQLX_BASE_IDLE:
			SQLX_UNSHIFT(cache, base, &(cache->beacon_idle), SQLX_BASE_IDLE);
			return;
		case SQLX_BASE_IDLE_HOT:
			SQLX_UNSHIFT(cache, base, &(cache->beacon_idle_hot), SQLX_BASE_IDLE_HOT);
			return;
		case SQLX_BASE_USED:
			SQLX_UNSHIFT(cache, base, &(cache->beacon_used), SQLX_BASE_USED);
			return;
		case SQLX_BASE_CLOSING:
		case SQLX_BASE_CLOSING_FOR_DELETION:
			base->status = status;
			return;
	}
}

static void
sqlx_base_move_to_list(sqlx_cache_t *cache, sqlx_base_t *base,
		enum sqlx_base_status_e status)
{
	register enum sqlx_base_status_e status0;

	if (status != (status0 = base->status)) {
		sqlx_base_remove_from_list(cache, base);
		sqlx_base_add_to_list(cache, base, status);
	}

	GRID_TRACE2("BASE [%d/%s] moved from %s to %s",
			base->index,
			hashstr_str(base->name),
			sqlx_status_to_str(status0),
			sqlx_status_to_str(status));
}

static gboolean
_has_idle_unlocked(sqlx_cache_t *cache)
{
	return cache->beacon_idle.first != -1 ||
			cache->beacon_idle_hot.first != -1;
}

static GError *
sqlx_base_reserve(sqlx_cache_t *cache, const hashstr_t *hs,
		sqlx_base_t **result)
{
	*result = NULL;
	if (cache->bases_used >= cache->bases_max_soft) {
		if (_has_idle_unlocked(cache)) {
			return NULL;  // No free base but we can recycle an idle one
		} else {
			return BUSY("Max bases reached");
		}
	}

	sqlx_base_t *base = sqlx_get_by_id(cache, cache->beacon_free.first);
	if (!base)
		return NULL;

	cache->bases_used ++;
	EXTRA_ASSERT(base->count_open == 0);

	/* base reserved and in PENDING state */
	g_free0 (base->name);
	base->name = hashstr_dup(hs);
	base->count_open = 1;
	base->handle = NULL;
	base->owner = g_thread_self();
	sqlx_base_move_to_list(cache, base, SQLX_BASE_USED);
	sqlx_save_id(cache, base);

	sqlx_base_debug(__FUNCTION__, base);
	*result = base;
	return NULL;
}

static void
_signal_base(sqlx_base_t *base)
{
	EXTRA_ASSERT(base != NULL);
	g_cond_signal(&(base->cond_prio));
	g_cond_signal(&(base->cond));
}

/**
 * PRE:
 * - The base must be owned by the current thread
 * - it must be opened only once and locked only once
 * - the cache-wide lock must be owned by the current thread
 *
 * POST:
 * - The base is returned to the FREE list
 * - the base is not owned by any thread
 * - The cache-wide lock is still owned
 */
static void
_expire_base(sqlx_cache_t *cache, sqlx_base_t *b, gboolean deleted)
{
	gpointer handle = b->handle;

	sqlx_base_debug("FREEING", b);
	EXTRA_ASSERT(b->owner != NULL);
	EXTRA_ASSERT(b->count_open == 0);
	EXTRA_ASSERT(b->status == SQLX_BASE_USED);

	sqlx_base_move_to_list(cache, b,
			deleted? SQLX_BASE_CLOSING_FOR_DELETION : SQLX_BASE_CLOSING);

	/* the base is for the given thread, it is time to REALLY close it.
	 * But this can take a lot of time. So we can release the pool,
	 * free the handle and unlock the cache */
	_signal_base(b),
	g_mutex_unlock(&cache->lock);
	if (cache->close_hook)
		cache->close_hook(handle);
	g_mutex_lock(&cache->lock);

	hashstr_t *n = b->name;

	b->handle = NULL;
	b->heat = 0;
	b->owner = NULL;
	b->name = NULL;
	b->count_open = 0;
	b->last_update = 0;
	sqlx_base_move_to_list(cache, b, SQLX_BASE_FREE);

	g_tree_remove(cache->bases_by_name, n);
	g_free(n);
}

static gint
_expire_specific_base(sqlx_cache_t *cache, sqlx_base_t *b,
		const gint64 now, const gint64 grace_delay)
{
	/* TODO(jfs): this is way to complicated. ASAP change the logic */
	if (now > 0) {
	   if (grace_delay <= 0 || b->last_update > OLDEST(now, grace_delay))
			return 0;
	}

	/* At this point, I have the global lock, and the base is IDLE.
	 * We know no one have the lock on it. So we make the base USED
	 * and we get the lock on it. because we have the lock, it is
	 * protected from other uses */

	EXTRA_ASSERT(b->status == SQLX_BASE_IDLE || b->status == SQLX_BASE_IDLE_HOT);
	EXTRA_ASSERT(b->count_open == 0);
	EXTRA_ASSERT(b->owner == NULL);

	/* make it used and locked by the current thread */
	b->owner = g_thread_self();
	sqlx_base_move_to_list(cache, b, SQLX_BASE_USED);

	_expire_base(cache, b, FALSE);

	/* If someone is waiting on the base while it is being closed
	 * (this arrives when someone tries to read it again after
	 * waiting exactly the grace delay), we must notify him so it can
	 * retry (and open it in another file descriptor). */
	_signal_base(b);

	return 1;
}

static gint
sqlx_expire_first_idle_base(sqlx_cache_t *cache, gint64 now)
{
	gint rc = 0, bd_idle;

	/* Poll the next idle base, and respect the increasing order of the 'heat' */
	if (0 <= (bd_idle = cache->beacon_idle.last))
		rc = _expire_specific_base(cache, GET(cache, bd_idle), now,
				_cache_grace_delay_cool);
	if (!rc && 0 <= (bd_idle = cache->beacon_idle_hot.last))
		rc = _expire_specific_base(cache, GET(cache, bd_idle), now,
				_cache_grace_delay_hot);

	if (rc) {
		GRID_TRACE("Expired idle base at pos %d", bd_idle);
	}

	return rc;
}

/* ------------------------------------------------------------------------- */

void
sqlx_cache_reconfigure(sqlx_cache_t *cache)
{
	if (!cache)
		return;

	if (sqliterepo_repo_max_bases_soft > 0)
		cache->bases_max_soft =
			CLAMP(sqliterepo_repo_max_bases_soft, 1, cache->bases_max_hard);
	else
		cache->bases_max_soft = cache->bases_max_hard;
}

void
sqlx_cache_set_running(sqlx_cache_t *cache, gboolean is_running)
{
	cache->is_running = is_running;
}

void
sqlx_cache_set_unlock_hook(sqlx_cache_t *cache, sqlx_cache_unlock_hook hook)
{
	EXTRA_ASSERT(cache != NULL);
	cache->unlock_hook = hook;
}

void
sqlx_cache_set_close_hook(sqlx_cache_t *cache, sqlx_cache_close_hook hook)
{
	EXTRA_ASSERT(cache != NULL);
	cache->close_hook = hook;
}

sqlx_cache_t *
sqlx_cache_init(void)
{
	sqlx_cache_t *cache = g_malloc0(sizeof(*cache));
	g_mutex_init(&cache->lock);
	cache->bases_by_name = g_tree_new_full(hashstr_quick_cmpdata,
			NULL, NULL, NULL);
	BEACON_RESET(&(cache->beacon_free));
	BEACON_RESET(&(cache->beacon_idle));
	BEACON_RESET(&(cache->beacon_idle_hot));
	BEACON_RESET(&(cache->beacon_used));

	cache->bases_used = 0;
	cache->bases_max_hard = sqliterepo_repo_max_bases_hard? : 1024;
	cache->bases_max_soft = CLAMP(sqliterepo_repo_max_bases_soft, 1, cache->bases_max_hard);
	cache->bases = g_malloc0(cache->bases_max_hard * sizeof(sqlx_base_t));

	time_t now = oio_ext_monotonic_seconds();
	for (guint i=0; i<cache->bases_max_hard ;i++) {
		sqlx_base_t *base = cache->bases + i;
		base->index = i;
		base->link.prev = base->link.next = -1;
		g_cond_init(&base->cond);
		g_cond_init(&base->cond_prio);
		base->open_attempts = grid_single_rrd_create(now, 60);
		base->open_wait_time = grid_single_rrd_create(now, 60);
	}

	/* stack all the bases in the FREE list, so that the first bases are
	 * preferred. */
	for (guint i=cache->bases_max_hard; i>0 ;i--) {
		sqlx_base_t *base = cache->bases + i - 1;
		SQLX_UNSHIFT(cache, base, &(cache->beacon_free), SQLX_BASE_FREE);
	}

	cache->is_running = TRUE;
	return cache;
}

void
sqlx_cache_clean(sqlx_cache_t *cache)
{
	GRID_DEBUG("%s(%p) *** CLEANUP ***", __FUNCTION__, (void*)cache);
	if (!cache)
		return;

	if (cache->bases) {
		for (guint bd=0; bd < cache->bases_max_hard ;bd++) {
			sqlx_base_t *base = cache->bases + bd;

			switch (base->status) {
				case SQLX_BASE_FREE:
					EXTRA_ASSERT(base->name == NULL);
					break;
				case SQLX_BASE_IDLE:
				case SQLX_BASE_IDLE_HOT:
				case SQLX_BASE_USED:
					sqlx_base_debug(__FUNCTION__, base);
					break;
				case SQLX_BASE_CLOSING:
				case SQLX_BASE_CLOSING_FOR_DELETION:
					GRID_ERROR("Base being closed while the cache is being cleaned");
					break;
			}

			g_cond_clear(&base->cond);
			g_cond_clear(&base->cond_prio);
			grid_single_rrd_destroy(base->open_attempts);
			grid_single_rrd_destroy(base->open_wait_time);
			g_free0 (base->name);
			base->name = NULL;
		}
		g_free(cache->bases);
	}

	g_mutex_clear(&cache->lock);
	if (cache->bases_by_name)
		g_tree_destroy(cache->bases_by_name);

	g_free(cache);
}

/**
 * Check if the database was accessed during the period
 * and it is under minimal load.
 */
static gboolean
_base_is_accessible(sqlx_base_t *base, gint64 now, guint64 period)
{
	period = CLAMP(period / G_TIME_SPAN_SECOND, 1, 60);
	return grid_single_rrd_get_delta(base->open_attempts,
			now / G_TIME_SPAN_SECOND,
			period) >= (period * _cache_min_load_on_heavy_load);
}

/**
 * From the average waiting time of the last 10 seconds, check if the request
 * can still wait (return 0) or if it doesn't have much time left
 * (return the average wait time in second).
 */
static double
_load_too_high(sqlx_base_t *base, gint64 now, gint64 remaining_time)
{
	guint64 dx = grid_single_rrd_get_delta(base->open_attempts,
			now / G_TIME_SPAN_SECOND, 10);
	guint64 dt = grid_single_rrd_get_delta(base->open_wait_time,
			now / G_TIME_SPAN_SECOND, 10);
	/* Check if the database is under minimal load on the last 10 seconds */
	if (dx >= (10 * _cache_min_load_on_heavy_load)) {
		/* Check if the average waiting time does not exceed the remaining time
		 * for the request */
		gint64 avg_waiting_time = dt / dx;
		if (avg_waiting_time > remaining_time) {
			return (double)avg_waiting_time / (double)G_TIME_SPAN_SECOND;
		}
		return 0;
	}
	/* No requests have successfully opened the database recently,
	 * retry until the deadline is reached */
	return 0;
}

GError *
sqlx_cache_open_and_lock_base(sqlx_cache_t *cache, const hashstr_t *hname,
		gboolean urgent, gint *result, gint64 deadline)
{
	gint bd;
	GError *err = NULL;
	sqlx_base_t *base = NULL;
	gint attempts = 0;

	EXTRA_ASSERT(cache != NULL);
	EXTRA_ASSERT(hname != NULL);
	EXTRA_ASSERT(result != NULL);

	const gint64 start = oio_ext_monotonic_time();
	const gint64 local_deadline = start + _cache_timeout_open;
	deadline = (deadline <= 0) ? local_deadline : MIN(deadline, local_deadline);
	/* Half the request timeout, or 2 wait periods. */
	const gint64 deadline_margin =
			MIN((deadline - start) / 2, 2 * _cache_period_cond_wait);

	GRID_TRACE2("%s(%p,%s,%p) delay = %" G_GINT64_FORMAT "ms", __FUNCTION__,
			(void*)cache, hname ? hashstr_str(hname) : "NULL",
			(void*)result, (deadline - start) / G_TIME_SPAN_MILLISECOND);

	gboolean base_has_been_opened = FALSE;
	g_mutex_lock(&cache->lock);
retry:
	attempts++;

	if (!cache->is_running) {
		err = BUSY("service exiting");
	}
	else if ((bd = sqlx_lookup_id(cache, hname)) < 0) {
		if (!(err = sqlx_base_reserve(cache, hname, &base))) {
			if (base) {
				bd = base->index;
				*result = base->index;
				sqlx_base_debug("OPEN", base);
			} else {
				if (sqlx_expire_first_idle_base(cache, 0) >= 0)
					goto retry;
				err = NEWERROR(CODE_UNAVAILABLE, "No idle base in cache");
			}
		}
		EXTRA_ASSERT((base != NULL) ^ (err != NULL));
	}
	else {
		base = GET(cache, bd);

		GCond *wait_cond = urgent? &base->cond_prio : &base->cond;

		const gint64 now = oio_ext_monotonic_time ();

		gint64 remaining_time = deadline - now;
		if (remaining_time <= 0) {
			gint64 wait_time = now - start;
			if (base->status == SQLX_BASE_USED
					&& base->owner == g_thread_self()) {
				/* The database is already open but the deadline has passed */
				err = TIMEOUT("Deadline reached");
			} else if (attempts < 2) {
				/* The deadline was reached without having had time
				 * to make the slightest attempt */
				err = BUSY(
						"DB busy (deadline reached after %"G_GINT64_FORMAT \
						" us): no attempt to open", wait_time);
			} else if (_cache_fail_on_heavy_load
					&& _base_is_accessible(base, now, wait_time)) {
				/* Several attempts were made to fail to open the database,
				 * but other requests succeeded in opening it
				 * within the same amount of time */
				err = EXCESSIVE_LOAD(_load_too_high(base, now, 0), "true");
			} else {
				err = BUSY(
						"DB busy (deadline reached after %"G_GINT64_FORMAT \
						" us)", wait_time);
			}
		} else switch (base->status) {

			case SQLX_BASE_FREE:
				EXTRA_ASSERT(base->count_open == 0);
				EXTRA_ASSERT(base->count_waiting == 0);
				EXTRA_ASSERT(base->owner == NULL);
				GRID_ERROR("free base referenced");
				g_assert_not_reached();
				break;

			case SQLX_BASE_IDLE:
			case SQLX_BASE_IDLE_HOT:
				/* Base unused right now, the current thread get it! */
				EXTRA_ASSERT(base->count_open == 0);
				EXTRA_ASSERT(base->owner == NULL);
				sqlx_base_move_to_list(cache, base, SQLX_BASE_USED);
				base->count_open ++;
				base->owner = g_thread_self();
				*result = base->index;
				base_has_been_opened = TRUE;
				break;

			case SQLX_BASE_USED:
				EXTRA_ASSERT(base->count_open > 0);
				EXTRA_ASSERT(base->owner != NULL);
				if (base->owner != g_thread_self()) {
					GRID_DEBUG("Base [%s] in use by another thread (%X), waiting...",
							hashstr_str(hname), oio_log_thread_id(base->owner));

					if (!urgent) {
						gint64 margin = 0;
						if (!_cache_fail_on_heavy_load
								&& _cache_alert_on_heavy_load) {
							/* Set a margin so we send the warning
							 * before actually failing. */
							margin = deadline_margin;
						}
						double avg_waiting_time = _load_too_high(
								base, now, remaining_time - margin);
						if (avg_waiting_time > 0) {
							if (_cache_fail_on_heavy_load) {
								err = EXCESSIVE_LOAD(avg_waiting_time, "false");
								break;
							}
							if (_cache_alert_on_heavy_load) {
								GRID_WARN(
										"Load too high on [%s] (waiting_" \
										"requests=%"G_GUINT32_FORMAT", " \
										"avg_waiting_time=%.6lf, " \
										"remaining_time=%.6lf, " \
										"reqid=%s)",
										hashstr_str(hname),
										base->count_waiting,
										avg_waiting_time,
										remaining_time/(double)G_TIME_SPAN_SECOND,
										oio_ext_get_reqid());
							}
						}
					}

					base->count_waiting ++;
					base->heat = 1;

					/* The lock is held by another thread/request.
					   Do not use 'now' because it can be a fake clock */
					g_cond_wait_until(wait_cond, &cache->lock,
							g_get_monotonic_time() + _cache_period_cond_wait);

					base->count_waiting --;
					goto retry;
				}
				/* Already opened by this thread */
				base->owner = g_thread_self();
				base->count_open ++;
				*result = base->index;
				break;

			case SQLX_BASE_CLOSING:
				EXTRA_ASSERT(base->owner != NULL);
				/* Just wait for a notification then retry
				   Do not use 'now' because it can be a fake clock */
				g_cond_wait_until(wait_cond, &cache->lock,
						g_get_monotonic_time() + _cache_period_cond_wait);
				goto retry;

			case SQLX_BASE_CLOSING_FOR_DELETION:
				err = NEWERROR(CODE_CONTAINER_NOTFOUND,
						"Base [%s] being deleted",
						hashstr_str(hname));
				break;
		}
	}

	if (base) {
		gint64 now = oio_ext_monotonic_time();
		time_t now_secs = now / G_TIME_SPAN_SECOND;
		gint64 wait_time = now - start;
		if (base_has_been_opened) {
			grid_single_rrd_add(base->open_attempts, now_secs, 1);
			grid_single_rrd_add(base->open_wait_time, now_secs, wait_time);
			/* Base has been opened very quickly, consider it cold. */
			if (attempts == 1
					&& wait_time < G_TIME_SPAN_MILLISECOND
					&& base->count_waiting < 2) {
				base->heat = 0;
			}
		}
		oio_ext_incr_db_wait(wait_time);
		oio_ext_add_perfdata("db_wait", wait_time / G_TIME_SPAN_SECOND);

		if (!err) {
			sqlx_base_debug(__FUNCTION__, base);
			EXTRA_ASSERT(base->owner == g_thread_self());
			EXTRA_ASSERT(base->count_open > 0);
		}
		_signal_base(base);
	}
	g_mutex_unlock(&cache->lock);
	return err;
}

GError *
sqlx_cache_unlock_and_close_base(sqlx_cache_t *cache, gint bd, guint32 flags)
{
	GError *err = NULL;
	gchar bname[LIMIT_LENGTH_BASENAME] = {0};

	GRID_TRACE2("%s(%p,%d,%d)", __FUNCTION__, (void*)cache, bd, flags);

	EXTRA_ASSERT(cache != NULL);
	if (base_id_out(cache, bd))
		return NEWERROR(CODE_INTERNAL_ERROR, "invalid base id=%d", bd);

	gint64 lock_time = 0;
	g_mutex_lock(&cache->lock);

	sqlx_base_t *base; base = GET(cache,bd);

	// base->name is no more valid after _expire_base()
	if (base->name)
		g_strlcpy(bname, hashstr_str(base->name), sizeof(bname));

	switch (base->status) {

		case SQLX_BASE_FREE:
			if (base->owner != NULL) {
				GRID_ERROR("[SQLX_BASE_FREE] Base still used by the thread "
						"%d %04X", getpid(), oio_log_thread_id(base->owner));
				g_assert_not_reached();
			} else if (base->count_open != 0) {
				GRID_ERROR("[SQLX_BASE_FREE] Base still open: %"
						G_GUINT32_FORMAT " openings", base->count_open);
				g_assert_not_reached();
			}
			err = NEWERROR(CODE_INTERNAL_ERROR, "base not used");
			break;

		case SQLX_BASE_IDLE:
		case SQLX_BASE_IDLE_HOT:
			if (base->owner != NULL) {
				GRID_ERROR("[SQLX_BASE_IDLE] Base still used by the thread "
						"%d %04X", getpid(), oio_log_thread_id(base->owner));
				g_assert_not_reached();
			} else if (base->count_open != 0) {
				GRID_ERROR("[SQLX_BASE_IDLE] Base still open: %"
						G_GUINT32_FORMAT " openings", base->count_open);
				g_assert_not_reached();
			}
			err = NEWERROR(CODE_INTERNAL_ERROR, "base closed");
			break;

		case SQLX_BASE_USED:
			if (base->owner == NULL) {
				GRID_ERROR("[SQLX_BASE_USED] Unknown thread use the base");
				g_assert_not_reached();
			} else if (base->owner != g_thread_self()) {
				GRID_ERROR("[SQLX_BASE_USED] Base used by the other thread "
						"%d %04X", getpid(), oio_log_thread_id(base->owner));
				g_assert_not_reached();
			} else if (base->count_open < 1) {
				GRID_ERROR("[SQLX_BASE_USED] Base not open");
				g_assert_not_reached();
			}
			lock_time = oio_ext_monotonic_time() - base->last_update;
			/* held by the current thread */
			if (!(-- base->count_open)) {  /* to be closed */
				if (flags & (SQLX_CLOSE_IMMEDIATELY|SQLX_CLOSE_FOR_DELETION)) {
					_expire_base(cache, base, flags & SQLX_CLOSE_FOR_DELETION);
				} else {
					sqlx_base_debug("CLOSING", base);

					if (cache->unlock_hook)
						cache->unlock_hook(base->handle);

					base->owner = NULL;
					if (base->heat >= _cache_heat_threshold)
						sqlx_base_move_to_list(cache, base, SQLX_BASE_IDLE_HOT);
					else
						sqlx_base_move_to_list(cache, base, SQLX_BASE_IDLE);

					/* Optimistic memory ceiling management.
					 * That logic will let an overhead on the limit: the base
					 * that will be expired won't return its memory pages to
					 * the kernel but to the sqlite3 pool. The pages will
					 * become available to other bases. */
					if (_ram_exhausted(cache) && _has_idle_unlocked(cache))
						sqlx_expire_first_idle_base(cache, 0);
				}
			}
			break;

		case SQLX_BASE_CLOSING:
		case SQLX_BASE_CLOSING_FOR_DELETION:
			if (base->owner == NULL) {
				GRID_ERROR("[SQLX_BASE_CLOSING] Unknown thread use the base");
				g_assert_not_reached();
			} else if (base->owner == g_thread_self()) {
				GRID_ERROR("[SQLX_BASE_CLOSING] Already closing "
						"by this thread");
				g_assert_not_reached();
			}
			err = NEWERROR(CODE_INTERNAL_ERROR, "base being closed");
			break;
	}

	if (base && !err) {
		sqlx_base_debug(__FUNCTION__, base);
		oio_ext_add_perfdata("db_lock", lock_time);
		if (lock_time > _cache_timeout_open * 3 / 4) {
			GRID_WARN("The current thread held a lock on [%s] for %"
					G_GINT64_FORMAT"us (sqliterepo.cache.timeout.open=%"
					G_GINT64_FORMAT", reqid=%s)", bname,
					lock_time, _cache_timeout_open, oio_ext_get_reqid());
		}
	}
	_signal_base(base),
	g_mutex_unlock(&cache->lock);
	return err;
}

void
sqlx_cache_debug(sqlx_cache_t *cache)
{
	EXTRA_ASSERT(cache != NULL);

	if (!GRID_DEBUG_ENABLED())
		return;

	GRID_DEBUG("--- REPO %p -----------------", (void*)cache);
	GRID_DEBUG(" > used     [%d, %d]",
			cache->beacon_used.first, cache->beacon_used.last);
	GRID_DEBUG(" > idle     [%d, %d]",
			cache->beacon_idle.first, cache->beacon_idle.last);
	GRID_DEBUG(" > idle_hot [%d, %d]",
			cache->beacon_idle_hot.first, cache->beacon_idle_hot.last);
	GRID_DEBUG(" > free     [%d, %d]",
			cache->beacon_free.first, cache->beacon_free.last);

	/* Dump all the bases */
	for (guint bd=0; bd < cache->bases_max_hard ;bd++) {
		sqlx_base_debug(__FUNCTION__, GET(cache,bd));
	}

	/* Now dump all te references in the hashtable */
	gboolean runner(gpointer k, gpointer v, gpointer u) {
		(void) u;
		GRID_DEBUG("REF %d <- %s", GPOINTER_TO_INT(v), hashstr_str(k));
		return FALSE;
	}
	g_tree_foreach(cache->bases_by_name, runner, NULL);
}

guint
sqlx_cache_expire_all(sqlx_cache_t *cache)
{
	guint nb;

	EXTRA_ASSERT(cache != NULL);

	g_mutex_lock(&cache->lock);
	for (nb=0; sqlx_expire_first_idle_base(cache, 0) ;nb++) { }
	g_mutex_unlock(&cache->lock);

	return nb;
}

guint
sqlx_cache_expire(sqlx_cache_t *cache, guint max, gint64 duration)
{
	guint nb = 0;
	gint64 deadline = oio_ext_monotonic_time () + duration;

	EXTRA_ASSERT(cache != NULL);

	g_mutex_lock(&cache->lock);

	for (nb=0; !max || nb < max ; nb++) {
		gint64 now = oio_ext_monotonic_time ();
		if (now > deadline || !sqlx_expire_first_idle_base(cache, now))
			break;
	}

	g_mutex_unlock(&cache->lock);
	return nb;
}

gpointer
sqlx_cache_get_handle(sqlx_cache_t *cache, gint bd)
{
	EXTRA_ASSERT(cache != NULL);
	EXTRA_ASSERT(bd >= 0);

	sqlx_base_t *base = GET(cache,bd);
	EXTRA_ASSERT(base != NULL);

	return base->handle;
}

void
sqlx_cache_set_handle(sqlx_cache_t *cache, gint bd, gpointer sq3)
{
	EXTRA_ASSERT(cache != NULL);
	EXTRA_ASSERT(bd >= 0);

	sqlx_base_t *base = GET(cache,bd);
	EXTRA_ASSERT(base != NULL);
	base->handle = sq3;
}

static guint
_count_beacon(sqlx_cache_t *cache, struct beacon_s *beacon)
{
	guint count = 0;
	g_mutex_lock(&cache->lock);
	for (gint idx = beacon->first; idx != -1 ;) {
		++ count;
		idx = GET(cache, idx)->link.next;
	}
	g_mutex_unlock(&cache->lock);
	return count;
}

struct cache_counts_s
sqlx_cache_count(sqlx_cache_t *cache)
{
	struct cache_counts_s count;

	memset(&count, 0, sizeof(count));
	if (cache) {
		count.max = cache->bases_max_hard;
		count.soft_max = cache->bases_max_soft;
		count.cold = _count_beacon(cache, &cache->beacon_idle);
		count.hot = _count_beacon(cache, &cache->beacon_idle_hot);
		count.used = _count_beacon(cache, &cache->beacon_used);
	}

	return count;
}

void
sqlx_cache_set_last_memory_usage(sqlx_cache_t *cache, gint64 usage)
{
	cache->last_memory_usage = usage;
}
