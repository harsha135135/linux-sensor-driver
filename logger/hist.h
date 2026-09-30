/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Log-linear histogram for nanosecond latencies: exact below 64 ns, then 64
 * sub-buckets per power of two, so any reported percentile is within 1/64
 * (~1.6%) of the true value. Fixed size (~30 KiB), no allocation, O(1) add,
 * mergeable, so each thread keeps its own and they are merged at exit.
 */
#ifndef VS_HIST_H
#define VS_HIST_H

#include <stdint.h>

#define HIST_SUB_BITS	6
#define HIST_SUB	(1U << HIST_SUB_BITS)
#define HIST_BUCKETS	((64 - HIST_SUB_BITS + 1) * HIST_SUB)

struct hist {
	uint64_t count;
	uint64_t sum;
	uint64_t min;
	uint64_t max;
	uint64_t b[HIST_BUCKETS];
};

void hist_init(struct hist *h);
void hist_add(struct hist *h, uint64_t v);
void hist_merge(struct hist *dst, const struct hist *src);
/* p in [0, 100]. Returns the midpoint of the bucket holding the p-th percentile. */
uint64_t hist_pct(const struct hist *h, double p);

#endif
