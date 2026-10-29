#!/usr/bin/env bash
# Build the 1 MiB FAT volume UniXNU boots from, and print how to install it.
#
# Why a FAT volume at all: every partition on this phone is ext4/f2fs, so EDK2
# has nothing to mount and the honest answer on the panel is "FAT NONE".  UniXNU
# needs a volume anyway -- it looks for \EFI\UniXNU\UniXNU.toml on one, and with
# no volume it fell back to mBuiltinConfig and printed "no boot entry found".
#
# The volume is deliberately only 1 MiB: it is written to the *start* of the
# 'flashinfo' partition (16 MiB, unused by Android), and the firmware's own log
# lives at +1 MiB inside the same partition.  Keeping the filesystem short stops
# the two from meeting.  FAT drivers size a volume from its BPB, not from the
# partition, so a small filesystem inside a big partition mounts fine.
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
SIZE_KIB=${ESP_SIZE_KIB:-1024}
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
mkfs.vfat -F 12 -n "$LABEL" "$OUT" >/dev/null

mmd -i "$OUT" ::/EFI
mmd -i "$OUT" ::/EFI/UniXNU

# Only what UniXNU reads at runtime: the config, the loader it starts, and the
# two drivers UniXNU can load through its own [driver] list later.
for f in UniXNU.toml DarwinBoot.efi UniXNUDxe.efi UniXNURamDiskDxe.efi; do
  if [ -f "$SRC/$f" ]; then
    mcopy -i "$OUT" "$SRC/$f" "::/EFI/UniXNU/$f"
  else
    echo "warning: $SRC/$f is missing, leaving it out"
  fi
done

echo "== $OUT"
mdir -i "$OUT" ::/EFI/UniXNU
print_steps
