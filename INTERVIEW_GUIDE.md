# Interview guide: vsensor driver and logger

## Walkthrough (one sample, end to end)
1. **Producer.** `vs_timer_fn` (`kernel/vsensor.c`, hrtimer callback, hardirq) takes
   `dev->lock`, then:
   - `hrtimer_forward_now` (counts overruns);
   - builds a `vsensor_record` (`seq = next_seq++`, `ktime_get_ns()`, `seed`,
     `vsensor_model_value`, `config_gen`);
   - for each non-frozen reader: tail drop if the ring is full, else store the record and
     `wake_up_interruptible_poll`.
2. **Reader thread.** `reader_main` (`logger/vslogger.c`) wakes from `epoll_wait` (or its
   timerfd in periodic mode) and calls `read()`.
3. **Kernel read.** `vs_read`: `mutex_lock_interruptible(read_mutex)` → peek records into
   `bounce` under the spinlock → `copy_to_user` → advance `tail` by whole records copied.
   Empty: `-EAGAIN`, or `wait_event_interruptible` without `read_mutex` held.
4. **Accounting.** `account()` stamps latency (`recv_ns - timestamp_ns`) and does gap,
   order, timestamp and generation accounting. `deliver()` pushes the batch into the bounded
   queue with drop or block semantics.
5. **Workers.** `worker_main` pops the batch, checks `value == vsensor_model_value(seed, seq)`,
   and applies busy work and CSV output.
6. **Shutdown.** SIGINT → `main_loop` (signalfd) → `request_stop` → reader breaks out →
   `VSENSOR_IOC_STOP` → drain to EOF → `GET_STATS` → trailing gap → `bq_close` → workers
   finish → three reconciliation checks → exit status.

## Likely questions

**1. Why an hrtimer, and what context does its callback run in?**
It gives microsecond-resolution periodic callbacks without a kthread. With `HRTIMER_MODE_REL`
on a non-RT kernel, the callback runs in **hardirq** context. So everything it touches is
protected by a spinlock taken with `irqsave` everywhere, it never sleeps, and it never
touches user memory. I measured what it costs: `producer_ns` is accumulated inside
`vs_timer_fn` (RESULTS.md, rate table).

**2. What does each lock protect?**
- `dev->lock` (spinlock): the reader list, every ring and its counters, and the device config
  snapshot and counters.
- `cfg_mutex`: open/release, SET_CONFIG and timer start/stop.
- `read_mutex`: per fd; the bounce buffer and the right to advance `tail`.

The table is in DESIGN.md §2. The main rule: `hrtimer_cancel` waits for the callback, which
takes `dev->lock`, so cancelling while holding `dev->lock` would deadlock. `vs_timer_stop`
therefore runs only under `cfg_mutex`.

**3. Why not `copy_to_user` directly from the ring under the spinlock?**
`copy_to_user` can fault and sleep, and sleeping with a spinlock held (IRQs disabled) is a
bug. `vs_read` copies into a per-file bounce buffer under the lock, drops it, then copies to
userspace holding only `read_mutex`.

**4. What if `copy_to_user` fails half way?**
- The records are only *peeked*, and `tail` advances by `(bytes - left) / 32`, i.e. the
  whole records actually copied. Uncopied records stay queued; if none could be copied the
  call returns `-EFAULT` and nothing changes.
- `partial_copy_fault` points the buffer 3.5 records before a `PROT_NONE` page and checks
  that the next `read()` continues at exactly the right seq.
- A mutant that consumed every peeked record was caught.

**5. Multiple readers: compete or broadcast? Why?**
- Broadcast: each fd has its own ring and sees every sample produced after its open.
  Competing consumers would make a gap in one reader ambiguous (dropped, or taken by another
  reader?), which rules out exact loss accounting.
- The cost is O(readers) work per tick, capped by `max_readers` (`-EMFILE`).
- Threads sharing one fd *do* compete, and each record goes to exactly one of them (tested).

**6. How can you claim drop accounting is exact?**
- `start_seq` and `end_seq` are read under the same lock that the producer increments
  `next_seq` under.
- Each sample in `[start, end)` increments exactly one of `accepted` or `dropped` for that
  reader, and `accepted = delivered + flushed + queued`.
- STOP freezes `end_seq` so the counters stop moving, then the logger drains to EOF.
- Checked every run: `end - start == delivered + dropped + flushed`, and observed gaps
  (leading, interior and **trailing**) equal `dropped + flushed`.
- A plain GET_STATS without STOP would race with production. That is why STOP exists.

**7. Why tail drop rather than overwrite-oldest?**
Overwrite-oldest means the producer moves `tail`. That breaks the peek/commit read, because
a record could be overwritten after a reader peeked it but before it committed, and it would
turn the loss into a moving window instead of one contiguous hole. Tail drop matches IIO/kfifo.
A mutant that overwrote the oldest record was caught by the stream reconciliation.

**8. What happens to blocking readers on signals, on STOP and on process kill?**
- `wait_event_interruptible` returns `-ERESTARTSYS`: `EINTR` without `SA_RESTART`,
  transparent restart with it. Both are tested with `pthread_kill`.
- STOP wakes waiters, which drain and then get 0 (EOF). A mutant without that wakeup hung
  and was caught by timeout.
- SIGKILL during a blocked read runs `vs_release` normally, and `readers` returns to 0
  (tested).

**9. How is module lifetime handled with open fds?**
`fops.owner = THIS_MODULE`, so the VFS holds a module reference per open file and `rmmod`
fails with EBUSY while any fd is open (tested with `exec 3</dev/vsensor`). The timer lives
only while readers exist. `vs_init` initialises all state before `cdev_add` and unwinds in
reverse order; `inject_init_fault=1..4` proves each unwind leaves no `/dev` node, class or
chrdev region. 50 load/use/unload cycles run clean against the kernel log.

**10. How is the ABI made safe?**
- Fixed-width `__u32`/`__u64` fields with no implicit padding, checked by `static_assert` in
  both kernel and userspace; the layout is identical for compat.
- `_IOR`/`_IOWR` encode the struct size, so a mismatched binary gets `ENOTTY` (tested).
- `flags` must be 0, and write access is required to configure.
- Records carry the **seed**, so a sample queued before a seed change still verifies. With
  `config_gen` alone, a worker could not know which seed produced an old record.

**11. Why does the logger use signalfd and timerfd instead of a signal handler?**
The main thread blocks SIGINT/SIGTERM in all threads and waits on a signalfd and a timerfd
in one epoll set. No async-signal-safety concerns, and the main thread **never blocks on the
data queue**, so it stays responsive under full backpressure. With 2 ms-per-record workers and
a full queue, the stop latency measured in `sigint_full_queue_discard` was 0.5 ms. A second
signal switches drain to discard.

**12. Where does the logger lose data, and how is that reported?**
- `--policy drop`: a full app queue drops the batch and counts `app_dropped`.
- `--policy block`: the reader blocks, the kernel ring fills and the *driver* drops.
- At 10 kHz with 250 µs/record on 2 workers (1.25× capacity), about 40k records were
  `app_dropped` under drop, versus about 35k `driver_dropped` under block, with p99 read
  latency rising from about 20 µs to about 128 ms (RESULTS.md).
- The reconciliation `received == processed + app_dropped + shutdown_discarded` holds in both
  cases.

**13. How did you measure latency, and what limits it?**
`recv_ns` (CLOCK_MONOTONIC right after `read()` returns) minus the record timestamp
(`ktime_get_ns()`, same clock, same guest). It is recorded per record in a log-linear
histogram (1/64 resolution), per thread and merged at exit. At 5–100 kHz, the median p50
is 8–15 µs and p99 17–26 µs. At 1 kHz, p50 rises to about 28 µs and p99 to about 146 µs:
between samples, the vCPUs go idle and halt, and waking them goes through the hypervisor.
Low rates are *slower* per sample, not faster.

**14. Why is the achieved rate below the requested rate at 100 kHz?**
With a 10 µs period the callback plus wakeups cannot always finish before the next expiry;
`hrtimer_forward_now` reports skipped periods, which I count as `timer_overruns` rather than
back-filling. Overruns account for the deficit (median 82.8 kHz achieved, 17.2k
overruns/s).

**15. How do you know the tests are meaningful?**
`tests/mutation_check.py` builds 11 mutants of the driver, among them:
- consuming uncopied records;
- not counting FLUSH or drops;
- overwrite-oldest;
- ignoring `O_NONBLOCK`;
- a stale seed in records;
- leaking the reader count;
- STOP without a wakeup;
- an off-by-one `end_seq`;
- accepting nonzero flags;
- swallowing `-ERESTARTSYS`.

All 11 fail their targeted tests. One was initially detected only by a hang (drops not
counted → the test waited forever), so I bounded that wait to make it fail with a message.

## Real problems encountered
No kernel crash or data-corruption bug appeared in the driver; the tests passed on the first
kernel run, which is why I added the mutation check. The real problems were in the ABI and in
measurement:

1. **ABI size miscalculated.** I hand-computed `sizeof(struct vsensor_stats)` as 104; it was
   96. The `static_assert` in the module failed the first build. The lesson was to keep the
   assert on both sides rather than trust arithmetic. It is 104 now, after adding
   `producer_ns`, and still asserted.
2. **Achieved rate was wrong under backpressure.** A 10 kHz run with blocking consumers
   reported 8.2 kHz achieved. The rate divided by wall time, which included the 0.65 s
   shutdown drain. It now uses the stream window (open → STOP); the same run reports
   9,999.8 Hz over 3.000 s.
3. **CPU usage from `/proc/stat` aliased with the timer tick.** At a 100 µs interval,
   system-wide busy time from `/proc/stat` was implausible: in the final benchmark it ranged
   from 0.00 to 3.33 CPU-s across 5 runs, while the logger alone used 0.55–0.63 CPU-s by
   `getrusage`. At 137 µs it was stable (0.27–0.36).
   - Cause: `/proc/stat` is sampled by the 1 kHz tick, and a 100 µs period divides the 1 ms
     tick exactly. Each tick therefore sees the same phase of the workload: always idle, or
     always busy, depending on the relative phase at start.
   - Fix: use `getrusage` (scheduler runtime) and add `producer_ns` inside the callback,
     because hardirq time is not charged to any process.
   - The cross-check is kept as a benchmark experiment (`cpuacct`).
4. **The batch-size experiment measured nothing.** Batch 1 and batch 64 both averaged exactly
   1.00 records per `read()`. An event-driven reader wakes on every sample and keeps up, so
   the batch limit never binds. I added `--read-period-us` (timer-driven reads). At 20 kHz,
   1 ms periods give about 20 records/read at about 0.5 ms median latency, with far less CPU.
5. **`__u64` is not `uint64_t` on arm64** (`unsigned long long` vs `unsigned long`), so
   `PRIu64` with ABI fields broke `-Werror` builds. Userspace prints ABI fields as
   `%llu` with explicit casts.
