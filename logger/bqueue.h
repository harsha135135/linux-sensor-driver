/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Bounded blocking queue of batch pointers (mutex + two condition variables).
 *
 * Shutdown is explicit so no thread can be stuck in a wait:
 *   bq_request_stop  producers blocked in BQ_WAIT return BQ_STOPPING without
 *                    enqueueing (they keep ownership of the item);
 *                    BQ_WAIT_DRAIN pushes still wait for space.
 *   bq_close         no more pushes will come; consumers drain what is queued
 *                    and then get BQ_CLOSED.
 *   bq_abort         every wait returns BQ_ABORTED immediately; queued items
 *                    stay in the queue and can be collected with bq_trypop.
 */
#ifndef VS_BQUEUE_H
#define VS_BQUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

enum bq_rc { BQ_OK = 0, BQ_FULL, BQ_EMPTY, BQ_STOPPING, BQ_CLOSED, BQ_ABORTED };
enum bq_push_mode { BQ_TRY, BQ_WAIT, BQ_WAIT_DRAIN };

struct bq {
	pthread_mutex_t mu;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
	void **slot;
	size_t cap;
	size_t head;
	size_t count;
	size_t max_count;	/* high-water mark */
	bool stopping;
	bool closed;
	bool aborted;
};

int bq_init(struct bq *q, size_t cap);
void bq_destroy(struct bq *q);
enum bq_rc bq_push(struct bq *q, void *item, enum bq_push_mode mode);
enum bq_rc bq_pop(struct bq *q, void **item);		/* blocking */
/* Non-blocking, ignores stop/close/abort state; BQ_EMPTY if nothing queued. */
enum bq_rc bq_trypop(struct bq *q, void **item);
void bq_request_stop(struct bq *q);
void bq_close(struct bq *q);
void bq_abort(struct bq *q);

#endif
