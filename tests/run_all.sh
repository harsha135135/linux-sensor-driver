#!/usr/bin/env bash
# Complete kernel-facing test run (root, inside the Linux VM). Builds must
# already exist (make all sanitizers). Scans the kernel log for splats that
# appeared during the run, which is where lockdep/KASAN/might_sleep report.
set -uo pipefail
cd "$(dirname "$0")/.."
[ "$(id -u)" -eq 0 ] || { echo "run_all.sh must run as root"; exit 2; }

marker="vsensor-run-all-$(date +%s)-$$"
echo "$marker" > /dev/kmsg
failed=0
step() { echo; echo "=== $*"; }
run() { "$@" || { echo "FAILED: $*"; failed=1; }; }

echo "kernel: $(uname -r)  cpus: $(nproc)"
grep -E '^CONFIG_(PROVE_LOCKING|KASAN|DEBUG_ATOMIC_SLEEP)=' "/boot/config-$(uname -r)" 2>/dev/null \
	|| echo "debug options: PROVE_LOCKING/KASAN/DEBUG_ATOMIC_SLEEP not enabled"

rmmod vsensor 2>/dev/null
insmod kernel/vsensor.ko || { echo "insmod failed"; exit 1; }
udevadm settle

step "unit tests (plain, TSan, ASan+UBSan)"
run build/unit_test
run setarch "$(uname -m)" -R build/unit_test-tsan
run build/unit_test-asan

step "driver functional tests"
run build/vsensor_test

step "logger integration tests"
run python3 tests/logger_tests.py

step "unload after tests (no leaked references)"
run rmmod vsensor

step "module lifetime tests"
run tests/load_unload.sh 50

step "kernel log scan"
if dmesg | sed -n "/$marker/,\$p" | grep -E 'BUG:|WARNING:|Oops|Call trace:|possible circular locking|inconsistent lock state|sleeping function called|KASAN|UBSAN|kmemleak'; then
	echo "FAILED: kernel reported problems (above)"
	failed=1
else
	echo "clean: no BUG/WARNING/lockdep/KASAN/UBSAN reports since $marker"
fi

echo
[ $failed -eq 0 ] && echo "ALL PASSED" || echo "SOME TESTS FAILED"
exit $failed
