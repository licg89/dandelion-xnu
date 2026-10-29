#!/usr/bin/env python3
"""Make DarwinBoot's window diagnostic reach the console, not just the framebuffer.

Why: AllocateWindow explains a failed placement by drawing on the framebuffer --

    NO WINDOW <size> BIG RUN <best> @ <start>
    FREE <start> + <bytes>            (up to seven rows)

-- and UniXNU redraws its menu the moment the boot entry returns, which wipes
those rows before anything can photograph them.  Measured twice: IMG_5791 lost
them to our own heartbeat flush, IMG_5792 to UniXNU's redraw.

ConOut, on the other hand, is captured by DandelionConsoleDxe's 64 KiB ring and
written to flash by PayloadBootDxe, which is how every other step of this bring-up
has been read.  So this adds one Print() beside each of the two DiagDraw() calls;
DiagDraw stays, because on a device with a panel and no UART both channels are
worth having.

Usage (inside WSL, before tools/build_darwinboot.sh):
    python3 tools/patch_darwinboot.py /root/DarwinBoot
Idempotent: it reports 0 patched sites the second time.
"""
import sys
import os

TARGET = os.path.join("DarwinBootPkg", "Driver", "DarwinBoot.c")

EDITS = [
    (
        "    DiagDraw (Line, 0);\n",
        '    Print (L"DARWINBOOT: %s\\n", Line);\n    DiagDraw (Line, 0);\n',
    ),
    (
        "        DiagDraw (Line, Row++);\n",
        '        Print (L"DARWINBOOT: %s\\n", Line);\n        DiagDraw (Line, Row++);\n',
    ),
]


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: patch_darwinboot.py <path to DarwinBoot checkout>")
    path = os.path.join(sys.argv[1], TARGET)
    if not os.path.isfile(path):
        sys.exit("not found: %s" % path)

    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    applied = 0
    for old, new in EDITS:
        if new in text:
            continue                      # already patched
        if old not in text:
            sys.exit("anchor missing in %s: %r" % (path, old))
        text = text.replace(old, new, 1)
        applied += 1

    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text)

    print("patched %d site(s) in %s" % (applied, path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
