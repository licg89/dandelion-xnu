#!/usr/bin/env python3
"""Apply the dandelion fixes that live in the EDK2 / Silicium tree.

Why this file exists: the GitHub workflow used to carry a `sed` that rewrote
SimpleFb.c's scanout pitch, and the local build did not.  The two builds then
produced different firmware -- locally the GOP reported 720 (sheared console),
in CI 736 (correct) -- which is exactly the kind of difference that makes
debugging lie to you.  Both paths now call this script instead.

Usage:
    python3 tools/prepare_uefi_tree.py <path-to-uefi-repo>
"""
import glob
import os
import sys

# dandelion scans out 736 pixels per line (2944 bytes) with 720 visible.
# Upstream SimpleFbDxe sets PixelsPerScanLine = Width, so every consumer that
# blits through the GOP shears by 16 px per row.
FIXES = [
    (
        "SimpleFb.c",
        "PixelsPerScanLine    = Width;",
        "PixelsPerScanLine    = 736;  // dandelion: 2944 B pitch, 720 visible",
    ),
]


def apply_fix(root, filename, old, new):
    changed = 0
    seen = 0
    for path in glob.glob(os.path.join(root, "**", filename), recursive=True):
        seen += 1
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as handle:
            text = handle.read()

        if new in text:
            print("  %-70s already fixed" % os.path.relpath(path, root))
            continue

        if old not in text:
            print("  %-70s PATTERN NOT FOUND" % os.path.relpath(path, root))
            continue

        with open(path, "w", encoding="utf-8", errors="surrogateescape", newline="") as handle:
            handle.write(text.replace(old, new))

        print("  %-70s patched" % os.path.relpath(path, root))
        changed += 1

    if seen == 0:
        sys.exit("no %s found under %s" % (filename, root))
    return changed


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    root = os.path.abspath(sys.argv[1])
    if not os.path.isdir(root):
        sys.exit("not a directory: %s" % root)

    total = 0
    for filename, old, new in FIXES:
        print("%s:" % filename)
        total += apply_fix(root, filename, old, new)

    print("done, %d file(s) changed" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
