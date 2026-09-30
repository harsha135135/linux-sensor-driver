# Résumé bullets

- Wrote a Linux kernel character-device driver in C for a **virtual** (hrtimer-driven) sensor:
  per-reader bounded rings with tail drop, blocking and non-blocking reads, poll/epoll, a
  fixed-width ioctl ABI, and exact per-reader loss accounting. Verified in an arm64 Linux VM
  with kernel-facing tests, 50 load/unload cycles with injected init failures, a lockdep +
  KASAN debug-kernel run with zero reports, and a mutation check (11/11 injected bugs caught).
- Built a multithreaded POSIX C logger (reader and worker threads, bounded queue,
  signalfd/timerfd shutdown) that reconciles every sample against kernel counters.
  Benchmarked 1–100 kHz in the VM: zero loss and 17 µs p99 producer-to-userspace latency at
  10–20 kHz. Timer-batched reads cut logger CPU 18× (21.6% → 1.2%) for +0.5 ms median latency.

Every number is from RESULTS.md (benchmark revision `888dd0f`, raw data in `results/raw/`,
test logs in `results/logs/`).
