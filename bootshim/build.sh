#!/bin/bash
# Build the Dandelion boot shim.
#
# Usage:
#   ./build.sh                  shim with the built-in wfe stand-in kernel
#   ./build.sh platform-test    shim + platform test payload (framebuffer log)
#
# Compiler selection (CC):
#   clang                       uses --target=aarch64-none-elf and lld
#   aarch64-linux-gnu-gcc       GNU cross toolchain
# When CC is unset, clang is preferred if it is on PATH.
set -euo pipefail
cd "$(dirname "$0")"

MODE=${1:-shim}

case "$MODE" in
shim|platform-test|trap-test|xnu|xnu-qemu|fbprobe) ;;
*)
	echo "usage: $0 [shim|platform-test|trap-test|xnu|xnu-qemu|fbprobe]" >&2
	exit 2
	;;
esac

if [ -z "${CC:-}" ]; then
	if command -v clang >/dev/null 2>&1; then
		CC=clang
	else
		CC=aarch64-linux-gnu-gcc
	fi
fi

case "$CC" in
*clang*)
	TARGET="--target=aarch64-none-elf"
	LINKER="-fuse-ld=lld"
	OBJCOPY=${OBJCOPY:-llvm-objcopy}
	CXX=${CXX:-${CC%clang}clang++}
	;;
*)
	TARGET=""
	LINKER=""
	OBJCOPY=${OBJCOPY:-${CC%gcc}objcopy}
	CXX=${CXX:-${CC%gcc}g++}
	;;
esac

CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only \
        -mstrict-align -fno-stack-protector -fno-builtin -mcpu=cortex-a53 \
        -fno-asynchronous-unwind-tables -Wall -Wextra -I../pexpert/pexpert/arm64"
CXXFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-exceptions -fno-rtti \
        -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin \
        -mcpu=cortex-a53 -fno-asynchronous-unwind-tables -std=c++17 \
        -Wall -Wextra"

mkdir -p out

build_payload()
{
	main_src=$1
	entry=$2
	shift 2
	extra_objs=""

	echo "=== Building payload ($main_src, entry $entry) ==="

	# The payload must not touch ramoops: build without pstore.
	$CC $TARGET $CFLAGS -DPSTORE_ENABLED=0 -c fb.c -o out/payload_fb.o
	$CC $TARGET $CFLAGS -I../pexpert -DPAYLOAD_ENTRY=$entry \
		-c payload_start.S -o out/payload_start.o
	$CC $TARGET $CFLAGS -I../pexpert -c "$main_src" -o out/payload_main.o

	for src in "$@"; do
		obj="out/payload_$(basename "${src%.*}").o"
		$CC $TARGET $CFLAGS -I../pexpert -c "$src" -o "$obj"
		extra_objs="$extra_objs $obj"
	done

	$CXX $TARGET $CXXFLAGS -I. -I../pexpert \
		-c ../iokit/Kernel/arm/DandelionPlatformExpert.cpp \
		-o out/platform_expert.o

	echo "  Linking payload..."
	$CC $TARGET $CFLAGS $LINKER -static -T payload.ld \
		-Wl,--build-id=none -Wl,-e,_start \
		out/payload_start.o out/payload_main.o out/platform_expert.o \
		out/payload_fb.o $extra_objs \
		-o out/payload.elf

	$OBJCOPY -O binary out/payload.elf out/payload.bin

	{
		echo '	.section .test_kernel, "ax"'
		echo '	.globl test_kernel_start'
		echo 'test_kernel_start:'
		echo '	.incbin "out/payload.bin"'
		echo '	.globl test_kernel_end'
		echo 'test_kernel_end:'
	} > out/payload_blob.S
	$CC $TARGET $CFLAGS -c out/payload_blob.S \
		-o out/payload_blob.o

	echo "  payload: $(wc -c < out/payload.bin) bytes"
}

EXTRA_CFLAGS=""
EXTRA_OBJS=""
case "$MODE" in
platform-test)
	build_payload platform_test.c platform_test_main
	EXTRA_CFLAGS="-DPLATFORM_TEST_BLOB"
	EXTRA_OBJS="out/payload_blob.o"
	;;
trap-test)
	build_payload trap_test.c trap_test_main trap.S
	EXTRA_CFLAGS="-DPLATFORM_TEST_BLOB"
	EXTRA_OBJS="out/payload_blob.o"
	;;
fbprobe)
	# Framebuffer address probe: paints both candidate buffers and powers
	# the phone off.  Nothing else runs, so no payload is needed.
	EXTRA_CFLAGS="-DFB_PROBE"
	;;
xnu)
	# Real kernel: the payload travels in the boot image's ramdisk section
	# and handoff.c loads it from the device tree's initrd properties.
	EXTRA_CFLAGS="-DXNU_HANDOFF"
	;;
xnu-qemu)
	# Same handoff, but for QEMU's virt machine: console on the PL011 and an
	# ADT that describes QEMU's GICv3.  Lets the boot path be exercised
	# without the phone.
	EXTRA_CFLAGS="-DXNU_HANDOFF -DQEMU_VIRT"
	;;
esac

echo "=== Building dandelion bootshim ($MODE) with $CC ==="

for f in start.S shim.c fdt.c fb.c adt.c rng.c handoff.c gic_probe.c; do
	echo "  $f"
	$CC $TARGET $CFLAGS $EXTRA_CFLAGS -I../pexpert -c "$f" -o "out/${f%.*}.o"
done

echo "  Linking..."
$CC $TARGET $CFLAGS $LINKER -static -T shim.ld -Wl,--build-id=none -Wl,-e,_head \
	out/start.o out/shim.o out/fdt.o out/fb.o \
	out/adt.o out/rng.o out/handoff.o out/gic_probe.o \
	$EXTRA_OBJS \
	-o out/shim.elf

echo "  Creating binary..."
$OBJCOPY -O binary out/shim.elf out/Image
cp out/Image "out/Image.$MODE"

echo ""
echo "=== Build complete ==="
ls -lh out/Image out/shim.elf
echo "Image size: $(wc -c < out/Image) bytes"
