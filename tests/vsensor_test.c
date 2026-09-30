// SPDX-License-Identifier: GPL-2.0
/*
 * Functional tests for /dev/vsensor. Runs against the real module (root
 * required for SET_CONFIG). Output is TAP. Usage: vsensor_test [name...]
 *
 * Timing policy: the VM may deschedule us at any time, so tests never assert
 * an upper bound on how long something takes, and every stream check allows
 * for drops as long as they are exactly accounted for. The only timing
 * assertions are lower bounds (e.g. a blocking read did wait), which a slow
 * VM cannot violate.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "uapi/vsensor.h"
#include "vsensor_model.h"

#define RS sizeof(struct vsensor_record)
#define DEV VSENSOR_DEVICE_PATH

static char fail_buf[512];

#define CHECK(cond, ...)                                                    \
	do {                                                                \
		if (!(cond)) {                                              \
			int n_ = snprintf(fail_buf, sizeof(fail_buf),       \
					  "%s:%d: %s: ", __func__, __LINE__, #cond); \
			snprintf(fail_buf + n_, sizeof(fail_buf) - (size_t)n_, \
				 __VA_ARGS__);                              \
			return -1;                                          \
		}                                                           \
	} while (0)

/* ---- helpers ---- */

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_ms(unsigned int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static int ctl_open(void)
{
	return open(DEV, O_WRONLY | O_CLOEXEC);
}

static int rd_open(int flags)
{
	return open(DEV, O_RDONLY | O_CLOEXEC | flags);
}

/* Returns the new config_gen, or -1. */
static long set_cfg(uint32_t interval_us, uint32_t seed)
{
	struct vsensor_config c = { .interval_us = interval_us, .seed = seed };
	int fd = ctl_open();
	long ret = -1;

	if (fd >= 0 && ioctl(fd, VSENSOR_IOC_SET_CONFIG, &c) == 0)
		ret = c.config_gen;
	if (fd >= 0)
		close(fd);
	return ret;
}

static int get_stats(int fd, struct vsensor_stats *st)
{
	return ioctl(fd, VSENSOR_IOC_GET_STATS, st);
}

struct recvec {
	struct vsensor_record *r;
	size_t n, cap;
};

static void rv_push(struct recvec *v, const struct vsensor_record *r, size_t n)
{
	if (v->n + n > v->cap) {
		v->cap = (v->n + n) * 2 + 1024;
		v->r = realloc(v->r, v->cap * RS);
		if (!v->r)
			abort();
	}
	memcpy(v->r + v->n, r, n * RS);
	v->n += n;
}

/* Read until EOF (the fd must already be frozen). */
static int drain(int fd, struct recvec *v)
{
	struct vsensor_record buf[64];
	ssize_t r;

	for (;;) {
		r = read(fd, buf, sizeof(buf));
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			return -1;
		if (r == 0)
			return 0;
		rv_push(v, buf, (size_t)r / RS);
	}
}

/*
 * Verify a complete, frozen reader stream against the documented semantics:
 * strictly increasing seq inside [start_seq, end_seq), every value matches
 * the model for the seed carried in the record, timestamps and config_gen
 * never go backwards, and the number of missing seqs (leading, interior and
 * trailing) equals dropped + flushed.
 */
static int check_stream(const struct recvec *v, const struct vsensor_stats *st)
{
	uint64_t expect = st->start_seq, gaps = 0, prev_ts = 0;
	uint32_t prev_gen = 0;
	size_t i;

	CHECK(st->flags & VSENSOR_STATS_FROZEN, "stream not frozen");
	CHECK(st->queued == 0, "queued=%u after drain", st->queued);
	CHECK(st->end_seq - st->start_seq == st->accepted + st->dropped,
	      "interval %llu != accepted %llu + dropped %llu",
	      (unsigned long long)(st->end_seq - st->start_seq), (unsigned long long)(st->accepted), (unsigned long long)(st->dropped));
	CHECK(st->accepted == st->delivered + st->flushed,
	      "accepted %llu != delivered %llu + flushed %llu",
	      (unsigned long long)(st->accepted), (unsigned long long)(st->delivered), (unsigned long long)(st->flushed));
	CHECK(v->n == st->delivered, "received %zu != delivered %llu", v->n,
	      (unsigned long long)(st->delivered));
	for (i = 0; i < v->n; i++) {
		const struct vsensor_record *r = &v->r[i];

		CHECK(r->seq >= expect, "seq %llu < expected %llu", (unsigned long long)(r->seq), (unsigned long long)(expect));
		CHECK(r->seq < st->end_seq, "seq %llu >= end %llu", (unsigned long long)(r->seq),
		      (unsigned long long)(st->end_seq));
		gaps += r->seq - expect;
		expect = r->seq + 1;
		CHECK(r->value == vsensor_model_value(r->seed, r->seq),
		      "seq %llu value %d != model %d", (unsigned long long)(r->seq), r->value,
		      vsensor_model_value(r->seed, r->seq));
		CHECK(r->flags == 0, "flags %u", r->flags);
		CHECK(r->timestamp_ns >= prev_ts, "timestamp went backwards at %llu", (unsigned long long)(r->seq));
		CHECK(r->config_gen >= prev_gen, "config_gen went backwards at %llu", (unsigned long long)(r->seq));
		prev_ts = r->timestamp_ns;
		prev_gen = r->config_gen;
	}
	gaps += st->end_seq - expect;	/* trailing */
	CHECK(gaps == st->dropped + st->flushed,
	      "gaps %llu != dropped %llu + flushed %llu",
	      (unsigned long long)(gaps), (unsigned long long)(st->dropped), (unsigned long long)(st->flushed));
	return 0;
}

static int stop_drain_check(int fd, struct recvec *v, struct vsensor_stats *st)
{
	CHECK(ioctl(fd, VSENSOR_IOC_STOP) == 0, "STOP: %s", strerror(errno));
	CHECK(drain(fd, v) == 0, "drain: %s", strerror(errno));
	CHECK(get_stats(fd, st) == 0, "GET_STATS: %s", strerror(errno));
	return check_stream(v, st);
}

/* ---- tests ---- */

static int t_record_contents(void)
{
	struct vsensor_stats st;
	struct recvec v = { 0 };
	long gen = set_cfg(500, 0xC0FFEE);
	int fd, ret;
	size_t i;

	CHECK(gen > 0, "SET_CONFIG failed");
	fd = rd_open(0);	/* opened after SET: every sample has the new config */
	CHECK(fd >= 0, "open: %s", strerror(errno));
	sleep_ms(200);
	ret = stop_drain_check(fd, &v, &st);
	close(fd);
	if (ret)
		return ret;
	CHECK(v.n > 10, "only %zu records in 200 ms at 2 kHz", v.n);
	CHECK(v.r[0].seq >= st.start_seq, "first seq before start_seq");
	for (i = 0; i < v.n; i++) {
		CHECK(v.r[i].seed == 0xC0FFEE, "seed %u", v.r[i].seed);
		CHECK(v.r[i].config_gen == (uint32_t)gen, "gen %u != %ld", v.r[i].config_gen, gen);
	}
	free(v.r);
	return 0;
}

static int t_nonblock_eagain(void)
{
	char buf[RS];
	int fd;
	ssize_t r;

	CHECK(set_cfg(VSENSOR_INTERVAL_MAX_US, 7) > 0, "SET_CONFIG");
	fd = rd_open(O_NONBLOCK);
	CHECK(fd >= 0, "open");
	CHECK(ioctl(fd, VSENSOR_IOC_FLUSH) == 0, "FLUSH");
	r = read(fd, buf, sizeof(buf));
	CHECK(r == -1 && errno == EAGAIN, "read returned %zd errno %d", r, errno);
	close(fd);
	return 0;
}

static int t_blocking_read_waits(void)
{
	struct vsensor_record rec;
	struct vsensor_stats st;
	uint64_t t0, dt;
	ssize_t r;
	int fd;

	CHECK(set_cfg(100000, 8) > 0, "SET_CONFIG");	/* 100 ms */
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	CHECK(ioctl(fd, VSENSOR_IOC_FLUSH) == 0, "FLUSH");
	CHECK(get_stats(fd, &st) == 0, "GET_STATS");
	t0 = now_ns();
	r = read(fd, &rec, sizeof(rec));
	dt = now_ns() - t0;
	CHECK(r == (ssize_t)RS, "read returned %zd: %s", r, strerror(errno));
	CHECK(rec.seq >= st.end_seq, "got a sample produced before the flush");
	CHECK(dt >= 5000000ULL, "read returned after %llu ns without blocking", (unsigned long long)(dt));
	close(fd);
	return 0;
}

static int t_read_size_rules(void)
{
	char buf[64];
	ssize_t r;
	int fd;

	CHECK(set_cfg(100, 9) > 0, "SET_CONFIG");
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	r = read(fd, buf, 0);
	CHECK(r == -1 && errno == EINVAL, "count 0: %zd errno %d", r, errno);
	r = read(fd, buf, RS - 1);
	CHECK(r == -1 && errno == EINVAL, "count 31: %zd errno %d", r, errno);
	r = read(fd, buf, RS + 18);	/* not a multiple: whole records only */
	CHECK(r == (ssize_t)RS, "count 50 returned %zd", r);
	CHECK(lseek(fd, 0, SEEK_SET) == -1 && errno == ESPIPE, "lseek should be ESPIPE");
	close(fd);
	return 0;
}

static int t_poll_timeout_and_ready(void)
{
	struct pollfd pfd;
	struct epoll_event ev, out;
	int fd, ep, n;

	CHECK(set_cfg(VSENSOR_INTERVAL_MAX_US, 10) > 0, "SET_CONFIG");	/* 1 s */
	fd = rd_open(O_NONBLOCK);
	CHECK(fd >= 0, "open");
	CHECK(ioctl(fd, VSENSOR_IOC_FLUSH) == 0, "FLUSH");

	pfd.fd = fd;
	pfd.events = POLLIN;
	n = poll(&pfd, 1, 100);
	CHECK(n == 0, "poll on empty ring returned %d (revents 0x%x)", n, pfd.revents);

	ep = epoll_create1(0);
	CHECK(ep >= 0, "epoll_create1");
	ev.events = EPOLLIN;
	ev.data.fd = fd;
	CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0, "epoll_ctl");
	n = epoll_wait(ep, &out, 1, 50);
	CHECK(n == 0, "epoll_wait on empty ring returned %d", n);

	/* Next sample is due within 1 s of the SET; allow a generous margin. */
	n = epoll_wait(ep, &out, 1, 5000);
	CHECK(n == 1 && (out.events & EPOLLIN), "epoll_wait for data returned %d", n);
	n = poll(&pfd, 1, 0);
	CHECK(n == 1 && (pfd.revents & POLLIN), "poll after data: %d 0x%x", n, pfd.revents);
	close(ep);
	close(fd);
	return 0;
}

static int t_ioctl_validation(void)
{
	struct vsensor_config before, after, c;
	struct vsensor_stats st;
	struct pollfd pfd;
	char buf[RS];
	int ctl, rd;

	CHECK(set_cfg(1000, 11) > 0, "SET_CONFIG");
	ctl = ctl_open();
	rd = rd_open(O_NONBLOCK);
	CHECK(ctl >= 0 && rd >= 0, "open");
	CHECK(ioctl(ctl, VSENSOR_IOC_GET_CONFIG, &before) == 0, "GET_CONFIG");

	c = (struct vsensor_config){ .interval_us = 0, .seed = 1 };
	CHECK(ioctl(ctl, VSENSOR_IOC_SET_CONFIG, &c) == -1 && errno == EINVAL, "interval 0");
	c.interval_us = VSENSOR_INTERVAL_MIN_US - 1;
	CHECK(ioctl(ctl, VSENSOR_IOC_SET_CONFIG, &c) == -1 && errno == EINVAL, "interval min-1");
	c.interval_us = VSENSOR_INTERVAL_MAX_US + 1;
	CHECK(ioctl(ctl, VSENSOR_IOC_SET_CONFIG, &c) == -1 && errno == EINVAL, "interval max+1");
	c.interval_us = 1000;
	c.flags = 1;
	CHECK(ioctl(ctl, VSENSOR_IOC_SET_CONFIG, &c) == -1 && errno == EINVAL, "flags != 0");
	CHECK(ioctl(ctl, VSENSOR_IOC_SET_CONFIG, (void *)8) == -1 && errno == EFAULT,
	      "bad pointer SET");
	CHECK(ioctl(ctl, VSENSOR_IOC_GET_STATS, (void *)8) == -1 && errno == EFAULT,
	      "bad pointer GET_STATS");
	CHECK(ioctl(ctl, VSENSOR_IOC_GET_CONFIG, NULL) == -1 && errno == EFAULT,
	      "NULL GET_CONFIG");
	CHECK(ioctl(ctl, _IO(VSENSOR_IOC_MAGIC, 99)) == -1 && errno == ENOTTY, "unknown cmd");
	CHECK(ioctl(ctl, _IOR(VSENSOR_IOC_MAGIC, 3, uint64_t), &st) == -1 && errno == ENOTTY,
	      "size-mismatched GET_STATS must not be accepted");

	/* Permission and handle-type rules. */
	c = (struct vsensor_config){ .interval_us = 1000, .seed = 1 };
	CHECK(ioctl(rd, VSENSOR_IOC_SET_CONFIG, &c) == -1 && errno == EPERM,
	      "SET_CONFIG on read-only fd");
	CHECK(ioctl(ctl, VSENSOR_IOC_FLUSH) == -1 && errno == EINVAL, "FLUSH on control");
	CHECK(ioctl(ctl, VSENSOR_IOC_STOP) == -1 && errno == EINVAL, "STOP on control");
	CHECK(read(ctl, buf, sizeof(buf)) == -1 && errno == EBADF, "read on control");
	CHECK(get_stats(ctl, &st) == 0 && !(st.flags & VSENSOR_STATS_READER),
	      "control handle must not report reader stats");
	pfd.fd = ctl;
	pfd.events = POLLIN;
	CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLERR), "poll on control");

	/* Rejected requests must not have changed anything. */
	CHECK(ioctl(ctl, VSENSOR_IOC_GET_CONFIG, &after) == 0, "GET_CONFIG");
	CHECK(memcmp(&before, &after, sizeof(before)) == 0, "config changed by rejected SET");
	close(rd);
	close(ctl);
	return 0;
}

static int t_config_change_seed_queued(void)
{
	/* Change seed/interval while earlier samples are still queued in the ring. */
	static const uint32_t seeds[3] = { 0xAAAA, 0xBBBB, 0xCCCC };
	static const uint32_t ivals[3] = { 500, 300, 700 };
	uint32_t gens[3];
	struct vsensor_stats st;
	struct recvec v = { 0 };
	unsigned int seen = 0;
	int fd, i, ret;
	size_t k;

	gens[0] = (uint32_t)set_cfg(ivals[0], seeds[0]);
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	for (i = 1; i < 3; i++) {
		sleep_ms(80);
		gens[i] = (uint32_t)set_cfg(ivals[i], seeds[i]);
	}
	sleep_ms(80);
	ret = stop_drain_check(fd, &v, &st);	/* values checked with per-record seed */
	close(fd);
	if (ret)
		return ret;
	for (k = 0; k < v.n; k++) {
		for (i = 0; i < 3; i++)
			if (v.r[k].config_gen == gens[i])
				break;
		CHECK(i < 3, "unexpected config_gen %u", v.r[k].config_gen);
		CHECK(v.r[k].seed == seeds[i], "gen %u carries seed %u, expected %u",
		      v.r[k].config_gen, v.r[k].seed, seeds[i]);
		seen |= 1U << i;
	}
	CHECK(seen == 7, "not all generations observed (mask %u)", seen);
	free(v.r);
	return 0;
}

static unsigned int module_param_uint(const char *name)
{
	char path[128];
	unsigned int v = 0;
	FILE *f;

	snprintf(path, sizeof(path), "/sys/module/vsensor/parameters/%s", name);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%u", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

static int t_overflow_trailing_drops(void)
{
	struct vsensor_stats st;
	struct recvec v = { 0 };
	unsigned int cap = module_param_uint("capacity");
	int fd, ret;
	size_t i;

	CHECK(cap >= 16, "capacity param unreadable");
	CHECK(set_cfg(20, 12) > 0, "SET_CONFIG");	/* 50 kHz */
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	/* Never read until the ring has overflowed several times over. */
	for (i = 0; i < 200; i++) {
		sleep_ms(50);
		CHECK(get_stats(fd, &st) == 0, "GET_STATS");
		if (st.dropped >= 2ULL * cap)
			break;
	}
	CHECK(st.dropped >= 2ULL * cap, "only %llu drops after 10 s with a full ring "
	      "(queued=%u)", (unsigned long long)st.dropped, st.queued);
	ret = stop_drain_check(fd, &v, &st);
	close(fd);
	if (ret)
		return ret;
	/* Tail drop: we keep exactly the first `cap` samples; every drop is trailing. */
	CHECK(v.n == cap, "received %zu, expected capacity %u", v.n, cap);
	CHECK(v.r[0].seq == st.start_seq, "first seq %llu != start %llu",
	      (unsigned long long)(v.r[0].seq), (unsigned long long)(st.start_seq));
	for (i = 1; i < v.n; i++)
		CHECK(v.r[i].seq == v.r[i - 1].seq + 1, "interior gap at %zu", i);
	CHECK(st.end_seq - (v.r[v.n - 1].seq + 1) == st.dropped,
	      "trailing gap %llu != dropped %llu",
	      (unsigned long long)(st.end_seq - (v.r[v.n - 1].seq + 1)), (unsigned long long)(st.dropped));
	CHECK(st.dev_dropped >= st.dropped, "device drop total below reader drops");
	free(v.r);
	return 0;
}

static int t_flush_accounting(void)
{
	struct vsensor_stats before, st;
	struct recvec v = { 0 };
	int fd, ret;

	CHECK(set_cfg(200, 13) > 0, "SET_CONFIG");
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	sleep_ms(50);
	CHECK(get_stats(fd, &before) == 0, "GET_STATS");
	CHECK(ioctl(fd, VSENSOR_IOC_FLUSH) == 0, "FLUSH");
	CHECK(get_stats(fd, &st) == 0, "GET_STATS");
	CHECK(st.queued == 0 || st.flushed > 0, "FLUSH left samples queued");
	CHECK(st.flushed >= before.queued, "flushed %llu < queued-before %u",
	      (unsigned long long)(st.flushed), before.queued);
	sleep_ms(50);
	ret = stop_drain_check(fd, &v, &st);	/* flushed seqs count as gaps */
	close(fd);
	if (ret)
		return ret;
	CHECK(st.flushed > 0, "nothing was flushed");
	free(v.r);
	return 0;
}

static int t_partial_copy_fault(void)
{
	long pg = sysconf(_SC_PAGESIZE);
	struct vsensor_record good[4];
	struct vsensor_stats st, st2;
	uint64_t first;
	char *base, *p;
	ssize_t r, rf;
	int fd;

	CHECK(set_cfg(50, 14) > 0, "SET_CONFIG");
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	do {
		sleep_ms(5);
		CHECK(get_stats(fd, &st) == 0, "GET_STATS");
	} while (st.queued < 16);
	CHECK(ioctl(fd, VSENSOR_IOC_STOP) == 0, "STOP");	/* freeze: deterministic ring */
	CHECK(get_stats(fd, &st) == 0, "GET_STATS");

	base = mmap(NULL, (size_t)pg * 2, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(base != MAP_FAILED, "mmap");
	CHECK(mprotect(base + pg, (size_t)pg, PROT_NONE) == 0, "mprotect");

	/* Entirely inaccessible buffer: -EFAULT and nothing consumed. */
	r = read(fd, base + pg, 4 * RS);
	CHECK(r == -1 && errno == EFAULT, "read into PROT_NONE returned %zd errno %d", r, errno);
	CHECK(get_stats(fd, &st2) == 0 && st2.delivered == st.delivered &&
	      st2.queued == st.queued, "failed read consumed samples");

	/* 3.5 records fit before the guard page. */
	p = base + pg - 3 * RS - RS / 2;
	r = read(fd, p, 8 * RS);
	CHECK(r > 0 && r % (ssize_t)RS == 0 && r <= (ssize_t)(3 * RS),
	      "partial read returned %zd (errno %d)", r, errno);
	memcpy(&first, p, sizeof(first));	/* seq of the first record copied */

	/* The record that straddled the guard page must be returned next. */
	rf = read(fd, good, sizeof(good));
	CHECK(rf > 0, "follow-up read: %zd errno %d", rf, errno);
	CHECK(good[0].seq == first + (uint64_t)r / RS,
	      "lost samples: next seq %llu, expected %llu",
	      (unsigned long long)(good[0].seq), (unsigned long long)(first + (uint64_t)r / RS));
	CHECK(get_stats(fd, &st2) == 0, "GET_STATS");
	CHECK(st2.delivered == st.delivered + (uint64_t)(r + rf) / RS,
	      "delivered %llu, expected %llu", (unsigned long long)(st2.delivered),
	      (unsigned long long)(st.delivered + (uint64_t)(r + rf) / RS));
	munmap(base, (size_t)pg * 2);
	close(fd);
	return 0;
}

struct mr_arg {
	int fd;
	unsigned int slow_us;
	volatile bool *stop;
	struct recvec v;
	struct vsensor_stats st;
	int ret;
	char err[512];
};

static void *mr_thread(void *p)
{
	struct mr_arg *a = p;
	struct vsensor_record buf[32];
	ssize_t r;

	while (!*a->stop) {
		r = read(a->fd, buf, a->slow_us ? RS : sizeof(buf));
		if (r > 0)
			rv_push(&a->v, buf, (size_t)r / RS);
		if (a->slow_us)
			usleep(a->slow_us);
	}
	a->ret = stop_drain_check(a->fd, &a->v, &a->st);
	if (a->ret)
		memcpy(a->err, fail_buf, sizeof(a->err));
	return NULL;
}

static int t_concurrent_independent_readers(void)
{
	enum { N = 4 };
	struct mr_arg args[N];
	pthread_t th[N];
	volatile bool stop = false;
	uint64_t lo = 0, hi = UINT64_MAX;
	int i, j;
	size_t k, idx[N] = { 0 };

	CHECK(set_cfg(100, 15) > 0, "SET_CONFIG");	/* 10 kHz */
	memset(args, 0, sizeof(args));
	for (i = 0; i < N; i++) {
		args[i].fd = rd_open(0);
		CHECK(args[i].fd >= 0, "open %d", i);
		args[i].stop = &stop;
		args[i].slow_us = i == N - 1 ? 2000 : 0;	/* one slow reader */
	}
	for (i = 0; i < N; i++)
		pthread_create(&th[i], NULL, mr_thread, &args[i]);
	sleep_ms(600);
	stop = true;
	for (i = 0; i < N; i++)
		pthread_join(th[i], NULL);
	for (i = 0; i < N; i++) {
		close(args[i].fd);
		if (args[i].ret) {
			memcpy(fail_buf, args[i].err, sizeof(fail_buf));
			return -1;
		}
	}
	CHECK(args[N - 1].st.dropped > 0, "slow reader never dropped");
	for (i = 0; i < N - 1; i++)
		CHECK(args[i].v.n > args[N - 1].v.n,
		      "fast reader %d got fewer samples than the slow one", i);

	/* Broadcast: a seq seen by two readers must be the identical sample. */
	for (i = 0; i < N; i++) {
		if (args[i].st.start_seq > lo)
			lo = args[i].st.start_seq;
		if (args[i].st.end_seq < hi)
			hi = args[i].st.end_seq;
	}
	for (k = 0; k < args[0].v.n; k++) {
		const struct vsensor_record *r0 = &args[0].v.r[k];

		if (r0->seq < lo || r0->seq >= hi)
			continue;
		for (j = 1; j < N; j++) {
			while (idx[j] < args[j].v.n && args[j].v.r[idx[j]].seq < r0->seq)
				idx[j]++;
			if (idx[j] < args[j].v.n && args[j].v.r[idx[j]].seq == r0->seq)
				CHECK(memcmp(r0, &args[j].v.r[idx[j]], RS) == 0,
				      "reader %d differs at seq %llu", j, (unsigned long long)(r0->seq));
		}
	}
	for (i = 0; i < N; i++)
		free(args[i].v.r);
	return 0;
}

struct sh_arg {
	int fd;
	struct recvec v;
};

static void *sh_thread(void *p)
{
	struct sh_arg *a = p;
	struct vsensor_record buf[7];
	ssize_t r;

	for (;;) {
		r = read(a->fd, buf, sizeof(buf));
		if (r > 0)
			rv_push(&a->v, buf, (size_t)r / RS);
		else if (r == 0 || errno != EINTR)
			break;	/* 0 = frozen and drained */
	}
	return NULL;
}

static int cmp_seq(const void *a, const void *b)
{
	uint64_t x = ((const struct vsensor_record *)a)->seq;
	uint64_t y = ((const struct vsensor_record *)b)->seq;

	return x < y ? -1 : x > y;
}

static int t_shared_fd_competing_threads(void)
{
	struct sh_arg a[2] = { 0 };
	struct recvec all = { 0 };
	struct vsensor_stats st;
	pthread_t th[2];
	int fd, i, ret;

	CHECK(set_cfg(100, 16) > 0, "SET_CONFIG");
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	for (i = 0; i < 2; i++) {
		a[i].fd = fd;
		pthread_create(&th[i], NULL, sh_thread, &a[i]);
	}
	sleep_ms(400);
	CHECK(ioctl(fd, VSENSOR_IOC_STOP) == 0, "STOP");	/* wakes both with EOF */
	for (i = 0; i < 2; i++)
		pthread_join(th[i], NULL);
	CHECK(get_stats(fd, &st) == 0, "GET_STATS");
	close(fd);
	for (i = 0; i < 2; i++) {
		CHECK(a[i].v.n > 0, "thread %d received nothing", i);
		rv_push(&all, a[i].v.r, a[i].v.n);
		free(a[i].v.r);
	}
	/* Each sample is delivered to exactly one of the threads sharing the fd. */
	qsort(all.r, all.n, RS, cmp_seq);
	for (size_t k = 1; k < all.n; k++)
		CHECK(all.r[k].seq != all.r[k - 1].seq, "seq %llu delivered twice",
		      (unsigned long long)(all.r[k].seq));
	ret = check_stream(&all, &st);
	free(all.r);
	return ret;
}

static int t_max_readers(void)
{
	unsigned int max = module_param_uint("max_readers");
	struct vsensor_stats st;
	int fds[64], ctl, extra;
	unsigned int i;

	CHECK(max >= 1 && max <= 64, "max_readers param unreadable");
	CHECK(set_cfg(1000, 17) > 0, "SET_CONFIG");
	for (i = 0; i < max; i++) {
		fds[i] = rd_open(0);
		CHECK(fds[i] >= 0, "open reader %u: %s", i, strerror(errno));
	}
	extra = rd_open(0);
	CHECK(extra == -1 && errno == EMFILE, "reader %u should fail with EMFILE", max);
	ctl = ctl_open();
	CHECK(ctl >= 0, "control handle must not count as a reader");
	CHECK(get_stats(ctl, &st) == 0 && st.readers == max, "readers=%u", st.readers);
	for (i = 0; i < max; i++)
		close(fds[i]);
	CHECK(get_stats(ctl, &st) == 0 && st.readers == 0, "readers=%u after close", st.readers);
	close(ctl);
	return 0;
}

static volatile sig_atomic_t sig_hits;

static void on_usr1(int sig)
{
	(void)sig;
	sig_hits++;
}

struct sig_arg {
	ssize_t r;
	int err;
	uint64_t seq;
};

static void *sig_reader(void *p)
{
	struct sig_arg *a = p;
	struct vsensor_record rec;
	int fd = rd_open(0);

	ioctl(fd, VSENSOR_IOC_FLUSH);
	a->r = read(fd, &rec, sizeof(rec));
	a->err = errno;
	a->seq = rec.seq;
	close(fd);
	return NULL;
}

static int run_signal_case(int sa_flags, struct sig_arg *a)
{
	struct sigaction sa = { 0 };
	pthread_t th;

	sa.sa_handler = on_usr1;
	sa.sa_flags = sa_flags;
	sigemptyset(&sa.sa_mask);
	CHECK(sigaction(SIGUSR1, &sa, NULL) == 0, "sigaction");
	CHECK(set_cfg(VSENSOR_INTERVAL_MAX_US, 18) > 0, "SET_CONFIG");	/* 1 s */
	sig_hits = 0;
	memset(a, 0, sizeof(*a));
	pthread_create(&th, NULL, sig_reader, a);
	sleep_ms(150);
	pthread_kill(th, SIGUSR1);
	pthread_join(th, NULL);
	CHECK(sig_hits == 1, "handler ran %d times", (int)sig_hits);
	return 0;
}

static int t_signal_interrupts_read(void)
{
	struct sig_arg a;

	if (run_signal_case(0, &a))
		return -1;
	CHECK(a.r == -1 && a.err == EINTR, "without SA_RESTART: r=%zd errno=%d", a.r, a.err);
	if (run_signal_case(SA_RESTART, &a))
		return -1;
	CHECK(a.r == (ssize_t)RS, "with SA_RESTART read should restart and return a sample: "
	      "r=%zd errno=%d", a.r, a.err);
	signal(SIGUSR1, SIG_DFL);
	return 0;
}

static void *stop_blocked_reader(void *p)
{
	struct sig_arg *a = p;
	struct vsensor_record rec;

	a->r = read((int)a->seq, &rec, sizeof(rec));
	a->err = errno;
	return NULL;
}

static int t_stop_wakes_blocked_reader(void)
{
	struct sig_arg a = { 0 };
	struct pollfd pfd;
	pthread_t th;
	int fd;

	CHECK(set_cfg(VSENSOR_INTERVAL_MAX_US, 19) > 0, "SET_CONFIG");
	fd = rd_open(0);
	CHECK(fd >= 0, "open");
	CHECK(ioctl(fd, VSENSOR_IOC_FLUSH) == 0, "FLUSH");
	a.seq = (uint64_t)fd;
	pthread_create(&th, NULL, stop_blocked_reader, &a);
	sleep_ms(100);
	CHECK(ioctl(fd, VSENSOR_IOC_STOP) == 0, "STOP");
	pthread_join(th, NULL);
	/* Either EOF, or a sample that raced in before STOP (then drain -> EOF). */
	CHECK(a.r == 0 || a.r == (ssize_t)RS, "blocked read returned %zd errno %d", a.r, a.err);
	if (a.r == (ssize_t)RS) {
		struct vsensor_record rec;

		while (read(fd, &rec, sizeof(rec)) > 0)
			;
	}
	pfd.fd = fd;
	pfd.events = POLLIN;
	CHECK(poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLHUP), "poll after drain: 0x%x",
	      pfd.revents);
	CHECK(ioctl(fd, VSENSOR_IOC_STOP) == 0, "STOP must be idempotent");
	close(fd);
	return 0;
}

static int t_child_killed_while_blocked(void)
{
	struct vsensor_stats st;
	int pfd[2], ctl, status;
	char c;
	pid_t pid;

	CHECK(set_cfg(VSENSOR_INTERVAL_MAX_US, 20) > 0, "SET_CONFIG");
	CHECK(pipe(pfd) == 0, "pipe");
	pid = fork();
	CHECK(pid >= 0, "fork");
	if (pid == 0) {
		struct vsensor_record rec;
		int fd = rd_open(0);

		ioctl(fd, VSENSOR_IOC_FLUSH);
		if (write(pfd[1], "r", 1) != 1)
			_exit(3);
		for (;;)
			if (read(fd, &rec, sizeof(rec)) < 0 && errno != EINTR)
				_exit(4);
	}
	close(pfd[1]);
	CHECK(read(pfd[0], &c, 1) == 1, "child never became ready");
	close(pfd[0]);
	sleep_ms(50);
	kill(pid, SIGKILL);
	CHECK(waitpid(pid, &status, 0) == pid, "waitpid");
	CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "child status 0x%x", status);
	ctl = ctl_open();
	CHECK(ctl >= 0, "open control");
	CHECK(get_stats(ctl, &st) == 0 && st.readers == 0,
	      "killed child's reader not released (readers=%u)", st.readers);
	close(ctl);
	return 0;
}

struct test {
	const char *name;
	int (*fn)(void);
};

static const struct test tests[] = {
	{ "record_contents", t_record_contents },
	{ "nonblock_eagain", t_nonblock_eagain },
	{ "blocking_read_waits", t_blocking_read_waits },
	{ "read_size_rules", t_read_size_rules },
	{ "poll_timeout_and_ready", t_poll_timeout_and_ready },
	{ "ioctl_validation", t_ioctl_validation },
	{ "config_change_seed_queued", t_config_change_seed_queued },
	{ "overflow_trailing_drops", t_overflow_trailing_drops },
	{ "flush_accounting", t_flush_accounting },
	{ "partial_copy_fault", t_partial_copy_fault },
	{ "concurrent_independent_readers", t_concurrent_independent_readers },
	{ "shared_fd_competing_threads", t_shared_fd_competing_threads },
	{ "max_readers", t_max_readers },
	{ "signal_interrupts_read", t_signal_interrupts_read },
	{ "stop_wakes_blocked_reader", t_stop_wakes_blocked_reader },
	{ "child_killed_while_blocked", t_child_killed_while_blocked },
};

int main(int argc, char **argv)
{
	size_t i, n = sizeof(tests) / sizeof(tests[0]), run = 0;
	int failed = 0, a;

	printf("TAP version 13\n");
	for (i = 0; i < n; i++) {
		bool sel = argc == 1;

		for (a = 1; a < argc; a++)
			sel |= strcmp(argv[a], tests[i].name) == 0;
		if (!sel)
			continue;
		run++;
		fail_buf[0] = 0;
		if (tests[i].fn() == 0) {
			printf("ok %zu - %s\n", run, tests[i].name);
		} else {
			printf("not ok %zu - %s\n#   %s\n", run, tests[i].name, fail_buf);
			failed++;
		}
		fflush(stdout);
	}
	printf("1..%zu\n", run);
	set_cfg(1000, 1);	/* leave the device at its load-time defaults */
	return failed ? 1 : 0;
}
