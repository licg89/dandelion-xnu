#!/usr/bin/env python3
"""Wrap a UEFI image so the firmware can load it from the 'flashinfo' partition.

Why this exists: PayloadBootDxe can boot a payload out of the firmware volume, but
the FV has ~1.4 MiB of room left and the payload this project waits for --
UniXNU.efi -- will not fit.  The phone's own partitions are ext4/f2fs, which EDK2
cannot read (the honest `FAT NONE` in the boot log).  The one partition Android
does not use is `flashinfo`, 16 MiB, and the firmware already parks its log in it
at +1 MiB.  The payload goes at +2 MiB.

Layout written here (all little-endian, first sector only):

    0   "DDNPAY01"
    8   u32 image size in bytes
    12  u32 checksum: sum = sum * 31 + byte, over the image
    16  zero padding to 512
    512 the image, zero padded to a whole number of 512-byte sectors

The header is not decoration.  EFI_BOOT_SERVICES.LoadImage needs the exact image
size and a PE file does not carry one that can be trusted for that, so the sender
states it; the checksum then proves the bytes survived adb and dd.

    # 1) wrap
    python tools/put_flash_payload.py UniXNU.efi -o out/payload.bin

    # 2) push it into the partition from Android (root)
    adb push out/payload.bin /sdcard/payload.bin
    adb shell su -c "dd if=/sdcard/payload.bin of=/dev/block/by-name/flashinfo bs=512 seek=4096 conv=notrunc"

    # 3) confirm afterwards, from a dump of the partition
    adb shell su -c "dd if=/dev/block/by-name/flashinfo of=/sdcard/fi.img bs=4096"
    adb pull /sdcard/fi.img out/fi.img
    python tools/put_flash_payload.py --check out/fi.img -o out/payload-from-phone.bin

`seek=4096` is +2 MiB in 512-byte sectors and must match
PAYLOAD_FROM_FLASH_OFFSET in PayloadBootDxe.c.  `conv=notrunc` matters: without
it dd would truncate the partition.
"""
import argparse
import os
import struct
import sys

MAGIC = b"DDNPAY01"
HEADER_BYTES = 512
OFFSET_SECTORS = 4096          # +2 MiB, matches PAYLOAD_FROM_FLASH_OFFSET
MAX_BYTES = 12 * 1024 * 1024   # matches PAYLOAD_FROM_FLASH_MAX


def checksum(data):
    total = 0
    for byte in data:
        total = (total * 31 + byte) & 0xFFFFFFFF
    return total


def build(image):
    if len(image) > MAX_BYTES:
        sys.exit("image is %d bytes, the firmware will refuse anything over %d"
                 % (len(image), MAX_BYTES))
    header = bytearray(HEADER_BYTES)
    header[0:8] = MAGIC
    struct.pack_into("<I", header, 8, len(image))
    struct.pack_into("<I", header, 12, checksum(image))
    padded = (len(image) + 511) // 512 * 512
    return bytes(header) + image + b"\0" * (padded - len(image))


def check(dump):
    """Pull the payload back out of a raw dump of the partition."""
    base = OFFSET_SECTORS * 512
    if len(dump) < base + HEADER_BYTES:
        sys.exit("dump is %d bytes, too short to contain the payload" % len(dump))
    header = dump[base:base + HEADER_BYTES]
    if header[0:8] != MAGIC:
        sys.exit("no payload at +2 MiB: found %r" % bytes(header[0:8]))
    size, want = struct.unpack_from("<II", header, 8)
    if size == 0 or size > MAX_BYTES:
        sys.exit("header says %d bytes, which the firmware would reject" % size)
    image = dump[base + HEADER_BYTES:base + HEADER_BYTES + size]
    if len(image) != size:
        sys.exit("dump ends inside the image (%d of %d bytes)" % (len(image), size))
    got = checksum(image)
    if got != want:
        sys.exit("checksum mismatch: header 0x%08x, data 0x%08x" % (want, got))
    return image


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="UEFI image to wrap (a .efi file)")
    ap.add_argument("-o", "--out", help="output file")
    ap.add_argument("--check", metavar="DUMP",
                    help="verify and extract from a raw dump instead of wrapping")
    args = ap.parse_args()

    if args.check:
        if not args.out:
            sys.exit("--check needs -o to say where to write the extracted image")
        with open(args.check, "rb") as fh:
            dump = fh.read()
        image = check(dump)
        with open(args.out, "wb") as fh:
            fh.write(image)
        print("ok     : %d bytes, checksum verified, from +2 MiB of %s"
              % (len(image), args.check))
        return 0

    if not args.image:
        sys.exit("give an image to wrap, or use --check on a dump")
    with open(args.image, "rb") as fh:
        image = fh.read()

    blob = build(image)
    out = args.out or os.path.splitext(args.image)[0] + ".flashpayload.bin"
    with open(out, "wb") as fh:
        fh.write(blob)

    print("image  : %d bytes  checksum 0x%08x" % (len(image), checksum(image)))
    print("wrote  : %s (%d bytes = %d sectors)" % (out, len(blob), len(blob) // 512))
    print("")
    print("push it with:")
    print('  adb push "%s" /sdcard/payload.bin' % out)
    print('  adb shell su -c "dd if=/sdcard/payload.bin '
          'of=/dev/block/by-name/flashinfo bs=512 seek=%d conv=notrunc"' % OFFSET_SECTORS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
