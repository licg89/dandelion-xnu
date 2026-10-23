#!/usr/bin/env python3
"""Minimal DTB dumper: print nodes whose name or compatible matches a filter.

Usage: python tools/dump_dtb.py <file.dtb> [substring]
"""
import struct
import sys


def be32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def cstr(d, o):
    return d[o:d.index(b"\0", o)].decode("latin1")


def main():
    path = sys.argv[1]
    needle = sys.argv[2] if len(sys.argv) > 2 else ""

    d = open(path, "rb").read()
    magic, total, off, off_str, off_rsv, ver, lcpu, cpuid, sz_str, sz_struct = \
        struct.unpack(">10I", d[:40])
    assert magic == 0xD00DFEED, hex(magic)

    p = off
    end = off + sz_struct
    stack = []          # list of [name, {prop: bytes}]
    while p + 4 <= end:
        tok = be32(d, p)
        p += 4
        if tok == 1:                      # BEGIN_NODE
            e = d.index(b"\0", p)
            name = d[p:e].decode("latin1")
            p = (e + 4) & ~3
            stack.append([name, {}])
        elif tok == 2:                    # END_NODE
            name, props = stack.pop()
            path = "/" + "/".join(n for n, _ in stack + [[name, {}]])
            comp = props.get("compatible", b"").split(b"\0")
            text = name + " " + " ".join(c.decode("latin1") for c in comp)
            if needle.lower() in text.lower():
                print(path)
                for k, v in props.items():
                    if k in ("reg", "compatible", "interrupts",
                             "interrupt-parent", "interrupt-controller",
                             "#interrupt-cells", "clock-frequency",
                             "enable-method", "#address-cells",
                             "#size-cells"):
                        if k in ("reg", "interrupts"):
                            words = [be32(v, i) for i in range(0, len(v), 4)]
                            print("    %-18s %s" % (k, words))
                        elif k == "compatible":
                            print("    %-18s %s" % (
                                k, [c.decode("latin1") for c in comp if c]))
                        else:
                            print("    %-18s %s" % (k, v.hex()))
        elif tok == 3:                    # PROP
            ln, nm = struct.unpack_from(">II", d, p)
            p += 8
            stack[-1][1][cstr(d, off_str + nm)] = d[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            break
        else:
            raise SystemExit("bad token %d at %d" % (tok, p - 4))


if __name__ == "__main__":
    main()
