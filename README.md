# vsensor: Linux virtual sensor driver and concurrent logger

A Linux kernel module that exposes a **virtual** (synthetic, hrtimer-driven) sensor as
`/dev/vsensor`, plus a multithreaded C logger that reads, verifies and records the stream.
The project is about character-device semantics, kernel/user concurrency, exact loss
accounting and module lifetime. There is no real hardware.

- **Driver** (`kernel/vsensor.c`):
  - deterministic samples, each with a sequence number and `CLOCK_MONOTONIC` timestamp;
    configurable interval (10 µs–1 s)
  - a bounded per-reader ring with tail drop
  - blocking and non-blocking `read`, `poll`/`epoll`
  - ioctls for config, stats, flush and stop
  - exact drop accounting; safe init unwinding and teardown
- **Logger** (`logger/`): a reader thread and worker threads connected by a bounded queue.
  Signals and duration are handled through `signalfd`/`timerfd`. It has drop/block
  backpressure policies, drain/discard shutdown, a deterministic verify mode and a JSON
  benchmark mode.
- **Tests**:
  - 16 driver tests against the real module
  - 12 logger integration tests, including TSan and ASan/UBSan logger runs
  - load/unload lifetime tests with injected init failures
  - a mutation check showing the tests catch 11 injected driver bugs

See [DESIGN.md](DESIGN.md) for decisions and locking, [RESULTS.md](RESULTS.md) for measured
numbers, [LIMITATIONS.md](LIMITATIONS.md), and [INTERVIEW_GUIDE.md](INTERVIEW_GUIDE.md).

## Layout
```
include/uapi/vsensor.h    record + ioctl ABI (fixed-width, shared with userspace)
include/vsensor_model.h   deterministic sample function (kernel + userspace)
kernel/                   module (kbuild)
logger/                   vslogger: vslogger.c, bqueue.[ch] (bounded queue), hist.[ch]
tools/vsctl.c             get/set config, stats, read N samples
tests/                    vsensor_test.c (driver, TAP), logger_tests.py, load_unload.sh,
                          unit_test.c (queue/histogram), mutation_check.py, run_all.sh
bench/                    run_bench.py (raw JSON), summarize.py (tables)
results/                  raw/ per-run JSON, summary.md, summary_runs.csv
scripts/                  vm_setup.sh (Lima VM on macOS), demo.sh
```

## Setup (macOS host → Linux VM)
Kernel code is built and run only inside a Linux VM. From macOS:
```sh
scripts/vm_setup.sh          # Lima + Ubuntu 24.04 arm64 (vz), 6 vCPU, 8 GiB, workspace mounted
limactl shell kdev
cd ~/developer/embedded/linux-sensor-driver   # same path as on the host
```
Any Linux machine with kernel headers works: `apt install build-essential linux-headers-$(uname -r) python3 sparse`.

## Build
```sh
make all          # module (kbuild against /lib/modules/$(uname -r)/build) + userspace
make sanitizers   # TSan and ASan/UBSan builds of the logger and unit tests
make sparse checkpatch
```

## Test
```sh
make check                          # builds, then sudo tests/run_all.sh:
                                    #   unit tests (plain/TSan/ASan), driver tests, logger tests,
                                    #   rmmod, 50 load/unload cycles, kernel-log scan
sudo python3 tests/mutation_check.py   # 11 injected driver bugs must each fail a test
```

## Use
```sh
sudo insmod kernel/vsensor.ko capacity=1024 max_readers=8 interval_us=1000 seed=1
sudo build/vsctl set 100 42       # 10 kHz, seed 42 (needs write access)
build/vsctl read 5
sudo build/vslogger -t 10 -w 2 -b 64 -m verify          # exits 1 on any accounting/data error
sudo build/vslogger -t 10 -l 250000 -p block -m bench -j run.json
sudo scripts/demo.sh              # short end-to-end demo
```
`vslogger --help` lists all options: batch, queue, workers, per-record work, drop/block policy,
drain/discard shutdown, `--read-period-us`, CSV output.

## Benchmark
```sh
sudo python3 bench/run_bench.py --reps 5 --duration 20   # ~30 min, writes results/raw/
python3 bench/summarize.py                               # regenerates results/summary.md
```

## Semantics in one paragraph
- Every fd opened for reading is an independent stream of the samples produced after its
  `open()`. Threads that share one fd compete for its records.
- Each stream is the seq interval `[start_seq, end_seq)`. `VSENSOR_IOC_STOP` fixes `end_seq`;
  after draining, `read()` returns 0.
- Every seq in the interval is exactly one of delivered, dropped (ring full, tail drop) or
  flushed (`VSENSOR_IOC_FLUSH`), so observed gaps always equal `dropped + flushed`, trailing
  drops included.
- `read()` returns whole 32-byte records only. A fault part way through a copy returns the
  records that were fully copied and leaves the rest queued.
