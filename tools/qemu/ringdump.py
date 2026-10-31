#!/usr/bin/env python3
"""Read XNU's in-memory console out of a running QEMU and print it.

The dandelion kernel keeps no serial port, so osfmk/kern/printf.c mirrors every
console character into two places inside physical memory:

  0x5FC00000  a 256 KiB ring: magic 0x58545831 ("XTX1"), then a u32 length,
              then the text (DANDELION_TRACE_PA in printf.c)
  0x7EC50000  a 1280x1600x32 framebuffer, 8x16 glyph cells from y=860, x=8
              (DANDELION_FB_* in printf.c)

Both survive in QEMU's RAM, so the console of a kernel that cannot print
anything else is still readable: attach gdb to QEMU's gdbstub, stop the guest,
dump the two ranges, decode.  This is how the boot is followed before zzz22
adds the PL011 sink, and it stays useful afterwards as a second channel.

Usage:
    python3 tools/qemu/ringdump.py                    # ring only
    python3 tools/qemu/ringdump.py --fb --registers   # plus framebuffer and CPU state
    python3 tools/qemu/ringdump.py --port 1234 --keep-dumps
"""
import argparse
import os
import struct
import subprocess
import sys

RING_PA = 0x5FC00000
RING_SIZE = 0x40000
RING_MAGIC = 0x58545831
RING_HDR = 8

FB_PA = 0x7EC50000
FB_STRIDE = 2944
FB_HEIGHT = 1600
FB_X0 = 8
FB_TEXTW = 480
FB_Y0 = 860
FB_YMAX = 1580
FB_FG = 0xFFFFFF00

GDB = os.environ.get("GDB", "gdb-multiarch")


def gdb(port, commands):
    script = ["set pagination off", "set confirm off", "set width 0"]
    script.append("target remote :%d" % port)
    script.append("monitor stop")
    script.extend(commands)
    script.append("detach")
    script.append("quit")
    args = [GDB, "-batch", "-nx", "-q"]
    for line in script:
        args += ["-ex", line]
    done = subprocess.run(args, capture_output=True, text=True, timeout=600)
    return done.returncode, done.stdout + done.stderr


def decode_ring(path):
    if not os.path.exists(path):
        return "(no ring dump)"
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < RING_HDR:
        return "(ring dump too short)"
    magic, length = struct.unpack_from("<II", data, 0)
    if magic != RING_MAGIC:
        return ("(no trace: magic 0x%08x, expected 0x%08x -- the kernel never "
                "reached its first console write)" % (magic, RING_MAGIC))
    length = min(length, len(data) - RING_HDR)
    text = data[RING_HDR:RING_HDR + length].split(b"\0", 1)[0]
    return text.decode("utf-8", "replace")


def decode_framebuffer(path, font_module):
    """Turn the framebuffer dump back into the text the console painted."""
    with open(path, "rb") as handle:
        data = handle.read()
    height = len(data) // FB_STRIDE
    rows = []
    for y in range(FB_Y0, min(FB_YMAX, height - 16), 16):
        line = []
        blank = True
        for x in range(FB_X0, FB_TEXTW - 8, 8):
            glyph = bytearray()
            for gy in range(16):
                bits = 0
                base = (y + gy) * FB_STRIDE + x * 4
                if base + 32 > len(data):
                    bits = 0
                    glyph.append(bits)
                    continue
                for gx in range(8):
                    pixel = int.from_bytes(data[base + gx * 4:base + gx * 4 + 4],
                                           "little")
                    if pixel == FB_FG:
                        bits |= 0x80 >> gx
                glyph.append(bits)
            match = "?"
            for index, candidate in enumerate(font_module.FONT):
                if candidate == bytes(glyph):
                    match = chr(font_module.FIRST_CHAR + index)
                    break
            if match != " ":
                blank = False
            line.append(match)
        text = "".join(line).rstrip()
        if text:
            rows.append(text)
        elif not blank:
            rows.append("")
    return rows


def load_font(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("font8x16", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=1234, help="QEMU gdbstub port")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(
        os.path.abspath(__file__)), "out"), help="where to keep the dumps")
    ap.add_argument("--decoder", default=os.path.join(os.path.dirname(
        os.path.abspath(__file__)), "font8x16.py"), help="8x16 font module")
    ap.add_argument("--fb", action="store_true", help="also dump the framebuffer")
    ap.add_argument("--registers", action="store_true", help="also print CPU state")
    ap.add_argument("--keep-dumps", action="store_true", help="keep the raw dumps")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    ring_bin = os.path.join(args.out, "ring.bin")
    fb_bin = os.path.join(args.out, "framebuffer.bin")

    commands = ["dump binary memory %s 0x%x 0x%x"
                % (ring_bin, RING_PA, RING_PA + RING_SIZE)]
    if args.fb:
        commands.append("dump binary memory %s 0x%x 0x%x"
                        % (fb_bin, FB_PA, FB_PA + FB_STRIDE * FB_HEIGHT))
    if args.registers:
        commands.append("info registers")
        commands.append("info all-registers")

    rc, output = gdb(args.port, commands)
    if rc != 0 and "Remote communication error" in output:
        print("ringdump: could not talk to QEMU on port %d" % args.port,
              file=sys.stderr)
        print(output.strip(), file=sys.stderr)
        return 1

    print("=== XNU trace ring (0x%x) ===" % RING_PA)
    text = decode_ring(ring_bin)
    print(text.rstrip() if text.strip() else text)
    print()

    if args.registers:
        wanted = [line for line in output.splitlines()
                  if any(key in line for key in
                         ("pc ", "pstate", "cpsr", "esr", "sp ", "x29", "x30"))]
        print("=== cpu state ===")
        print("\n".join(wanted))
        print()

    if args.fb:
        print("=== framebuffer text (0x%x) ===" % FB_PA)
        if os.path.exists(fb_bin) and os.path.getsize(fb_bin) >= FB_STRIDE * 100:
            try:
                rows = decode_framebuffer(fb_bin, load_font(args.decoder))
            except Exception as error:      # noqa: BLE001 - report, do not die
                rows = ["(decode failed: %s)" % error]
            for row in rows:
                print(row)
        else:
            print("(no framebuffer dump)")
        print()

    if not args.keep_dumps:
        for path in (ring_bin, fb_bin):
            try:
                if os.path.exists(path) and path != fb_bin:
                    os.unlink(path)
            except OSError:
                pass
    print("=== dumps in %s ===" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
