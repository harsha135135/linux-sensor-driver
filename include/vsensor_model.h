/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Deterministic synthetic signal shared by the kernel module and userspace.
 *
 * value(seed, seq) = 15000 + triangle(seq) + noise(seed, seq)
 *   triangle: period 1024 samples, range [-10220, +10220]
 *   noise:    8-bit hash of (seed, seq), range [-128, +127]
 *
 * Integer-only (no FPU in kernel context) and free of 64-bit division, so it
 * builds unchanged on 32-bit targets. Userspace recomputes this function to
 * verify every received sample.
 */
#ifndef _VSENSOR_MODEL_H
#define _VSENSOR_MODEL_H

#include <linux/types.h>

/* splitmix64 finaliser: a cheap, well-distributed 64-bit mix. */
static inline __u64 vsensor_mix64(__u64 x)
{
	x += 0x9E3779B97F4A7C15ULL;
	x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
	x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
	return x ^ (x >> 31);
}

static inline __s32 vsensor_model_value(__u32 seed, __u64 seq)
{
	__u32 phase = (__u32)(seq & 1023U);
	__s32 tri = (__s32)(phase < 512U ? phase : 1023U - phase) * 40 - 10220;
	__s32 noise = (__s32)(vsensor_mix64(((__u64)seed << 32) ^ seq) & 0xffU) - 128;

	return 15000 + tri + noise;
}

#endif /* _VSENSOR_MODEL_H */
