#!/usr/bin/env python3
"""Build dandelion boot.img using MTK format (like mouseos).
Uses original boot_dump.img as template, replaces kernel and ramdisk.
"""
import struct
import subprocess
import zlib
import sys
import os

MTK_MAGIC = 0x58881688
MTK_HEADER_SIZE = 512

def gzip_data(data):
    return subprocess.run(['gzip', '-n', '-9', '-c'], input=data,
                         stdout=subprocess.PIPE, check=True).stdout

def pad(data, alignment):
    return data + bytes(-len(data) % alignment)

def mtk_section(name, payload):
    """Create MTK section: 512-byte header + payload"""
    header = struct.pack('<II32s', MTK_MAGIC, len(payload), name)
    header += b'\xff' * (MTK_HEADER_SIZE - len(header))
    return header + payload

def main():
    boot_dump = sys.argv[1]  # original boot.img
    shim_bin = sys.argv[2]   # raw shim binary
    dtb_file = sys.argv[3]   # DTB file
    payload_file = sys.argv[4] if len(sys.argv) > 4 else None  # optional payload
    out_file = sys.argv[5] if len(sys.argv) > 5 else 'out.img'

    # Read original boot.img
    with open(boot_dump, 'rb') as f:
        original = f.read()

    # Parse Android boot header
    if original[:8] != b'ANDROID!':
        print("ERROR: Not an Android boot image")
        sys.exit(1)

    fields = struct.unpack_from('<10I', original, 8)
    ksize, kaddr, rsize, raddr, ssize, saddr, tags, page, version, osver = fields
    cmdline = original[64:576].split(b'\x00', 1)[0]

    print(f"Original: kernel={ksize}, ramdisk={rsize}, page={page}, v{version}")
    print(f"  kaddr=0x{kaddr:08x} raddr=0x{raddr:08x} tags=0x{tags:08x}")
    print(f"  cmdline: {cmdline.decode('ascii', errors='replace')}")

    # Parse original kernel section (skip 512-byte MTK header)
    page1 = original[page:page+ksize]
    old_kernel_mtk_hdr = page1[:MTK_HEADER_SIZE]

    # We use our own DTB, no need to extract from original
    print(f"Original kernel section: {ksize} bytes (using our own DTB)")

    # Parse original ramdisk section
    roff = page + ((ksize + page - 1) // page) * page
    page2 = original[roff:roff+rsize]
    old_ramdisk_mtk_hdr = page2[:MTK_HEADER_SIZE]
    old_ramdisk_payload = page2[MTK_HEADER_SIZE:]

    # Read our files
    with open(shim_bin, 'rb') as f:
        shim = f.read()
    with open(dtb_file, 'rb') as f:
        dtb = f.read()

    print(f"Shim: {len(shim)} bytes")
    print(f"DTB: {len(dtb)} bytes")

    # Build new kernel section: gzip(shim) + pad512 + DTB
    gz_shim = gzip_data(shim)
    gz_padded = pad(gz_shim, 512)
    new_kernel_payload = gz_padded + dtb
    new_kernel = mtk_section(b'KERNEL', new_kernel_payload)

    # Build new ramdisk section
    if payload_file:
        with open(payload_file, 'rb') as f:
            payload = f.read()
        print(f"Payload: {len(payload)} bytes")
        # Check magic
        if payload[:8] != b'DNDLXNU0':
            print(f"WARNING: payload magic is {payload[:8]}, expected DNDLXNU0")
        new_ramdisk = mtk_section(b'ROOTFS', payload)
    else:
        # Empty cpio ramdisk
        cpio = pad(b'070701' + b'0' * 104 + b'TRAILER!!!\x00', 4)
        cpio = pad(cpio, 512)
        new_ramdisk = mtk_section(b'ROOTFS', gzip_data(cpio))

    # Build new boot image
    header = bytearray(page)
    header[:8] = b'ANDROID!'
    struct.pack_into('<10I', header, 8,
        len(new_kernel), kaddr,
        len(new_ramdisk), raddr,
        0, saddr, tags, page, 0, 0)
    # Preserve original cmdline
    header[64:64+len(cmdline)] = cmdline

    # Calculate offsets
    k_pages = (len(new_kernel) + page - 1) // page
    r_off = page + k_pages * page

    image = bytes(header)
    image += pad(new_kernel, page)
    image += pad(new_ramdisk, page)

    with open(out_file, 'wb') as f:
        f.write(image)

    print(f"\nOutput: {out_file} ({len(image)} bytes)")
    print(f"  Kernel section: {len(new_kernel)} bytes")
    print(f"  Ramdisk section: {len(new_ramdisk)} bytes")

if __name__ == '__main__':
    main()
