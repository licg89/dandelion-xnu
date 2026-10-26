#!/usr/bin/env python3
"""Inspect an Android boot image and pull out its device tree blob.

usage: bootimg_info.py <boot.img> [--extract-dtb out.dtb]
                       [--extract-kernel out.gz] [--extract-ramdisk out.bin]
"""
import argparse
import struct
import sys

FDT_MAGIC = b"\xd0\x0d\xfe\xed"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image")
    parser.add_argument("--extract-dtb")
    parser.add_argument("--extract-kernel")
    parser.add_argument("--extract-ramdisk")
    args = parser.parse_args()

    with open(args.image, "rb") as handle:
        data = handle.read()

    if data[:8] != b"ANDROID!":
        sys.exit("not an Android boot image")

    (ksize, kaddr, rsize, raddr, ssize, saddr, tags, page, header_version,
     osver) = struct.unpack_from("<10I", data, 8)
    name = data[48:64].rstrip(b"\0").decode("ascii", "replace")
    cmdline = data[64:64 + 512].split(b"\0", 1)[0].decode("ascii", "replace")

    def up(n):
        return (n + page - 1) & ~(page - 1)

    kernel_off = page
    ramdisk_off = kernel_off + up(ksize)
    second_off = ramdisk_off + up(rsize)
    dtb_off = second_off + up(ssize)

    print("name            : %s" % name)
    print("cmdline         : %s" % cmdline)
    print("page            : %d" % page)
    print("header_version  : %d" % header_version)
    print("kernel          : %8d @ 0x%08x" % (ksize, kaddr))
    print("ramdisk         : %8d @ 0x%08x" % (rsize, raddr))
    print("second          : %8d @ 0x%08x" % (ssize, saddr))
    print("tags            :            0x%08x" % tags)
    print("file size       : %d" % len(data))
    print("sections        : kernel@%d ramdisk@%d second@%d -> dtb@%d"
          % (kernel_off, ramdisk_off, second_off, dtb_off))

    has_dtb = data[dtb_off:dtb_off + 4] == FDT_MAGIC
    print("dtb             : %s" % ("present" if has_dtb else "absent"))
    if has_dtb:
        total = struct.unpack_from(">I", data, dtb_off + 4)[0]
        print("dtb size        : %d bytes" % total)
        if args.extract_dtb:
            with open(args.extract_dtb, "wb") as handle:
                handle.write(data[dtb_off:dtb_off + total])
            print("wrote %s" % args.extract_dtb)

    if args.extract_kernel:
        with open(args.extract_kernel, "wb") as handle:
            handle.write(data[kernel_off:kernel_off + ksize])
        print("wrote %s (%d bytes)" % (args.extract_kernel, ksize))
    if args.extract_ramdisk:
        with open(args.extract_ramdisk, "wb") as handle:
            handle.write(data[ramdisk_off:ramdisk_off + rsize])
        print("wrote %s (%d bytes)" % (args.extract_ramdisk, rsize))
    return 0


if __name__ == "__main__":
    sys.exit(main())
