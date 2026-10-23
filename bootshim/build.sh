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
shim|platform-test) ;;
*)
	echo "usage: $0 [shim|platform-test]" >&2
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
        -fno-asynchronous-unwind-tables -Wall -Wextra"
CXXFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -fno-exceptions -fno-rtti \
        -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin \
        -mcpu=cortex-a53 -fno-asynchronous-unwind-tables -std=c++17 \
        -Wall -Wextra"

mkdir -p out

build_payload()
{
	echo "=== Building platform test payload ==="

	# The payload must not touch ramoops: build without pstore.
	$CC $TARGET $CFLAGS -DPSTORE_ENABLED=0 -c fb.c -o out/payload_fb.o
	$CC $TARGET $CFLAGS -I../pexpert -c platform_test.c -o out/platform_test.o
	$CC $TARGET $CFLAGS -c payload_start.S -o out/payload_start.o
	$CXX $TARGET $CXXFLAGS -I. -I../pexpert \
		-c ../iokit/Kernel/arm/DandelionPlatformExpert.cpp \
		-o out/platform_expert.o

	echo "  Linking payload..."
	$CC $TARGET $CFLAGS $LINKER -static -T payload.ld \
		-Wl,--build-id=none -Wl,-e,_start \
		out/payload_start.o out/platform_test.o out/platform_expert.o \
		out/payload_fb.o \
		-o out/platform_test.elf

	$OBJCOPY -O binary out/platform_test.elf out/platform_test.bin

	{
		echo '	.section .test_kernel, "ax"'
		echo '	.globl test_kernel_start'
		echo 'test_kernel_start:'
		echo '	.incbin "out/platform_test.bin"'
		echo '	.globl test_kernel_end'
		echo 'test_kernel_end:'
	} > out/platform_test_blob.S
	$CC $TARGET $CFLAGS -c out/platform_test_blob.S \
		-o out/platform_test_blob.o

	echo "  payload: $(wc -c < out/platform_test.bin) bytes"
}

EXTRA_CFLAGS=""
EXTRA_OBJS=""
if [ "$MODE" = "platform-test" ]; then
	build_payload
	EXTRA_CFLAGS="-DPLATFORM_TEST_BLOB"
	EXTRA_OBJS="out/platform_test_blob.o"
fi

echo "=== Building dandelion bootshim ($MODE) with $CC ==="

for f in start.S shim.c fdt.c fb.c adt.c rng.c handoff.c; do
	echo "  $f"
	$CC $TARGET $CFLAGS $EXTRA_CFLAGS -I../pexpert -c "$f" -o "out/${f%.*}.o"
done

echo "  Linking..."
$CC $TARGET $CFLAGS $LINKER -static -T shim.ld -Wl,--build-id=none -Wl,-e,_head \
	out/start.o out/shim.o out/fdt.o out/fb.o \
	out/adt.o out/rng.o out/handoff.o \
	$EXTRA_OBJS \
	-o out/shim.elf

echo "  Creating binary..."
$OBJCOPY -O binary out/shim.elf out/Image
cp out/Image "out/Image.$MODE"

echo ""
echo "=== Build complete ==="
ls -lh out/Image out/shim.elf
echo "Image size: $(wc -c < out/Image) bytes"
