#!/usr/bin/env python3
"""Package a Mach-O kernel into the DNDLXNU0 payload the shim loads.

The shim maps the window [phys_base, phys_base + mem_size) at
[virt_base, virt_base + mem_size).  XNU's bootstrap page tables are built from
2 MiB block entries, so the window base and the address the kernel is linked at
must be congruent modulo 2 MiB.  The default virt_base is therefore the 2 MiB
aligned base of the kernel virtual address space, and the kernel text ends up at

    phys_base + (VM_KERNEL_LINK_ADDRESS - virt_base)

i.e. 0x67004000 for the default phys_base of 0x60000000.

Usage:
    mkxnu.py build/mach_kernel -o bootshim/out/xnu-payload.bin
"""

import argparse
import struct
import sys

MAGIC = b"DNDLXNU0"
PAYLOAD_VERSION = 1

HEADER_SIZE = 688          # sizeof(struct xnu_payload) in bootshim/handoff.h
SEGMENT_SIZE = 32          # sizeof(struct xnu_segment)

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02

VM_KERNEL_LINK_ADDRESS = 0xFFFF_FFF0_0700_4000
DEFAULT_VIRT_BASE = 0xFFFF_FFF0_0000_0000   # 2 MiB aligned VA window base
DEFAULT_PHYS_BASE = 0x6000_0000             # 32 MiB aligned DRAM window base
DEFAULT_MEM_SIZE = 0x1800_0000              # 384 MiB

SEGMENT_ALIGN = 0x1000
KERNEL_END_ALIGN = 0x4000                   # XNU_PAGE in the shim


class Error(Exception):
    pass


def align_up(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


class Segment:
    def __init__(self, name, vmaddr, vmsize, fileoff, filesize):
        self.name = name
        self.vmaddr = vmaddr
        self.vmsize = vmsize
        self.fileoff = fileoff
        self.filesize = filesize
        self.physical = 0
        self.payload_offset = 0

    def __repr__(self):
        return ("%s vmaddr=0x%016x vmsize=0x%x fileoff=0x%x filesize=0x%x"
                % (self.name, self.vmaddr, self.vmsize, self.fileoff,
                   self.filesize))


def parse_macho(data):
    if len(data) < 32:
        raise Error("file is too small to be a Mach-O")
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, _ = \
        struct.unpack_from("<IiiIIIII", data, 0)
    if magic != MH_MAGIC_64:
        raise Error("not a 64-bit little-endian Mach-O (magic 0x%08x)" % magic)
    if cputype != 0x0100000C:   # CPU_TYPE_ARM64
        raise Error("not arm64 (cputype 0x%x)" % cputype)

    segments = []
    symtab = None
    offset = 32
    for _ in range(ncmds):
        if offset + 8 > len(data):
            raise Error("truncated load commands")
        cmd, cmdsize = struct.unpack_from("<II", data, offset)
        if cmdsize < 8 or offset + cmdsize > len(data):
            raise Error("bad load command size at 0x%x" % offset)
        if cmd == LC_SEGMENT_64:
            name = data[offset + 8:offset + 24].split(b"\0", 1)[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from(
                "<QQQQ", data, offset + 24)
            if vmsize:
                segments.append(Segment(name, vmaddr, vmsize, fileoff,
                                        filesize))
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<IIII", data, offset + 8)
        offset += cmdsize

    if not segments:
        raise Error("no segments")
    if symtab is None:
        raise Error("no symbol table; pass --entry")
    return segments, symtab


def find_symbol(data, symtab, wanted):
    symoff, nsyms, stroff, strsize = symtab
    if stroff + strsize > len(data):
        raise Error("symbol table outside the file")
    for index in range(nsyms):
        entry = symoff + index * 16
        if entry + 16 > len(data):
            break
        strx, n_type, _sect, _desc, n_value = struct.unpack_from(
            "<IBBHQ", data, entry)
        if not strx or strx >= strsize:
            continue
        start = stroff + strx
        end = data.find(b"\0", start)
        if end < 0:
            continue
        if data[start:end] == wanted:
            return n_value
    raise Error("symbol %r not found" % wanted.decode())


def build_payload(args):
    with open(args.kernel, "rb") as handle:
        image = handle.read()

    segments, symtab = parse_macho(image)
    segments.sort(key=lambda s: s.vmaddr)

    lowest = segments[0].vmaddr
    if lowest != VM_KERNEL_LINK_ADDRESS:
        raise Error("kernel is linked at 0x%016x, expected 0x%016x"
                    % (lowest, VM_KERNEL_LINK_ADDRESS))

    if args.entry:
        entry_va = int(args.entry, 0)
    else:
        entry_va = find_symbol(image, symtab, b"_start")

    virt_base = int(args.virt_base, 0)
    phys_base = int(args.phys_base, 0)
    mem_size = int(args.mem_size, 0)

    if virt_base != (virt_base & ~(0x200000 - 1)):
        raise Error("virt_base must be 2 MiB aligned")
    if phys_base != (phys_base & ~(0x200000 - 1)):
        raise Error("phys_base must be 2 MiB aligned")
    if mem_size & (KERNEL_END_ALIGN - 1):
        raise Error("mem_size must be 16 KiB aligned")
    if virt_base > lowest:
        raise Error("virt_base is above the kernel link address")

    for segment in segments:
        if segment.vmaddr < virt_base:
            raise Error("%s starts below virt_base" % segment.name)
        segment.physical = phys_base + (segment.vmaddr - virt_base)
        if segment.filesize > segment.vmsize:
            raise Error("%s file size exceeds memory size" % segment.name)
        if segment.filesize and segment.fileoff + segment.filesize > len(image):
            raise Error("%s is truncated" % segment.name)

    span = (segments[-1].vmaddr + segments[-1].vmsize) - virt_base
    if span > mem_size:
        raise Error("kernel spans 0x%x bytes, mem_size is 0x%x"
                    % (span, mem_size))

    # Lay the segment data out after the header and the descriptor array.
    cursor = align_up(HEADER_SIZE + len(segments) * SEGMENT_SIZE, SEGMENT_ALIGN)
    data_offset = cursor
    for segment in segments:
        if not segment.filesize:
            segment.payload_offset = 0
            continue
        segment.payload_offset = cursor
        cursor = align_up(cursor + segment.filesize, SEGMENT_ALIGN)
    total_size = cursor

    kernel_end = max(s.physical + s.vmsize for s in segments)
    kernel_end_pa = align_up(kernel_end, KERNEL_END_ALIGN)

    cmdline = args.cmdline.encode()
    if len(cmdline) >= 608:
        raise Error("command line is too long")

    payload = bytearray(total_size)
    payload[0:8] = MAGIC
    struct.pack_into("<II", payload, 8, PAYLOAD_VERSION, len(segments))
    struct.pack_into("<QQQQQ", payload, 16,
                     virt_base, phys_base, mem_size, entry_va, kernel_end_pa)
    struct.pack_into("<QQQ", payload, 56, HEADER_SIZE, data_offset, total_size)
    payload[80:80 + len(cmdline)] = cmdline

    for index, segment in enumerate(segments):
        struct.pack_into("<QQQQ", payload, HEADER_SIZE + index * SEGMENT_SIZE,
                         segment.physical, segment.vmsize,
                         segment.payload_offset, segment.filesize)
        if segment.filesize:
            payload[segment.payload_offset:
                    segment.payload_offset + segment.filesize] = \
                image[segment.fileoff:segment.fileoff + segment.filesize]

    return payload, segments, {
        "virt_base": virt_base,
        "phys_base": phys_base,
        "mem_size": mem_size,
        "entry_va": entry_va,
        "entry_pa": phys_base + (entry_va - virt_base),
        "kernel_end_pa": kernel_end_pa,
        "data_offset": data_offset,
        "total_size": total_size,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernel", help="Mach-O kernel (mach_kernel)")
    parser.add_argument("-o", "--output", required=True, help="payload to write")
    parser.add_argument("--virt-base", default=hex(DEFAULT_VIRT_BASE))
    parser.add_argument("--phys-base", default=hex(DEFAULT_PHYS_BASE))
    parser.add_argument("--mem-size", default=hex(DEFAULT_MEM_SIZE))
    parser.add_argument("--entry", help="entry point VA (default: symbol _start)")
    parser.add_argument("--cmdline", default="-v serial=3 debug=0x14e keepsyms=1 cpus=1 -noprogress")
    args = parser.parse_args()

    try:
        payload, segments, info = build_payload(args)
    except (Error, OSError) as error:
        print("mkxnu.py: %s" % error, file=sys.stderr)
        return 1

    with open(args.output, "wb") as handle:
        handle.write(payload)

    print("wrote %s (%d bytes)" % (args.output, len(payload)))
    for segment in segments:
        print("  %-16s va=0x%016x pa=0x%09x size=0x%-8x file=0x%x"
              % (segment.name, segment.vmaddr, segment.physical,
                 segment.vmsize, segment.filesize))
    for key, value in info.items():
        print("  %-14s 0x%x" % (key, value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
