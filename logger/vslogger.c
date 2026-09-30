// SPDX-License-Identifier: GPL-2.0
/*
 * vslogger - concurrent logger for /dev/vsensor.
 *
 * Threads:
 *   main    owns signalfd (SIGINT/SIGTERM) and timerfd (--duration). It never
 *           touches the data queue except through the non-blocking stop and
 *           abort calls, so it stays responsive however full the queue is.
 *   reader  epoll over the device fd (O_NONBLOCK) and a stop eventfd; reads
 *           batches, performs sequence/timestamp accounting and latency
 *           stamping, and hands batches to the workers via a bounded queue.
 *   workers pop batches, verify each value against the model using the seed
 *           carried in the record, apply a configurable busy-work load, and
 *           optionally write CSV.
 *
 * Accounting (all checked at exit; see DESIGN.md):
 *   kernel:  end_seq - start_seq == delivered + dropped + flushed
 *   stream:  received == delivered, gaps == dropped + flushed
 *   app:     received == processed + app_dropped + shutdown_discarded
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "uapi/vsensor.h"
#include "vsensor_model.h"
#include "bqueue.h"
#include "hist.h"

_Static_assert(sizeof(struct vsensor_record) == 32, "record ABI");
_Static_assert(sizeof(struct vsensor_stats) == 104, "stats ABI");

#define RS sizeof(struct vsensor_record)

enum policy { POLICY_DROP, POLICY_BLOCK };
enum on_stop { ON_STOP_DRAIN, ON_STOP_DISCARD };

struct opts {
	const char *device;
	double duration_s;	/* 0 = until signal */
	unsigned int batch;	/* records per read() */
	unsigned int queue;	/* batches in the app queue */
	unsigned int workers;
	unsigned int work_ns;	/* busy work per record */
	unsigned int read_period_us;	/* 0 = read on readiness; else on a timer */
	enum policy policy;
	enum on_stop on_stop;
	bool verify;		/* --mode=verify: strict exit status */
	long set_interval_us;	/* -1 = leave device config alone */
	long long set_seed;	/* -1 = keep current seed */
	const char *out_csv;
	const char *json_path;
	const char *label;
};

struct batch {
	size_t n;
	uint64_t recv_ns;
	struct vsensor_record *recs;
};

struct worker {
	pthread_t th;
	struct app *app;
	uint64_t processed;
	uint64_t value_errors;
	uint64_t discarded;	/* popped but not processed because of abort */
	struct hist e2e;	/* production -> worker start, ns */
};

struct cpu_snap {
	struct timespec wall;
	struct rusage ru;
	uint64_t sys_total;
	uint64_t sys_idle;
};

struct app {
	struct opts o;
	int devfd;
	int stop_efd;		/* main -> reader */
	int done_efd;		/* reader -> main */
	struct bq q;		/* full batches, reader -> workers */
	struct bq pool;		/* empty batches */
	struct batch *batches;
	size_t nbatches;
	atomic_bool stop;
	atomic_bool abort;
	FILE *csv;
	pthread_mutex_t csv_mu;

	struct vsensor_config cfg;
	struct vsensor_stats st_start;
	struct vsensor_stats st_end;

	/* reader-thread state */
	bool have_prev;
	uint64_t expected_seq;
	uint64_t prev_ts;
	uint32_t prev_gen;
	uint64_t received;
	uint64_t reads;
	uint64_t gaps;		/* missing seqs incl. leading and trailing */
	uint64_t gap_events;
	uint64_t trailing_gap;
	uint64_t order_errors;
	uint64_t ts_errors;
	uint64_t gen_errors;
	uint64_t gens_seen;
	uint64_t app_dropped;
	uint64_t shutdown_discarded;	/* reader + main */
	struct hist read_lat;	/* production -> read() return, ns */
	struct hist jitter;	/* |inter-sample delta - interval|, ns */
	uint64_t stop_req_ns;
	uint64_t stop_done_ns;
	uint64_t open_ns;	/* just before open(): stream start */
	uint64_t freeze_ns;	/* just before VSENSOR_IOC_STOP: stream end */

	struct worker *w;
	int fatal_errno;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void die(const char *what)
{
	fprintf(stderr, "vslogger: %s: %s\n", what, strerror(errno));
	exit(2);
}

static void notify(int efd)
{
	uint64_t one = 1;

	if (write(efd, &one, sizeof(one)) != sizeof(one))
		die("eventfd write");
}

/* ---- batch pool ---- */

static struct batch *pool_get(struct app *a)
{
	void *p;

	/*
	 * The pool holds queue + workers + 2 batches: at most `queue` are
	 * queued, each worker holds one, and the reader holds at most two
	 * (current + pending), so this never finds the pool empty.
	 */
	if (bq_trypop(&a->pool, &p) != BQ_OK) {
		fprintf(stderr, "vslogger: batch pool exhausted (bug)\n");
		abort();
	}
	((struct batch *)p)->n = 0;
	return p;
}

static void pool_put(struct app *a, struct batch *b)
{
	b->n = 0;
	if (bq_push(&a->pool, b, BQ_TRY) != BQ_OK) {
		fprintf(stderr, "vslogger: batch pool overflow (bug)\n");
		abort();
	}
}

/* ---- reader thread ---- */

static void account(struct app *a, const struct batch *b)
{
	uint64_t interval_ns = (uint64_t)a->cfg.interval_us * 1000ULL;
	size_t i;

	for (i = 0; i < b->n; i++) {
		const struct vsensor_record *r = &b->recs[i];

		if (!a->have_prev) {
			/* Leading gap: samples dropped before our first read. */
			if (r->seq < a->st_start.start_seq) {
				a->order_errors++;
			} else if (r->seq > a->st_start.start_seq) {
				a->gaps += r->seq - a->st_start.start_seq;
				a->gap_events++;
			}
			a->gens_seen = 1;
		} else {
			if (r->seq < a->expected_seq) {
				a->order_errors++;
			} else if (r->seq > a->expected_seq) {
				a->gaps += r->seq - a->expected_seq;
				a->gap_events++;
			}
			if (r->timestamp_ns < a->prev_ts)
				a->ts_errors++;
			if (r->config_gen < a->prev_gen)
				a->gen_errors++;
			else if (r->config_gen != a->prev_gen)
				a->gens_seen++;
			/* Jitter only between adjacent samples of the start config. */
			if (r->seq == a->expected_seq &&
			    r->config_gen == a->prev_gen &&
			    r->config_gen == a->cfg.config_gen &&
			    r->timestamp_ns >= a->prev_ts) {
				uint64_t d = r->timestamp_ns - a->prev_ts;

				hist_add(&a->jitter, d > interval_ns ? d - interval_ns
								     : interval_ns - d);
			}
		}
		a->have_prev = true;
		a->expected_seq = r->seq + 1;
		a->prev_ts = r->timestamp_ns;
		a->prev_gen = r->config_gen;
		hist_add(&a->read_lat, b->recv_ns >= r->timestamp_ns ?
				       b->recv_ns - r->timestamp_ns : 0);
	}
	a->received += b->n;
}

/*
 * Hand a freshly read batch to the workers according to --policy.
 * Returns false if the batch could not be queued because a stop was
 * requested; the caller then still owns *bp and must finish it.
 */
static bool deliver(struct app *a, struct batch **bp)
{
	struct batch *b = *bp;
	enum bq_rc rc;

	rc = bq_push(&a->q, b, a->o.policy == POLICY_DROP ? BQ_TRY : BQ_WAIT);
	switch (rc) {
	case BQ_OK:
		*bp = pool_get(a);
		return true;
	case BQ_FULL:
		a->app_dropped += b->n;
		b->n = 0;
		return true;
	case BQ_STOPPING:
		return false;
	default:	/* aborted */
		a->shutdown_discarded += b->n;
		b->n = 0;
		return false;
	}
}

/* Shutdown path: queue (drain) or count (discard/abort) a received batch. */
static void finish(struct app *a, struct batch **bp)
{
	struct batch *b = *bp;

	if (b->n == 0)
		return;
	if (a->o.on_stop == ON_STOP_DRAIN && !atomic_load(&a->abort) &&
	    bq_push(&a->q, b, BQ_WAIT_DRAIN) == BQ_OK) {
		*bp = pool_get(a);
		return;
	}
	a->shutdown_discarded += b->n;
	b->n = 0;
}

static void *reader_main(void *arg)
{
	struct app *a = arg;
	struct epoll_event ev, evs[2];
	struct batch *b;
	ssize_t r;
	int ep, i, n, wake_fd = a->devfd;
	bool eof = false;

	ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0)
		die("epoll_create1");
	/*
	 * Readiness mode wakes on every sample. Periodic mode wakes on a timer
	 * and drains whatever accumulated, trading latency for fewer wakeups
	 * and read() calls.
	 */
	if (a->o.read_period_us) {
		struct itimerspec its = { 0 };

		its.it_interval.tv_sec = a->o.read_period_us / 1000000;
		its.it_interval.tv_nsec = (long)(a->o.read_period_us % 1000000) * 1000;
		its.it_value = its.it_interval;
		wake_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
		if (wake_fd < 0 || timerfd_settime(wake_fd, 0, &its, NULL))
			die("reader timerfd");
	}
	ev.events = EPOLLIN;
	ev.data.fd = wake_fd;
	if (epoll_ctl(ep, EPOLL_CTL_ADD, wake_fd, &ev))
		die("epoll_ctl wake");
	ev.data.fd = a->stop_efd;
	if (epoll_ctl(ep, EPOLL_CTL_ADD, a->stop_efd, &ev))
		die("epoll_ctl stop");

	b = pool_get(a);
	while (!atomic_load(&a->stop) && !eof) {
		n = epoll_wait(ep, evs, 2, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("epoll_wait");
		}
		for (i = 0; i < n; i++)
			if (evs[i].data.fd == a->stop_efd)
				goto shutdown;
		if (wake_fd != a->devfd) {
			uint64_t ticks;

			if (read(wake_fd, &ticks, sizeof(ticks)) < 0 && errno != EAGAIN)
				die("reader timerfd read");
		}
		/* Device readable: read until EAGAIN, re-checking stop each time. */
		while (!atomic_load(&a->stop)) {
			r = read(a->devfd, b->recs, a->o.batch * RS);
			if (r < 0) {
				if (errno == EAGAIN)
					break;
				if (errno == EINTR)
					continue;
				die("read");
			}
			if (r == 0) {	/* frozen by someone else */
				eof = true;
				break;
			}
			b->n = (size_t)r / RS;
			b->recv_ns = now_ns();
			a->reads++;
			account(a, b);
			if (!deliver(a, &b))
				goto shutdown;
		}
	}

shutdown:
	/* Freeze the stream so end_seq is fixed, then drain what is queued. */
	a->freeze_ns = now_ns();
	if (ioctl(a->devfd, VSENSOR_IOC_STOP))
		die("VSENSOR_IOC_STOP");
	finish(a, &b);
	for (;;) {
		r = read(a->devfd, b->recs, a->o.batch * RS);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			die("drain read");
		}
		if (r == 0)
			break;
		b->n = (size_t)r / RS;
		b->recv_ns = now_ns();
		a->reads++;
		account(a, b);
		finish(a, &b);
	}
	pool_put(a, b);
	if (ioctl(a->devfd, VSENSOR_IOC_GET_STATS, &a->st_end))
		die("VSENSOR_IOC_GET_STATS");
	/* Trailing gap: samples dropped after the last one we received. */
	{
		uint64_t last = a->have_prev ? a->expected_seq : a->st_end.start_seq;

		if (a->st_end.end_seq > last) {
			a->trailing_gap = a->st_end.end_seq - last;
			a->gaps += a->trailing_gap;
			a->gap_events++;
		}
	}
	bq_close(&a->q);
	if (wake_fd != a->devfd)
		close(wake_fd);
	close(ep);
	notify(a->done_efd);
	return NULL;
}

/* ---- workers ---- */

static void busy_wait(unsigned int ns)
{
	uint64_t end = now_ns() + ns;

	while (now_ns() < end)
		;
}

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	struct app *a = w->app;
	void *p;
	size_t i;

	while (bq_pop(&a->q, &p) == BQ_OK) {
		struct batch *b = p;
		uint64_t t = now_ns();

		if (a->csv)
			pthread_mutex_lock(&a->csv_mu);
		for (i = 0; i < b->n; i++) {
			const struct vsensor_record *r = &b->recs[i];

			if (atomic_load_explicit(&a->abort, memory_order_relaxed)) {
				w->discarded += b->n - i;
				break;
			}
			if (r->value != vsensor_model_value(r->seed, r->seq))
				w->value_errors++;
			hist_add(&w->e2e, t >= r->timestamp_ns ? t - r->timestamp_ns : 0);
			if (a->o.work_ns)
				busy_wait(a->o.work_ns);
			if (a->csv)
				fprintf(a->csv, "%llu,%llu,%d,%u,%u\n",
					(unsigned long long)(r->seq), (unsigned long long)(r->timestamp_ns), r->value, r->seed,
					r->config_gen);
			w->processed++;
		}
		if (a->csv)
			pthread_mutex_unlock(&a->csv_mu);
		pool_put(a, b);
	}
	return NULL;
}

/* ---- CPU accounting ---- */

static void read_proc_stat(uint64_t *total, uint64_t *idle)
{
	unsigned long long v[10] = { 0 };
	FILE *f = fopen("/proc/stat", "r");
	int i;

	*total = *idle = 0;
	if (!f)
		return;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
		   &v[8], &v[9]) >= 8) {
		/* user nice system idle iowait irq softirq steal (guest* are in user) */
		for (i = 0; i < 8; i++)
			*total += v[i];
		*idle = v[3] + v[4];
	}
	fclose(f);
}

static void snap(struct cpu_snap *s)
{
	clock_gettime(CLOCK_MONOTONIC, &s->wall);
	getrusage(RUSAGE_SELF, &s->ru);
	read_proc_stat(&s->sys_total, &s->sys_idle);
}

static double tv_s(struct timeval tv)
{
	return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static double ts_s(struct timespec ts)
{
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- options ---- */

static void usage(void)
{
	fprintf(stderr,
"usage: vslogger [options]\n"
"  -d, --device PATH        device (default " VSENSOR_DEVICE_PATH ")\n"
"  -t, --duration SEC       run time; 0 = until SIGINT/SIGTERM (default 0)\n"
"  -b, --batch N            records per read() (1..256, default 64)\n"
"  -q, --queue N            app queue capacity in batches (default 64)\n"
"  -w, --workers N          worker threads (default 2)\n"
"  -l, --work-ns N          busy work per record in ns (default 0)\n"
"  -r, --read-period-us N   read on a timer every N us instead of on readiness\n"
"  -p, --policy drop|block  app queue full: drop batch or block reader (default drop)\n"
"  -s, --on-stop drain|discard  pending data at shutdown (default drain)\n"
"  -m, --mode verify|bench  verify: nonzero exit on any check failure (default)\n"
"  -i, --interval-us N      set device interval before starting (needs write access)\n"
"  -S, --seed N             set device seed before starting (needs write access)\n"
"  -o, --out FILE           write processed records as CSV\n"
"  -j, --json FILE          write JSON summary to FILE (default stdout)\n"
"  -L, --label TEXT         label copied into the JSON summary\n"
"A second SIGINT/SIGTERM during a drain switches to discard.\n");
	exit(2);
}

static unsigned long parse_ul(const char *s, unsigned long lo, unsigned long hi)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end || v < lo || v > hi) {
		fprintf(stderr, "vslogger: bad value '%s' (range %lu..%lu)\n", s, lo, hi);
		exit(2);
	}
	return v;
}

static void parse_opts(struct opts *o, int argc, char **argv)
{
	static const struct option lo[] = {
		{ "device", required_argument, 0, 'd' },
		{ "duration", required_argument, 0, 't' },
		{ "batch", required_argument, 0, 'b' },
		{ "queue", required_argument, 0, 'q' },
		{ "workers", required_argument, 0, 'w' },
		{ "work-ns", required_argument, 0, 'l' },
		{ "read-period-us", required_argument, 0, 'r' },
		{ "policy", required_argument, 0, 'p' },
		{ "on-stop", required_argument, 0, 's' },
		{ "mode", required_argument, 0, 'm' },
		{ "interval-us", required_argument, 0, 'i' },
		{ "seed", required_argument, 0, 'S' },
		{ "out", required_argument, 0, 'o' },
		{ "json", required_argument, 0, 'j' },
		{ "label", required_argument, 0, 'L' },
		{ "help", no_argument, 0, 'h' },
		{ 0, 0, 0, 0 },
	};
	int c;
	char *end;

	*o = (struct opts){
		.device = VSENSOR_DEVICE_PATH, .batch = 64, .queue = 64,
		.workers = 2, .policy = POLICY_DROP, .on_stop = ON_STOP_DRAIN,
		.verify = true, .set_interval_us = -1, .set_seed = -1, .label = "",
	};
	while ((c = getopt_long(argc, argv, "d:t:b:q:w:l:r:p:s:m:i:S:o:j:L:h", lo, NULL)) != -1) {
		switch (c) {
		case 'd': o->device = optarg; break;
		case 't':
			o->duration_s = strtod(optarg, &end);
			if (*end || o->duration_s < 0)
				usage();
			break;
		case 'b': o->batch = parse_ul(optarg, 1, 256); break;
		case 'q': o->queue = parse_ul(optarg, 1, 65536); break;
		case 'w': o->workers = parse_ul(optarg, 1, 64); break;
		case 'l': o->work_ns = parse_ul(optarg, 0, 100000000); break;
		case 'r': o->read_period_us = parse_ul(optarg, 0, 1000000); break;
		case 'p':
			if (!strcmp(optarg, "drop")) o->policy = POLICY_DROP;
			else if (!strcmp(optarg, "block")) o->policy = POLICY_BLOCK;
			else usage();
			break;
		case 's':
			if (!strcmp(optarg, "drain")) o->on_stop = ON_STOP_DRAIN;
			else if (!strcmp(optarg, "discard")) o->on_stop = ON_STOP_DISCARD;
			else usage();
			break;
		case 'm':
			if (!strcmp(optarg, "verify")) o->verify = true;
			else if (!strcmp(optarg, "bench")) o->verify = false;
			else usage();
			break;
		case 'i':
			o->set_interval_us = (long)parse_ul(optarg, VSENSOR_INTERVAL_MIN_US,
							    VSENSOR_INTERVAL_MAX_US);
			break;
		case 'S': o->set_seed = (long long)parse_ul(optarg, 0, UINT32_MAX); break;
		case 'o': o->out_csv = optarg; break;
		case 'j': o->json_path = optarg; break;
		case 'L': o->label = optarg; break;
		default: usage();
		}
	}
	if (optind != argc)
		usage();
}

/* ---- setup and main loop ---- */

static void configure_device(struct app *a)
{
	struct vsensor_config cfg;
	int fd;

	if (a->o.set_interval_us < 0 && a->o.set_seed < 0)
		return;
	fd = open(a->o.device, O_WRONLY | O_CLOEXEC);	/* control handle: no stream */
	if (fd < 0)
		die("open control handle");
	if (ioctl(fd, VSENSOR_IOC_GET_CONFIG, &cfg))
		die("VSENSOR_IOC_GET_CONFIG");
	if (a->o.set_interval_us >= 0)
		cfg.interval_us = (uint32_t)a->o.set_interval_us;
	if (a->o.set_seed >= 0)
		cfg.seed = (uint32_t)a->o.set_seed;
	cfg.flags = 0;
	if (ioctl(fd, VSENSOR_IOC_SET_CONFIG, &cfg))
		die("VSENSOR_IOC_SET_CONFIG");
	close(fd);
}

static void request_stop(struct app *a)
{
	if (atomic_exchange(&a->stop, true))
		return;
	a->stop_req_ns = now_ns();
	notify(a->stop_efd);
	bq_request_stop(&a->q);
	if (a->o.on_stop == ON_STOP_DISCARD) {
		atomic_store(&a->abort, true);
		bq_abort(&a->q);
	}
}

static void request_abort(struct app *a)
{
	atomic_store(&a->abort, true);
	bq_abort(&a->q);
}

static int main_loop(struct app *a, int sfd, int tfd)
{
	struct epoll_event ev, evs[3];
	int ep, i, n, signals = 0;

	ep = epoll_create1(EPOLL_CLOEXEC);
	if (ep < 0)
		die("epoll_create1");
	ev.events = EPOLLIN;
	ev.data.fd = sfd;
	if (epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev))
		die("epoll_ctl signalfd");
	ev.data.fd = a->done_efd;
	if (epoll_ctl(ep, EPOLL_CTL_ADD, a->done_efd, &ev))
		die("epoll_ctl done");
	if (tfd >= 0) {
		ev.data.fd = tfd;
		if (epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev))
			die("epoll_ctl timerfd");
	}

	for (;;) {
		n = epoll_wait(ep, evs, 3, -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			die("epoll_wait main");
		}
		for (i = 0; i < n; i++) {
			int fd = evs[i].data.fd;

			if (fd == sfd) {
				struct signalfd_siginfo si;

				if (read(sfd, &si, sizeof(si)) != sizeof(si))
					die("signalfd read");
				if (++signals == 1)
					request_stop(a);
				else
					request_abort(a);
			} else if (fd == tfd) {
				uint64_t exp;

				if (read(tfd, &exp, sizeof(exp)) != sizeof(exp))
					die("timerfd read");
				request_stop(a);
			} else if (fd == a->done_efd) {
				close(ep);
				return signals;
			}
		}
	}
}

static void print_hist(FILE *f, const char *name, const struct hist *h, bool comma)
{
	fprintf(f, "  \"%s\": {\"count\": %llu, \"min\": %llu"
		   ", \"mean\": %.1f, \"p50\": %llu, \"p95\": %llu"
		   ", \"p99\": %llu, \"p999\": %llu, \"max\": %llu}%s\n",
		name, (unsigned long long)(h->count), (unsigned long long)(h->count ? h->min : 0),
		h->count ? (double)h->sum / (double)h->count : 0.0,
		(unsigned long long)(hist_pct(h, 50)), (unsigned long long)(hist_pct(h, 95)), (unsigned long long)(hist_pct(h, 99)), (unsigned long long)(hist_pct(h, 99.9)),
		(unsigned long long)(h->max), comma ? "," : "");
}

int main(int argc, char **argv)
{
	static struct app app;
	struct app *a = &app;
	struct cpu_snap s0, s1;
	struct hist e2e;
	struct utsname un;
	sigset_t mask;
	uint64_t processed = 0, value_errors = 0, interval_len, gaps_expected;
	double wall, stream_s, cpu_user, cpu_sys, sys_util = 0;
	int sfd, tfd = -1, signals;
	unsigned int i;
	bool ok_kernel, ok_stream, ok_app, ok_data, ok;
	FILE *jf = stdout;
	void *p;
	size_t nb;

	parse_opts(&a->o, argc, argv);

	/* Block the stop signals in every thread; only signalfd sees them. */
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	if (pthread_sigmask(SIG_BLOCK, &mask, NULL))
		die("pthread_sigmask");
	sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sfd < 0)
		die("signalfd");

	configure_device(a);
	a->open_ns = now_ns();
	a->devfd = open(a->o.device, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (a->devfd < 0)
		die("open device");
	if (ioctl(a->devfd, VSENSOR_IOC_GET_CONFIG, &a->cfg))
		die("VSENSOR_IOC_GET_CONFIG");
	if (ioctl(a->devfd, VSENSOR_IOC_GET_STATS, &a->st_start))
		die("VSENSOR_IOC_GET_STATS");

	a->stop_efd = eventfd(0, EFD_CLOEXEC);
	a->done_efd = eventfd(0, EFD_CLOEXEC);
	if (a->stop_efd < 0 || a->done_efd < 0)
		die("eventfd");
	if (a->o.duration_s > 0) {
		struct itimerspec its = { 0 };

		its.it_value.tv_sec = (time_t)a->o.duration_s;
		its.it_value.tv_nsec = (long)((a->o.duration_s - (double)its.it_value.tv_sec) * 1e9);
		tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
		if (tfd < 0 || timerfd_settime(tfd, 0, &its, NULL))
			die("timerfd");
	}
	if (a->o.out_csv) {
		a->csv = fopen(a->o.out_csv, "w");
		if (!a->csv)
			die("open csv");
		fprintf(a->csv, "seq,timestamp_ns,value,seed,config_gen\n");
		pthread_mutex_init(&a->csv_mu, NULL);
	}

	nb = a->o.queue + a->o.workers + 2;
	a->nbatches = nb;
	a->batches = calloc(nb, sizeof(*a->batches));
	if (!a->batches || bq_init(&a->q, a->o.queue) || bq_init(&a->pool, nb))
		die("alloc queues");
	for (i = 0; i < nb; i++) {
		a->batches[i].recs = calloc(a->o.batch, RS);
		if (!a->batches[i].recs)
			die("alloc batch");
		pool_put(a, &a->batches[i]);
	}
	hist_init(&a->read_lat);
	hist_init(&a->jitter);
	a->w = calloc(a->o.workers, sizeof(*a->w));
	if (!a->w)
		die("alloc workers");

	snap(&s0);
	for (i = 0; i < a->o.workers; i++) {
		a->w[i].app = a;
		hist_init(&a->w[i].e2e);
		if (pthread_create(&a->w[i].th, NULL, worker_main, &a->w[i]))
			die("pthread_create worker");
	}
	{
		pthread_t rt;

		if (pthread_create(&rt, NULL, reader_main, a))
			die("pthread_create reader");
		signals = main_loop(a, sfd, tfd);
		pthread_join(rt, NULL);
	}
	for (i = 0; i < a->o.workers; i++)
		pthread_join(a->w[i].th, NULL);
	/* After an abort, batches may remain queued: count them as discarded. */
	while (bq_trypop(&a->q, &p) == BQ_OK)
		a->shutdown_discarded += ((struct batch *)p)->n;
	a->stop_done_ns = now_ns();
	snap(&s1);

	hist_init(&e2e);
	for (i = 0; i < a->o.workers; i++) {
		processed += a->w[i].processed;
		value_errors += a->w[i].value_errors;
		a->shutdown_discarded += a->w[i].discarded;
		hist_merge(&e2e, &a->w[i].e2e);
	}

	wall = ts_s(s1.wall) - ts_s(s0.wall);
	/* Rates use the stream window (open -> freeze), excluding shutdown drain. */
	stream_s = (double)(a->freeze_ns - a->open_ns) / 1e9;
	cpu_user = tv_s(s1.ru.ru_utime) - tv_s(s0.ru.ru_utime);
	cpu_sys = tv_s(s1.ru.ru_stime) - tv_s(s0.ru.ru_stime);
	if (s1.sys_total > s0.sys_total)
		sys_util = 100.0 * (1.0 - (double)(s1.sys_idle - s0.sys_idle) /
				    (double)(s1.sys_total - s0.sys_total));

	interval_len = a->st_end.end_seq - a->st_end.start_seq;
	gaps_expected = a->st_end.dropped + a->st_end.flushed;
	ok_kernel = interval_len == a->st_end.delivered + a->st_end.dropped + a->st_end.flushed &&
		    a->st_end.queued == 0 && (a->st_end.flags & VSENSOR_STATS_FROZEN);
	ok_stream = a->received == a->st_end.delivered && a->gaps == gaps_expected;
	ok_app = a->received == processed + a->app_dropped + a->shutdown_discarded;
	ok_data = value_errors == 0 && a->order_errors == 0 && a->ts_errors == 0 &&
		  a->gen_errors == 0;
	ok = ok_kernel && ok_stream && ok_app && ok_data;

	if (a->o.json_path) {
		jf = fopen(a->o.json_path, "w");
		if (!jf)
			die("open json");
	}
	uname(&un);
	fprintf(jf, "{\n");
	fprintf(jf, "  \"label\": \"%s\",\n  \"kernel\": \"%s\",\n  \"machine\": \"%s\",\n"
		    "  \"ncpu\": %ld,\n", a->o.label, un.release, un.machine,
		sysconf(_SC_NPROCESSORS_ONLN));
	fprintf(jf, "  \"config\": {\"interval_us\": %u, \"seed\": %u, \"config_gen\": %u, "
		    "\"capacity\": %u, \"batch\": %u, \"queue\": %u, \"workers\": %u, "
		    "\"work_ns\": %u, \"read_period_us\": %u, \"policy\": \"%s\", \"on_stop\": \"%s\", "
		    "\"duration_s\": %.3f, \"mode\": \"%s\"},\n",
		a->cfg.interval_us, a->cfg.seed, a->cfg.config_gen, a->st_start.capacity,
		a->o.batch, a->o.queue, a->o.workers, a->o.work_ns, a->o.read_period_us,
		a->o.policy == POLICY_DROP ? "drop" : "block",
		a->o.on_stop == ON_STOP_DRAIN ? "drain" : "discard", a->o.duration_s,
		a->o.verify ? "verify" : "bench");
	fprintf(jf, "  \"wall_s\": %.6f,\n", wall);
	fprintf(jf, "  \"kernel_stats\": {\"start_seq\": %llu, \"end_seq\": %llu"
		    ", \"accepted\": %llu, \"delivered\": %llu, \"dropped\": %llu"
		    ", \"flushed\": %llu, \"queued\": %u, \"timer_overruns_total\": %llu"
		    ", \"timer_overruns_run\": %llu},\n",
		(unsigned long long)((uint64_t)a->st_end.start_seq), (unsigned long long)((uint64_t)a->st_end.end_seq), (unsigned long long)((uint64_t)a->st_end.accepted), (unsigned long long)((uint64_t)a->st_end.delivered),
		(unsigned long long)((uint64_t)a->st_end.dropped), (unsigned long long)((uint64_t)a->st_end.flushed), a->st_end.queued,
		(unsigned long long)((uint64_t)a->st_end.timer_overruns), (unsigned long long)((uint64_t)(a->st_end.timer_overruns - a->st_start.timer_overruns)));
	fprintf(jf, "  \"stream_s\": %.6f,\n  \"requested_rate_hz\": %.1f,\n"
		    "  \"achieved_rate_hz\": %.1f,\n",
		stream_s, 1e6 / a->cfg.interval_us,
		stream_s > 0 ? (double)interval_len / stream_s : 0.0);
	fprintf(jf, "  \"received\": %llu,\n  \"received_rate_hz\": %.1f,\n  \"reads\": %llu"
		    ",\n  \"processed\": %llu,\n  \"app_dropped\": %llu"
		    ",\n  \"shutdown_discarded\": %llu,\n  \"driver_dropped\": %llu"
		    ",\n  \"gaps\": %llu,\n  \"gap_events\": %llu"
		    ",\n  \"trailing_gap\": %llu,\n  \"queue_high_water\": %zu,\n",
		(unsigned long long)(a->received), wall > 0 ? (double)a->received / wall : 0.0, (unsigned long long)(a->reads), (unsigned long long)(processed),
		(unsigned long long)(a->app_dropped), (unsigned long long)(a->shutdown_discarded), (unsigned long long)((uint64_t)a->st_end.dropped), (unsigned long long)(a->gaps),
		(unsigned long long)(a->gap_events), (unsigned long long)(a->trailing_gap), a->q.max_count);
	fprintf(jf, "  \"errors\": {\"value\": %llu, \"order\": %llu"
		    ", \"timestamp\": %llu, \"config_gen\": %llu},\n",
		(unsigned long long)(value_errors), (unsigned long long)(a->order_errors), (unsigned long long)(a->ts_errors), (unsigned long long)(a->gen_errors));
	fprintf(jf, "  \"config_gens_seen\": %llu,\n", (unsigned long long)(a->gens_seen));
	fprintf(jf, "  \"cpu\": {\"user_s\": %.3f, \"sys_s\": %.3f, \"process_pct_of_one_cpu\": %.1f, "
		    "\"procstat_util_pct\": %.1f, \"producer_s\": %.4f},\n",
		cpu_user, cpu_sys, wall > 0 ? 100.0 * (cpu_user + cpu_sys) / wall : 0.0, sys_util,
		(double)(a->st_end.producer_ns - a->st_start.producer_ns) / 1e9);
	print_hist(jf, "read_latency_ns", &a->read_lat, true);
	print_hist(jf, "worker_latency_ns", &e2e, true);
	print_hist(jf, "jitter_ns", &a->jitter, true);
	fprintf(jf, "  \"signals\": %d,\n  \"stop_latency_ms\": %.3f,\n",
		signals, a->stop_req_ns ? (double)(a->stop_done_ns - a->stop_req_ns) / 1e6 : 0.0);
	fprintf(jf, "  \"checks\": {\"kernel_reconcile\": %s, \"stream_reconcile\": %s, "
		    "\"app_reconcile\": %s, \"data\": %s},\n",
		ok_kernel ? "true" : "false", ok_stream ? "true" : "false",
		ok_app ? "true" : "false", ok_data ? "true" : "false");
	fprintf(jf, "  \"ok\": %s\n}\n", ok ? "true" : "false");
	if (jf != stdout)
		fclose(jf);

	if (a->csv)
		fclose(a->csv);
	close(a->devfd);
	for (i = 0; i < nb; i++)
		free(a->batches[i].recs);
	free(a->batches);
	free(a->w);
	bq_destroy(&a->q);
	bq_destroy(&a->pool);

	if (!ok)
		fprintf(stderr, "vslogger: CHECK FAILED (kernel=%d stream=%d app=%d data=%d)\n",
			ok_kernel, ok_stream, ok_app, ok_data);
	return (a->o.verify && !ok) ? 1 : 0;
}
