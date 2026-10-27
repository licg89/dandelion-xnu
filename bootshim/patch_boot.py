#!/usr/bin/env python3
"""Replace the kernel and/or ramdisk of an Android boot image in place.

The Dandelion boot partition holds a normal (non-MTK-header) Android boot
image v2 whose kernel section is gzip(kernel) and whose device tree blob sits
*after* the ramdisk:

    [header][gzip(kernel)][ramdisk][dtb]

LK derives the ramdisk offset from kernel_size and finds the DTB right behind
the ramdisk, so we keep kernel_size and the header byte-for-byte and rebuild
the sections in the same order:

    [header][gzip(shim) + zero padding][ramdisk + zero padding][dtb]

The XNU payload is far larger than the stock Android ramdisk, so the ramdisk
section is replaced too.  That changes ramdisk_size in the header, which is
fine: everything after it is laid out again and LK still loads the ramdisk at
the unchanged ramdisk_addr.

Usage:
    python patch_boot.py <stock_boot.img> <Image> <dtb> <out.img> [payload]
                         [--kernel-addr 0x48880000]

`--kernel-addr` rewrites kernel_addr in the header, which is where LK places
(and then jumps to) the decompressed kernel section.  Use it when the payload
must land at a specific address: the UEFI FD has to be at [uefi_fd].base
(0x48880000 for dandelion, matching MemoryMapLib's "UEFI FD" entry), while the
vendor header says 0x40080000.  Everything else in the header - raddr, tags,
page size, kernel_size - stays exactly as the stock image had it, because that
layout is the one already proven to boot on this device.
"""
import gzip
import struct
import sys


def parse_kernel_addr(argv):
    """Strip our own --kernel-addr option out of argv, return (addr, rest)."""
    kernel_addr = None
    rest = []
    i = 0
    while i < len(argv):
        arg = argv[i]
        if arg == "--kernel-addr":
            if i + 1 >= len(argv):
                sys.exit("--kernel-addr needs a value")
            kernel_addr = int(argv[i + 1], 0)
            i += 2
            continue
        if arg.startswith("--kernel-addr="):
            kernel_addr = int(arg.split("=", 1)[1], 0)
            i += 1
            continue
        rest.append(arg)
        i += 1
    return kernel_addr, rest

PAGE_ALIGN = 512


def gz(data):
    # mtime=0 so the output is reproducible
    return gzip.compress(data, compresslevel=9, mtime=0)


def round_up(n, a):
    return (n + a - 1) & ~(a - 1)


def main():
    kernel_addr, args = parse_kernel_addr(sys.argv[1:])

    if len(args) not in (4, 5):
        print(__doc__)
        return 2

    stock_path, image_path, dtb_path, out_path = args[:4]
    payload_path = args[4] if len(args) == 5 else None

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

    kernel_off = page
    ramdisk_off = kernel_off + round_up(ksize, page)
    second_off = ramdisk_off + round_up(rsize, page)
    dtb_off = second_off + round_up(ssize, page)

    print("stock: page=%d ksize=%d rsize=%d header=v%d" %
          (page, ksize, rsize, header_version))
    print("  kernel@%d ramdisk@%d second@%d dtb@%d (%s)" %
          (kernel_off, ramdisk_off, second_off, dtb_off,
           "present" if stock[dtb_off:dtb_off + 4] == b"\xd0\x0d\xfe\xed" else "absent"))

    if kernel_addr is not None:
        print("kaddr: 0x%08X -> 0x%08X (payload will be decompressed here)" %
              (kaddr, kernel_addr))
        kaddr = kernel_addr
        # kernel_addr is the 2nd UINT32 of the header, i.e. offset 8 + 4.
        struct.pack_into("<I", stock, 12, kaddr)
    else:
        print("kaddr: 0x%08X (unchanged)" % kaddr)

    kernel = gz(shim)
    kernel += b"\0" * (-len(kernel) % PAGE_ALIGN)

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

    # Lay the image out again: header, kernel, ramdisk, second, dtb.  The DTB
    # keeps the place it has in the stock image, i.e. behind the ramdisk.
    out = bytearray()
    out += stock[:page]
    out += kernel + b"\0" * (round_up(ksize, page) - len(kernel))
    if payload is not None:
        out += payload + b"\0" * (-len(payload) % page)
    else:
        out += stock[ramdisk_off:ramdisk_off + round_up(rsize, page)]
    out += stock[second_off:second_off + round_up(ssize, page)]
    out += dtb + b"\0" * (-len(dtb) % page)

    with open(out_path, "wb") as f:
        f.write(out)

    print("wrote %s (%d bytes)" % (out_path, len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
