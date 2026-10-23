#!/usr/bin/env python3
"""Generate a synthetic MT6765-like DTB for host-side fdt_probe_platform tests.

The layout mirrors the nodes from mainline mt6765.dtsi that the shim cares
about (GICv3, ARM generic timer, MT6577 UARTs, TOPRGU watchdog, CPU nodes).
"""
import struct
import sys

FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_END = 9


class Dtb:
    def __init__(self):
        self.struct = bytearray()
        self.strings = bytearray()
        self.string_offsets = {}

    def _name_off(self, name):
        if name not in self.string_offsets:
            self.string_offsets[name] = len(self.strings)
            self.strings += name.encode() + b"\0"
        return self.string_offsets[name]

    def begin(self, name):
        self.struct += struct.pack(">I", FDT_BEGIN_NODE)
        self.struct += name.encode() + b"\0"
        while len(self.struct) % 4:
            self.struct += b"\0"

    def end(self):
        self.struct += struct.pack(">I", FDT_END_NODE)

    def prop(self, name, value):
        if isinstance(value, str):
            value = value.encode() + b"\0"
        self.struct += struct.pack(">III", FDT_PROP, len(value),
                                   self._name_off(name))
        self.struct += value
        while len(self.struct) % 4:
            self.struct += b"\0"

    def u32(self, name, *values):
        self.prop(name, struct.pack(">%dI" % len(values), *values))

    def cells(self, name, *values):
        self.prop(name, struct.pack(">%dI" % len(values), *values))

    def build(self):
        self.struct += struct.pack(">I", FDT_END)

        header_size = 40
        rsvmap = b"\0" * 16
        off_rsvmap = header_size
        off_struct = off_rsvmap + len(rsvmap)
        off_strings = off_struct + len(self.struct)
        total = off_strings + len(self.strings)

        header = struct.pack(
            ">10I",
            0xD00DFEED, total, off_struct, off_strings, off_rsvmap,
            17, 16, 0, len(self.strings), len(self.struct))
        return header + rsvmap + bytes(self.struct) + bytes(self.strings)


def make_dtb():
    d = Dtb()

    d.begin("")
    d.u32("#address-cells", 2)
    d.u32("#size-cells", 2)
    d.prop("compatible", b"xiaomi,dandelion\0mediatek,mt6762g\0")

    # GICv3: GICD, GICR, GICC (same values as mt6765.dtsi)
    d.begin("interrupt-controller@c000000")
    d.prop("compatible", "arm,gic-v3")
    d.prop("interrupt-controller", b"")
    d.u32("#interrupt-cells", 3)
    d.u32("#address-cells", 0)
    d.u32("#size-cells", 0)
    d.u32("reg",
          0, 0x0C000000, 0, 0x40000,
          0, 0x0C100000, 0, 0x200000,
          0, 0x0C400000, 0, 0x2000)
    d.u32("interrupts", 1, 9, 4)
    d.end()

    # ARM generic timer, virtual timer = PPI 11 -> INTID 27
    d.begin("timer")
    d.prop("compatible", "arm,armv8-timer")
    d.u32("interrupts", 1, 13, 4, 1, 14, 4, 1, 11, 4, 1, 10, 4)
    d.u32("clock-frequency", 13000000)
    d.end()

    d.begin("soc")
    d.prop("compatible", "simple-bus")
    d.u32("#address-cells", 2)
    d.u32("#size-cells", 2)

    for base in (0x11002000, 0x11003000):
        d.begin("serial@%x" % base)
        d.prop("compatible", "mediatek,mt6577-uart")
        d.u32("reg", 0, base, 0, 0x1000)
        d.end()

    d.begin("watchdog@10007000")
    d.prop("compatible", "mediatek,mt6589-wdt")
    d.u32("reg", 0, 0x10007000, 0, 0x1000)
    d.end()
    d.end()

    d.begin("cpus")
    d.u32("#address-cells", 1)
    d.u32("#size-cells", 0)
    d.prop("enable-method", "psci")
    for mpidr in (0x0, 0x1, 0x2, 0x3, 0x100, 0x101, 0x102, 0x103):
        d.begin("cpu@%x" % mpidr)
        d.prop("compatible", "arm,cortex-a53")
        d.prop("device_type", "cpu")
        d.u32("reg", mpidr)
        d.end()
    d.end()

    d.end()
    return d.build()


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "test.dtb"
    data = make_dtb()
    with open(path, "wb") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (path, len(data)))
