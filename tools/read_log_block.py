#!/usr/bin/env python3
"""Decode a Dandelion log-block dump into text.

The shim and the kernel write their log into the ``flashinfo`` partition with a
polled eMMC writer (bootshim/log_block.h).  This tool turns a raw dump of that
partition back into text, so a boot cycle is:

    # after a test boot, with the phone back in Android:
    adb shell su -c 'dd if=/dev/block/by-name/flashinfo bs=4096 count=132' > dump.bin
    python3 tools/read_log_block.py dump.bin

132 * 4096 bytes covers the 512 KiB slot area plus the header that follows it.

Usage:
    read_log_block.py <dump.bin> [--raw]
"""
import argparse
import struct
import sys

MAGIC = b"DLXLOG01"
SECTOR = 512
SLOT_SIZE = 64 * 1024
SLOTS = 8
HEADER_SIZE = SECTOR
TOTAL = HEADER_SIZE + SLOTS * SLOT_SIZE

# magic, version, header_bytes, boot, slot_count, slot_size, next_slot,
# text_bytes, cntpct, cntfrq, payload_pa, kernel_end_pa, shim_flags, note
HEADER_FORMAT = "<8sIIIIII I QQ Q I I 64s"


def parse_header(data):
    size = struct.calcsize(HEADER_FORMAT)
    if len(data) < size:
        return None
    values = struct.unpack_from(HEADER_FORMAT, data, 0)
    names = ("magic", "version", "header_bytes", "boot", "slot_count",
             "slot_size", "next_slot", "text_bytes", "cntpct", "cntfrq",
             "payload_pa", "kernel_end_pa", "shim_flags", "note")
    header = dict(zip(names, values))
    if header["magic"] != MAGIC:
        return None
    return header


def extract_text(data, header):
    slots_start = header["header_bytes"] or HEADER_SIZE
    slot_size = header["slot_size"] or SLOT_SIZE
    slot_count = min(header["slot_count"] or SLOTS, SLOTS)
    text_bytes = header["text_bytes"]
    next_slot = header["next_slot"]

    chunks = []
    for index in range(slot_count):
        offset = slots_start + index * slot_size
        chunk = data[offset:offset + slot_size]
        if not chunk:
            break
        chunks.append(chunk)
    blob = b"".join(chunks)

    # text_bytes is authoritative; next_slot is a slot index and is only used
    # when the byte count was never flushed (torn write).
    if 0 < text_bytes <= len(blob):
        blob = blob[:text_bytes]
    elif 0 < next_slot <= slot_count:
        blob = blob[:min(len(blob), next_slot * slot_size)]
    return blob.split(b"\0", 1)[0]


def scanned_text(data, header):
    """Recover the text without trusting ``text_bytes``.

    The kernel writer only refreshes the header every few sectors, and a kernel
    that dies stops refreshing it at all, so the last few KiB would otherwise
    be dropped.  Sectors that were never written are still uniformly 0x00
    (flashinfo is zeroed) or 0xFF, so the text ends at the first wholly blank
    sector.
    """
    start = header["header_bytes"] or HEADER_SIZE
    slot_size = header["slot_size"] or SLOT_SIZE
    slot_count = min(header["slot_count"] or SLOTS, SLOTS)
    end = min(start + slot_count * slot_size, len(data))

    chunks = []
    offset = start
    while offset < end:
        chunk = data[offset:offset + SECTOR]
        if not chunk:
            break
        if chunk == b"\x00" * len(chunk) or chunk == b"\xff" * len(chunk):
            break
        chunks.append(chunk)
        offset += SECTOR
    return b"".join(chunks).split(b"\0", 1)[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dump")
    parser.add_argument("--raw", action="store_true",
                        help="print the undecoded bytes as latin-1")
    parser.add_argument("--header", action="store_true",
                        help="print the header fields as well")
    parser.add_argument("--scan", action="store_true",
                        help="ignore text_bytes and stop at the first blank "
                             "sector (used for the kernel log, whose header is "
                             "only refreshed periodically)")
    args = parser.parse_args()

    with open(args.dump, "rb") as handle:
        data = handle.read()
    print("dump: %d bytes (expected at least %d)" % (len(data), TOTAL),
          file=sys.stderr)

    header = parse_header(data)
    if header is None:
        print("read_log_block: no DLXLOG01 header found.\n"
              "Either the writer never finished (most likely: the boot died "
              "before it flushed the header) or this is the wrong partition.",
              file=sys.stderr)
        # Still show anything printable that looks like text.
        text = data.split(b"\0", 1)[0]
        if text:
            print(text.decode("utf-8", "replace"))
        return 1

    if args.header:
        for key, value in header.items():
            if key == "magic":
                value = value.decode("ascii", "replace")
            print("%-14s %s" % (key, value))
        print()

    text = extract_text(data, header)
    if args.scan or not header["text_bytes"]:
        text = scanned_text(data, header)
    if args.raw:
        print(text.decode("latin-1", "replace"))
    else:
        print(text.decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
