#!/usr/bin/env python3
"""Pack a plain Android boot image (v0 header) without a stock template.

patch_boot.py keeps the stock header, which needs the original boot.img on the
host.  That file lives outside this repository (out/ is not committed), so this
tool builds the same layout from scratch:

    [header page][gzip(kernel)][ramdisk][second][dtb]

The section order and the load addresses are the ones the Dandelion boot
partition uses (see patch_boot.py and the README):

    base            0x40000000
    kernel_offset   0x00080000   -> kernel_addr   0x40080000
    ramdisk_offset  0x11b00000   -> ramdisk_addr  0x51b00000
    tags_offset     0x07880000   -> tags_addr     0x47880000
    page size       2048

Usage:
    mkbootimg_v0.py --kernel Image [--ramdisk payload.bin] [--dtb x.dtb]
                    --output boot.img [--cmdline "..."]
"""
import argparse
import gzip
import hashlib
import os
import struct
import sys

MAGIC = b"ANDROID!"
HEADER_SIZE = 1632
NAME = b"Dandelion"
DEFAULT_CMDLINE = b"bootopt=64S3,32N2,64N2"
BASE = 0x40000000
KERNEL_OFFSET = 0x00080000
RAMDISK_OFFSET = 0x11B00000
TAGS_OFFSET = 0x07880000
PAGE = 2048


def pad(data, align):
    return data + b"\0" * (-len(data) % align)


def build(kernel, ramdisk, second, dtb, cmdline, page, base, kernel_offset,
          ramdisk_offset, tags_offset, name):
    kernel_addr = base + kernel_offset
    ramdisk_addr = base + ramdisk_offset
    second_addr = base + kernel_offset + 0x01000000 if second else 0
    tags_addr = base + tags_offset

    header = bytearray(HEADER_SIZE)
    header[0:8] = MAGIC
    (header[8:12]) = struct.pack("<I", len(kernel))
    struct.pack_into("<I", header, 12, kernel_addr)
    struct.pack_into("<I", header, 16, len(ramdisk))
    struct.pack_into("<I", header, 20, ramdisk_addr)
    struct.pack_into("<I", header, 24, len(second))
    struct.pack_into("<I", header, 28, second_addr)
    struct.pack_into("<I", header, 32, tags_addr)
    struct.pack_into("<I", header, 36, page)
    struct.pack_into("<I", header, 40, 0)             # header_version
    struct.pack_into("<I", header, 44, 0)             # os_version
    header[48:48 + len(name)] = name
    cmdline = cmdline[:511]
    header[64:64 + len(cmdline)] = cmdline
    # The id field is the SHA-1 over every section, as mkbootimg computes it.
    digest = hashlib.sha1()
    for section in (kernel, ramdisk, second):
        digest.update(section)
    header[576:576 + 20] = digest.digest()

    out = bytearray()
    out += pad(bytes(header), page)
    out += pad(kernel, page)
    out += pad(ramdisk, page)
    out += pad(second, page)
    out += pad(dtb, page)
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--ramdisk", default=None)
    parser.add_argument("--second", default=None)
    parser.add_argument("--dtb", default=None)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cmdline", default=DEFAULT_CMDLINE.decode())
    parser.add_argument("--page-size", type=int, default=PAGE)
    parser.add_argument("--base", type=lambda v: int(v, 0), default=BASE)
    parser.add_argument("--kernel-offset", type=lambda v: int(v, 0),
                        default=KERNEL_OFFSET)
    parser.add_argument("--ramdisk-offset", type=lambda v: int(v, 0),
                        default=RAMDISK_OFFSET)
    parser.add_argument("--tags-offset", type=lambda v: int(v, 0),
                        default=TAGS_OFFSET)
    parser.add_argument("--name", default=NAME.decode())
    parser.add_argument("--gzip-kernel", action="store_true",
                        help="gzip the kernel section (LK expects gzip(shim))")
    parser.add_argument("--pad-kernel", type=int, default=0,
                        help="pad the kernel section to this multiple (512)")
    args = parser.parse_args()

    def read(path):
        if not path:
            return b""
        with open(path, "rb") as handle:
            return handle.read()

    kernel = read(args.kernel)
    ramdisk = read(args.ramdisk)
    second = read(args.second)
    dtb = read(args.dtb)

    if not kernel:
        sys.exit("mkbootimg_v0: empty kernel")
    if args.gzip_kernel:
        # Same shape as patch_boot.py: gzip(shim) then zero padding.  LK
        # decompresses this section, so a raw binary would not boot.
        kernel = gzip.compress(kernel, compresslevel=9, mtime=0)
    if args.pad_kernel:
        kernel += b"\0" * (-len(kernel) % args.pad_kernel)
    if dtb and dtb[:4] != b"\xd0\x0d\xfe\xed":
        sys.exit("mkbootimg_v0: dtb does not start with the FDT magic")

    image = build(kernel, ramdisk, second, dtb, args.cmdline.encode(),
                  args.page_size, args.base, args.kernel_offset,
                  args.ramdisk_offset, args.tags_offset, args.name.encode())
    with open(args.output, "wb") as handle:
        handle.write(image)

    print("wrote %s (%d bytes)" % (args.output, len(image)))
    print("  kernel  %8d -> 0x%08x" % (len(kernel), args.base + args.kernel_offset))
    print("  ramdisk %8d -> 0x%08x" % (len(ramdisk), args.base + args.ramdisk_offset))
    print("  dtb     %8d -> after ramdisk" % len(dtb))
    return 0


if __name__ == "__main__":
    sys.exit(main())
