#!/bin/bash
# Run the kernel under QEMU several times and report how far each run got.
#
# The corruption is not deterministic: some runs reach bsd_init: done, others die
# inside mockfs.  Catching a failing run with the probes in place is what tells us
# which call failed, so this loop just repeats the experiment and summarises.
#
#   tools/qemu/loop.sh [count] [seconds]
#
# Logs land in tools/qemu/out/loop_<n>.log
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
COUNT=${1:-3}
SECONDS_PER_RUN=${2:-200}

mkdir -p "$OUT"
cd "$REPO"

for i in $(seq 1 "$COUNT"); do
	LOG=$OUT/loop_$i.log
	timeout $((SECONDS_PER_RUN + 30)) bash tools/qemu/run.sh --seconds "$SECONDS_PER_RUN" --stdout >"$LOG" 2>&1
	done_n=$(grep -c 'bsd_init: done' "$LOG" 2>/dev/null || true)
	panic_n=$(grep -c 'panic(' "$LOG" 2>/dev/null || true)
	rvalue=$(grep -o 'mountroot done rvalue=[0-9]*' "$LOG" 2>/dev/null | head -1 || true)
	where=$(grep -o 'pc _[a-z_]*+0x[0-9a-f]*' "$LOG" 2>/dev/null | head -1 || true)
	echo "run $i: bsd_init_done=$done_n panics=$panic_n ${rvalue:-no-mountroot} ${where:-no-panic}"
done
