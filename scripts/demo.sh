#!/usr/bin/env bash
# Short end-to-end demo (inside the VM, from the project root, as root).
set -euo pipefail
cd "$(dirname "$0")/.."
make -s all >/dev/null
rmmod vsensor 2>/dev/null || true
insmod kernel/vsensor.ko capacity=256
udevadm settle
echo "--- device and configuration"
ls -l /dev/vsensor
build/vsctl set 500 42
echo "--- five samples (value is recomputed from seed and seq by vsctl)"
build/vsctl read 5
echo "--- 3 s logger run at 10 kHz with an overloaded consumer (drop policy)"
out=$(mktemp)
build/vslogger -t 3 -i 100 -w 1 -l 150000 -q 4 -b 32 -p drop -j "$out"
python3 - "$out" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(f"received {d['received']}, processed {d['processed']}, app drops {d['app_dropped']}, "
      f"driver drops {d['driver_dropped']}, gaps {d['gaps']}")
print(f"read latency p50/p99: {d['read_latency_ns']['p50'] / 1e3:.1f}/"
      f"{d['read_latency_ns']['p99'] / 1e3:.1f} us")
print("accounting checks:", d["checks"])
PY
rm -f "$out"
echo "--- unload refused while a reader is open"
exec 3</dev/vsensor
rmmod vsensor 2>&1 || true
exec 3<&-
rmmod vsensor && echo "unloaded after close"
