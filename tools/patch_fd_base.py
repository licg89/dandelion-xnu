#!/usr/bin/env python3
"""Relocate a built UEFI FD by rewriting its baked-in base address.

Why this exists
---------------
`PcdFdBaseAddress` is compiled into the FD: the entry stub (the branch in the
first 4 bytes) jumps to a decompressor that reads the compact FV from
`PcdFdBaseAddress + <offset>`.  If the FD is loaded somewhere else, the stub
reads the wrong memory and the SoC dies exactly like "the FD never ran" -- a
~10 s watchdog reset with the vendor logo still on screen.  That is a very
expensive way to learn the address is wrong, so it is worth being able to move
an already-built FD without rebuilding EDK2.

On dandelion the value is also constrained by LK: only 0x40080000 works
(measured, see docs/CORRECTIONS.md C8).  This tool is how we got from the
0x48880000 build to a bootable 0x40080000 image without an EDK2 tree.

Two sites are patched in a typical build:

  0x168   the FD header field the decompression stub reads
  0xc0d8  the "UEFI FD" entry in the platform memory map (MemoryMapLib data)

Usage
-----
  python tools/patch_fd_base.py artifacts/SILICIUM_UEFI.fd out/fd_4008.fd \
      --from 0x48880000 --to 0x40080000

Then package with the matching load address:

  python bootshim/patch_boot.py <stock boot.img> out/fd_4008.fd <dtb> \
      out/boot_uefi.img --kernel-addr 0x40080000
"""

import argparse
import struct
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--from", dest="old", required=True,
                    help="current baked base, e.g. 0x48880000")
    ap.add_argument("--to", dest="new", required=True,
                    help="desired base, e.g. 0x40080000")
    ap.add_argument("--expect", type=int, default=None,
                    help="fail unless exactly this many sites are patched")
    args = ap.parse_args()

    old = struct.unpack("<I", struct.pack("<I", int(args.old, 0)))[0]
    new = struct.unpack("<I", struct.pack("<I", int(args.new, 0)))[0]

    data = bytearray(open(args.src, "rb").read())
    pat_old = struct.pack("<I", old)
    pat_new = struct.pack("<I", new)

    sites = []
    i = data.find(pat_old)
    while i >= 0:
        sites.append(i)
        data[i:i + 4] = pat_new
        i = data.find(pat_old, i + 1)

    if not sites:
        print("no occurrence of 0x%08X: nothing to do (already relocated?)" % old)
        return 1

    for off in sites:
        print("patched 0x%05x" % off)
    if args.expect is not None and len(sites) != args.expect:
        print("expected %d sites, found %d -- refusing to write" %
              (args.expect, len(sites)), file=sys.stderr)
        return 2

    open(args.dst, "wb").write(bytes(data))
    print("wrote %s (%d bytes, %d site%s)" %
          (args.dst, len(data), len(sites), "" if len(sites) == 1 else "s"))
    print("now package with --kernel-addr 0x%08X" % new)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
