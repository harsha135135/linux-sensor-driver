#!/usr/bin/env python3
"""Mutation check for the driver test suite (root, inside the VM).

Each mutation injects one realistic bug into a temporary copy of the driver,
builds and loads it, and runs the test(s) that are supposed to catch that
class of bug. The check passes only if every mutant is *detected* (the
selected tests fail). This measures whether the tests can fail, which a list
of passing tests cannot show.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (name, original snippet, mutated snippet, tests expected to fail)
MUTANTS = [
    ("partial copy consumes uncopied records",
     "done = (bytes - left) / VS_REC_SIZE;",
     "done = n;",
     ["partial_copy_fault"]),
    ("FLUSH not counted",
     "r->flushed += r->head - r->tail;",
     "",
     ["flush_accounting"]),
    ("drop not counted",
     "r->dropped++;\n\t\t\td->dev_dropped++;\n\t\t\tcontinue;",
     "continue;",
     ["overflow_trailing_drops"]),
    ("overwrite oldest instead of tail drop",
     "if (r->head - r->tail >= capacity) {",
     "if (r->head - r->tail >= capacity) { r->tail++; r->dropped++; } if (0) {",
     ["overflow_trailing_drops"]),
    ("O_NONBLOCK ignored",
     "if (f->f_flags & O_NONBLOCK)\n\t\t\treturn -EAGAIN;",
     "",
     ["nonblock_eagain"]),
    ("record carries load-time seed",
     "rec.seed = d->seed;",
     "rec.seed = seed;",
     ["config_change_seed_queued"]),
    ("release leaks reader count",
     "\t\td->nreaders--;\n",
     "",
     ["max_readers"]),
    ("STOP does not wake blocked readers",
     "wake_up_interruptible_poll(&r->wq, EPOLLHUP);",
     "",
     ["stop_wakes_blocked_reader"]),
    ("end_seq taken before pending samples",
     "r->end_seq = d->next_seq;",
     "r->end_seq = d->next_seq - 1;",
     ["record_contents"]),
    ("SET_CONFIG accepts nonzero flags",
     "if (cfg.flags != 0 ||",
     "if (0 ||",
     ["ioctl_validation"]),
    ("interrupted wait swallowed",
     "\t\tif (ret)\n\t\t\treturn ret;\t/* -ERESTARTSYS: EINTR or restart */",
     "",
     ["signal_interrupts_read"]),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, **kw)


def main():
    if os.geteuid() != 0:
        print("run as root")
        return 2
    src = open(os.path.join(ROOT, "kernel", "vsensor.c")).read()
    tmp = tempfile.mkdtemp(prefix="vsmut-")
    shutil.copytree(os.path.join(ROOT, "include"), os.path.join(tmp, "include"))
    kdir = os.path.join(tmp, "kernel")
    os.mkdir(kdir)
    shutil.copy(os.path.join(ROOT, "kernel", "Kbuild"), kdir)
    test_bin = os.path.join(ROOT, "build", "vsensor_test")
    survived = 0
    print("TAP version 13")
    for i, (name, orig, mut, tests) in enumerate(MUTANTS, 1):
        if src.count(orig) != 1:
            print(f"not ok {i} - {name}: snippet not found exactly once")
            survived += 1
            continue
        open(os.path.join(kdir, "vsensor.c"), "w").write(src.replace(orig, mut))
        b = sh(f"make -s -C /lib/modules/$(uname -r)/build M={kdir} modules",
               capture_output=True, text=True)
        if b.returncode:
            print(f"not ok {i} - {name}: mutant failed to build\n# {b.stderr[-500:]}")
            survived += 1
            continue
        sh("rmmod vsensor 2>/dev/null")
        sh(f"insmod {kdir}/vsensor.ko && udevadm settle", check=True)
        try:
            r = subprocess.run([test_bin] + tests, capture_output=True, text=True, timeout=60)
            detected = r.returncode != 0
            detail = next((l for l in r.stdout.splitlines() if l.startswith("#")), "")
        except subprocess.TimeoutExpired:
            detected, detail = True, "# test timed out (hang detected)"
        sh("pkill -f build/vsensor_test; sleep 0.2; rmmod vsensor")
        if detected:
            print(f"ok {i} - killed: {name}\n{detail}")
        else:
            print(f"not ok {i} - SURVIVED: {name} (tests {tests} still pass)")
            survived += 1
    shutil.rmtree(tmp)
    print(f"1..{len(MUTANTS)}")
    print(f"# {len(MUTANTS) - survived}/{len(MUTANTS)} mutants detected")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
