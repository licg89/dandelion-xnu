#!/bin/bash
set -euo pipefail
cd /mnt/d/aios/dandelion-shim/v1

CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin -mcpu=cortex-a53 -Wall"

mkdir -p out

echo "=== Building TRNG probe ==="
$CC $CFLAGS -c start.S -o out/trng_start.o
$CC $CFLAGS -c trng_probe.c -o out/trng_trng_probe.o
$CC $CFLAGS -c fb.c -o out/trng_fb.o

echo "  Linking..."
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,-N \
  out/trng_start.o out/trng_trng_probe.o out/trng_fb.o -o out/trng_probe.elf

echo "  Binary..."
$OBJCOPY -O binary out/trng_probe.elf out/trng_probe_img

echo "  Boot.img..."
cd out
gzip -n -9 -c trng_probe_img > trng_probe.gz
python3 -c "
import os
size = os.path.getsize('trng_probe.gz')
padded = (size + 511) & ~511
d = open('trng_probe.gz', 'rb').read()
f = open('trng_probe.padded', 'wb')
f.write(d)
f.write(b'\x00' * (padded - size))
f.close()
"
cat trng_probe.padded /mnt/d/aios/dandelion-shim/out/dtb.dtb > trng_kernel.bin
mkbootimg --kernel trng_kernel.bin --ramdisk /mnt/d/aios/dandelion-shim/out/ramdisk_empty.bin \
  --header_version 0 --base 0x40000000 --kernel_offset 0x00080000 \
  --ramdisk_offset 0x11B00000 --tags_offset 0x07880000 \
  --pagesize 2048 --cmdline "bootopt=64S3,32N2,64N2" \
  -o trng-probe.img

ls -lh trng-probe.img
echo "READY"
