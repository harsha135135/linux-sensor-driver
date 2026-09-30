#!/usr/bin/env bash
# Module lifetime tests (root): repeated load/use/unload cycles, unload while
# an fd is open, rejected parameters, and injected init failures. After every
# failed load there must be no /dev node, class, or chrdev region left behind.
set -euo pipefail
cd "$(dirname "$0")/.."
KO=kernel/vsensor.ko
CYCLES=${1:-50}

fail() { echo "not ok - $*"; exit 1; }

no_residue() {
	udevadm settle
	[ ! -e /dev/vsensor ] || fail "$1: /dev/vsensor left behind"
	[ ! -e /sys/class/vsensor ] || fail "$1: /sys/class/vsensor left behind"
	! grep -qw vsensor /proc/devices || fail "$1: chrdev region left behind"
	! lsmod | grep -qw '^vsensor' || fail "$1: module still loaded"
}

rmmod vsensor 2>/dev/null || true
no_residue "initial"

for i in $(seq 1 "$CYCLES"); do
	cap=$((16 << (i % 8)))
	insmod "$KO" capacity=$cap max_readers=$(((i % 4) + 1)) interval_us=$((100 + i))
	udevadm settle
	[ -c /dev/vsensor ] || fail "cycle $i: no device node"
	build/vsctl read 3 >/dev/null || fail "cycle $i: read"
	rmmod vsensor
	no_residue "cycle $i"
done
echo "ok - $CYCLES load/read/unload cycles"

insmod "$KO"
udevadm settle
exec 3</dev/vsensor
if rmmod vsensor 2>/dev/null; then fail "rmmod succeeded with an open fd"; fi
exec 3<&-
rmmod vsensor || fail "rmmod after close"
no_residue "after close"
echo "ok - unload refused while an fd is open, allowed after close"

for p in capacity=1000 capacity=8 capacity=131072 max_readers=0 max_readers=65 \
	 interval_us=5 interval_us=2000000 inject_init_fault=5; do
	if insmod "$KO" "$p" 2>/dev/null; then fail "insmod accepted $p"; fi
	no_residue "$p"
done
echo "ok - invalid module parameters rejected without residue"

for n in 1 2 3 4; do
	if insmod "$KO" inject_init_fault=$n 2>/dev/null; then fail "fault $n loaded"; fi
	no_residue "inject_init_fault=$n"
done
echo "ok - injected init failures at steps 1-4 unwind completely"

insmod "$KO"
udevadm settle
build/vsctl read 3 >/dev/null || fail "final read"
rmmod vsensor
no_residue "final"
echo "ok - normal load after failures"
