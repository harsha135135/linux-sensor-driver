// SPDX-License-Identifier: GPL-2.0
/*
 * vsensor - virtual sensor character device.
 *
 * An hrtimer produces deterministic synthetic samples (see vsensor_model.h)
 * at a configurable interval. Every fd opened for reading is an independent
 * reader with its own bounded ring: each produced sample is offered to every
 * active reader (broadcast), and a reader whose ring is full drops the new
 * sample (tail drop) and counts it. See DESIGN.md for the rationale.
 *
 * Locking (see DESIGN.md "Synchronization"):
 *
 *  vs_dev.lock (spinlock, IRQ-safe; taken from the hrtimer callback)
 *	protects: readers list, every reader's ring contents, head, tail,
 *	frozen, start_seq/end_seq and counters; the device's next_seq,
 *	interval_us, seed, config_gen, timer_overruns, dev_dropped.
 *	Never held while sleeping or while touching user memory.
 *
 *  vs_dev.cfg_mutex (sleeping)
 *	serialises open/release of readers (nreaders), SET_CONFIG, and timer
 *	start/cancel. hrtimer_cancel() waits for a running callback, which
 *	takes vs_dev.lock, so it is only ever called with the spinlock *not*
 *	held.
 *
 *  vs_reader.read_mutex (sleeping)
 *	serialises read() and FLUSH on one fd; protects the bounce buffer and
 *	makes the reader the only party that advances tail. Never held while
 *	waiting for data.
 *
 * Lock order: cfg_mutex -> lock; read_mutex -> lock. cfg_mutex and
 * read_mutex are never nested.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/build_bug.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "../include/uapi/vsensor.h"
#include "../include/vsensor_model.h"

static_assert(sizeof(struct vsensor_record) == 32);
static_assert(sizeof(struct vsensor_config) == 16);
static_assert(sizeof(struct vsensor_stats) == 104);

#define VS_REC_SIZE		sizeof(struct vsensor_record)
#define VS_MAX_READ_BATCH	256U

/* Module parameters: read-only after load, validated in vs_init(). */
static unsigned int capacity = 1024;
module_param(capacity, uint, 0444);
MODULE_PARM_DESC(capacity, "Ring size per reader in records (power of two, 16..65536)");

static unsigned int max_readers = 8;
module_param(max_readers, uint, 0444);
MODULE_PARM_DESC(max_readers, "Maximum concurrently open reader fds (1..64)");

static unsigned int interval_us = 1000;
module_param(interval_us, uint, 0444);
MODULE_PARM_DESC(interval_us, "Initial sampling interval in microseconds (10..1000000)");

static unsigned int seed = 1;
module_param(seed, uint, 0444);
MODULE_PARM_DESC(seed, "Initial model seed");

/*
 * Test hook: make vs_init() fail after completing step N (1..4) so the
 * error-unwinding paths can be exercised on a real kernel. 0 = disabled.
 */
static unsigned int inject_init_fault;
module_param(inject_init_fault, uint, 0444);
MODULE_PARM_DESC(inject_init_fault, "Test only: fail init after step N (1..4)");

struct vs_dev {
	spinlock_t lock;		/* producer <-> readers; see top of file */
	struct list_head readers;	/* lock */
	u64 next_seq;			/* lock */
	u32 interval_us;		/* lock (written under cfg_mutex too) */
	u32 seed;			/* lock (written under cfg_mutex too) */
	u32 config_gen;			/* lock (written under cfg_mutex too) */
	u64 timer_overruns;		/* lock */
	u64 dev_dropped;		/* lock */
	u64 producer_ns;		/* lock */
	u32 nreaders;			/* lock for writes and snapshot reads */

	struct mutex cfg_mutex;		/* open/release, SET_CONFIG, timer start/stop */
	bool timer_active;		/* cfg_mutex */
	struct hrtimer timer;

	dev_t devt;
	struct cdev cdev;
	struct class *class;
	struct device *device;
};

struct vs_reader {
	struct vs_dev *dev;
	struct list_head node;		/* dev->lock */
	wait_queue_head_t wq;
	struct mutex read_mutex;	/* read()/FLUSH on this fd; bounce buffer */
	struct vsensor_record *bounce;	/* read_mutex */
	struct vsensor_record *ring;	/* slots: dev->lock */
	u32 head;			/* dev->lock; free-running producer index */
	u32 tail;			/* dev->lock; advanced only under read_mutex */
	bool frozen;			/* dev->lock */
	u64 start_seq;			/* dev->lock */
	u64 end_seq;			/* dev->lock; valid when frozen */
	u64 accepted;			/* dev->lock */
	u64 dropped;			/* dev->lock */
	u64 flushed;			/* dev->lock */
	u64 delivered;			/* dev->lock */
};

/*
 * Only a reader (FMODE_READ) has a vs_reader. A write-only open is a control
 * handle: it can query and (with write access) configure, but has no stream.
 */
struct vs_file {
	struct vs_dev *dev;
	struct vs_reader *reader;	/* NULL for control-only handles */
};

static struct vs_dev vsdev;

static inline u32 vs_mask(void)
{
	return capacity - 1;
}

static inline ktime_t vs_period(u32 us)
{
	return ns_to_ktime((u64)us * NSEC_PER_USEC);
}

/* ---- producer (hrtimer callback, hardirq context on !PREEMPT_RT) ---- */

static enum hrtimer_restart vs_timer_fn(struct hrtimer *t)
{
	struct vs_dev *d = container_of(t, struct vs_dev, timer);
	struct vsensor_record rec;
	struct vs_reader *r;
	unsigned long flags;
	u64 overrun, t0;

	spin_lock_irqsave(&d->lock, flags);
	t0 = ktime_get_ns();

	overrun = hrtimer_forward_now(t, vs_period(d->interval_us));
	if (overrun > 1)
		d->timer_overruns += overrun - 1;

	rec.seq = d->next_seq++;
	rec.timestamp_ns = t0;
	rec.seed = d->seed;
	rec.value = vsensor_model_value(d->seed, rec.seq);
	rec.config_gen = d->config_gen;
	rec.flags = 0;

	list_for_each_entry(r, &d->readers, node) {
		if (r->frozen)
			continue;
		if (r->head - r->tail >= capacity) {
			r->dropped++;
			d->dev_dropped++;
			continue;
		}
		r->ring[r->head & vs_mask()] = rec;
		r->head++;
		r->accepted++;
		wake_up_interruptible_poll(&r->wq, EPOLLIN | EPOLLRDNORM);
	}

	d->producer_ns += ktime_get_ns() - t0;
	spin_unlock_irqrestore(&d->lock, flags);
	return HRTIMER_RESTART;
}

/* Caller holds cfg_mutex and not d->lock. */
static void vs_timer_start(struct vs_dev *d)
{
	unsigned long flags;
	u32 us;

	lockdep_assert_held(&d->cfg_mutex);
	spin_lock_irqsave(&d->lock, flags);
	us = d->interval_us;
	spin_unlock_irqrestore(&d->lock, flags);
	hrtimer_start(&d->timer, vs_period(us), HRTIMER_MODE_REL);
	d->timer_active = true;
}

/* Caller holds cfg_mutex and not d->lock (hrtimer_cancel waits for the callback). */
static void vs_timer_stop(struct vs_dev *d)
{
	lockdep_assert_held(&d->cfg_mutex);
	hrtimer_cancel(&d->timer);
	d->timer_active = false;
}

/* ---- open / release ---- */

static int vs_open(struct inode *inode, struct file *f)
{
	struct vs_dev *d = container_of(inode->i_cdev, struct vs_dev, cdev);
	struct vs_reader *r = NULL;
	struct vs_file *vf;
	unsigned long flags;
	int ret;

	vf = kzalloc(sizeof(*vf), GFP_KERNEL);
	if (!vf)
		return -ENOMEM;
	vf->dev = d;

	if (f->f_mode & FMODE_READ) {
		r = kzalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			ret = -ENOMEM;
			goto err_vf;
		}
		r->ring = kvcalloc(capacity, VS_REC_SIZE, GFP_KERNEL);
		r->bounce = kvcalloc(min(capacity, VS_MAX_READ_BATCH), VS_REC_SIZE,
				     GFP_KERNEL);
		if (!r->ring || !r->bounce) {
			ret = -ENOMEM;
			goto err_reader;
		}
		r->dev = d;
		init_waitqueue_head(&r->wq);
		mutex_init(&r->read_mutex);
		INIT_LIST_HEAD(&r->node);

		mutex_lock(&d->cfg_mutex);
		if (d->nreaders >= max_readers) {
			mutex_unlock(&d->cfg_mutex);
			ret = -EMFILE;
			goto err_reader;
		}
		spin_lock_irqsave(&d->lock, flags);
		r->start_seq = d->next_seq;
		list_add_tail(&r->node, &d->readers);
		d->nreaders++;
		spin_unlock_irqrestore(&d->lock, flags);
		if (!d->timer_active)
			vs_timer_start(d);
		mutex_unlock(&d->cfg_mutex);
		vf->reader = r;
	}

	f->private_data = vf;
	return stream_open(inode, f);

err_reader:
	kvfree(r->bounce);
	kvfree(r->ring);
	kfree(r);
err_vf:
	kfree(vf);
	return ret;
}

static int vs_release(struct inode *inode, struct file *f)
{
	struct vs_file *vf = f->private_data;
	struct vs_reader *r = vf->reader;
	struct vs_dev *d = vf->dev;
	unsigned long flags;

	if (r) {
		mutex_lock(&d->cfg_mutex);
		spin_lock_irqsave(&d->lock, flags);
		list_del(&r->node);
		d->nreaders--;
		spin_unlock_irqrestore(&d->lock, flags);
		/*
		 * The callback walks the list under d->lock, so once list_del
		 * has been published under that lock it can no longer reach r.
		 */
		if (d->nreaders == 0 && d->timer_active)
			vs_timer_stop(d);
		mutex_unlock(&d->cfg_mutex);

		mutex_destroy(&r->read_mutex);
		kvfree(r->bounce);
		kvfree(r->ring);
		kfree(r);
	}
	kfree(vf);
	return 0;
}

/* ---- read / poll ---- */

static bool vs_readable(struct vs_reader *r)
{
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&r->dev->lock, flags);
	ret = r->head != r->tail || r->frozen;
	spin_unlock_irqrestore(&r->dev->lock, flags);
	return ret;
}

/*
 * read() returns whole records only.
 *
 * Records are *peeked* into the bounce buffer under the spinlock, copied to
 * userspace with no lock but read_mutex held, and only the records that were
 * copied completely are then consumed (tail advanced). If copy_to_user()
 * faults part way, the uncopied records stay at the head of the ring and are
 * returned by the next read(); if not even one record could be copied the
 * call fails with -EFAULT and the ring is unchanged. Holding read_mutex for
 * the whole peek/copy/commit sequence means no other read() or FLUSH on this
 * fd can consume the peeked slots, and the producer never overwrites
 * unconsumed slots (tail drop), so the peeked data stays valid.
 */
static ssize_t vs_read(struct file *f, char __user *buf, size_t count, loff_t *ppos)
{
	struct vs_file *vf = f->private_data;
	struct vs_reader *r = vf->reader;
	struct vs_dev *d = vf->dev;
	size_t want, n, i, bytes, left, done;
	unsigned long flags;
	bool eof;
	int ret;

	if (!r)
		return -EBADF;
	if (count < VS_REC_SIZE)
		return -EINVAL;
	want = min_t(size_t, count / VS_REC_SIZE, min(capacity, VS_MAX_READ_BATCH));

	for (;;) {
		if (mutex_lock_interruptible(&r->read_mutex))
			return -ERESTARTSYS;

		spin_lock_irqsave(&d->lock, flags);
		n = min_t(size_t, want, r->head - r->tail);
		for (i = 0; i < n; i++)
			r->bounce[i] = r->ring[(r->tail + i) & vs_mask()];
		eof = n == 0 && r->frozen;
		spin_unlock_irqrestore(&d->lock, flags);

		if (n || eof)
			break;
		mutex_unlock(&r->read_mutex);

		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(r->wq, vs_readable(r));
		if (ret)
			return ret;	/* -ERESTARTSYS: EINTR or restart */
	}

	if (eof) {
		mutex_unlock(&r->read_mutex);
		return 0;
	}

	bytes = n * VS_REC_SIZE;
	left = copy_to_user(buf, r->bounce, bytes);
	done = (bytes - left) / VS_REC_SIZE;
	if (done) {
		spin_lock_irqsave(&d->lock, flags);
		r->tail += done;
		r->delivered += done;
		spin_unlock_irqrestore(&d->lock, flags);
	}
	mutex_unlock(&r->read_mutex);

	return done ? (ssize_t)(done * VS_REC_SIZE) : -EFAULT;
}

static __poll_t vs_poll(struct file *f, struct poll_table_struct *pt)
{
	struct vs_file *vf = f->private_data;
	struct vs_reader *r = vf->reader;
	unsigned long flags;
	__poll_t mask = 0;

	if (!r)
		return EPOLLERR;

	poll_wait(f, &r->wq, pt);

	spin_lock_irqsave(&vf->dev->lock, flags);
	if (r->head != r->tail)
		mask |= EPOLLIN | EPOLLRDNORM;
	else if (r->frozen)
		mask |= EPOLLHUP;	/* drained end of stream: read() returns 0 */
	spin_unlock_irqrestore(&vf->dev->lock, flags);
	return mask;
}

/* ---- ioctl ---- */

static long vs_get_config(struct vs_dev *d, void __user *arg)
{
	struct vsensor_config cfg = { 0 };
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	cfg.interval_us = d->interval_us;
	cfg.seed = d->seed;
	cfg.config_gen = d->config_gen;
	spin_unlock_irqrestore(&d->lock, flags);

	return copy_to_user(arg, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}

static long vs_set_config(struct file *f, struct vs_dev *d, void __user *arg)
{
	struct vsensor_config cfg;
	unsigned long flags;

	if (!(f->f_mode & FMODE_WRITE))
		return -EPERM;
	if (copy_from_user(&cfg, arg, sizeof(cfg)))
		return -EFAULT;
	if (cfg.flags != 0 ||
	    cfg.interval_us < VSENSOR_INTERVAL_MIN_US ||
	    cfg.interval_us > VSENSOR_INTERVAL_MAX_US)
		return -EINVAL;

	mutex_lock(&d->cfg_mutex);
	spin_lock_irqsave(&d->lock, flags);
	d->interval_us = cfg.interval_us;
	d->seed = cfg.seed;
	d->config_gen++;
	cfg.config_gen = d->config_gen;
	spin_unlock_irqrestore(&d->lock, flags);
	/*
	 * Restart the timer so the new interval applies immediately rather
	 * than after one (possibly 1 s long) old period.
	 */
	if (d->timer_active) {
		vs_timer_stop(d);
		vs_timer_start(d);
	}
	mutex_unlock(&d->cfg_mutex);

	return copy_to_user(arg, &cfg, sizeof(cfg)) ? -EFAULT : 0;
}

static long vs_get_stats(struct vs_file *vf, void __user *arg)
{
	struct vs_reader *r = vf->reader;
	struct vs_dev *d = vf->dev;
	struct vsensor_stats st = { 0 };
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	st.next_seq = d->next_seq;
	st.timer_overruns = d->timer_overruns;
	st.dev_dropped = d->dev_dropped;
	st.producer_ns = d->producer_ns;
	st.readers = d->nreaders;
	st.capacity = capacity;
	if (r) {
		st.flags |= VSENSOR_STATS_READER;
		if (r->frozen)
			st.flags |= VSENSOR_STATS_FROZEN;
		st.start_seq = r->start_seq;
		st.end_seq = r->frozen ? r->end_seq : d->next_seq;
		st.accepted = r->accepted;
		st.dropped = r->dropped;
		st.flushed = r->flushed;
		st.delivered = r->delivered;
		st.queued = r->head - r->tail;
	}
	spin_unlock_irqrestore(&d->lock, flags);

	return copy_to_user(arg, &st, sizeof(st)) ? -EFAULT : 0;
}

static long vs_flush(struct vs_reader *r)
{
	struct vs_dev *d = r->dev;
	unsigned long flags;

	/* read_mutex: never discard records a concurrent read() has peeked. */
	if (mutex_lock_interruptible(&r->read_mutex))
		return -ERESTARTSYS;
	spin_lock_irqsave(&d->lock, flags);
	r->flushed += r->head - r->tail;
	r->tail = r->head;
	spin_unlock_irqrestore(&d->lock, flags);
	mutex_unlock(&r->read_mutex);
	return 0;
}

static long vs_stop(struct vs_reader *r)
{
	struct vs_dev *d = r->dev;
	unsigned long flags;

	spin_lock_irqsave(&d->lock, flags);
	if (!r->frozen) {
		r->frozen = true;
		r->end_seq = d->next_seq;
	}
	spin_unlock_irqrestore(&d->lock, flags);
	/* Blocked readers re-check: they drain, then see end of stream. */
	wake_up_interruptible_poll(&r->wq, EPOLLHUP);
	return 0;
}

static long vs_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct vs_file *vf = f->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case VSENSOR_IOC_GET_CONFIG:
		return vs_get_config(vf->dev, uarg);
	case VSENSOR_IOC_SET_CONFIG:
		return vs_set_config(f, vf->dev, uarg);
	case VSENSOR_IOC_GET_STATS:
		return vs_get_stats(vf, uarg);
	case VSENSOR_IOC_FLUSH:
		return vf->reader ? vs_flush(vf->reader) : -EINVAL;
	case VSENSOR_IOC_STOP:
		return vf->reader ? vs_stop(vf->reader) : -EINVAL;
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vs_fops = {
	.owner		= THIS_MODULE,
	.open		= vs_open,
	.release	= vs_release,
	.read		= vs_read,
	.poll		= vs_poll,
	.unlocked_ioctl	= vs_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

/* ---- module init / exit ---- */

static char *vs_devnode(const struct device *dev, umode_t *mode)
{
	/* Anyone may read; only root may open for write (i.e. SET_CONFIG). */
	if (mode)
		*mode = 0644;
	return NULL;
}

static bool vs_params_valid(void)
{
	if (capacity < 16 || capacity > 65536 || !is_power_of_2(capacity)) {
		pr_err("capacity=%u must be a power of two in [16, 65536]\n", capacity);
		return false;
	}
	if (max_readers < 1 || max_readers > 64) {
		pr_err("max_readers=%u must be in [1, 64]\n", max_readers);
		return false;
	}
	if (interval_us < VSENSOR_INTERVAL_MIN_US || interval_us > VSENSOR_INTERVAL_MAX_US) {
		pr_err("interval_us=%u must be in [%u, %u]\n", interval_us,
		       VSENSOR_INTERVAL_MIN_US, VSENSOR_INTERVAL_MAX_US);
		return false;
	}
	if (inject_init_fault > 4) {
		pr_err("inject_init_fault=%u must be in [0, 4]\n", inject_init_fault);
		return false;
	}
	return true;
}

static int __init vs_init(void)
{
	struct vs_dev *d = &vsdev;
	int ret;

	if (!vs_params_valid())
		return -EINVAL;

	/* Fully initialise state before cdev_add() makes the device live. */
	spin_lock_init(&d->lock);
	mutex_init(&d->cfg_mutex);
	INIT_LIST_HEAD(&d->readers);
	d->interval_us = interval_us;
	d->seed = seed;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&d->timer, vs_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&d->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	d->timer.function = vs_timer_fn;
#endif

	ret = alloc_chrdev_region(&d->devt, 0, 1, "vsensor");
	if (ret)
		return ret;
	if (inject_init_fault == 1) {
		ret = -EIO;
		goto err_region;
	}

	cdev_init(&d->cdev, &vs_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret)
		goto err_region;
	if (inject_init_fault == 2) {
		ret = -EIO;
		goto err_cdev;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	d->class = class_create("vsensor");
#else
	d->class = class_create(THIS_MODULE, "vsensor");
#endif
	if (IS_ERR(d->class)) {
		ret = PTR_ERR(d->class);
		goto err_cdev;
	}
	d->class->devnode = vs_devnode;
	if (inject_init_fault == 3) {
		ret = -EIO;
		goto err_class;
	}

	d->device = device_create(d->class, NULL, d->devt, NULL, "vsensor");
	if (IS_ERR(d->device)) {
		ret = PTR_ERR(d->device);
		goto err_class;
	}
	if (inject_init_fault == 4) {
		ret = -EIO;
		goto err_device;
	}

	pr_info("loaded: major=%u capacity=%u max_readers=%u interval_us=%u seed=%u\n",
		MAJOR(d->devt), capacity, max_readers, interval_us, seed);
	return 0;

err_device:
	device_destroy(d->class, d->devt);
err_class:
	class_destroy(d->class);
err_cdev:
	cdev_del(&d->cdev);
err_region:
	unregister_chrdev_region(d->devt, 1);
	if (inject_init_fault)
		pr_info("init failed at injected step %u (ret=%d), cleaned up\n",
			inject_init_fault, ret);
	return ret;
}

static void __exit vs_exit(void)
{
	struct vs_dev *d = &vsdev;

	/*
	 * fops.owner pins the module while any fd is open, so no reader
	 * exists here and the timer was cancelled by the last release.
	 * Cancel anyway so a future bug cannot leave a live timer pointing
	 * into unloaded code.
	 */
	WARN_ON(!list_empty(&d->readers));
	hrtimer_cancel(&d->timer);

	device_destroy(d->class, d->devt);
	class_destroy(d->class);
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->devt, 1);
	mutex_destroy(&d->cfg_mutex);
	pr_info("unloaded: produced=%llu dropped=%llu overruns=%llu\n",
		d->next_seq, d->dev_dropped, d->timer_overruns);
}

module_init(vs_init);
module_exit(vs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harshavardhan M");
MODULE_DESCRIPTION("Virtual sensor character device with per-reader bounded queues");
