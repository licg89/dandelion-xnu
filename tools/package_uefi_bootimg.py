#!/usr/bin/env python3
"""Package the built UEFI FD into a flashable Android boot image.

This is the dandelion (line B / UEFI) counterpart of the XNU-era
bootshim/patch_boot.py flow: the FD *is* the kernel section, no BootShim.

Why no BootShim: the FD's first four bytes are 0x140023EA, an AArch64
`b +0x8FA8` that jumps to a small decompress/entry stub. So all LK has to do
is decompress the kernel section to kernel_addr and branch there.

The one address that matters is kernel_addr: it must equal [uefi_fd].base in
Resources/Configs/dandelion.toml, which in turn must equal the "UEFI FD" entry
in Platforms/Xiaomi/dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c. This
script reads the toml and passes it to patch_boot.py as --kernel-addr, then
verifies the result.

Usage:
    python3 tools/package_uefi_bootimg.py \
        --stock artifacts/boot_backup.img \
        --fd   ../uefi-a04br3/Build/dandelionPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd \
        --out  artifacts/boot-uefi.img

Every argument has a default that matches the layout of this machine, so with
no arguments it rebuilds ../uefi-a04br3/.../SILICIUM_UEFI.fd into
artifacts/boot-uefi.img.
"""
import argparse
import os
import struct
import subprocess
import sys
import tomllib

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FD = os.path.join(
    os.path.dirname(REPO), "uefi-a04br3", "Build", "dandelionPkg",
    "DEBUG_CLANGPDB", "FV", "SILICIUM_UEFI.fd")
DEFAULT_DTB = os.path.join(REPO, "Resources", "DTBs", "dandelion.dtb")
DEFAULT_STOCK = os.path.join(os.path.dirname(REPO), "artifacts", "boot_backup.img")
DEFAULT_OUT = os.path.join(os.path.dirname(REPO), "artifacts", "boot-uefi.img")
TOML = os.path.join(REPO, "Resources", "Configs", "dandelion.toml")


def fd_base():
    with open(TOML, "rb") as f:
        config = tomllib.load(f)
    return config["uefi_fd"]["base"]


def verify(img, fd_path, want_kaddr):
    with open(img, "rb") as f:
        data = f.read(64)
    if data[:8] != b"ANDROID!":
        sys.exit("output is not an Android boot image")
    (ksize, kaddr, rsize, raddr, ssize, saddr, tags,
     page, hver, osver) = struct.unpack_from("<10I", data, 8)
    print("verify : kaddr=0x%08X (want 0x%08X) ksize=%d page=%d hver=%d" %
          (kaddr, want_kaddr, ksize, page, hver))
    print("         raddr=0x%08X rsize=%d tags=0x%08X" % (raddr, rsize, tags))
    if kaddr != want_kaddr:
        sys.exit("kernel_addr does not match [uefi_fd].base")
    print("         fd on disk: %d bytes" % os.path.getsize(fd_path))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stock", default=DEFAULT_STOCK,
                    help="stock boot partition image (default: %(default)s)")
    ap.add_argument("--fd", default=DEFAULT_FD,
                    help="built SILICIUM_UEFI.fd (default: %(default)s)")
    ap.add_argument("--dtb", default=DEFAULT_DTB,
                    help="device tree blob (default: %(default)s)")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="output boot image (default: %(default)s)")
    args = ap.parse_args()

    for path in (args.stock, args.fd, args.dtb):
        if not os.path.isfile(path):
            sys.exit("missing input: %s" % path)

    base = fd_base()
    patch_boot = os.path.join(REPO, "bootshim", "patch_boot.py")

    cmd = [sys.executable, patch_boot, args.stock, args.fd, args.dtb, args.out,
           "--kernel-addr", "0x%X" % base]
    print("package: %s" % " ".join(cmd[1:]))
    if subprocess.run(cmd).returncode != 0:
        sys.exit("patch_boot.py failed")

    verify(args.out, args.fd, base)
    print("ok     : %s (%d bytes)" % (args.out, os.path.getsize(args.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
