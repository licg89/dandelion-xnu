#!/usr/bin/env python3
"""Emit the smallest arm64 Mach-O that XNU will accept as process 1.

`load_init_program()` on this port ends every boot with

    load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development

because the root filesystem is mockfs, and mockfs turns the *whole* root device
into one executable file node.  The root device is `Resources/UniXNU/kernel.macho.rd`
(4 KiB of zeros today), so exec reads zeros, finds no Mach-O, and user space
never starts.  This script manufactures the missing half: a static
`MH_EXECUTE`/AArch64 image whose payload proves it ran (it stores a beacon at a
fixed address, then loops), with no libc, no dyld, no signature.

    py -3.13 tools/mk_init_macho.py --out out/kernel.macho.rd --verify
    py -3.13 tools/mk_init_macho.py --out out/kernel.macho.rd --verify --tree-tools

Every load command below is annotated with the XNU check that consumes it.  The
tree read for this script is D:\\aios\\xnu (xnu-8796.141.3 + patches/xnu), and
the line numbers are that tree's.

WHAT THE LOADER DEMANDS (bsd/kern/mach_loader.c, in the order it runs)
---------------------------------------------------------------------
magic/cputype     parse_machfile:809,834  MH_MAGIC_64; cputype & ~CPU_ARCH_MASK
                                          must equal cpu_type() (CPU_TYPE_ARM64).
cpusubtype        parse_machfile:839      grade_binary() must accept it; ARM64_ALL
                                          grades 9 on an ARM64_V8 or ARM64E host
                                          (bsd/dev/arm/kern_machdep.c:86-103).
filetype/flags    parse_machfile:847-870  MH_EXECUTE.  A static image (no
                                          MH_DYLDLINK) is allowed only under
                                          `#if DEVELOPMENT || DEBUG` (:866) --
                                          otherwise LOAD_FAILURE.  We must NOT set
                                          MH_DYLDLINK: that sets needs_dynlinker
                                          (:857) and then :1462 "if we need dyld,
                                          we got it" fails, because there is no
                                          /usr/lib/dyld on this port.
header+cmds read  parse_machfile:895-923  sizeofcmds must fit the file, and the
                                          read is round_page()d: a short read sets
                                          resid and returns LOAD_BADMACHO.  With
                                          PAGE_SIZE = 16 KiB on arm64 this is why
                                          the image must be >= 16384 bytes.
__PAGEZERO        mach_loader.c:2220-2291 the page-zero segment (vmaddr 0,
                                          filesize 0, init/max prot NONE) raises
                                          the map's min_offset to 0x100000000.
                                          load_machfile:640-646 then requires
                                          vm_map_has_hard_pagezero(map, 4 GiB),
                                          i.e. min_offset >= 4 GiB, for every
                                          64-bit arm64 image.  Without __PAGEZERO
                                          exec fails with EBADMACHO.
__TEXT            mach_loader.c:1154-1162 exactly one segment may map file offset
                                          0 with filesize > 0, and its initprot
                                          must include READ|EXEC.  :2175-2184
                                          requires fileoff to be aligned to
                                          PAGE_MASK_64 *and* the map page mask
                                          (16 KiB for a 64-bit arm64 process).
                                          :2293-2298 rejects vmaddr == 0 for any
                                          non-pagezero segment.
__DATA            mach_loader.c:2431-2468 a segment whose filesize is 0 gets its
                                          whole vmsize mapped as anonymous
                                          zero-fill (map_segment:1850-1854 maps
                                          nothing when file_start == file_end).
                                          This is the only writable address in
                                          the image; __TEXT is R+X by the rule
                                          above, so it cannot hold the beacon.
LC_UNIXTHREAD     mach_loader.c:1222-1242 -> load_unixthread:2733.  Unlike
                                          LC_MAIN (load_main:2670 sets
                                          needs_dynlinker and only stores
                                          `entryoff` for dyld to use), this sets
                                          result->entry_point and
                                          result->unixproc, so the kernel jumps
                                          straight at the pc.  Thread state must
                                          be ARM_THREAD_STATE64 with count
                                          ARM_THREAD_STATE64_COUNT (= 68) or
                                          thread_entrypoint (osfmk/arm64/status.c:
                                          1992-2012) and thread_userstack
                                          (:1846-1855) return KERN_INVALID_ARGUMENT
                                          and the load fails.  All registers but
                                          pc are left zero on purpose: with sp == 0
                                          thread_userstack reports "no custom
                                          stack" and create_unix_stack
                                          (bsd/kern/kern_exec.c:6648) allocates the
                                          default one.
entry-in-__TEXT   mach_loader.c:1012-1027 pass 3 fails unless
                                          result->validentry is set, and that is
                                          set only in load_segment:2503-2512, for
                                          an entry point inside a R+X segment.
LC_BUILD_VERSION   mach_loader.c:1416-1441 optional (result->ip_platform only);
                                          included so the process looks like an
                                          iOS binary instead of PLATFORM_UNKNOWN.
LC_SYMTAB          not consumed by the kernel at all (parse_machfile:1442 "other
                                          commands are ignored"); present so
                                          tools/macho_sym.py in this repository can
                                          resolve the entry point, which is how
                                          --tree-tools verifies this file against
                                          the tree's own parser.

Deliberately absent: LC_CODE_SIGNATURE, LC_LOAD_DYLINKER, LC_MAIN, LC_UUID.
mach_loader.c:1457 would fail an unsigned image, but only when
`cs_process_global_enforcement()` is true, and that is 0 for this board's config
(bsd/kern/kern_cs.c:94-100, no CONFIG_ENFORCE_SIGNED_CODE in
config/MASTER.arm64.dandelion).  See docs/INIT-PROGRAM.md for the full gate list.
"""
import argparse
import os
import struct
import subprocess
import sys

# ---------------------------------------------------------------------------
# Mach-O constants we actually use (EXTERNAL_HEADERS/mach-o/loader.h, nlist.h)
# ---------------------------------------------------------------------------
MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C          # CPU_ARCH_ABI64 | CPU_TYPE_ARM
CPU_SUBTYPE_ARM64_ALL = 0
CPU_SUBTYPE_MASK = 0xFF000000
MH_EXECUTE = 0x2
MH_DYLDLINK = 0x4                    # must stay clear: it demands dyld
MH_PIE = 0x200000                    # clear: keeps slide 0, so vmaddrs are fixed

LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
LC_BUILD_VERSION = 0x32
LC_SYMTAB = 0x2
LC_MAIN = 0x80000028                 # rejected on purpose: see below
LC_CODE_SIGNATURE = 0x1D

PLATFORM_IOS = 2
ARM_THREAD_STATE64 = 6               # osfmk/mach/arm/thread_status.h:62
ARM_THREAD_STATE64_COUNT = 68        # sizeof(arm_thread_state64_t) / 4

VM_PROT_NONE = 0
VM_PROT_READ = 1
VM_PROT_WRITE = 2
VM_PROT_EXECUTE = 4

S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400

N_EXT = 0x01
N_SECT = 0x0E

PAGE = 16384                         # arm64 PAGE_SIZE in the kernel build
BEACON_VMADDR = 0x100005000          # inside __DATA, page 1: harmless, writable
BEACON_VALUE = int.from_bytes(b"initproc", "little")   # 0x636F727074 696E69

# Segment placement.  __TEXT must be at file offset 0 and must not start at
# address 0; __DATA starts on the next 16 KiB page after __TEXT.
TEXT_VMADDR = 0x100000000
DATA_VMADDR = TEXT_VMADDR + PAGE


# ---------------------------------------------------------------------------
# A very small A64 assembler.  Only the five encodings the payload needs, each
# written from the ARM ARM field layout so the bytes can be decoded back.
# ---------------------------------------------------------------------------
def movz(rd, imm16, shift=0):
    """MOVZ Xd, #imm16, LSL #shift   (sf=1, opc=10, 100101)"""
    assert shift % 16 == 0 and 0 <= shift <= 48
    return 0xD2800000 | ((shift // 16) << 21) | ((imm16 & 0xFFFF) << 5) | rd


def movk(rd, imm16, shift=0):
    """MOVK Xd, #imm16, LSL #shift   (sf=1, opc=11, 100101)"""
    assert shift % 16 == 0 and 0 <= shift <= 48
    return 0xF2800000 | ((shift // 16) << 21) | ((imm16 & 0xFFFF) << 5) | rd


def str_x_imm(rt, rn, imm12=0):
    """STR Xt, [Xn, #imm12*8]        (size=11, opc=00, 111001, unsigned offset)"""
    assert 0 <= imm12 < 4096
    return 0xF9000000 | (imm12 << 10) | (rn << 5) | rt


def b_imm26(imm26):
    """B #(imm26*4)                  (opc=000101, immediate)"""
    return 0x14000000 | (imm26 & 0x03FFFFFF)


def decode(word):
    """Decode one of the above back into (mnemonic, fields).  Used by --verify."""
    if word & 0xFF800000 == 0xD2800000:          # sf=1 op=10 100101
        return ("movz", {"rd": word & 0x1F, "imm16": (word >> 5) & 0xFFFF,
                         "shift": ((word >> 21) & 3) * 16})
    if word & 0xFF800000 == 0xF2800000:          # sf=1 op=11 100101
        return ("movk", {"rd": word & 0x1F, "imm16": (word >> 5) & 0xFFFF,
                         "shift": ((word >> 21) & 3) * 16})
    if word & 0xFFC00000 == 0xF9000000:          # size=11 opc=00 111001
        return ("str", {"rt": word & 0x1F, "rn": (word >> 5) & 0x1F,
                        "imm12": (word >> 10) & 0xFFF, "size": (word >> 30) & 3})
    if word & 0xFC000000 == 0x14000000:
        imm26 = word & 0x03FFFFFF
        if imm26 & 0x02000000:
            imm26 -= 0x04000000
        return ("b", {"imm26": imm26})
    return ("unknown", {"word": word})


def build_payload():
    """The code that proves user space runs: store the beacon, then spin.

    No syscalls: this port has no libc and no dyld, so svc #0x80 would land in
    unix_syscall() with a NULL uuai and is not part of this rung.
    """
    words = [
        # x1 = BEACON_VMADDR (0x00000001_00005000)
        movz(1, (BEACON_VMADDR >> 0) & 0xFFFF, 0),
        movk(1, (BEACON_VMADDR >> 32) & 0xFFFF, 32),
        # x0 = BEACON_VALUE ("initproc" little-endian), one 16-bit slice at a time
        movz(0, (BEACON_VALUE >> 0) & 0xFFFF, 0),
        movk(0, (BEACON_VALUE >> 16) & 0xFFFF, 16),
        movk(0, (BEACON_VALUE >> 32) & 0xFFFF, 32),
        movk(0, (BEACON_VALUE >> 48) & 0xFFFF, 48),
        # [beacon] = x0
        str_x_imm(0, 1),
        # and never come back
        b_imm26(0),
    ]
    return b"".join(struct.pack("<I", w) for w in words), words


# ---------------------------------------------------------------------------
# Mach-O builders
# ---------------------------------------------------------------------------
def segment64(name, vmaddr, vmsize, fileoff, filesize, maxprot, initprot, sections):
    cmdsize = 72 + 80 * len(sections)
    out = struct.pack("<II16sQQQQiiII", LC_SEGMENT_64, cmdsize, name.encode(),
                      vmaddr, vmsize, fileoff, filesize, maxprot, initprot,
                      len(sections), 0)
    for sect in sections:
        out += struct.pack("<16s16sQQIIIIIIII",
                           sect["name"].encode(), name.encode(),
                           sect["addr"], sect["size"], sect["offset"],
                           sect["align"], 0, 0, sect["flags"], 0, 0, 0)
    assert len(out) == cmdsize
    return out


def unixthread64(pc):
    """LC_UNIXTHREAD carrying a whole ARM_THREAD_STATE64.

    Every general register is 0 except pc: sp == 0 makes thread_userstack()
    (osfmk/arm64/status.c:1861-1871) report "no custom stack", so
    create_unix_stack() allocates the default one and thread_setuserstack()
    (bsd/kern/kern_exec.c:1745) points sp at the real stack afterwards.  Note
    that this tree reads the *32-bit* sp field for flavor 6
    (status.c:1854 casts to arm_thread_state32_t), so a non-zero x[6] high word
    would be misread as a custom stack -- another reason to keep it zero.
    """
    regs = [0] * 29                       # x0..x28
    regs += [0, 0]                        # fp (x29), lr (x30)
    regs += [0]                           # sp  (x31)  -- see above
    regs += [pc]                          # pc
    assert len(regs) == 33
    body = struct.pack("<II", ARM_THREAD_STATE64, ARM_THREAD_STATE64_COUNT)
    body += b"".join(struct.pack("<Q", v) for v in regs)
    body += struct.pack("<II", 0, 0)      # cpsr, pad: the 34th 64-bit slot
    assert len(body) == 8 + ARM_THREAD_STATE64_COUNT * 4
    return struct.pack("<II", LC_UNIXTHREAD, 8 + len(body)) + body


def build_version_cmd():
    # platform / minos / sdk are xxxx.yy.zz packed as (x<<16)|(y<<8)|z
    return struct.pack("<IIIIII", LC_BUILD_VERSION, 24, PLATFORM_IOS,
                       0x000D0000, 0x00110000, 0)


def symtab_cmd(symoff, nsyms, stroff, strsize):
    return struct.pack("<IIIIII", LC_SYMTAB, 24, symoff, nsyms, stroff, strsize)


SYMBOL_NAME = "_start"


def build(pad_to=PAGE, payload_style="beacon"):
    """Assemble the whole image.  Returns (bytes, layout dict)."""
    payload, words = build_payload()
    if payload_style == "trap":
        # Diagnostic variant: fault on purpose so the panic report names the pc.
        payload = struct.pack("<I", 0xD4200000)  # BRK #0
        words = [0xD4200000]

    # Sizes that do not depend on offsets first, so the command block length is
    # known before anything that lives after it can be placed.
    n_cmds = 6
    cmds_size_fixed = (72                    # __PAGEZERO
                       + 152                 # __TEXT + 1 section
                       + 72                  # __DATA
                       + 8 + 8 + 272         # LC_UNIXTHREAD
                       + 24                  # LC_BUILD_VERSION
                       + 24)                 # LC_SYMTAB
    header_len = 32
    payload_off = (header_len + cmds_size_fixed + 15) & ~15
    text_end = payload_off + len(payload)
    symoff = (text_end + 7) & ~7
    strtab = b"\0" + SYMBOL_NAME.encode() + b"\0"
    stroff = symoff + 16                     # one nlist_64
    end = stroff + len(strtab)

    if end > pad_to:
        raise SystemExit("mk_init_macho: image needs %d bytes, --pad-to %d is too small"
                         % (end, pad_to))

    entry = TEXT_VMADDR + payload_off

    cmds = []
    # __PAGEZERO: see the module docstring; load_machfile:640-646 makes this
    # mandatory for a 64-bit arm64 process.
    cmds.append(segment64("__PAGEZERO", 0, 0x100000000, 0, 0,
                          VM_PROT_NONE, VM_PROT_NONE, []))
    # __TEXT: maps the whole file R+X, must be fileoff 0 (header segment rule).
    cmds.append(segment64("__TEXT", TEXT_VMADDR, PAGE, 0, pad_to,
                          VM_PROT_READ | VM_PROT_EXECUTE,
                          VM_PROT_READ | VM_PROT_EXECUTE,
                          [{"name": "__text", "addr": entry, "size": len(payload),
                            "offset": payload_off, "align": 2,
                            "flags": S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS}]))
    # __DATA: filesize 0 => pure zero-fill (mach_loader.c:2431-2468), RW.
    cmds.append(segment64("__DATA", DATA_VMADDR, PAGE, 0, 0,
                          VM_PROT_READ | VM_PROT_WRITE,
                          VM_PROT_READ | VM_PROT_WRITE, []))
    cmds.append(unixthread64(entry))
    cmds.append(build_version_cmd())
    cmds.append(symtab_cmd(symoff, 1, stroff, len(strtab)))
    assert len(cmds) == n_cmds

    sizeofcmds = sum(len(c) for c in cmds)
    assert sizeofcmds == cmds_size_fixed, (sizeofcmds, cmds_size_fixed)

    header = struct.pack("<IiIIIIII", MH_MAGIC_64, CPU_TYPE_ARM64,
                         CPU_SUBTYPE_ARM64_ALL, MH_EXECUTE, n_cmds, sizeofcmds,
                         0, 0)            # flags 0: no MH_DYLDLINK, no MH_PIE

    image = bytearray(header + b"".join(cmds))
    image.extend(b"\0" * (payload_off - len(image)))
    image.extend(payload)
    image.extend(b"\0" * (symoff - len(image)))

    nlist = struct.pack("<IBBhQ", 1, N_SECT | N_EXT, 1, 0, entry)
    image.extend(nlist)
    image.extend(strtab)
    image.extend(b"\0" * (pad_to - len(image)))
    assert len(image) == pad_to

    layout = {
        "entry": entry,
        "payload_off": payload_off,
        "payload_len": len(payload),
        "words": words,
        "sizeofcmds": sizeofcmds,
        "ncmds": n_cmds,
        "pad_to": pad_to,
        "beacon_addr": BEACON_VMADDR,
        "beacon_value": BEACON_VALUE,
    }
    return bytes(image), layout


# ---------------------------------------------------------------------------
# Verification: parse the bytes we just wrote as the kernel would.
# ---------------------------------------------------------------------------
class Fail(Exception):
    pass


def _need(cond, msg):
    if not cond:
        raise Fail(msg)


def _unixthread_pc_offset(data):
    """File offset of the pc field inside the first LC_UNIXTHREAD (arm64 state)."""
    ncmds, sizeofcmds = struct.unpack_from("<II", data, 16)
    off = 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == LC_UNIXTHREAD:
            flavor, count = struct.unpack_from("<II", data, off + 8)
            if flavor != ARM_THREAD_STATE64 or count != ARM_THREAD_STATE64_COUNT:
                raise Fail("LC_UNIXTHREAD is not an ARM_THREAD_STATE64/%d"
                           % ARM_THREAD_STATE64_COUNT)
            return off + 16 + 32 * 8      # 0..28 x, 29 fp, 30 lr, 31 sp, 32 pc
        off += cmdsize
    raise Fail("no LC_UNIXTHREAD")


def verify(data, layout=None, verbose=False):
    """Re-parse a Mach-O image and assert every property the loader checks.

    Deliberately independent of build(): everything here comes from the bytes
    on disk, so a mistake in the writer cannot hide behind a shared variable.
    """
    checks = []

    def ok(msg):
        checks.append(msg)
        if verbose:
            print("  ok  " + msg)

    _need(len(data) >= 32, "file shorter than a mach_header_64")
    (magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags,
     reserved) = struct.unpack_from("<IiIIIIII", data, 0)
    _need(magic == MH_MAGIC_64, "magic 0x%08x != MH_MAGIC_64" % magic)
    ok("magic MH_MAGIC_64 (parse_machfile:809)")
    _need(cputype & ~0xFF000000 == CPU_TYPE_ARM64 & ~0xFF000000,
          "cputype 0x%x is not arm64 (parse_machfile:834)" % cputype)
    ok("cputype 0x%08x is CPU_TYPE_ARM64" % cputype)
    _need((cpusubtype & 0x00FFFFFF) == CPU_SUBTYPE_ARM64_ALL,
          "cpusubtype 0x%x is not ARM64_ALL (grade_binary)" % cpusubtype)
    _need(cputype & 0x01000000, "cputype lacks CPU_ARCH_ABI64 => LC_SEGMENT_64 is rejected "
                                "(mach_loader.c:1189)")
    ok("cpusubtype ARM64_ALL, ABI64 bit set")
    _need(filetype == MH_EXECUTE, "filetype %d != MH_EXECUTE" % filetype)
    ok("filetype MH_EXECUTE")
    _need(not (flags & MH_DYLDLINK),
          "MH_DYLDLINK set: mach_loader.c:857 sets needs_dynlinker and :1462 then "
          "requires LC_LOAD_DYLINKER, which needs /usr/lib/dyld")
    _need(not (flags & MH_PIE), "MH_PIE set: vmaddrs would be slid, the fixed beacon "
                                "address would move")
    ok("flags 0x%x: static, not slid" % flags)

    # The 16 KiB page-rounded read in parse_machfile:895-923.
    _need(32 + sizeofcmds <= len(data),
          "header+sizeofcmds (%d) exceeds the file (%d)" % (32 + sizeofcmds, len(data)))
    _need((32 + sizeofcmds + PAGE - 1) // PAGE * PAGE <= len(data),
          "parse_machfile reads round_page(header+sizeofcmds) = %d bytes but the file "
          "is %d; the short read sets resid and returns LOAD_BADMACHO "
          "(mach_loader.c:917-923)" % ((32 + sizeofcmds + PAGE - 1) // PAGE * PAGE, len(data)))
    ok("round_page(header+sizeofcmds) fits the file (mach_loader.c:917)")

    off = 32
    seen = {}
    pagezero_end = 0
    header_segment = 0
    entry = None
    symtab = None
    for i in range(ncmds):
        _need(off + 8 <= 32 + sizeofcmds, "command %d runs past sizeofcmds" % i)
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        _need(cmdsize >= 8 and off + cmdsize <= 32 + sizeofcmds,
              "command %d cmdsize %d overflows the command block" % (i, cmdsize))
        if cmd == LC_SEGMENT_64:
            _need(cmdsize >= 72, "segment cmdsize %d < 72 (mach_loader.c:2111)" % cmdsize)
            (segname, vmaddr, vmsize, fileoff, filesize, maxprot, initprot,
             nsects, segflags) = struct.unpack_from("<16sQQQQiiII", data, off + 8)
            segname = segname.rstrip(b"\0").decode()
            seen[segname] = (vmaddr, vmsize, fileoff, filesize, maxprot, initprot)
            _need(cmdsize - 72 >= 80 * nsects,
                  "%s: nsects %d does not fit cmdsize (mach_loader.c:2151)"
                  % (segname, nsects))
            _need(fileoff + filesize <= len(data),
                  "%s: fileoff+filesize %d > file size %d (mach_loader.c:2142)"
                  % (segname, fileoff + filesize, len(data)))
            if fileoff == 0 and filesize > 0:
                _need(header_segment == 0, "more than one segment maps file offset 0 "
                                           "(mach_loader.c:1154-1162)")
                _need(initprot & (VM_PROT_READ | VM_PROT_EXECUTE) ==
                      (VM_PROT_READ | VM_PROT_EXECUTE),
                      "%s maps offset 0 but is not R+X" % segname)
                header_segment = 1
            if vmaddr == 0 and filesize == 0 and vmsize and initprot == 0 and maxprot == 0:
                pagezero_end = vmaddr + vmsize
            else:
                _need(vmaddr != 0, "%s has vmaddr 0 and is not PAGEZERO "
                                   "(mach_loader.c:2293-2298)" % segname)
                _need(fileoff % PAGE == 0,
                      "%s fileoff 0x%x is not 16 KiB aligned (mach_loader.c:2175)"
                      % (segname, fileoff))
            for s in range(nsects):
                (sectname, sseg, addr, size, soff, align, _r, _n, sflags,
                 _r1, _r2, _r3) = struct.unpack_from("<16s16sQQIIIIIIII", data,
                                                     off + 72 + 80 * s)
                sectname = sectname.rstrip(b"\0").decode()
                _need(soff + size <= fileoff + filesize,
                      "section %s,%s escapes its segment" % (sseg.rstrip(b'\0').decode(), sectname))
                if sflags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS):
                    ok("section __TEXT,%s at 0x%x (file 0x%x, %d bytes)"
                       % (sectname, addr, soff, size))
        elif cmd == LC_UNIXTHREAD:
            flavor, count = struct.unpack_from("<II", data, off + 8)
            _need(flavor == ARM_THREAD_STATE64,
                  "thread flavor %d != ARM_THREAD_STATE64" % flavor)
            _need(count == ARM_THREAD_STATE64_COUNT,
                  "thread count %d != %d: thread_entrypoint returns KERN_INVALID_ARGUMENT "
                  "(osfmk/arm64/status.c:1996)" % (count, ARM_THREAD_STATE64_COUNT))
            _need(cmdsize == 8 + 8 + count * 4,
                  "LC_UNIXTHREAD cmdsize %d != 8+8+%d*4" % (cmdsize, count))
            base = off + 16
            # 29 x-registers, fp, lr, sp, pc, then cpsr+pad sharing one 64-bit slot
            regs = struct.unpack_from("<34Q", data, base)
            pc = regs[32]
            sp = regs[31]
            _need(pc != 0, "pc is 0: thread_entrypoint would report VM_MIN_ADDRESS "
                           "(status.c:2005-2009) and pass 3 would fail")
            _need(sp == 0, "sp != 0: status.c:1854 reads the 32-bit sp field for flavor 6, "
                           "so this would be misread as a custom stack")
            entry = pc
            ok("LC_UNIXTHREAD ARM_THREAD_STATE64/%d, pc 0x%x, sp 0" % (count, pc))
        elif cmd == LC_MAIN:
            raise Fail("LC_MAIN present: load_main sets needs_dynlinker "
                       "(mach_loader.c:2670) and the kernel never uses entryoff")
        elif cmd == LC_SYMTAB:
            symtab = struct.unpack_from("<IIII", data, off + 8)
        elif cmd == LC_CODE_SIGNATURE:
            ok("LC_CODE_SIGNATURE present (unexpected but legal)")
        off += cmdsize

    _need(off == 32 + sizeofcmds,
          "walked %d bytes of load commands, header says %d" % (off - 32, sizeofcmds))
    ok("ncmds %d, sizeofcmds %d, walk lands exactly at the end" % (ncmds, sizeofcmds))
    _need(set(seen) == {"__PAGEZERO", "__TEXT", "__DATA"},
          "segments are %s" % sorted(seen))
    _need(pagezero_end == 0x100000000,
          "no __PAGEZERO reaching 4 GiB: load_machfile:640-646 requires "
          "vm_map_has_hard_pagezero(map, 0x100000000) for every 64-bit arm64 image")
    ok("__PAGEZERO covers [0, 0x%x) => min_offset >= 4 GiB" % pagezero_end)
    _need(seen["__TEXT"][2] == 0 and seen["__TEXT"][3] > 0, "__TEXT must map file offset 0")
    _need(seen["__DATA"][3] == 0, "__DATA must be zero-fill only (filesize 0)")
    ok("__TEXT fileoff 0 filesize %d, __DATA zero-fill %d bytes"
       % (seen["__TEXT"][3], seen["__DATA"][1]))
    _need(header_segment, "no segment maps the start of the file (mach_loader.c:1034-1037)")
    _need(entry is not None, "no entry point from LC_UNIXTHREAD")

    t_vmaddr, t_vmsize, _, _, _, t_initprot = seen["__TEXT"]
    _need(t_vmaddr <= entry < t_vmaddr + t_vmsize,
          "entry 0x%x is outside __TEXT [0x%x,0x%x): load_segment:2503-2512 leaves "
          "validentry 0 and pass 3 fails" % (entry, t_vmaddr, t_vmaddr + t_vmsize))
    _need(t_initprot & (VM_PROT_READ | VM_PROT_EXECUTE) ==
          (VM_PROT_READ | VM_PROT_EXECUTE), "__TEXT is not R+X")
    ok("entry 0x%x inside R+X __TEXT" % entry)

    d_vmaddr, d_vmsize, _, _, d_maxprot, _ = seen["__DATA"]
    _need(d_maxprot & VM_PROT_WRITE, "__DATA is not writable")
    _need(not (d_vmaddr < t_vmaddr + t_vmsize and t_vmaddr < d_vmaddr + d_vmsize),
          "__DATA overlaps __TEXT")
    ok("__DATA writable at 0x%x+%d, disjoint from __TEXT" % (d_vmaddr, d_vmsize))

    # The payload itself: decode it and check it does what the docstring claims.
    payload_off = entry - TEXT_VMADDR
    words = list(struct.unpack_from("<%dI" % ((len(data) - payload_off) // 4),
                                    data, payload_off))
    decoded = [decode(w) for w in words]
    loop_index = None
    for idx, (mnem, f) in enumerate(decoded):
        if mnem == "b" and f["imm26"] == 0:
            loop_index = idx
            break
    _need(loop_index is not None, "payload has no self-branch: it would run off "
                                  "the end of the mapped page")
    code = decoded[:loop_index + 1]
    _need(all(m != "unknown" for m, _ in code),
          "payload contains an instruction this assembler cannot explain: %r" % (code,))
    regs = {}
    stores = []
    for mnem, f in code:
        if mnem == "movz":
            regs[f["rd"]] = f["imm16"] << f["shift"]
        elif mnem == "movk":
            old = regs.get(f["rd"], 0)
            mask = 0xFFFF << f["shift"]
            regs[f["rd"]] = (old & ~mask) | (f["imm16"] << f["shift"])
        elif mnem == "str":
            _need(f["size"] == 3, "store is not 64-bit: %r" % (f,))
            stores.append((f["rn"], f["rt"], f["imm12"] * 8))
    _need(len(stores) == 1, "expected exactly one store, decoded %r" % (stores,))
    rn, rt, delta = stores[0]
    _need(rn in regs and rt in regs, "store uses a register the payload never sets")
    target = regs[rn] + delta
    _need(target == BEACON_VMADDR, "store targets 0x%x, expected 0x%x"
          % (target, BEACON_VMADDR))
    _need(regs[rt] == BEACON_VALUE, "beacon value 0x%x, expected 0x%x"
          % (regs[rt], BEACON_VALUE))
    _need(d_vmaddr <= target < d_vmaddr + d_vmsize,
          "beacon 0x%x is not in the writable __DATA range" % target)
    ok("payload: %d instructions, stores 0x%016x at 0x%x (in __DATA), then b ."
       % (len(code), BEACON_VALUE, target))
    _need(BEACON_VALUE.to_bytes(8, "little") == b"initproc",
          "beacon is not the literal 'initproc'")
    ok("beacon bytes are the ASCII 'initproc'")

    # LC_SYMTAB, so tools/macho_sym.py can name the entry point.
    _need(symtab is not None, "no LC_SYMTAB: tools/macho_sym.py cannot resolve anything")
    symoff, nsyms, stroff, strsize = symtab
    _need(nsyms >= 1, "LC_SYMTAB has no symbols")
    _need(symoff + 16 * nsyms <= len(data) and stroff + strsize <= len(data),
          "symtab/strtab outside the file")
    n_strx, n_type, n_sect, _desc, n_value = struct.unpack_from("<IBBhQ", data, symoff)
    name = data[stroff + n_strx:data.index(b"\0", stroff + n_strx)].decode()
    _need(name == SYMBOL_NAME, "symbol 0 is %r" % name)
    _need(n_value == entry, "symbol %s is 0x%x, entry is 0x%x" % (name, n_value, entry))
    ok("LC_SYMTAB: %s = 0x%x" % (name, entry))

    if layout is not None:
        _need(entry == layout["entry"], "entry drifted from the builder's value")
        _need(layout["pad_to"] == len(data), "file length != requested pad")
    if verbose:
        print("verify: %d checks passed on %d bytes" % (len(checks), len(data)))
    return checks


# ---------------------------------------------------------------------------
# Cross-check against the repository's own Mach-O reader
# ---------------------------------------------------------------------------
def check_tree_tools(path, entry):
    """Feed the image to tools/macho_sym.py and require it to name _start.

    macho_sym.py is the tree's only Mach-O parser.  Two runs: no arguments
    (lists the symbols closest to the image base) and an explicit address (must
    resolve to _start+0x0).  Both prove the header block it unpacks --
    "<IiiIIIII" at offset 0, i.e. magic, cputype, cpusubtype, filetype, ncmds,
    sizeofcmds, flags -- decodes to what this script meant to write.
    """
    tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), "macho_sym.py")
    if not os.path.exists(tool):
        raise Fail("tools/macho_sym.py is missing; cannot cross-check")
    runs = [([path], "0x%016x %s" % (entry, SYMBOL_NAME)),
            ([path, "0x%x" % entry], "0x%016x  %s+0x0" % (entry, SYMBOL_NAME))]
    for args, expected in runs:
        r = subprocess.run([sys.executable, tool] + args,
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise Fail("macho_sym.py %s failed: %s" % (" ".join(args), r.stderr.strip()))
        if expected not in r.stdout:
            raise Fail("macho_sym.py %s did not print %r; got %r"
                       % (" ".join(args), expected, r.stdout.strip()))
    print("  ok  tools/macho_sym.py resolves %s at 0x%x" % (SYMBOL_NAME, entry))


# ---------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join("out", "kernel.macho.rd"),
                    help="where to write the image (default: out/kernel.macho.rd)")
    ap.add_argument("--pad-to", type=lambda v: int(v, 0), default=PAGE,
                    help="pad the file to this many bytes (default 16384 = PAGE_SIZE; "
                         "must be a multiple of 4096, >= 16384 to survive "
                         "parse_machfile's page-rounded read)")
    ap.add_argument("--payload", choices=("beacon", "trap"), default="beacon",
                    help="'beacon' stores the value and loops (default); 'trap' "
                         "executes BRK #0 so the panic report names the pc")
    ap.add_argument("--verify", action="store_true",
                    help="re-read the file from disk and assert every loader check")
    ap.add_argument("--verify-only", metavar="PATH", default=None,
                    help="verify an existing image instead of building one (useful "
                         "for the -rd copy that is already on the ESP)")
    ap.add_argument("--tree-tools", action="store_true",
                    help="additionally verify with tools/macho_sym.py")
    ap.add_argument("--json", action="store_true", help="print the layout as JSON")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)

    if args.verify_only:
        with open(args.verify_only, "rb") as f:
            data = f.read()
        print("verify: reading %s" % args.verify_only)
        try:
            checks = verify(data, None, verbose=not args.quiet)
        except Fail as exc:
            raise SystemExit("verify FAILED: %s" % exc)
        if args.tree_tools:
            entry = struct.unpack_from("<Q", data, _unixthread_pc_offset(data))[0]
            try:
                check_tree_tools(args.verify_only, entry)
            except Fail as exc:
                raise SystemExit("tree-tool check FAILED: %s" % exc)
        if not args.quiet:
            print("verify: OK (%d checks)" % len(checks))
        return 0

    if args.pad_to % 4096:
        raise SystemExit("mk_init_macho: --pad-to must be a multiple of 4096")
    if args.pad_to < PAGE and args.payload == "beacon":
        print("mk_init_macho: warning: --pad-to %d < %d; parse_machfile reads a "
              "page-rounded command block, so this image cannot exec "
              "(see docs/INIT-PROGRAM.md)" % (args.pad_to, PAGE), file=sys.stderr)

    image, layout = build(args.pad_to, args.payload)
    out_dir = os.path.dirname(os.path.abspath(args.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(image)
    if not args.quiet:
        print("wrote %s: %d bytes, entry 0x%x, %d load commands (%d bytes), "
              "%d instructions"
              % (args.out, len(image), layout["entry"], layout["ncmds"],
                 layout["sizeofcmds"], len(layout["words"])))

    if args.json:
        import json
        print(json.dumps({k: v for k, v in layout.items() if k != "words"},
                         indent=2, sort_keys=True))

    if args.verify:
        print("verify: re-reading %s" % args.out)
        with open(args.out, "rb") as f:
            data = f.read()
        try:
            verify(data, layout, verbose=not args.quiet)
        except Fail as exc:
            raise SystemExit("verify FAILED: %s" % exc)
        if args.tree_tools:
            try:
                check_tree_tools(args.out, layout["entry"])
            except Fail as exc:
                raise SystemExit("tree-tool check FAILED: %s" % exc)
        if not args.quiet:
            print("verify: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
