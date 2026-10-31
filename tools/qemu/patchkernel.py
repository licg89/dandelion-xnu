#!/usr/bin/env python3
"""Make a copy of mach_kernel with one function disabled, for QEMU only.

Why: the kernel under test has probes that only make sense on the phone (the
mediaTek GIC bring-up, the eMMC log channel), and re-building it with a patched
out probe costs a 45-minute CI round.  For a run where the question is "what
comes *after* that probe", it is enough to make the probe return immediately --
the same trick as gdb's `return 0`, applied to the file QEMU loads.

    py -3.13 tools/qemu/patchkernel.py \\
        --kernel Resources/UniXNU/mach_kernel \\
        --symbol dandelion_gic_late_init \\
        --out tools/qemu/out/mach_kernel.nogic

    tools/qemu/run.sh --kernel tools/qemu/out/mach_kernel.nogic --ring

AArch64, and only the first two instructions are touched:

    mov w0, #0      0x52800000
    ret             0xd65f03c0

so a function that returns int (or is not expected to return at all) is
neutralised without disturbing anything else.  --as nop writes a single `ret`
instead, for a void function.
"""
import argparse
import os
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02

MOV_W0_0 = 0x52800000
RET = 0xD65F03C0
NOP = 0xD503201F


def parse(data):
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != MH_MAGIC_64:
        sys.exit("not a 64-bit Mach-O")
    segments, symtab, offset = [], None, 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmd == LC_SEGMENT_64:
            name = data[offset + 8:offset + 24].split(b"\0", 1)[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                "<QQQQ", data, offset + 24)
            segments.append((name, vmaddr, vmsize, fileoff, filesize))
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<IIII", data, offset + 8)
        offset += cmdsize
    return segments, symtab


def find_symbol(data, symtab, wanted):
    symoff, nsyms, stroff, strsize = symtab
    for index in range(nsyms):
        strx, _type, _sect, _desc, value = struct.unpack_from(
            "<IBBHQ", data, symoff + index * 16)
        if not strx or strx >= strsize or not value:
            continue
        start = stroff + strx
        end = data.find(b"\0", start)
        if data[start:end] == wanted:
            return value
    return None


def file_offset(segments, va):
    for name, vmaddr, vmsize, fileoff, filesize in segments:
        if vmaddr <= va < vmaddr + vmsize:
            return fileoff + (va - vmaddr), name
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", default="Resources/UniXNU/mach_kernel")
    ap.add_argument("--symbol", action="append", required=True,
                    help="symbol to neutralise (repeatable)")
    ap.add_argument("--mode", "-m", choices=("return0", "ret"), default="return0",
                    help="return0: mov w0,#0; ret.  ret: one instruction")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.kernel, "rb") as handle:
        data = bytearray(handle.read())

    segments, symtab = parse(data)
    if symtab is None:
        sys.exit("kernel has no symbol table (keepsyms=1 builds do)")
    for symbol in args.symbol:
        va = find_symbol(data, symtab, symbol.encode())
        if va is None:
            # Mach-O symbol names keep the C underscore; accept either spelling.
            va = find_symbol(data, symtab, ("_" + symbol).encode())
        if va is None:
            sys.exit("symbol %s not found" % symbol)
        where, segment = file_offset(segments, va)
        if where is None:
            sys.exit("symbol %s is not in a file-backed segment" % symbol)

        struct.pack_into("<I", data, where,
                         MOV_W0_0 if args.mode == "return0" else RET)
        if args.mode == "return0":
            struct.pack_into("<I", data, where + 4, RET)
        print("%s at va 0x%016x -> file offset 0x%x in %s (%s)"
              % (symbol, va, where, segment,
                 "mov w0,#0; ret" if args.mode == "return0" else "ret"))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as handle:
        handle.write(data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
