# Limitations

## Scope
- **Virtual sensor.** There is no hardware, bus (I2C/SPI), interrupt line, device tree or
  platform driver. The "interrupt" is an hrtimer. Real drivers would also cover probe/remove,
  power management and DMA, none of which are exercised here.
- **One device instance.** A single `/dev/vsensor` exists (minor 0). Multiple instances would
  need per-instance state allocation and an IDA for minors.
- **Development and testing environment:** only arm64 (Apple Virtualization.framework guest),
  Ubuntu 24.04, kernel 6.8. The code has version guards for `class_create` (6.4) and
  `hrtimer_setup` (6.13), but those guarded paths were not compiled or tested on other kernels
  or architectures. PREEMPT_RT was not tested (there, the timer callback would run in softirq
  context and `spinlock_t` would sleep; the design should hold, but that is unverified).

## Behaviour not supported
- No `mmap` interface, no per-reader wakeup watermark, and no `write()` (samples are
  produced in-kernel only).
- STOP is irreversible for an fd. A reader that wants a new stream reopens the device.
- Ring capacity and `max_readers` are fixed at load time. SET_CONFIG changes only the
  interval and seed.
- Any local user can open readers (mode 0644), so one user can occupy all `max_readers`
  slots and cause `EMFILE` for others. A production driver would restrict this or apply a
  per-user limit.
- The producer runs one callback per period. If the timer runs late, the missed periods are
  counted (`timer_overruns`) and not back-filled, so the achieved rate can be below the
  requested rate (measured: a median of 82.8 kHz when 100 kHz is requested; see RESULTS.md).

## Untested or partially tested paths
- `kvcalloc` failure in `open()` (ENOMEM path) is not fault-injected. The init-time unwind
  paths are, via `inject_init_fault`.
- `mutex_lock_interruptible` returning `-ERESTARTSYS` in `read()`/FLUSH (a signal while
  waiting for `read_mutex`, as opposed to waiting for data) is not exercised deterministically.
- `compat_ioctl` from a 32-bit process was not tested; there was no 32-bit userspace in the
  guest.
- Lockdep, KASAN, UBSAN and kmemleak ran on a separate 6.8.12 debug kernel (RESULTS.md).
  Hrtimer debug objects were not enabled, and KCSAN (data-race detection) was not run.
- The mutation check (`tests/mutation_check.py`) covers 11 injected bugs. It shows the tests
  can fail for those bug classes, not that the test suite is complete.

## Measurement caveats
- **VM scheduling.** All measurements come from a 6-vCPU guest whose vCPUs are host threads.
  Host scheduling, vCPU halt/wake latency and timer virtualization affect jitter and latency
  (discussed in RESULTS.md). The numbers describe this setup and are not bare-metal figures.
- **Producer CPU accounting** (`producer_ns`) measures time inside the callback only. Timer
  interrupt entry/exit and hypervisor overhead are excluded.
- The latency histogram has 1/64 relative resolution.

## Realistic next steps
1. Replace the per-reader spinlock ring with a lock-free SPSC ring for the data slots, and
   measure whether the producer cost changes.
2. Add a per-reader wakeup watermark (IIO-style), with a timeout, in the driver.
3. Test on x86_64 and a newer kernel (6.12 LTS) in CI, including `hrtimer_setup`.
4. Fault-inject `open()` allocation failures (`failslab` with `fail_page_alloc` filtering)
   and add a 32-bit compat ioctl test.
5. Turn the virtual producer into a platform driver bound through a device-tree overlay, to
   exercise probe/remove.
