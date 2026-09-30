#!/usr/bin/env python3
"""Driver/logger benchmark (root, inside the Linux VM, module loaded).

Experiments (each configuration is repeated REPS times; repetitions are
interleaved so slow drift in the VM affects every configuration equally):

  rate     requested sample rate 1 kHz .. 100 kHz, batch 64, no consumer load:
           achieved rate, timer overruns, jitter, producer->read() latency.
  batch    20 kHz: readiness-driven reads (batch limit 1, 64) vs timer-driven
           reads every 1 ms / 5 ms (batch limit 256): syscalls, CPU, latency.
  load     10 kHz, per-record consumer work 0 / 100 / 250 us on 2 workers,
           queue policy drop vs block: where records are lost and latency.
  cpuacct  interval 100 us vs 137 us: rusage vs /proc/stat CPU accounting
           (documents the tick-aliasing problem found during development).

Raw JSON per run goes to results/raw/<experiment>/; bench/summarize.py
regenerates results/summary.md and results/summary.csv from those files.
"""
import argparse
import json
import os
import platform
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOGGER = os.path.join(ROOT, "build", "vslogger")
RAW = os.path.join(ROOT, "results", "raw")


def experiments(duration):
    d = str(duration)
    exps = {"rate": [], "batch": [], "load": [], "cpuacct": []}
    for iv in (1000, 200, 100, 50, 20, 10):
        exps["rate"].append((f"iv{iv}", ["-t", d, "-i", str(iv), "-b", "64", "-w", "2"]))
    # Readiness-driven reads (batch limit 1 vs 64) vs timer-driven reads that
    # let samples accumulate (1 ms and 5 ms periods, batch limit 256).
    for name, extra in (("ready_b1", ["-b", "1"]), ("ready_b64", ["-b", "64"]),
                        ("period1ms_b256", ["-b", "256", "-r", "1000"]),
                        ("period5ms_b256", ["-b", "256", "-r", "5000"])):
        exps["batch"].append((name, ["-t", d, "-i", "50", "-w", "2"] + extra))
    for work in (0, 100000, 250000):
        for pol in ("drop", "block"):
            exps["load"].append((f"w{work // 1000}us_{pol}",
                                 ["-t", d, "-i", "100", "-b", "64", "-w", "2",
                                  "-l", str(work), "-q", "64", "-p", pol]))
    for iv in (100, 137):
        exps["cpuacct"].append((f"iv{iv}", ["-t", "5", "-i", str(iv), "-b", "64", "-w", "2"]))
    return exps


def sh(cmd):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


def environment():
    params = {}
    pdir = "/sys/module/vsensor/parameters"
    if os.path.isdir(pdir):
        for p in os.listdir(pdir):
            with open(os.path.join(pdir, p)) as f:
                params[p] = f.read().strip()
    return {
        "date_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "nproc": os.cpu_count(),
        "mem_total": sh("grep MemTotal /proc/meminfo"),
        "cpu_model": sh("lscpu | grep -E 'Model name|Vendor ID' | tr -s ' '"),
        "virt": sh("systemd-detect-virt"),
        "gcc": sh("gcc --version | head -1"),
        "git_rev": sh(f"git -C {ROOT} rev-parse --short HEAD"),
        "git_dirty": bool(sh(f"git -C {ROOT} status --porcelain --untracked-files=no")),
        "module_params": params,
        "clocksource": sh("cat /sys/devices/system/clocksource/clocksource0/current_clocksource"),
        "hz": sh("grep '^CONFIG_HZ=' /boot/config-$(uname -r)"),
        "preempt": sh("uname -v"),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--duration", type=float, default=20)
    ap.add_argument("--only", nargs="*", help="experiment names to run")
    args = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("run as root (the logger sets the device configuration)")

    exps = experiments(args.duration)
    if args.only:
        exps = {k: v for k, v in exps.items() if k in args.only}
    os.makedirs(RAW, exist_ok=True)
    with open(os.path.join(RAW, "environment.json"), "w") as f:
        json.dump(environment(), f, indent=2)

    total = sum(len(v) for v in exps.values()) * args.reps
    n = 0
    for rep in range(1, args.reps + 1):
        for exp, configs in exps.items():
            os.makedirs(os.path.join(RAW, exp), exist_ok=True)
            for name, largs in configs:
                n += 1
                out = os.path.join(RAW, exp, f"{name}_rep{rep}.json")
                cmd = [LOGGER, "-m", "bench", "-S", "1", "-L", f"{exp}/{name}",
                       "-j", out] + largs
                print(f"[{n}/{total}] {exp}/{name} rep {rep}", flush=True)
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    print(r.stderr, file=sys.stderr)
                    sys.exit(f"run failed: {' '.join(cmd)}")
                with open(out) as f:
                    d = json.load(f)
                if not d["ok"]:
                    print(f"  WARNING: accounting checks failed: {d['checks']}", flush=True)
                time.sleep(1)
    subprocess.run([LOGGER.replace("build/vslogger", "build/vsctl"), "set", "1000", "1"],
                   stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
