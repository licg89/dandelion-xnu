#!/usr/bin/env python3
"""Extract the firmware log that PayloadBootDxe writes into flash.

Why: the panel is 68 rows and a photograph is a race.  When a block device exists,
the firmware parks a whole 64 KiB copy of its own log in unused space inside the
'flashinfo' partition (see FlushLogToFlash in PayloadBootDxe.c).  This pulls it
back out of any raw image of that partition -- or of the whole disk, which is the
usual way to get it:

    # on the phone (Android, root) -- or from a dump taken any other way
    adb shell su -c 'dd if=/dev/block/by-name/flashinfo of=/sdcard/flashinfo.img'
    adb pull /sdcard/flashinfo.img

    python tools/extract_log_block.py flashinfo.img
    python tools/extract_log_block.py flashinfo.img --out last-boot.log --verbose

The block layout (all little-endian, first sector only):

    0   "DDNLOG01"
    8   u32 version
    12  u32 text size in bytes
    16  u32 total sectors in the block
    20  u32 checksum: sum = sum * 31 + byte, over the text
    24  u64 LBA the block was written at (self-referential, for sanity)
    512 text

The scan is deliberately forgiving: it looks for the magic on every 512-byte
boundary and validates before printing, so a stale block from an earlier boot is
reported rather than silently preferred.
"""
import argparse
import sys

MAGIC = b"DDNLOG01"
SECTOR = 512


def find_blocks(data):
    """Yield (offset, header dict) for every valid log block in `data`."""
    start = 0
    while True:
        index = data.find(MAGIC, start)
        if index < 0:
            return
        start = index + 1

        # The block starts at a sector boundary; a magic anywhere else is noise.
        if index % SECTOR != 0:
            continue
        if len(data) - index < SECTOR:
            return

        header = data[index:index + SECTOR]
        version, size, sectors, checksum = (
            int.from_bytes(header[8:12], "little"),
            int.from_bytes(header[12:16], "little"),
            int.from_bytes(header[16:20], "little"),
            int.from_bytes(header[20:24], "little"),
        )
        lba = int.from_bytes(header[24:32], "little")

        if not (0 < size <= 32 * 1024 * 1024):
            continue
        if not (1 <= sectors <= 4096):
            continue
        if index + sectors * SECTOR > len(data):
            continue

        text = data[index + SECTOR:index + SECTOR + size]

        total = 0
        for byte in text:
            total = (total * 31 + byte) & 0xFFFFFFFF

        yield index, {
            "version": version,
            "size": size,
            "sectors": sectors,
            "checksum": checksum,
            "checksum_ok": total == checksum,
            "lba": lba,
            "text": text,
        }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="raw image containing the log block")
    ap.add_argument("--out", help="write the decoded text here (default: stdout)")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument(
        "--lba",
        type=int,
        help="only consider the block at this sector (the firmware moved it from "
             "32512 to 30512; the old one stays in the partition, still valid, and "
             "silently wins if both are accepted)",
    )
    args = ap.parse_args()

    with open(args.image, "rb") as fh:
        data = fh.read()

    blocks = list(find_blocks(data))
    if not blocks:
        sys.exit("no log block found in %s" % args.image)

    # Later blocks win: the firmware overwrites the same span every boot.
    if args.lba is not None:
        wanted = args.lba * 512
        blocks = [b for b in blocks if b[0] == wanted]
        if not blocks:
            sys.exit("no log block at sector %d" % args.lba)

    # More than one valid block means the firmware moved the log and left the old
    # one behind, still checksum-valid.  Do not guess: neither the offset order nor
    # the size says which boot wrote which.  (The log moved 32512 -> 30512, so the
    # stale block is the one at the HIGHER offset -- pref-erring either end is wrong.)
    if len(blocks) > 1:
        print("found %d valid log blocks:" % len(blocks))
        for index, (where, info) in enumerate(blocks):
            print("  sector %-6d size %-6d lba %d" % (where // 512, info["size"], info["lba"]))
        sys.exit("pass --lba <sector> to choose one; the later block is not reliably the newest")

    offset, block = blocks[-1]

    if args.verbose:
        print(
            "found %d block(s); using offset 0x%x  version=%d size=%d sectors=%d lba=%d checksum=%s"
            % (
                len(blocks),
                offset,
                block["version"],
                block["size"],
                block["sectors"],
                block["lba"],
                "ok" if block["checksum_ok"] else "MISMATCH",
            ),
            file=sys.stderr,
        )

    text = block["text"].rstrip(b"\x00")
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(text)
        print("wrote %d bytes to %s" % (len(text), args.out))
    else:
        sys.stdout.write(text.decode("utf-8", "replace"))
        if not text.endswith(b"\n"):
            sys.stdout.write("\n")

    if not block["checksum_ok"]:
        print("warning: checksum mismatch, the block may be truncated", file=sys.stderr)


if __name__ == "__main__":
    main()
