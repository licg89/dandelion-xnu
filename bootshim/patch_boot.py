#!/usr/bin/env python3
"""Replace the kernel payload of an Android boot image without touching its
layout.

The Dandelion boot partition holds a normal (non-MTK-header) boot image whose
kernel section is gzip(kernel).  LK computes the ramdisk offset from
kernel_size, so instead of rebuilding the image we keep kernel_size and the
header byte-for-byte and only overwrite the kernel section with

    gzip(shim) + zero padding to 512 + device DTB

followed by zeros up to the original kernel_size.  Everything the bootloader
saw in the stock image stays where it was.

Usage:
    python patch_boot.py <stock_boot.img> <Image> <dtb> <out.img>
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
    if len(sys.argv) != 5:
        print(__doc__)
        return 2

    stock_path, image_path, dtb_path, out_path = sys.argv[1:]

    with open(stock_path, "rb") as f:
        stock = bytearray(f.read())
    with open(image_path, "rb") as f:
        shim = f.read()
    with open(dtb_path, "rb") as f:
        dtb = f.read()

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

    payload = gz(shim)
    payload += b"\0" * (-len(payload) % PAGE_ALIGN)
    payload += dtb

    print("stock: page=%d ksize=%d rsize=%d header=v%d" %
          (page, ksize, rsize, header_version))
    print("shim : %d bytes -> gzip %d" % (len(shim), len(gz(shim))))
    print("dtb  : %d bytes" % len(dtb))
    print("kernel payload: %d bytes (limit %d)" % (len(payload), ksize))

    if len(payload) > ksize:
        sys.exit("payload does not fit in the stock kernel section")

    off = page
    stock[off:off + len(payload)] = payload
    stock[off + len(payload):off + ksize] = b"\0" * (ksize - len(payload))

    # Keep the declared layout; trim the trailing partition padding.
    total = page + round_up(ksize, page) + round_up(rsize, page) + \
        round_up(ssize, page)
    if header_version >= 2:
        dtb_size = struct.unpack_from("<I", stock, 1632)[0]
        total += round_up(dtb_size, page)

    with open(out_path, "wb") as f:
        f.write(stock[:total])

    print("wrote %s (%d bytes)" % (out_path, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
