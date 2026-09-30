// SPDX-License-Identifier: GPL-2.0
/*
 * Unit tests for the logger's bounded queue and latency histogram. These do
 * not need the kernel module and are also built with TSan and ASan/UBSan.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bqueue.h"
#include "hist.h"

static int failures;

#define EXPECT(cond)                                                          \
	do {                                                                  \
		if (!(cond)) {                                                \
			fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #cond); \
			failures++;                                           \
		}                                                             \
	} while (0)

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void test_hist_exact_small(void)
{
	struct hist h;
	uint64_t v;

	hist_init(&h);
	for (v = 1; v <= 50; v++)
		hist_add(&h, v);
	EXPECT(h.count == 50 && h.min == 1 && h.max == 50);
	EXPECT(hist_pct(&h, 50) == 25);
	EXPECT(hist_pct(&h, 100) == 50);
	EXPECT(hist_pct(&h, 0) == 1);
}

static void test_hist_relative_error(void)
{
	struct hist h;
	uint64_t v, got, want;
	double p, err, worst = 0;

	hist_init(&h);
	for (v = 1; v <= 1000000; v++)
		hist_add(&h, v * 997);	/* spans 997 ns .. ~1 s */
	for (p = 1; p <= 99.9; p += 0.7) {
		want = (uint64_t)(p / 100.0 * 1000000.0 + 0.999999) * 997;
		got = hist_pct(&h, p);
		err = (double)(got > want ? got - want : want - got) / (double)want;
		if (err > worst)
			worst = err;
	}
	EXPECT(worst <= 1.0 / 64);
}

static void test_hist_merge(void)
{
	struct hist a, b;

	hist_init(&a);
	hist_init(&b);
	hist_add(&a, 10);
	hist_add(&b, 1000000);
	hist_merge(&a, &b);
	EXPECT(a.count == 2 && a.min == 10 && a.max == 1000000 && a.sum == 1000010);
	hist_init(&b);
	EXPECT(hist_pct(&b, 99) == 0);	/* empty */
}

static void test_bq_fifo_and_full(void)
{
	struct bq q;
	int items[4];
	void *p;

	EXPECT(bq_init(&q, 3) == 0);
	for (int i = 0; i < 3; i++)
		EXPECT(bq_push(&q, &items[i], BQ_TRY) == BQ_OK);
	EXPECT(bq_push(&q, &items[3], BQ_TRY) == BQ_FULL);
	for (int i = 0; i < 3; i++)
		EXPECT(bq_pop(&q, &p) == BQ_OK && p == &items[i]);
	EXPECT(bq_trypop(&q, &p) == BQ_EMPTY);
	EXPECT(q.max_count == 3);
	bq_destroy(&q);
}

struct push_arg {
	struct bq *q;
	enum bq_push_mode mode;
	enum bq_rc rc;
	void *item;
};

static void *pusher(void *p)
{
	struct push_arg *a = p;

	a->rc = bq_push(a->q, a->item, a->mode);
	return NULL;
}

static void test_bq_stop_releases_blocked_pusher(void)
{
	struct bq q;
	int x, y;
	struct push_arg a = { &q, BQ_WAIT, BQ_OK, &y };
	pthread_t th;
	void *p;

	bq_init(&q, 1);
	bq_push(&q, &x, BQ_TRY);
	pthread_create(&th, NULL, pusher, &a);
	sleep_ms(50);			/* pusher is now blocked on a full queue */
	bq_request_stop(&q);
	pthread_join(th, NULL);
	EXPECT(a.rc == BQ_STOPPING);	/* not enqueued; caller keeps the item */
	EXPECT(q.count == 1);

	/* A drain-mode push still waits for space after a stop request. */
	a.mode = BQ_WAIT_DRAIN;
	pthread_create(&th, NULL, pusher, &a);
	sleep_ms(50);
	EXPECT(bq_pop(&q, &p) == BQ_OK && p == &x);
	pthread_join(th, NULL);
	EXPECT(a.rc == BQ_OK);
	EXPECT(bq_pop(&q, &p) == BQ_OK && p == &y);
	bq_destroy(&q);
}

static void test_bq_abort_releases_drain_pusher(void)
{
	struct bq q;
	int x, y;
	struct push_arg a = { &q, BQ_WAIT_DRAIN, BQ_OK, &y };
	pthread_t th;
	void *p;

	bq_init(&q, 1);
	bq_push(&q, &x, BQ_TRY);
	pthread_create(&th, NULL, pusher, &a);
	sleep_ms(50);
	bq_abort(&q);
	pthread_join(th, NULL);
	EXPECT(a.rc == BQ_ABORTED);
	EXPECT(bq_pop(&q, &p) == BQ_ABORTED);
	EXPECT(bq_trypop(&q, &p) == BQ_OK && p == &x);	/* still collectable */
	bq_destroy(&q);
}

static void *popper(void *p)
{
	struct bq *q = p;
	void *item;

	return (void *)(intptr_t)bq_pop(q, &item);
}

static void test_bq_close_drains_then_closed(void)
{
	struct bq q;
	int x;
	void *p, *ret;
	pthread_t th;

	bq_init(&q, 2);
	bq_push(&q, &x, BQ_TRY);
	bq_close(&q);
	EXPECT(bq_push(&q, &x, BQ_TRY) == BQ_CLOSED);
	EXPECT(bq_pop(&q, &p) == BQ_OK && p == &x);
	EXPECT(bq_pop(&q, &p) == BQ_CLOSED);
	bq_destroy(&q);

	/* A consumer blocked on an empty queue is woken by close. */
	bq_init(&q, 2);
	pthread_create(&th, NULL, popper, &q);
	sleep_ms(50);
	bq_close(&q);
	pthread_join(th, &ret);
	EXPECT((intptr_t)ret == BQ_CLOSED);
	bq_destroy(&q);
}

#define NPROD 3
#define NCONS 3
#define PER   20000

struct mpmc {
	struct bq q;
	uint32_t vals[NPROD * PER];
	uint8_t seen[NPROD * PER];
	pthread_mutex_t mu;
	int producer_id;
};

static void *mp_prod(void *p)
{
	struct mpmc *m = p;
	int id, i;

	pthread_mutex_lock(&m->mu);
	id = m->producer_id++;
	pthread_mutex_unlock(&m->mu);
	for (i = 0; i < PER; i++) {
		uint32_t *v = &m->vals[id * PER + i];

		*v = (uint32_t)(id * PER + i);
		if (bq_push(&m->q, v, BQ_WAIT) != BQ_OK)
			abort();
	}
	return NULL;
}

static void *mp_cons(void *p)
{
	struct mpmc *m = p;
	void *item;

	while (bq_pop(&m->q, &item) == BQ_OK) {
		uint32_t v = *(uint32_t *)item;

		pthread_mutex_lock(&m->mu);
		m->seen[v]++;
		pthread_mutex_unlock(&m->mu);
	}
	return NULL;
}

static void test_bq_mpmc_no_loss_no_dup(void)
{
	static struct mpmc m;
	pthread_t p[NPROD], c[NCONS];
	int i;

	memset(&m, 0, sizeof(m));
	bq_init(&m.q, 8);
	pthread_mutex_init(&m.mu, NULL);
	for (i = 0; i < NCONS; i++)
		pthread_create(&c[i], NULL, mp_cons, &m);
	for (i = 0; i < NPROD; i++)
		pthread_create(&p[i], NULL, mp_prod, &m);
	for (i = 0; i < NPROD; i++)
		pthread_join(p[i], NULL);
	bq_close(&m.q);
	for (i = 0; i < NCONS; i++)
		pthread_join(c[i], NULL);
	for (i = 0; i < NPROD * PER; i++)
		if (m.seen[i] != 1) {
			EXPECT(m.seen[i] == 1);
			break;
		}
	pthread_mutex_destroy(&m.mu);
	bq_destroy(&m.q);
}

int main(void)
{
	test_hist_exact_small();
	test_hist_relative_error();
	test_hist_merge();
	test_bq_fifo_and_full();
	test_bq_stop_releases_blocked_pusher();
	test_bq_abort_releases_drain_pusher();
	test_bq_close_drains_then_closed();
	test_bq_mpmc_no_loss_no_dup();
	if (failures) {
		printf("unit_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("unit_test: all passed\n");
	return 0;
}
