#!/bin/bash
# Run the dandelion kernel under QEMU's aarch64 "virt" machine.
#
#   tools/qemu/run.sh                       # boot, serial log to tools/qemu/out/serial.log
#   tools/qemu/run.sh --seconds 60          # give it longer
#   tools/qemu/run.sh --stdout              # serial straight to the terminal
#   tools/qemu/run.sh --gdb                 # stop at reset, print the gdb line to run
#   tools/qemu/run.sh --ring                # also dump XNU's 0x5fc00000 trace ring
#                                           #   and the framebuffer text console
#
# What is on the wire:
#   the shim (tools/qemu/shim) prints its own four lines and jumps to
#   mach_kernel; everything after that is the kernel.  The kernel's *serial*
#   output only exists once zzz22 (the PL011 sink) is in the build -- until
#   then the kernel's console is the framebuffer and its 0x5fc00000 trace
#   ring, and --ring is how you read both.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$HERE/out
SHIM=${SHIM:-$HERE/shim/out/shim.elf}
KERNEL=${KERNEL:-$REPO/Resources/UniXNU/mach_kernel}
MACHO_PA=0x48000000
RAMDISK=${RAMDISK-$REPO/Resources/UniXNU/kernel.macho.rd}
RAMDISK_PA=0x4E000000

BUDGET=20
RAM=2048
SMP=1
CPU=cortex-a53
GIC=3
LOG=$OUT/serial.log
TO_STDOUT=0
GDB=0
RING=0
RING_FB=0
RING_REGS=0
GDB_PORT=1234

usage()
{
	sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	--seconds)   BUDGET=${2:?}; shift 2 ;;
	--ram)       RAM=${2:?}; shift 2 ;;
	--smp)       SMP=${2:?}; shift 2 ;;
	--cpu)       CPU=${2:?}; shift 2 ;;
	--gic)       GIC=${2:?}; shift 2 ;;
	--kernel)    KERNEL=${2:?}; shift 2 ;;
	--shim)      SHIM=${2:?}; shift 2 ;;
	--log)       LOG=${2:?}; shift 2 ;;
	--stdout)    TO_STDOUT=1; shift ;;
	--gdb)       GDB=1; shift ;;
	--gdb-port)  GDB_PORT=${2:?}; shift 2 ;;
	--ring)      RING=1; shift ;;
	--fb)        RING_FB=1; shift ;;
	--registers|--regs) RING_REGS=1; shift ;;
	-h|--help)   usage 0 ;;
	*)           echo "run.sh: unknown argument $1" >&2; usage 2 ;;
	esac
done

mkdir -p "$OUT"
[ -f "$SHIM" ]   || { echo "run.sh: no shim at $SHIM -- run tools/qemu/build.sh" >&2; exit 1; }
[ -f "$KERNEL" ] || { echo "run.sh: no kernel at $KERNEL" >&2; exit 1; }
command -v qemu-system-aarch64 >/dev/null || { echo "run.sh: qemu-system-aarch64 not on PATH" >&2; exit 1; }
[ "$RING" = 1 ] && ! command -v gdb-multiarch >/dev/null && \
	{ echo "run.sh: --ring needs gdb-multiarch (apt install gdb-multiarch)" >&2; exit 1; }

if [ "$TO_STDOUT" = 1 ]; then
	SERIAL="stdio"
else
	SERIAL="file:$LOG"
	: > "$LOG"
fi

QEMU_ARGS=(
	-M "virt,gic-version=$GIC"
	-cpu "$CPU"
	-m "$RAM"
	-smp "$SMP"
	-kernel "$SHIM"
	-device "loader,file=$KERNEL,addr=$MACHO_PA,force-raw=on"
	-display none
	-serial "$SERIAL"
	-no-reboot
)

# The root device: the same 16 KiB Mach-O the phone flashes as kernel.macho.rd.
# The shim advertises it in /chosen/memory-map/RAMDisk; the command line says
# rd=md0.  RAMDISK= (empty) runs without one.
if [ -n "$RAMDISK" ] && [ -f "$RAMDISK" ]; then
	QEMU_ARGS+=(-device "loader,file=$RAMDISK,addr=$RAMDISK_PA,force-raw=on")
fi

if [ "$GDB" = 1 ]; then
	QEMU_ARGS+=(-s -S)
	echo "=== qemu: waiting for a debugger on port $GDB_PORT ==="
	echo "    gdb-multiarch -x tools/qemu/gdb.txt"
	echo "    (gdb) target remote :$GDB_PORT"
	echo "    (gdb) continue"
else
	QEMU_ARGS+=(-monitor none)
fi

echo "=== qemu: shim $(basename "$SHIM"), kernel $(basename "$KERNEL") ($(stat -c %s "$KERNEL") bytes) ==="
echo "=== qemu: ${BUDGET}s budget, ${RAM} MiB RAM, smp ${SMP}, ${CPU}, GICv${GIC} ==="

if [ "$RING" = 1 ]; then
	# The dump has to happen while the guest is alive, so it cannot be a
	# foreground timeout: run QEMU behind us and stop the guest with gdb.
	QEMU_ARGS+=(-gdb "tcp::$GDB_PORT")
	qemu-system-aarch64 "${QEMU_ARGS[@]}" &
	QPID=$!
	trap 'kill -TERM "$QPID" 2>/dev/null || true' EXIT
	sleep "$BUDGET"
	RING_ARGS=(--port "$GDB_PORT" --out "$OUT" --decoder "$HERE/font8x16.py")
	[ "$RING_FB" = 1 ] && RING_ARGS+=(--fb)
	[ "$RING_REGS" = 1 ] && RING_ARGS+=(--registers)
	python3 "$HERE/ringdump.py" "${RING_ARGS[@]}"
	kill -TERM "$QPID" 2>/dev/null || true
	wait "$QPID" 2>/dev/null || true
	trap - EXIT
else
	set +e
	timeout --foreground "$BUDGET" qemu-system-aarch64 "${QEMU_ARGS[@]}"
	RC=$?
	set -e
	# 124 = the budget ran out.  For a kernel that boots and then hangs that
	# is the normal outcome, not a failure.
	[ "$RC" = 124 ] && echo "=== qemu: stopped after ${BUDGET}s (timeout) ==="
	[ "$RC" = 0 ]   && echo "=== qemu: exited by itself ==="
fi

if [ "$TO_STDOUT" = 0 ]; then
	echo "=== serial log ($LOG) ==="
	cat "$LOG" 2>/dev/null || true
	echo "=== end of serial log ==="
fi
exit 0
