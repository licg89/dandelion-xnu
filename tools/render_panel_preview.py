#!/usr/bin/env python3
"""Render an offline preview of the dandelion instrument strip.

Why: the only way to check a panel layout change used to be flash the phone and
take a photo.  Two real bugs were found that way and neither needed hardware to
spot -- labels shorter than four characters silently truncated their value
(CORRECTIONS C12), and rows on a 26-pixel pitch touched each other.  This tool
draws the same rectangles the drivers draw, with the same font and the same
coordinates, into a PNG, so a layout mistake costs a second instead of a flash.

It deliberately duplicates the geometry instead of sharing it: the drivers are
C in an EDK2 module and this is a desk tool.  The constants below carry the file
and line they mirror; if you change a driver, change it here too.

    python tools/render_panel_preview.py --out panel.png
    python tools/render_panel_preview.py --intr 0x8000 --left 512 --out fail.png

Nothing here runs on the phone and nothing here is compiled into the firmware.
"""
import argparse
import struct
import sys
import zlib

WIDTH = 720
HEIGHT = 1600
STRIDE = 736  # the panel scans out 736 pixels per line; see DandelionConsoleDxe

# MsdcImplLib.c: MSDC_PANEL_*
PANEL_TOP = 1244
PANEL_LEFT = 8
PANEL_COL2 = 372
PANEL_ROW = 30
PANEL_SCALE = 5

# MsdcImplLib.c: mMsdcPanelLabel -- four characters each, on purpose.
LABELS = ["INTR", "MCFG", "FIFO", "SSTS", "DCRC",
          "LEFT", "R1  ", "SCMD", "SCFG", "BLKN"]

# tools/gen_small_font.py: ORDER
FONT_ORDER = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ= "

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)


def load_font(header_path):
    """Parse the generated 3x5 font: one UINT16 per glyph, 5 rows of 3 bits."""
    glyphs = {}
    order_index = 0
    with open(header_path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line.startswith("0x"):
                continue
            value = int(line.split(",")[0], 16)
            ch = FONT_ORDER[order_index]
            glyphs[ch] = value
            order_index += 1
    if order_index != len(FONT_ORDER):
        sys.exit("font header has %d glyphs, expected %d" % (order_index, len(FONT_ORDER)))
    return glyphs


class Canvas(object):
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.pixels = bytearray(b"\x00" * (width * height * 3))

    def fill(self, left, top, w, h, colour):
        left = max(0, left)
        top = max(0, top)
        right = min(self.width, left + w)
        bottom = min(self.height, top + h)
        row = bytes(colour) * max(0, right - left)
        for y in range(top, bottom):
            off = (y * self.width + left) * 3
            self.pixels[off:off + len(row)] = row

    def glyph(self, x, y, bits):
        # Row 0 lives in bits 14..12, row 4 in bits 2..0.
        for row in range(5):
            for col in range(3):
                if (bits >> (14 - (row * 3 + col))) & 1:
                    self.fill(x + col * PANEL_SCALE, y + row * PANEL_SCALE,
                              PANEL_SCALE, PANEL_SCALE, WHITE)

    def text(self, x, y, s, font):
        for ch in s:
            self.glyph(x, y, font.get(ch, font[" "]))
            x += 4 * PANEL_SCALE

    def save(self, path):
        raw = bytearray()
        for y in range(self.height):
            raw.append(0)  # filter type 0
            off = y * self.width * 3
            raw += self.pixels[off:off + self.width * 3]

        def chunk(tag, data):
            out = struct.pack(">I", len(data)) + tag + data
            return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.width, self.height, 8, 2, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        png += chunk(b"IEND", b"")
        with open(path, "wb") as fh:
            fh.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="panel_preview.png")
    ap.add_argument("--font", default="Silicon/MediaTek/MT6765Pkg/Library/MsdcImplLib/MsdcSmallFont.h")
    ap.add_argument("--intr", default="0x00004000")
    ap.add_argument("--mcfg", default="0x0200049B")
    ap.add_argument("--fifo", default="0x00000000")
    ap.add_argument("--ssts", default="0x00100000")
    ap.add_argument("--dcrc", default="0x00000000")
    ap.add_argument("--left", default="512")
    ap.add_argument("--r1", default="0x00000900")
    ap.add_argument("--scmd", default="0x02000888")
    ap.add_argument("--scfg", default="0x05000009")
    ap.add_argument("--blkn", default="1")
    ap.add_argument("--cells", default="6:yellow,7:red")
    args = ap.parse_args()

    font = load_font(args.font)
    canvas = Canvas(WIDTH, HEIGHT)
    canvas.fill(0, 0, WIDTH, HEIGHT, BLACK)

    # Console text area (rows 0..67) left black; draw a divider so the preview
    # shows where the console stops.
    canvas.fill(0, 1087, WIDTH, 2, (40, 40, 40))

    # StorageBeaconDxe banners (y 1100 and 1140, 30 px tall) and dot rows.
    canvas.fill(8, 1100, 704, 30, (255, 0, 0))
    canvas.fill(8, 1140, 704, 30, (255, 0, 0))
    canvas.fill(8, 1190, 20, 20, (0, 0, 255))
    canvas.fill(8, 1220, 20, 20, (0, 0, 255))
    canvas.fill(36, 1220, 20, 20, (0, 0, 255))

    values = [
        args.intr, args.mcfg, args.fifo, args.ssts, args.dcrc,
        args.left, args.r1, args.scmd, args.scfg, args.blkn,
    ]
    for slot, raw in enumerate(values):
        x = PANEL_LEFT if slot < 5 else PANEL_COL2
        y = PANEL_TOP + (slot % 5) * PANEL_ROW
        if raw.lower().startswith("0x"):
            digits = "%08X" % (int(raw, 16) & 0xFFFFFFFF)
        else:
            digits = "%08X" % (int(raw) & 0xFFFFFFFF)
        canvas.text(x, y, LABELS[slot] + "=" + digits, font)

    # Stage cells: three rows of eight, 80x44 on an 88 pitch, y 1400/1448/1496.
    colours = {
        "yellow": (255, 255, 0), "red": (255, 0, 0), "green": (0, 255, 0),
        "cyan": (0, 255, 255), "white": (255, 255, 255), "orange": (255, 128, 0),
        "magenta": (255, 0, 255), "purple": (128, 0, 255),
    }
    for item in args.cells.split(","):
        item = item.strip()
        if not item:
            continue
        stage, colour = item.split(":")
        stage = int(stage)
        top = 1400 if stage < 8 else (1448 if stage < 16 else 1496)
        canvas.fill(PANEL_LEFT + (stage & 7) * 88, top, 80, 44, colours[colour.strip()])

    # PlatformProgressDxe white band.
    canvas.fill(0, 1560, WIDTH, 8, (255, 255, 255))

    canvas.save(args.out)
    print("wrote %s (%dx%d)" % (args.out, WIDTH, HEIGHT))


if __name__ == "__main__":
    main()
