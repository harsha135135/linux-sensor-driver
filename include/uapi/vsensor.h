/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vsensor userspace ABI: record layout and ioctl interface for /dev/vsensor.
 *
 * All structures use fixed-width types, contain no implicit padding and have
 * identical layout on 32- and 64-bit ABIs, so compat_ptr_ioctl is sufficient.
 * The ioctl numbers encode sizeof(struct), so a caller built against a
 * different layout gets -ENOTTY instead of a silently misread structure.
 */
#ifndef _UAPI_VSENSOR_H
#define _UAPI_VSENSOR_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VSENSOR_DEVICE_PATH	"/dev/vsensor"

/*
 * One sample. read() returns an integral number of these (32 bytes each).
 *
 * seq          Device-wide sequence number, +1 per produced sample, starting
 *              at 0 when the module loads. Every reader sees the same seq for
 *              the same sample.
 * timestamp_ns CLOCK_MONOTONIC time (ktime_get_ns()) at which the sample was
 *              produced. Comparable with clock_gettime(CLOCK_MONOTONIC).
 * value        vsensor_model_value(seed, seq); see vsensor_model.h.
 * seed         Seed in effect when this sample was produced, so a consumer can
 *              verify samples produced before a configuration change.
 * config_gen   Incremented by every successful VSENSOR_IOC_SET_CONFIG.
 * flags        Reserved, currently always 0.
 */
struct vsensor_record {
	__u64 seq;
	__u64 timestamp_ns;
	__s32 value;
	__u32 seed;
	__u32 config_gen;
	__u32 flags;
};

#define VSENSOR_INTERVAL_MIN_US	10U
#define VSENSOR_INTERVAL_MAX_US	1000000U

/*
 * Device-wide configuration.
 *
 * GET_CONFIG: all fields are outputs.
 * SET_CONFIG: interval_us and seed are inputs; flags must be 0 and
 *             config_gen is ignored on input. On success the applied
 *             configuration (including the new config_gen) is written back.
 *             Requires an fd opened with write access (-EPERM otherwise).
 *             The sampling timer is restarted, so the new interval applies
 *             from the next sample.
 */
struct vsensor_config {
	__u32 interval_us;
	__u32 seed;
	__u32 flags;
	__u32 config_gen;
};

/* vsensor_stats.flags */
#define VSENSOR_STATS_READER	(1U << 0) /* fd is a reader (opened for read) */
#define VSENSOR_STATS_FROZEN	(1U << 1) /* VSENSOR_IOC_STOP was issued */

/*
 * Statistics snapshot, taken atomically under the device lock.
 *
 * Device-wide:
 *   next_seq        seq that the next produced sample will carry
 *                   (== number of samples produced since load).
 *   timer_overruns  sampling periods skipped because the timer ran late.
 *                   Skipped periods do not consume sequence numbers.
 *   dev_dropped     samples dropped for any reader since load, including
 *                   readers that have since closed.
 *   producer_ns     total time spent inside the sampling timer callback
 *                   (hardirq context, not charged to any process).
 *   readers         currently open reader fds; capacity: ring size/reader.
 *
 * Per reader (only if flags & VSENSOR_STATS_READER; zero otherwise). The
 * reader's stream is the half-open seq interval [start_seq, end_seq).
 *   start_seq       next_seq at open().
 *   end_seq         next_seq at VSENSOR_IOC_STOP if FROZEN, otherwise
 *                   next_seq at the time of this snapshot.
 *   accepted        samples placed in this reader's ring.
 *   dropped         samples discarded because this reader's ring was full.
 *   flushed         samples discarded by VSENSOR_IOC_FLUSH.
 *   delivered       samples copied to userspace by read().
 *   queued          samples currently in the ring.
 *
 * Invariants (at any snapshot):
 *   end_seq - start_seq == accepted + dropped
 *   accepted == delivered + flushed + queued
 */
struct vsensor_stats {
	__u32 flags;
	__u32 reserved0;
	__u64 next_seq;
	__u64 timer_overruns;
	__u64 dev_dropped;
	__u64 producer_ns;
	__u32 readers;
	__u32 capacity;
	__u64 start_seq;
	__u64 end_seq;
	__u64 accepted;
	__u64 dropped;
	__u64 flushed;
	__u64 delivered;
	__u32 queued;
	__u32 reserved1;
};

#define VSENSOR_IOC_MAGIC	'V'

#define VSENSOR_IOC_GET_CONFIG	_IOR(VSENSOR_IOC_MAGIC, 1, struct vsensor_config)
#define VSENSOR_IOC_SET_CONFIG	_IOWR(VSENSOR_IOC_MAGIC, 2, struct vsensor_config)
#define VSENSOR_IOC_GET_STATS	_IOR(VSENSOR_IOC_MAGIC, 3, struct vsensor_stats)
/* Discard every sample currently queued for this reader (counted in flushed). */
#define VSENSOR_IOC_FLUSH	_IO(VSENSOR_IOC_MAGIC, 4)
/*
 * Freeze this reader: fixes end_seq and stops enqueueing (and dropping) for
 * this fd. Already-queued samples remain readable; once they are drained,
 * read() returns 0 (end of stream) and poll() reports EPOLLHUP. Idempotent
 * and irreversible for the lifetime of the fd.
 */
#define VSENSOR_IOC_STOP	_IO(VSENSOR_IOC_MAGIC, 5)

#endif /* _UAPI_VSENSOR_H */
