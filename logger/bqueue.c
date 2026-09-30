// SPDX-License-Identifier: GPL-2.0
#include "bqueue.h"

#include <errno.h>
#include <stdlib.h>

int bq_init(struct bq *q, size_t cap)
{
	int ret;

	if (cap == 0)
		return EINVAL;
	q->slot = calloc(cap, sizeof(*q->slot));
	if (!q->slot)
		return ENOMEM;
	q->cap = cap;
	q->head = q->count = q->max_count = 0;
	q->stopping = q->closed = q->aborted = false;
	ret = pthread_mutex_init(&q->mu, NULL);
	if (!ret)
		ret = pthread_cond_init(&q->not_empty, NULL);
	if (!ret)
		ret = pthread_cond_init(&q->not_full, NULL);
	return ret;
}

void bq_destroy(struct bq *q)
{
	pthread_cond_destroy(&q->not_full);
	pthread_cond_destroy(&q->not_empty);
	pthread_mutex_destroy(&q->mu);
	free(q->slot);
	q->slot = NULL;
}

static void put_locked(struct bq *q, void *item)
{
	q->slot[(q->head + q->count) % q->cap] = item;
	q->count++;
	if (q->count > q->max_count)
		q->max_count = q->count;
	pthread_cond_signal(&q->not_empty);
}

static void *get_locked(struct bq *q)
{
	void *item = q->slot[q->head];

	q->head = (q->head + 1) % q->cap;
	q->count--;
	pthread_cond_signal(&q->not_full);
	return item;
}

enum bq_rc bq_push(struct bq *q, void *item, enum bq_push_mode mode)
{
	enum bq_rc rc = BQ_OK;

	pthread_mutex_lock(&q->mu);
	for (;;) {
		if (q->aborted) {
			rc = BQ_ABORTED;
			break;
		}
		if (q->closed) {
			rc = BQ_CLOSED;
			break;
		}
		if (q->count < q->cap) {
			put_locked(q, item);
			break;
		}
		if (mode == BQ_TRY) {
			rc = BQ_FULL;
			break;
		}
		if (mode == BQ_WAIT && q->stopping) {
			rc = BQ_STOPPING;
			break;
		}
		pthread_cond_wait(&q->not_full, &q->mu);
	}
	pthread_mutex_unlock(&q->mu);
	return rc;
}

enum bq_rc bq_pop(struct bq *q, void **item)
{
	enum bq_rc rc = BQ_OK;

	pthread_mutex_lock(&q->mu);
	for (;;) {
		if (q->aborted) {
			rc = BQ_ABORTED;
			break;
		}
		if (q->count) {
			*item = get_locked(q);
			break;
		}
		if (q->closed) {
			rc = BQ_CLOSED;
			break;
		}
		pthread_cond_wait(&q->not_empty, &q->mu);
	}
	pthread_mutex_unlock(&q->mu);
	return rc;
}

enum bq_rc bq_trypop(struct bq *q, void **item)
{
	enum bq_rc rc = BQ_EMPTY;

	pthread_mutex_lock(&q->mu);
	if (q->count) {
		*item = get_locked(q);
		rc = BQ_OK;
	}
	pthread_mutex_unlock(&q->mu);
	return rc;
}

void bq_request_stop(struct bq *q)
{
	pthread_mutex_lock(&q->mu);
	q->stopping = true;
	pthread_cond_broadcast(&q->not_full);
	pthread_mutex_unlock(&q->mu);
}

void bq_close(struct bq *q)
{
	pthread_mutex_lock(&q->mu);
	q->closed = true;
	pthread_cond_broadcast(&q->not_empty);
	pthread_cond_broadcast(&q->not_full);
	pthread_mutex_unlock(&q->mu);
}

void bq_abort(struct bq *q)
{
	pthread_mutex_lock(&q->mu);
	q->aborted = true;
	pthread_cond_broadcast(&q->not_empty);
	pthread_cond_broadcast(&q->not_full);
	pthread_mutex_unlock(&q->mu);
}
