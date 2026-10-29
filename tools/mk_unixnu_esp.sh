#!/usr/bin/env bash
# Build the 1 MiB FAT volume UniXNU boots from, and print how to install it.
#
# Why a FAT volume at all: every partition on this phone is ext4/f2fs, so EDK2
# has nothing to mount and the honest answer on the panel is "FAT NONE".  UniXNU
# needs a volume anyway -- it looks for \EFI\UniXNU\UniXNU.toml on one, and with
# no volume it fell back to mBuiltinConfig and printed "no boot entry found".
#
# It is written to the *start* of the 'flashinfo' partition (16 MiB, unused by
# Android).  The layout inside that partition is now:
#
#     0         .. 15872 KiB  this FAT volume
#     15 MiB .. 15 MiB+  the firmware's own log block (128 sectors)
#
# which is why the volume is 14336 KiB and LOG_PARTITION_OFFSET in
# PayloadBootDxe.c is 30720.  FAT drivers size a volume from its BPB, not from
# the partition, so a filesystem that stops short of the log is fine.
#
# It used to be 1 MiB with the log at +1 MiB, which was enough for a boot manager
# and not enough for a kernel.
#
# DESTRUCTIVE: this overwrites the vendor's "DOWNLOAD INFORMATION!!" record in
# the first 64 KiB of flashinfo.  Take the backup first:
#     adb shell su -c "dd if=/dev/block/by-name/flashinfo of=/sdcard/fi.img bs=4096"
# and keep the first MiB somewhere.
#
# Usage (inside WSL):
#   bash tools/mk_unixnu_esp.sh                 # -> out/unixnu-esp.img
#   bash tools/mk_unixnu_esp.sh --print         # only print the install commands
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${ESP_OUT:-$REPO/out/unixnu-esp.img}
SRC=$REPO/Resources/UniXNU
SIZE_KIB=${ESP_SIZE_KIB:-16256}
LABEL=${ESP_LABEL:-UNIXNU}

print_steps() {
  cat <<EOF
install it with (root adb, and it overwrites flashinfo's first 1 MiB):

  adb push "$OUT" /sdcard/unixnu-esp.img
  adb shell su -c "dd if=/sdcard/unixnu-esp.img of=/dev/block/by-name/flashinfo bs=512 seek=0 conv=notrunc"

back it up first:

  adb shell su -c "dd if=/dev/block/by-name/flashinfo of=/sdcard/fi.img bs=4096"
  adb pull /sdcard/fi.img
EOF
}

if [ "${1:-}" = "--print" ]; then
  print_steps
  exit 0
fi

for tool in mkfs.vfat mmd mcopy; do
  command -v "$tool" >/dev/null || { echo "error: $tool missing (apt-get install dosfstools mtools)" >&2; exit 1; }
done

mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
truncate -s "${SIZE_KIB}K" "$OUT"
# No -F: 14 MiB with 4 KiB clusters needs FAT16, and mkfs picks it.  Forcing
# FAT12 here was fine at 1 MiB and is not fine now.
mkfs.vfat -n "$LABEL" "$OUT" >/dev/null

mmd -i "$OUT" ::/EFI
mmd -i "$OUT" ::/EFI/UniXNU

#
# \UniXNU.efi at the root, because PayloadBootDxe's first candidate name is
# \UniXNU.efi -- that is what makes the firmware load the boot manager off this
# volume instead of out of the flash payload slot, which no longer exists now
# that the volume starts at sector 0.
#
if [ -f "$SRC/UniXNU.efi" ]; then
  mcopy -i "$OUT" "$SRC/UniXNU.efi" "::/UniXNU.efi"
fi

# What UniXNU reads at runtime: the config, the loader it starts, and the drivers
# it can load through its own [driver] list.  kernel.macho joins them once the
# macOS build produces one.
for f in UniXNU.toml DarwinBoot.efi UniXNUDxe.efi UniXNURamDiskDxe.efi mach_kernel; do
  if [ -f "$SRC/$f" ]; then
    if [ "$f" = "mach_kernel" ]; then dst="::/EFI/UniXNU/kernel.macho"; else dst="::/EFI/UniXNU/$f"; fi
    mcopy -i "$OUT" "$SRC/$f" "$dst"
  elif [ "$f" = "mach_kernel" ]; then
    echo "note: no $SRC/mach_kernel yet -- run tools/ci/fetch_kernel.ps1"
  else
    echo "warning: $SRC/$f is missing, leaving it out"
  fi
done

echo "== $OUT"
mdir -i "$OUT" ::/
mdir -i "$OUT" ::/EFI/UniXNU
print_steps
