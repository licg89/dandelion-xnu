#!/usr/bin/env python3
"""Replace the kernel and/or ramdisk of an Android boot image in place.

The Dandelion boot partition holds a normal (non-MTK-header) boot image whose
kernel section is gzip(kernel).  LK computes the ramdisk offset from
kernel_size, so instead of rebuilding the image we keep kernel_size and the
header byte-for-byte and only overwrite the kernel section with

    gzip(shim) + zero padding to 512 + device DTB

Everything the bootloader saw in the stock image stays where it was.

The XNU payload is far larger than the stock Android ramdisk, so the ramdisk
section can also be replaced.  That changes ramdisk_size in the header, which
is fine: the sections after it are laid out again and LK loads the ramdisk at
the unchanged ramdisk_addr.

Usage:
    python patch_boot.py <stock_boot.img> <Image> <dtb> <out.img> [payload]
"""
import gzip
import struct
import sys

PAGE_ALIGN = 512


def gz(data):
    # mtime=0 so the output is reproducible
    return gzip.compress(data, compresslevel=9, mtime=0)


def round_up(n, a):
    return (n + a - 1) & ~(a - 1)


def main():
    if len(sys.argv) not in (5, 6):
        print(__doc__)
        return 2

    stock_path, image_path, dtb_path, out_path = sys.argv[1:5]
    payload_path = sys.argv[5] if len(sys.argv) == 6 else None

    with open(stock_path, "rb") as f:
        stock = bytearray(f.read())
    with open(image_path, "rb") as f:
        shim = f.read()
    with open(dtb_path, "rb") as f:
        dtb = f.read()
    payload = None
    if payload_path:
        with open(payload_path, "rb") as f:
            payload = f.read()

    if stock[:8] != b"ANDROID!":
        sys.exit("stock image is not an Android boot image")
    if dtb[:4] != b"\xd0\x0d\xfe\xed":
        sys.exit("dtb does not start with the FDT magic")

    (ksize, kaddr, rsize, raddr, ssize, saddr, tags,
     page, header_version, osver) = struct.unpack_from("<10I", stock, 8)
    if header_version > 2:
        sys.exit("unsupported header version %d" % header_version)

    if kaddr == 0:
        sys.exit("stock image has kernel_addr 0")

    # Sections appended after second_size: recovery_dtbo in a v2 image, the
    # device tree blob in a v1 image.
    extra_size = struct.unpack_from("<I", stock, 1632)[0] if header_version >= 1 else 0

    kernel = gz(shim)
    kernel += b"\0" * (-len(kernel) % PAGE_ALIGN)
    kernel += dtb

    print("stock: page=%d ksize=%d rsize=%d header=v%d" %
          (page, ksize, rsize, header_version))
    print("shim : %d bytes -> gzip %d" % (len(shim), len(gz(shim))))
    print("dtb  : %d bytes" % len(dtb))
    print("kernel payload: %d bytes (limit %d)" % (len(kernel), ksize))

    if len(kernel) > ksize:
        sys.exit("payload does not fit in the stock kernel section")

    if payload is not None:
        if len(payload) % page:
            sys.exit("ramdisk payload must be a multiple of the page size")
        print("ramdisk payload: %d bytes (was %d)" % (len(payload), rsize))
        rsize = len(payload)
        struct.pack_into("<I", stock, 16, rsize)

    # Lay the image out again: header, kernel, ramdisk, second, extra.
    out = bytearray()
    out += stock[:page]
    out += kernel + b"\0" * (round_up(ksize, page) - len(kernel))
    if payload is not None:
        out += payload
    else:
        start = page + round_up(ksize, page)
        out += stock[start:start + round_up(rsize, page)]
    out += stock[page + round_up(ksize, page) + round_up(rsize, page):
                 page + round_up(ksize, page) + round_up(rsize, page) +
                 round_up(ssize, page)]
    if extra_size:
        # In a v2 image the extra section moved; copy it from the stock image
        # by its recorded offset instead of recomputing the layout.
        offset = struct.unpack_from("<Q", stock, 1636)[0]
        out += stock[offset:offset + round_up(extra_size, page)]

    with open(out_path, "wb") as f:
        f.write(out)

    print("wrote %s (%d bytes)" % (out_path, len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
