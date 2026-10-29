#!/usr/bin/env python3
"""Extract the log XNU writes into flashinfo through its eMMC console.

Why this exists: dandelion has no UART, so the kernel's kprintf has nowhere to go
unless the firmware gives it somewhere.  patches/xnu/native-emmc-serial.patch does
exactly that -- a pe_serial_functions vtable whose transmit_data() buffers a
character and writes a 512-byte sector per completed line -- and it picks its
target out of the ADT node "emmc-log" (log-lba / log-sectors / clock-div).

Layout inside flashinfo, agreed between bootshim/adt.c, tools/mk_unixnu_esp.sh and
PayloadBootDxe.c:

    0      .. 30000   FAT volume
    30000  .. 30512   XNU's log channel, 512 sectors = 256 KiB
    30512  .. 30640   the firmware's own log block

The kernel writes its own header (EMMC_LOG_MAGIC "DLXLOG01", written last so a
torn header is detectable), then the text:

    0x00  "DLXLOG01"
    0x08  u32 version
    0x0C  u32 header bytes
    0x10  u32 boot
    0x14  u32 slot count
    0x18  u32 slot size
    0x1C  u32 next slot
    0x20  u32 text bytes
    0x24  u64 CNTPCT
    0x2C  u64 CNTFRQ

    adb shell su -c "dd if=/dev/block/by-name/flashinfo of=/sdcard/fi.img bs=4096"
    adb pull /sdcard/fi.img out/fi.img
    python tools/extract_xnu_log.py out/fi.img
    python tools/extract_xnu_log.py out/fi.img --lba 30000 --out out/xnu.log
"""
import argparse
import struct
import sys

MAGIC = b"DLXLOG01"
SECTOR = 512
DEFAULT_LBA_IN_PARTITION = 30000


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="raw dump of the flashinfo partition")
    ap.add_argument("--lba", type=int, default=DEFAULT_LBA_IN_PARTITION,
                    help="log channel start, in sectors from the partition start "
                         "(default: %(default)s)")
    ap.add_argument("--out", help="write the text here as well")
    ap.add_argument("--raw", action="store_true",
                    help="print every sector with non-zero bytes when there is no header")
    args = ap.parse_args()

    with open(args.dump, "rb") as fh:
        data = fh.read()

    base = args.lba * SECTOR
    if len(data) < base + SECTOR:
        sys.exit("dump is %d bytes, shorter than the channel at %d" % (len(data), base))

    header = data[base:base + SECTOR]
    if header[0:8] != MAGIC:
        print("no DLXLOG01 at sector %d: found %r" % (args.lba, bytes(header[0:8])))
        if not args.raw:
            print("(the kernel did not initialise the channel; pass --raw to see what is there)")
            return 1
        shown = 0
        for index in range(args.lba, args.lba + 512):
            chunk = data[index * SECTOR:(index + 1) * SECTOR]
            if chunk.strip(b"\x00") and chunk.strip(b"\xff"):
                print("sector %d: %r" % (index, chunk[:64]))
                shown += 1
                if shown > 12:
                    break
        return 1

    version, hdr_bytes, boot, slots, slot_size, next_slot, text_bytes = \
        struct.unpack_from("<7I", header, 8)
    cntpct, cntfrq = struct.unpack_from("<2Q", header, 0x24)

    print("magic        %s" % MAGIC.decode())
    print("version      %d" % version)
    print("header bytes %d" % hdr_bytes)
    print("boot         %d" % boot)
    print("slots        %d x %d bytes, next %d" % (slots, slot_size, next_slot))
    print("text bytes   %d" % text_bytes)
    if cntfrq:
        print("uptime       %.3f s (CNTPCT %d / CNTFRQ %d)"
              % (cntpct / cntfrq, cntpct, cntfrq))
    print("")

    # The text follows the header.  A ring, so it may wrap; read what is there and
    # let the reader see the order rather than guess at slot bookkeeping.
    span = min(args.lba + 512, len(data) // SECTOR)
    text = bytearray()
    for index in range(args.lba + 1, span):
        chunk = data[index * SECTOR:(index + 1) * SECTOR]
        if chunk.strip(b"\x00") == b"":
            continue
        text += chunk
    if text_bytes and text_bytes <= len(text):
        text = text[:text_bytes]

    printable = bytes(c if (32 <= c < 127 or c in (9, 10, 13)) else 46 for c in text)
    decoded = printable.decode("ascii", errors="replace").rstrip("\x00").rstrip()
    if decoded:
        print(decoded)
    else:
        print("(header is valid but the text is empty -- the kernel never printed)")

    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(decoded.encode("ascii", errors="replace"))
        print("\nwrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
