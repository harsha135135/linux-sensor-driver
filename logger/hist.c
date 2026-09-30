// SPDX-License-Identifier: GPL-2.0
#include "hist.h"

#include <math.h>
#include <string.h>

void hist_init(struct hist *h)
{
	memset(h, 0, sizeof(*h));
	h->min = UINT64_MAX;
}

static unsigned int bucket_of(uint64_t v)
{
	unsigned int e;

	if (v < HIST_SUB)
		return (unsigned int)v;
	e = 63U - (unsigned int)__builtin_clzll(v);	/* e >= HIST_SUB_BITS */
	return (e - HIST_SUB_BITS + 1) * HIST_SUB +
	       (unsigned int)((v >> (e - HIST_SUB_BITS)) & (HIST_SUB - 1));
}

static void bucket_range(unsigned int idx, uint64_t *lo, uint64_t *width)
{
	unsigned int e, m;

	if (idx < HIST_SUB) {
		*lo = idx;
		*width = 1;
		return;
	}
	e = idx / HIST_SUB + HIST_SUB_BITS - 1;
	m = idx % HIST_SUB;
	*lo = ((uint64_t)HIST_SUB + m) << (e - HIST_SUB_BITS);
	*width = 1ULL << (e - HIST_SUB_BITS);
}

void hist_add(struct hist *h, uint64_t v)
{
	h->b[bucket_of(v)]++;
	h->count++;
	h->sum += v;
	if (v < h->min)
		h->min = v;
	if (v > h->max)
		h->max = v;
}

void hist_merge(struct hist *dst, const struct hist *src)
{
	unsigned int i;

	for (i = 0; i < HIST_BUCKETS; i++)
		dst->b[i] += src->b[i];
	dst->count += src->count;
	dst->sum += src->sum;
	if (src->min < dst->min)
		dst->min = src->min;
	if (src->max > dst->max)
		dst->max = src->max;
}

uint64_t hist_pct(const struct hist *h, double p)
{
	uint64_t rank, cum = 0, lo, width, mid;
	unsigned int i;

	if (h->count == 0)
		return 0;
	rank = (uint64_t)ceil(p / 100.0 * (double)h->count);
	if (rank < 1)
		rank = 1;
	for (i = 0; i < HIST_BUCKETS; i++) {
		cum += h->b[i];
		if (cum >= rank)
			break;
	}
	bucket_range(i, &lo, &width);
	mid = lo + width / 2;
	if (mid < h->min)
		mid = h->min;
	if (mid > h->max)
		mid = h->max;
	return mid;
}
