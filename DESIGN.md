# Design: vsensor driver and vslogger

The sensor is **virtual**: an hrtimer inside the module produces deterministic synthetic
samples. There is no hardware and no bus driver. The point is the character-device, buffering,
concurrency and lifetime machinery around a data source, with a userspace consumer that can
prove it received exactly what the kernel produced.

## 1. Data path

```
 hrtimer callback (hardirq)        per-reader ring (tail drop)          vslogger
 ─────────────────────────         ──────────────────────────          ──────────────────────────
 seq = next_seq++           ──►    reader A: [r r r r . . . .]  read() ─► reader thread ─► bounded
 ts  = ktime_get_ns()       ──►    reader B: [r r r r r r r r]  (full ─► dropped++)        queue ─► workers
 value = model(seed, seq)          ...                                    (gap/latency acct)       (verify, load, CSV)
```

## 2. Kernel module (`kernel/vsensor.c`)

### Device registration
`alloc_chrdev_region` + `cdev_add` + `class_create` + `device_create` create `/dev/vsensor`
(mode 0644 through `vs_devnode`). I chose this over `misc_register` because it shows the
full sequence and its unwinding. `vs_init` fully initialises `vsdev` (lock, lists, timer)
**before** `cdev_add`, because the device can be opened as soon as `cdev_add` returns. Error
paths unwind in reverse order through goto labels. The `inject_init_fault=N` parameter forces
a failure after step N, so every unwind path is exercised on a real kernel
(`tests/load_unload.sh`).

### Sample model
`include/vsensor_model.h` (`vsensor_model_value`) returns a triangle wave plus an 8-bit
splitmix64 hash of `(seed, seq)`. It uses integers only (no FPU in hardirq context), avoids
64-bit division (32-bit safe), and is shared verbatim with userspace so every sample can be
verified.

### Record and ioctl ABI (`include/uapi/vsensor.h`)
- `struct vsensor_record` is 32 bytes: `u64 seq, u64 timestamp_ns, s32 value, u32 seed,
  u32 config_gen, u32 flags`. It has no implicit padding, and the layout is identical on
  32/64-bit, so `compat_ptr_ioctl` suffices. Both sides check it with `static_assert`, which
  caught a real size miscalculation during development (see INTERVIEW_GUIDE).
- **The seed is carried in every record.** `config_gen` alone would not tell a worker which
  seed produced a record that was still queued when the seed changed. Carrying the seed makes
  each record self-verifying. This costs 4 bytes, which fit in space that would otherwise have
  been padding.
- ioctls encode `sizeof(struct)` in the command number, so a caller built against another
  layout gets `-ENOTTY` rather than a misread struct. `flags` must be 0 on `SET_CONFIG`
  (reserved for extension).
- `SET_CONFIG` requires an fd opened with write access (`-EPERM` otherwise). With mode 0644,
  only root can reconfigure; any user can read.
- An `O_WRONLY` open is a **control handle**. It has no ring, is not a reader and does not
  count toward `max_readers`, so tools like `vsctl` can configure and query the device without
  creating a stream that silently accumulates drops.

### Multiple readers: independent broadcast streams
Every fd opened for reading gets its own ring. Each sample is offered to every active reader,
and all readers see the same `seq` for the same sample.

| Option | Pros | Cons |
|---|---|---|
| **Independent streams (chosen)** | exact per-reader gap accounting; one slow reader cannot starve or corrupt another's view | O(readers) work per tick → capped by `max_readers` (default 8, `-EMFILE` beyond) |
| Competing consumers (one shared queue) | O(1) per tick | a gap in one reader's sequence may be a record another reader took, so drop accounting is only possible globally |

Threads that **share one fd** do compete for that fd's records. Each record goes to exactly
one of them, which `shared_fd_competing_threads` tests.

### Overflow: tail drop
When a reader's ring is full, the **new** sample is dropped and `dropped` is incremented,
both per reader and device-wide. This is the IIO/kfifo behaviour. The reader keeps a
contiguous prefix and the loss appears as a single hole. With drop-oldest, the producer would
have to move the consumer's tail, which races with a reader that has already peeked those
slots (see read below). Ring capacity is a load-time parameter (power of two, 16..65536),
so rings are never reallocated under readers.

### Sequence accounting: exact by construction
- Each reader's stream is the half-open interval `[start_seq, end_seq)`. `start_seq` is
  `next_seq` at open, taken under `dev->lock`, the same lock the producer holds when it
  increments `next_seq`.
- `VSENSOR_IOC_STOP` freezes a reader: under the lock, `end_seq = next_seq` and the producer
  skips it from then on. The reader drains what is queued; `read()` then returns 0 (EOF) and
  `poll` reports `EPOLLHUP`.
- Every sample in the interval is exactly one of `accepted` or `dropped`, and every accepted
  sample is exactly one of `delivered`, `flushed` or still `queued`. So after STOP and drain:
  `end - start == delivered + dropped + flushed`.
- Trailing drops (after the last received record) are only visible through `end_seq`. The
  logger counts them as a trailing gap, and `overflow_trailing_drops` tests this case
  explicitly.
- A `GET_STATS` taken without STOP is a consistent snapshot, but the counts keep moving. That
  is why reconciliation always freezes first.
- Timer overruns (late callbacks) produce **one** sample and are counted in `timer_overruns`.
  They do not consume sequence numbers, so gaps mean drops or flushes and nothing else.
- `FLUSH` discards what is queued and counts it in `flushed`, kept separate from `dropped`.

### read(): peek, copy, commit
Under `read_mutex`:
1. Under the spinlock, copy up to n records from the ring into a per-file bounce buffer
   **without** consuming them.
2. Drop the spinlock and `copy_to_user`.
3. Under the spinlock, advance `tail` by the number of **whole** records actually copied.

- If `copy_to_user` faults part way, the uncopied records stay at the head of the ring and the
  next `read()` returns them. If nothing could be copied, the call returns `-EFAULT` and the
  ring is unchanged.
- `partial_copy_fault` tests this with a buffer that runs into a `PROT_NONE` page.
- The peeked slots cannot change during the copy, because only the reader advances `tail`
  (under `read_mutex`) and tail drop means the producer never overwrites unconsumed slots.
- `FLUSH` also takes `read_mutex`, so it can never discard records that a concurrent `read()`
  has already staged. The bounce buffer holds copies only; the ring stays authoritative.
- `count < 32` → `-EINVAL`, and a non-multiple is rounded down to whole records.
- Blocking reads wait in `wait_event_interruptible` **without** holding `read_mutex`, then
  retry. `-ERESTARTSYS` becomes `EINTR`, or a transparent restart under `SA_RESTART`; both are
  tested.
- `O_NONBLOCK` returns `-EAGAIN`. `stream_open` makes the fd non-seekable (`lseek` →
  `ESPIPE`).

### Synchronization

| Lock | Type / context | Protects | Never |
|---|---|---|---|
| `vs_dev.lock` | spinlock, `irqsave` (taken in hardirq by the timer) | reader list; every ring's slots, `head`, `tail`, `frozen`, `start/end_seq`, counters; device `next_seq`, `interval_us`, `seed`, `config_gen`, `timer_overruns`, `dev_dropped`, `producer_ns`, `nreaders` | held across sleep, `copy_*_user`, `hrtimer_cancel` |
| `vs_dev.cfg_mutex` | mutex, process context | open/release transitions, `SET_CONFIG`, timer start/stop (`timer_active`) | nested with `read_mutex` |
| `vs_reader.read_mutex` | mutex, process context | the bounce buffer; exclusive right to advance `tail` (read, FLUSH) | held while waiting for data |

Lock order: `cfg_mutex → lock` and `read_mutex → lock`.
- `hrtimer_cancel` waits for a running callback, which takes `lock`. It is therefore only
  called with `cfg_mutex` held and `lock` free.
- `SET_CONFIG` updates the config under `lock` and then restarts the timer, so the new
  interval applies to the next sample instead of after one old period (up to 1 s).
- The wait-queue wakeups happen under `lock`. The wait-queue lock nests inside it, and the
  read path never takes `lock` while holding the wait-queue lock.

### Lifetime
- `fops.owner = THIS_MODULE` pins the module while any fd is open, so `rmmod` gets `EBUSY`
  (tested). The timer starts at the first reader open and is cancelled at the last release.
- A reader is freed only after `list_del` under `lock`, which is also the lock the callback
  iterates under. The callback therefore cannot reach freed memory.
- A process killed while blocked in `read()` releases normally (tested with SIGKILL).
- `vs_exit` still `WARN_ON`s a non-empty reader list and cancels the timer defensively.

## 3. Logger (`logger/`)

- **main thread** owns a `signalfd` (SIGINT/SIGTERM are blocked in every thread before
  threads are created) and a `timerfd` for `--duration`. It never blocks on the data queue.
  - On the first stop it sets `stop`, signals the reader's eventfd and calls
    `bq_request_stop`.
  - A second signal, or `--on-stop=discard`, calls `bq_abort`.
  - It therefore stays responsive however full the queue is. Tests measure 0.5 ms from SIGINT
    to exit with a full queue and 2 ms-per-record workers.
- **reader thread**: epoll on the device (O_NONBLOCK) and the stop eventfd, or on a periodic
  timerfd with `--read-period-us`.
  - It stamps each batch with `CLOCK_MONOTONIC` right after `read()` returns. That timestamp
    minus the record timestamp is the producer→userspace latency; both clocks are
    `CLOCK_MONOTONIC` in the same guest.
  - It does sequence accounting (leading, interior and trailing gaps) and pushes batches with
    `--policy drop` (non-blocking push; a full queue counts `app_dropped`) or `block` (waits,
    so backpressure turns into driver drops).
- **workers** verify `value == model(record.seed, record.seq)`, run `--work-ns` of busy work
  per record, and optionally write CSV. They check `abort` per record so shutdown latency is
  bounded by one record's work.
- **queue** (`bqueue.c`): mutex plus two condvars. It has explicit `stopping`/`closed`/`aborted`
  states, so a blocked push or pop always has a way out. Batches come from a preallocated pool
  sized `queue + workers + 2`, which provably never runs dry (asserted).
- **Shutdown**: the reader issues `STOP`, then drains the kernel ring. With `drain` it pushes
  the rest with `BQ_WAIT_DRAIN`, which ignores `stopping` but not `abort`. With `discard` it
  counts the rest as `shutdown_discarded`.
- **Checks at exit** (exit status 1 in verify mode):
  - kernel `end-start == delivered+dropped+flushed`
  - stream `received == delivered` and `gaps == dropped+flushed`
  - app `received == processed + app_dropped + shutdown_discarded`
  - zero value, order, timestamp and config-generation errors
- **Latency histogram** (`hist.c`): log-linear with 64 sub-buckets per power of two, so
  reported percentiles are within 1/64 (tested). Each thread keeps its own and they are merged
  at exit, so the hot path takes no locks.

## 4. Measurement decisions
- **CPU:** process CPU comes from `getrusage` (scheduler runtime, precise). Producer cost is
  measured inside the callback (`producer_ns`), because hardirq time is not charged to any
  process. Tick-sampled `/proc/stat` was rejected after it proved to alias with the 1 kHz tick:
  0–3.3 CPU-s for a workload using about 0.6 (RESULTS.md, "CPU accounting").
- **Rates** use the stream window (open → STOP), not wall time, which would include
  shutdown drain.
- **Jitter** is |Δtimestamp − interval| between consecutive samples of the same configuration.
  It measures when the callback actually ran.

## 5. Alternatives not taken
- **Lock-free SPSC ring** (only `smp_load_acquire`/`smp_store_release` on head/tail). This is
  possible because each ring has one producer and, under `read_mutex`, one consumer. I kept
  the spinlock because the producer also updates shared counters and the reader list, and
  the measured callback cost is 1–2 µs per sample at 10–100 kHz, including wakeups
  (RESULTS.md).
- **Per-reader watermark / wakeup threshold in the driver.** Instead, the logger's timer mode
  shows the same batching tradeoff from userspace, without adding driver state.
- **mmap'd ring buffer.** Zero-copy, but much more complex lifetime and ABI; out of scope.
