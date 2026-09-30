#!/usr/bin/env python3
"""Integration tests for vslogger against the real /dev/vsensor (run as root).

Each test runs the logger as a subprocess, optionally signals it, and checks
the JSON summary. The logger itself verifies every sample and reconciles its
counters with the kernel's (kernel/stream/app checks); these tests assert that
those checks pass under slow consumers, full queues, config changes and
shutdown, and that shutdown stays responsive.
"""
import json
import os
import platform
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGGER = os.path.join(ROOT, "build", "vslogger")
VSCTL = os.path.join(ROOT, "build", "vsctl")


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def start(args, binary=LOGGER, prefix=()):
    fd, path = tempfile.mkstemp(suffix=".json")
    os.close(fd)
    cmd = list(prefix) + [binary, "-j", path] + list(args)
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return p, path


def finish(p, path, timeout=60):
    out, err = p.communicate(timeout=timeout)
    with open(path) as f:
        text = f.read()
    os.unlink(path)
    check(text, f"no JSON written (rc={p.returncode}, stderr={err!r})")
    return p.returncode, json.loads(text), err


def run(args, **kw):
    p, path = start(args, **kw)
    return finish(p, path)


def assert_ok(rc, d, err=""):
    check(d["ok"], f"logger checks failed: {d['checks']} errors={d['errors']} stderr={err!r}")
    check(rc == 0, f"exit status {rc}, stderr={err!r}")


def t_verify_default():
    rc, d, err = run(["-t", "2", "-i", "200", "-S", "5"])
    assert_ok(rc, d, err)
    check(d["received"] > 1000, f"received only {d['received']}")
    check(d["config"]["seed"] == 5, "seed not applied")


def t_slow_workers_drop_policy():
    # 20 kHz in, one worker at 200 us/record (~5 kHz): the app queue overflows.
    rc, d, err = run(["-t", "2", "-i", "50", "-w", "1", "-l", "200000",
                      "-q", "4", "-b", "16", "-p", "drop"])
    assert_ok(rc, d, err)
    check(d["app_dropped"] > 0, "expected application drops")
    check(d["received"] == d["processed"] + d["app_dropped"] + d["shutdown_discarded"],
          "app accounting")


def t_slow_workers_block_policy():
    # Same load with backpressure: the reader blocks, so the kernel ring drops.
    rc, d, err = run(["-t", "2", "-i", "50", "-w", "1", "-l", "200000",
                      "-q", "4", "-b", "16", "-p", "block"])
    assert_ok(rc, d, err)
    check(d["app_dropped"] == 0, "block policy must not drop in the app")
    check(d["driver_dropped"] > 0, "expected driver drops under backpressure")
    check(d["gaps"] == d["driver_dropped"], "gaps must equal driver drops")


def slow_full_queue_args(on_stop):
    # 2 ms per record, 64-record batches, 4-batch queue, blocking reader:
    # the queue and the kernel ring are both full within a second.
    return ["-i", "50", "-w", "1", "-l", "2000000", "-q", "4", "-b", "64",
            "-p", "block", "-s", on_stop]


def t_sigint_full_queue_discard():
    p, path = start(slow_full_queue_args("discard"))
    time.sleep(1.5)
    t0 = time.monotonic()
    p.send_signal(signal.SIGINT)
    rc, d, err = finish(p, path, timeout=30)
    elapsed = time.monotonic() - t0
    assert_ok(rc, d, err)
    check(d["signals"] == 1, "one signal expected")
    check(d["shutdown_discarded"] > 0, "queued work should have been discarded")
    # Draining this backlog would take >2.5 s of worker time; discard must not.
    check(d["stop_latency_ms"] < 1000, f"stop took {d['stop_latency_ms']} ms")
    print(f"#   discard: stop_latency_ms={d['stop_latency_ms']:.1f} "
          f"process exit {elapsed * 1000:.0f} ms after SIGINT, "
          f"discarded={d['shutdown_discarded']}")


def t_sigint_drain_then_second_signal():
    p, path = start(slow_full_queue_args("drain"))
    time.sleep(1.5)
    p.send_signal(signal.SIGINT)
    time.sleep(0.3)           # drain in progress (it needs >2.5 s)
    p.send_signal(signal.SIGINT)
    rc, d, err = finish(p, path, timeout=30)
    assert_ok(rc, d, err)
    check(d["signals"] == 2, f"signals={d['signals']}")
    check(d["shutdown_discarded"] > 0, "second signal should discard the rest")
    check(d["stop_latency_ms"] < 2000, f"stop took {d['stop_latency_ms']} ms")


def t_sigterm_drain_completes():
    p, path = start(["-i", "100", "-w", "2", "-l", "100000", "-q", "8", "-b", "64",
                     "-p", "block", "-s", "drain"])
    time.sleep(1.5)
    p.send_signal(signal.SIGTERM)
    rc, d, err = finish(p, path, timeout=60)
    assert_ok(rc, d, err)
    check(d["shutdown_discarded"] == 0, "drain must not discard")
    check(d["processed"] == d["received"] - d["app_dropped"], "drain lost records")


def t_duration_expiry_full_queue():
    t0 = time.monotonic()
    rc, d, err = run(["-t", "1.5"] + slow_full_queue_args("discard"))
    elapsed = time.monotonic() - t0
    assert_ok(rc, d, err)
    check(d["signals"] == 0, "no signal was sent")
    check(d["stop_latency_ms"] < 1000, f"stop took {d['stop_latency_ms']} ms")
    check(elapsed < 10, f"run took {elapsed:.1f} s")


def t_seed_change_during_run():
    # Slow consumer + blocking queue: old-seed samples sit in the kernel ring
    # and in the app queue while the seed changes underneath them.
    p, path = start(["-t", "4", "-i", "100", "-w", "1", "-l", "300000",
                     "-q", "8", "-b", "32", "-p", "block"])
    time.sleep(0.8)
    for iv, seed in ((100, 111), (150, 222), (100, 333)):
        subprocess.run([VSCTL, "set", str(iv), str(seed)], check=True,
                       stdout=subprocess.DEVNULL)
        time.sleep(0.7)
    rc, d, err = finish(p, path)
    assert_ok(rc, d, err)
    check(d["errors"]["value"] == 0, "value mismatch across seed change")
    check(d["config_gens_seen"] >= 3, f"only {d['config_gens_seen']} generations seen")


def t_periodic_reads_batch():
    # Timer-driven reads every 2 ms at 10 kHz must batch ~20 samples per read.
    rc, d, err = run(["-t", "2", "-i", "100", "-b", "256", "-r", "2000"])
    assert_ok(rc, d, err)
    check(d["received"] / d["reads"] > 5, f"{d['received'] / d['reads']:.1f} records/read")


def t_csv_output():
    fd, csv = tempfile.mkstemp(suffix=".csv")
    os.close(fd)
    rc, d, err = run(["-t", "1", "-i", "500", "-o", csv])
    assert_ok(rc, d, err)
    with open(csv) as f:
        lines = f.read().splitlines()
    os.unlink(csv)
    check(lines[0] == "seq,timestamp_ns,value,seed,config_gen", "CSV header")
    check(len(lines) - 1 == d["processed"], f"{len(lines) - 1} rows vs {d['processed']}")


def sanitizer_run(binary, prefix=()):
    p, path = start(["-i", "100", "-w", "3", "-l", "20000", "-q", "4", "-b", "8",
                     "-p", "drop"], binary=binary, prefix=prefix)
    time.sleep(2)
    p.send_signal(signal.SIGINT)
    rc, d, err = finish(p, path)
    check("Sanitizer" not in err and "runtime error" not in err,
          f"sanitizer report:\n{err[:4000]}")
    assert_ok(rc, d, err)


def t_tsan_logger():
    # TSan needs a low-entropy address space layout on recent kernels.
    sanitizer_run(os.path.join(ROOT, "build", "vslogger-tsan"),
                  prefix=("setarch", platform.machine(), "-R"))


def t_asan_ubsan_logger():
    sanitizer_run(os.path.join(ROOT, "build", "vslogger-asan"))


TESTS = [t_verify_default, t_slow_workers_drop_policy, t_slow_workers_block_policy,
         t_sigint_full_queue_discard, t_sigint_drain_then_second_signal,
         t_sigterm_drain_completes, t_duration_expiry_full_queue,
         t_seed_change_during_run, t_periodic_reads_batch, t_csv_output, t_tsan_logger, t_asan_ubsan_logger]


def main():
    sel = sys.argv[1:]
    failed = 0
    n = 0
    print("TAP version 13")
    for t in TESTS:
        name = t.__name__[2:]
        if sel and name not in sel:
            continue
        n += 1
        try:
            t()
            print(f"ok {n} - {name}")
        except Exception as e:  # report and continue
            failed += 1
            print(f"not ok {n} - {name}\n#   {type(e).__name__}: {e}")
        sys.stdout.flush()
    print(f"1..{n}")
    subprocess.run([VSCTL, "set", "1000", "1"], stdout=subprocess.DEVNULL)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
