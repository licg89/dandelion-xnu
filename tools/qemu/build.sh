#!/bin/bash
# Build the QEMU boot shim (tools/qemu/shim) -- a standalone AArch64 program
# that QEMU's -kernel loads at 0x40080000 and that hands mach_kernel the boot
# environment DarwinBoot gives it on the phone.
#
# Needs an AArch64 cross compiler and nothing else:
#   aarch64-linux-gnu-gcc   (apt install gcc-aarch64-linux-gnu)
#   clang + ld.lld          (CC=clang, --target=aarch64-none-elf)
#
# Usage: tools/qemu/build.sh [--clean]
set -euo pipefail
cd "$(dirname "$0")/shim"

if [ "${1:-}" = "--clean" ]; then
	rm -rf out
	echo "cleaned"
	exit 0
fi

if [ -z "${CC:-}" ]; then
	if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
		CC=aarch64-linux-gnu-gcc
	elif command -v clang >/dev/null 2>&1; then
		CC=clang
	else
		echo "build.sh: no aarch64 cross compiler found" >&2
		exit 1
	fi
fi

case "$CC" in
*clang*)
	TARGET="--target=aarch64-none-elf"
	LINKER="-fuse-ld=lld"
	OBJDUMP=llvm-objdump
	;;
*)
	TARGET=""
	LINKER=""
	OBJDUMP=aarch64-linux-gnu-objdump
	;;
esac

CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only
        -mstrict-align -fno-stack-protector -fno-builtin -mcpu=cortex-a53
        -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-function"

mkdir -p out
echo "=== building the QEMU shim with $CC ==="
$CC $TARGET $CFLAGS -c start.S   -o out/start.o
$CC $TARGET $CFLAGS -c shim.c    -o out/shim.o
$CC $TARGET $CFLAGS -c adt_qemu.c -o out/adt_qemu.o
$CC $TARGET $CFLAGS $LINKER -static -T shim.ld -Wl,--build-id=none \
	-Wl,-e,_start out/start.o out/shim.o out/adt_qemu.o -o out/shim.elf

echo ""
$OBJDUMP -h out/shim.elf | sed -n '1,20p'
echo ""
echo "shim: $(stat -c %s out/shim.elf) bytes"
