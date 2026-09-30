# Results

Everything here was measured on a **virtual** sensor running in a **VM**. The numbers
describe this setup, not bare metal or real sensor hardware.

## Environment
| | |
|---|---|
| Host | MacBook Pro, Apple M4 Pro (12 cores), 24 GiB, macOS 26.5.2 |
| VM | Lima 2.2.0, Apple Virtualization.framework (`vz`), 6 vCPUs, 8 GiB, arm64 |
| Guest | Ubuntu 24.04.4, kernel `6.8.0-134-generic` (`PREEMPT_DYNAMIC`), `CONFIG_HZ=1000`, clocksource `arch_sys_counter` |
| Toolchain | gcc 13.3.0 (guest) |
| Code revision | `888dd0f` (benchmark). Later commits change only docs and scripts. |
| Module params | `capacity=1024 max_readers=8` (defaults) |

The VM was created with `scripts/vm_setup.sh`.

## Functional validation (stock kernel)
Command: `make check` (runs `tests/run_all.sh` as root). Log:
[results/logs/run_all_stock.txt](results/logs/run_all_stock.txt).

| Suite | Result |
|---|---|
| Unit tests (queue, histogram): plain, TSan, ASan+UBSan | pass |
| Driver tests (`tests/vsensor_test.c`) | 16/16 pass |
| Logger integration (`tests/logger_tests.py`, incl. TSan and ASan/UBSan logger runs against the device) | 12/12 pass |
| Module lifetime (`tests/load_unload.sh`): 50 load/read/unload cycles, rmmod-while-open refused, 8 invalid parameter sets rejected, injected init failure at each of 4 steps unwinds with no residue | pass |
| Kernel log scan for BUG/WARNING/lockdep/KASAN/UBSAN during the run | clean |
| sparse (`make sparse`), checkpatch `--strict` | no warnings; 0 errors / 0 warnings / 0 checks ([log](results/logs/static_checks_stock.txt)) |
| Mutation check (`tests/mutation_check.py`) | 11/11 injected bugs detected ([log](results/logs/mutation_check.txt)) |

These tests ran on a stock kernel **without** lockdep or KASAN. See the separate section
below for the debug-kernel run.

Selected behaviour observed in the tests:
- **Partial copy:** with a buffer ending 3.5 records before a `PROT_NONE` page, `read()`
  returned whole records only, and the next `read()` continued at the exact next seq. A
  fully inaccessible buffer returned `EFAULT` and consumed nothing.
- **Shutdown:** with a full queue and 2 ms-per-record workers, SIGINT with `--on-stop=discard`
  stopped the logger in 0.5 ms (`stop_latency_ms` in the test output).

## Benchmark
Command: `sudo python3 bench/run_bench.py --reps 5 --duration 20`, then
`python3 bench/summarize.py`.

- Raw data: [results/raw/](results/raw/) (one JSON per run, plus `environment.json`).
  Tables: [results/summary.md](results/summary.md); per-run CSV:
  [results/summary_runs.csv](results/summary_runs.csv).
- Each configuration ran 5 times for 20 s (the CPU-accounting check 5 × 5 s), with
  repetitions interleaved across configurations. Cells are median [min–max].
- All 90 runs passed the logger's kernel, stream, application and data checks.
- Latency is producer (`ktime_get_ns()` in the timer callback) → userspace (`CLOCK_MONOTONIC`
  right after `read()` returns), both `CLOCK_MONOTONIC` in the same guest.
- **Disclosure:** during repetitions 1–2, the host also ran short single-threaded FTL builds,
  tests and simulations (a few minutes in total). The p99 latency medians of repetitions 1–2
  and 3–5 differ by at most 5 µs in every configuration except 1 kHz (153 vs 146 µs), so
  they are reported together.

### Sample-rate ladder (batch 64, 2 workers, no consumer work)
| requested Hz | achieved Hz | timer overruns/s | driver drops | jitter p50 µs | jitter p99 µs | latency p50 µs | latency p99 µs | logger CPU % of 1 CPU | producer CPU % |
|---|---|---|---|---|---|---|---|---|---|
| 1 000 | 993 [982–999] | 6.9 | 0 | 22.1 | 469 [206–676] | 27.8 [24.4–52.5] | 146 [112–194] | 3.7 | 0.75 |
| 5 000 | 5 000 | 0.2 | 0 | 1.4 | 35.1 | 15.2 | 19.6 | 6.3 | 1.08 |
| 10 000 | 10 000 | 0.4 | 0 | 0.8 | 26.8 | 14.7 | 17.0 | 11.6 | 1.89 |
| 20 000 | 19 996 | 4.1 | 0 | 0.7 | 25.0 | 12.6 | 17.0 | 21.9 | 3.70 |
| 50 000 | 49 802 | 198 | 0 | 5.4 | 16.5 | 11.7 | 18.6 | 52.3 | 7.07 |
| 100 000 | 82 768 [82 128–83 794] | 17 232 | 0 | 4.8 | 13.6 | 8.4 | 25.7 | 71.4 | 8.71 |

- **Achieved vs requested.** Up to 50 kHz the timer keeps the requested rate within 0.4%.
  At 100 kHz (10 µs period) the callback plus the reader wakeups cannot always complete
  within the period. `hrtimer_forward_now` skips about 17k periods/s, and those skipped
  periods account for the shortfall (82.8k produced + 17.2k skipped ≈ 100k/s). Samples are
  not back-filled, so there are no sequence gaps and no drops.
- **Producer cost** is the time inside `vs_timer_fn` (producer CPU % / achieved rate):
  about 1.9 µs per sample at 10 kHz and about 1.05 µs at 100 kHz. At 1 kHz it is about
  7.5 µs per sample; the same code runs on a colder cache and wakes an idle vCPU each time.
- **Latency is highest at the lowest rate.** At 1 kHz, the vCPUs go idle between samples.
  The reader's vCPU halts, and waking it requires the hypervisor to schedule the vCPU thread
  on the host. At 5–100 kHz the vCPUs stay busy enough that p50 is 8–15 µs and p99 17–26 µs.
  Jitter follows the same pattern (p99 469 µs at 1 kHz vs 14–35 µs at higher rates), because
  the timer interrupt itself is delivered to a halted vCPU.

### Read strategy at 20 kHz (2 workers, no consumer work)
| wakeup | max records/read | read() calls/s | records/read | logger CPU % | latency p50 µs | latency p99 µs |
|---|---|---|---|---|---|---|
| readiness (epoll) | 1 | 19 998 | 1.00 | 21.6 | 12.5 | 16.8 |
| readiness (epoll) | 64 | 19 989 | 1.00 | 22.8 | 13.2 | 18.0 |
| timer, 1 ms | 256 | 1 010 | 19.8 | 1.2 | 498 | 979 |
| timer, 5 ms | 256 | 223 | 89.8 | 0.3 | 2 474 | 5 210 |

- With readiness-driven reads, the batch limit does not matter. The reader wakes on every
  sample and finds exactly one, so the 64-record limit never binds. This was an unexpected
  result in the first version of this experiment; see INTERVIEW_GUIDE.
- Letting samples accumulate trades latency for CPU: a 1 ms read period uses about 18×
  less logger CPU and adds about 0.5 ms median latency (about half the period, as expected
  for uniformly arriving samples).

### Consumer load and queue policy at 10 kHz (2 workers, batch 64, queue 64 batches)
| work µs/record | policy | received | app drops | driver drops | read latency p99 µs | worker latency p99 µs | logger CPU % |
|---|---|---|---|---|---|---|---|
| 0 | block / drop | 199 989 / 199 991 | 0 | 0 | 17.3 / 18.6 | 34.0 / 36.1 | 11.7 |
| 100 | block / drop | 199 989 / 199 991 | 0 | 0 | 20.4 / 19.1 | 36.1 / 35.1 | 113.5 |
| 250 | block | 165 122 | 0 | 34 800 | 127 402 | 649 980 | 200.1 |
| 250 | drop | 199 982 | 40 017 | 0 | 19.1 | 8 094 | 204.7 |

- At 250 µs per record, 2 workers can process 8 000 records/s against 10 000 arriving, so
  the consumer is overloaded by 25%.
- With `drop`, the reader keeps up; the application discards whole batches (40k records),
  and read latency stays at about 19 µs p99.
- With `block`, the reader stalls on the full queue, the 1024-record kernel ring fills and
  the driver drops (34.8k). Read latency p99 grows to 127 ms: a full ring at 10 kHz is about
  100 ms of samples, plus the reader's own stall.
- In both cases, every lost record is attributed to exactly one counter, and all
  reconciliation checks pass.

### CPU accounting cross-check
| interval µs | logger CPU-s (`getrusage`) per run | whole-system busy CPU-s (`/proc/stat`) per run |
|---|---|---|
| 100 | 0.55, 0.62, 0.57, 0.58, 0.63 | 0.00, 3.33, 0.06, 1.65, 0.48 |
| 137 | 0.42, 0.46, 0.41, 0.41, 0.46 | 0.30, 0.33, 0.27, 0.27, 0.36 |

`/proc/stat` is sampled by the 1 kHz scheduler tick. A 100 µs period divides the tick, so
each tick lands at the same phase of the workload, and the estimate depends on that phase:
anywhere from 0 to about 5× the logger's own CPU. At 137 µs the phase drifts and the estimate
is stable. This is why CPU numbers in this project come from `getrusage` (process) and
`producer_ns` (the timer callback), not `/proc/stat`.

## Kernel debug-option run (lockdep / KASAN / UBSAN / kmemleak)
This was a separate run on a debug kernel, not mixed with the performance numbers above. No
benchmark was run on it: KASAN and lockdep slow everything down.

- **Kernel:** kernel.org 6.8.12 with the stock Ubuntu config trimmed by `localmodconfig`
  (`scripts/build_debug_kernel.sh`), booted once via `grub-reboot`. `/boot/config` of the
  booted kernel contains `CONFIG_PROVE_LOCKING=y`, `CONFIG_DEBUG_ATOMIC_SLEEP=y`,
  `CONFIG_KASAN=y` (generic), `CONFIG_UBSAN=y`, `CONFIG_DEBUG_KMEMLEAK=y` and
  `CONFIG_DEBUG_LIST=y`.
  - The boot log confirms "KernelAddressSanitizer initialized (generic)" and "RCU lockdep
    checking is enabled".
  - `DEBUG_OBJECTS_HRTIMERS` was requested but did not survive `olddefconfig`, so hrtimer
    object debugging was **not** active.
- **Suite:** the full `tests/run_all.sh` passed; module rebuilt against `6.8.12-vsdebug`.
  Log: [results/logs/run_all_debug_kernel.txt](results/logs/run_all_debug_kernel.txt). It
  covers all unit, driver, logger (including TSan/ASan logger runs), lifetime and
  injected-failure tests, and 50 load/unload cycles.
- **Kernel log:** zero BUG/WARNING/lockdep/KASAN/UBSAN/might-sleep reports, both at boot and
  after the suite.
- **lockdep coverage:** `/proc/lockdep` shows the driver's lock classes and the dependency
  edges DESIGN.md documents: `&d->cfg_mutex → &d->lock`, `&r->read_mutex → &d->lock`,
  `&d->lock → &r->wq`, with `&d->lock` marked as used in hardirq context.
  [log](results/logs/lockdep_classes_debug_kernel.txt)
- **kmemleak** (two forced scans after the suite): one report, a 32 KiB vmalloc from
  `arm64_efi_rt_init` by pid 1 at boot, before the module was loaded. There were no reports
  involving vsensor. [log](results/logs/kmemleak_debug_kernel.txt)

What this does and does not show: these checkers found no problem on the code paths the
tests exercise. Lockdep validates lock orders it has observed; it cannot prove the absence of
orders it never saw. KASAN only checks accesses that actually happen.

Two script problems found and fixed while doing this:
1. `set -o pipefail` plus `yes '' | make localmodconfig` aborted the build when `yes` got
   SIGPIPE.
2. `grub-set-default 0` does not pin the stock kernel, because entry 0 follows the newest
   kernel version, and the VM booted the debug kernel a second time. The default is now pinned
   by entry name, and a reboot confirmed the VM returns to `6.8.0-134-generic`.
