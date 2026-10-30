#!/usr/bin/env python3
"""Resolve addresses in a Mach-O kernel to symbols.

The panic line on the panel gives a caller address (and, in some panics, more
than one).  Turning it into "which function" is the difference between guessing
and knowing, and the mach_kernel we flash carries a full symbol table.

Usage:
    macho_sym.py mach_kernel 0xfffffff00797cac8 [more addresses...]

With no addresses it lists the 20 symbols closest to the image base.
"""
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SYMTAB = 0x2


def read_symbols(path):
    data = open(path, "rb").read()
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, _ = \
        struct.unpack_from("<IiiIIIII", data, 0)
    if magic != MH_MAGIC_64:
        raise SystemExit("not a 64-bit Mach-O: magic=0x%08x" % magic)

    off = 32
    symtab = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<IIII", data, off + 8)
        off += cmdsize
    if symtab is None:
        raise SystemExit("no LC_SYMTAB")

    symoff, nsyms, stroff, strsize = symtab
    strtab = data[stroff:stroff + strsize]
    syms = []
    for i in range(nsyms):
        n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from(
            "<IBBhQ", data, symoff + i * 16)
        if n_value == 0:
            continue
        end = strtab.find(b"\0", n_strx)
        name = strtab[n_strx:end].decode("utf-8", "replace")
        syms.append((n_value, name, n_type))
    syms.sort()
    return syms


def resolve(syms, addr):
    lo, hi = 0, len(syms)
    while lo < hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo == 0:
        return None, 0
    value, name, _ = syms[lo - 1]
    return name, addr - value


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    syms = read_symbols(argv[1])
    addrs = argv[2:]
    if not addrs:
        for value, name, _ in syms[:20]:
            print("0x%016x %s" % (value, name))
        return
    for text in addrs:
        addr = int(text, 0)
        name, delta = resolve(syms, addr)
        if name is None:
            print("0x%016x  <below image base>" % addr)
        else:
            print("0x%016x  %s+0x%x" % (addr, name, delta))


if __name__ == "__main__":
    main(sys.argv)
