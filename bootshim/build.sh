#!/bin/bash
# Build dandelion boot shim v1
set -euo pipefail
cd "$(dirname "$0")"

CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only \
        -mstrict-align -fno-stack-protector -fno-builtin -mcpu=cortex-a53 -Wall -Wextra"

mkdir -p out

echo "=== Building dandelion bootshim v1 ==="

for f in start.S shim.c fdt.c fb.c adt.c rng.c handoff.c; do
	echo "  $f"
	$CC $CFLAGS -c $f -o out/${f%.*}.o 2>&1
done

echo "  Linking..."
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,-N \
    out/start.o out/shim.o out/fdt.o out/fb.o \
    out/adt.o out/rng.o out/handoff.o \
    -o out/shim.elf

echo "  Creating binary..."
$OBJCOPY -O binary out/shim.elf out/Image

echo ""
echo "=== Build complete ==="
ls -lh out/Image out/shim.elf
echo "Image size: $(wc -c < out/Image) bytes"
