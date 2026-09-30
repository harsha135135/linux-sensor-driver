// SPDX-License-Identifier: GPL-2.0
/*
 * vsctl - inspect and configure /dev/vsensor.
 *
 *   vsctl get                     print device configuration
 *   vsctl set INTERVAL_US SEED    apply configuration (needs write access)
 *   vsctl stats                   print device-wide statistics
 *   vsctl read N                  open a reader, print N samples, verify them
 *
 * get/set/stats use a write-only control handle, which is not a reader and
 * therefore has no stream, ring or drop accounting of its own.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "uapi/vsensor.h"
#include "vsensor_model.h"

static const char *dev = VSENSOR_DEVICE_PATH;

static int xopen(int flags)
{
	int fd = open(dev, flags | O_CLOEXEC);

	if (fd < 0) {
		fprintf(stderr, "vsctl: open %s: %s\n", dev, strerror(errno));
		exit(1);
	}
	return fd;
}

static void xioctl(int fd, unsigned long cmd, void *arg, const char *name)
{
	if (ioctl(fd, cmd, arg)) {
		fprintf(stderr, "vsctl: %s: %s\n", name, strerror(errno));
		exit(1);
	}
}

static unsigned long num(const char *s)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || *end) {
		fprintf(stderr, "vsctl: bad number '%s'\n", s);
		exit(2);
	}
	return v;
}

static void usage(void)
{
	fprintf(stderr, "usage: vsctl [-d DEV] get | set INTERVAL_US SEED | stats | read N\n");
	exit(2);
}

int main(int argc, char **argv)
{
	struct vsensor_config cfg;
	struct vsensor_stats st;
	int fd;

	if (argc > 2 && !strcmp(argv[1], "-d")) {
		dev = argv[2];
		argc -= 2;
		argv += 2;
	}
	if (argc < 2)
		usage();

	if (!strcmp(argv[1], "get") && argc == 2) {
		fd = xopen(O_WRONLY);
		xioctl(fd, VSENSOR_IOC_GET_CONFIG, &cfg, "GET_CONFIG");
		printf("interval_us=%u seed=%u config_gen=%u\n",
		       cfg.interval_us, cfg.seed, cfg.config_gen);
	} else if (!strcmp(argv[1], "set") && argc == 4) {
		fd = xopen(O_WRONLY);
		memset(&cfg, 0, sizeof(cfg));
		cfg.interval_us = (uint32_t)num(argv[2]);
		cfg.seed = (uint32_t)num(argv[3]);
		xioctl(fd, VSENSOR_IOC_SET_CONFIG, &cfg, "SET_CONFIG");
		printf("interval_us=%u seed=%u config_gen=%u\n",
		       cfg.interval_us, cfg.seed, cfg.config_gen);
	} else if (!strcmp(argv[1], "stats") && argc == 2) {
		fd = xopen(O_WRONLY);
		xioctl(fd, VSENSOR_IOC_GET_STATS, &st, "GET_STATS");
		printf("next_seq=%llu timer_overruns=%llu dev_dropped=%llu"
		       " readers=%u capacity=%u\n", (unsigned long long)(st.next_seq), (unsigned long long)(st.timer_overruns),
		       (unsigned long long)(st.dev_dropped), st.readers, st.capacity);
	} else if (!strcmp(argv[1], "read") && argc == 3) {
		unsigned long n = num(argv[2]), i, bad = 0;
		struct vsensor_record r;

		fd = xopen(O_RDONLY);
		for (i = 0; i < n; i++) {
			if (read(fd, &r, sizeof(r)) != sizeof(r)) {
				fprintf(stderr, "vsctl: read: %s\n", strerror(errno));
				return 1;
			}
			if (r.value != vsensor_model_value(r.seed, r.seq))
				bad++;
			printf("seq=%llu t=%llu value=%d seed=%u gen=%u\n",
			       (unsigned long long)(r.seq), (unsigned long long)(r.timestamp_ns), r.value, r.seed, r.config_gen);
		}
		xioctl(fd, VSENSOR_IOC_GET_STATS, &st, "GET_STATS");
		printf("reader: start_seq=%llu delivered=%llu dropped=%llu"
		       " model_mismatches=%lu\n", (unsigned long long)(st.start_seq), (unsigned long long)(st.delivered), (unsigned long long)(st.dropped), bad);
		if (bad)
			return 1;
	} else {
		usage();
	}
	close(fd);
	return 0;
}
