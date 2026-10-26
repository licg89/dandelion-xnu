#!/usr/bin/env python3
"""Sanity check: compare the payload inside a boot image with a mach_kernel."""
import struct
import sys


def main():
    img_path, kernel_path = sys.argv[1], sys.argv[2]
    data = open(img_path, "rb").read()
    ksize, kaddr, rsize, raddr = struct.unpack_from("<4I", data, 8)
    page = struct.unpack_from("<I", data, 36)[0]
    kernel_off = page
    ramdisk_off = kernel_off + ((ksize + page - 1) // page) * page
    payload = data[ramdisk_off:ramdisk_off + rsize]
    print("boot image: page=%d ksize=%d rsize=%d raddr=0x%x" %
          (page, ksize, rsize, raddr))
    print("payload: %d bytes at %d, magic=%r" %
          (len(payload), ramdisk_off, payload[:8]))
    if payload[:8] != b"DNDLXNU0":
        sys.exit("not a DNDLXNU0 payload")
    ver, nseg = struct.unpack_from("<II", payload, 8)
    virt, phys, mem, entry_va, kend = struct.unpack_from("<QQQQQ", payload, 16)
    segs_off, data_off, total = struct.unpack_from("<QQQ", payload, 56)
    cmdline = payload[80:80 + 608].split(b"\0", 1)[0]
    print("version=%d segs=%d virt=0x%x phys=0x%x mem=0x%x" %
          (ver, nseg, virt, phys, mem))
    print("entry_va=0x%x entry_pa=0x%x kernel_end_pa=0x%x total=0x%x" %
          (entry_va, phys + (entry_va - virt), kend, total))
    print("cmdline=%r" % cmdline)
    image = open(kernel_path, "rb").read()
    ok = True
    for i in range(nseg):
        spa, vsize, off, fsize = struct.unpack_from(
            "<QQQQ", payload, segs_off + i * 32)
        blk = payload[off:off + fsize]
        # find this block inside the mach-o (segments are copied verbatim)
        if fsize and blk not in image:
            print("  seg %d pa=0x%x size=0x%x: NOT FOUND in %s" %
                  (i, spa, fsize, kernel_path))
            ok = False
        else:
            print("  seg %d pa=0x%x vmsize=0x%x filesize=0x%x: match" %
                  (i, spa, vsize, fsize))
    print("RESULT:", "identical to %s" % kernel_path if ok else "DIFFERENT")


main()
