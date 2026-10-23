#!/usr/bin/env python3
"""Dump the memory layout of a flattened device tree (for bring-up planning).

usage: dtbmem.py <dtb> [--all]
"""
import struct
import sys


def parse(data):
    magic, totalsize, off_struct, off_strings, off_rsvmap, version, lastcomp, bootcpu, size_strings, size_struct = \
        struct.unpack_from(">10I", data, 0)
    if magic != 0xD00DFEED:
        raise SystemExit("not a DTB")

    reserved = []
    pos = off_rsvmap
    while True:
        addr, size = struct.unpack_from(">QQ", data, pos)
        pos += 16
        if addr == 0 and size == 0:
            break
        reserved.append((addr, size))

    strings = data[off_strings:off_strings + size_strings]

    def string_at(offset):
        end = strings.find(b"\0", offset)
        return strings[offset:end].decode("latin-1")

    nodes = []
    pos = off_struct
    stack = []
    while True:
        token = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if token == 1:          # FDT_BEGIN_NODE
            end = data.find(b"\0", pos)
            name = data[pos:end].decode("latin-1")
            pos = (end + 4) & ~3
            path = "/" + "/".join([p for p in stack + [name] if p])
            stack.append(name)
            nodes.append({"path": path if name else "/", "props": {}})
        elif token == 2:        # FDT_END_NODE
            stack.pop()
        elif token == 3:        # FDT_PROP
            length, nameoff = struct.unpack_from(">II", data, pos)
            pos += 8
            value = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
            nodes[-1]["props"][string_at(nameoff)] = value
        elif token == 4:        # FDT_NOP
            continue
        elif token == 9:        # FDT_END
            break
        else:
            raise SystemExit("bad token 0x%x at 0x%x" % (token, pos))
    return reserved, nodes


def u32s(value):
    return list(struct.unpack(">%dI" % (len(value) // 4), value))


def u64_pairs(value):
    return [struct.unpack_from(">QQ", value, i) for i in range(0, len(value), 16)]


def main():
    path = sys.argv[1]
    show_all = "--all" in sys.argv
    data = open(path, "rb").read()
    reserved, nodes = parse(data)

    print("=== memory nodes ===")
    for node in nodes:
        props = node["props"]
        if "device_type" in props and props["device_type"].rstrip(b"\0") == b"memory":
            for base, size in u64_pairs(props.get("reg", b"")):
                print("  %-28s base=0x%09x size=0x%09x end=0x%09x"
                      % (node["path"], base, size, base + size))

    print("=== /reserved-memory ===")
    for node in nodes:
        if not node["path"].startswith("/reserved-memory/"):
            continue
        props = node["props"]
        regs = u64_pairs(props.get("reg", b""))
        if not regs:
            print("  %-40s (no reg)" % node["path"])
            continue
        for base, size in regs:
            print("  %-40s base=0x%09x size=0x%09x end=0x%09x"
                  % (node["path"], base, size, base + size))

    print("=== reserved map (DTB header) ===")
    for base, size in reserved:
        print("  base=0x%09x size=0x%09x end=0x%09x" % (base, size, base + size))

    if show_all:
        print("=== nodes ===")
        for node in nodes:
            print("  " + node["path"])


if __name__ == "__main__":
    main()
