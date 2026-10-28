"""Offline check for tools/extract_log_block.py.

Builds a synthetic 'flashinfo' image with a log block at the same offset
FlushLogToFlash uses (partition start + 1 MiB), then asks the extractor to read it
back.  This is how the on-disk format gets tested without a phone.
"""
import os
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import extract_log_block as ex

SECTOR = 512
TEXT = (
    "DANDELION UEFI\nddn pio-read FAIL int=00004000 left=512\nddn  cfg=0200049B "
    "fifo=00000000 sts=00100000 dcrc=00000000\nddn data cmd=8 r1=00000900 len=512\n"
)
BLOCK_SECTORS = 128


def build(path, text_bytes):
    size = 2 * 1024 * 1024
    data = bytearray(b"\xff" * size)
    offset = 2048 * SECTOR  # partition start + 1 MiB

    header = bytearray(SECTOR)
    header[0:8] = ex.MAGIC
    checksum = 0
    for byte in text_bytes:
        checksum = (checksum * 31 + byte) & 0xFFFFFFFF

    struct.pack_into("<I", header, 8, 1)
    struct.pack_into("<I", header, 12, len(text_bytes))
    struct.pack_into("<I", header, 16, BLOCK_SECTORS)
    struct.pack_into("<I", header, 20, checksum)
    struct.pack_into("<Q", header, 24, 2048)

    data[offset:offset + SECTOR] = header
    data[offset + SECTOR:offset + SECTOR + len(text_bytes)] = text_bytes
    with open(path, "wb") as fh:
        fh.write(data)
    return offset


def main():
    with tempfile.TemporaryDirectory() as tmp:
        image = os.path.join(tmp, "flashinfo.img")
        build(image, TEXT.encode())

        blocks = list(ex.find_blocks(open(image, "rb").read()))
        assert len(blocks) == 1, "expected exactly one block, got %d" % len(blocks)
        offset, block = blocks[0]
        assert block["size"] == len(TEXT)
        assert block["checksum_ok"], "checksum mismatch"
        assert block["sectors"] == BLOCK_SECTORS
        assert block["lba"] == 2048
        recovered = block["text"].decode()
        assert recovered == TEXT, "text mismatch"

        # And end to end, through the CLI.
        out = os.path.join(tmp, "log.txt")
        result = subprocess.run(
            [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "extract_log_block.py"),
             image, "--out", out, "--verbose"],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            sys.exit("extractor failed: %s%s" % (result.stdout, result.stderr))
        assert open(out, "rb").read().decode() == TEXT

        # A block whose text is shorter than the sector padding must come back
        # at exactly the declared length, not padded with NULs.
        second = os.path.join(tmp, "short.img")
        build(second, b"hi\n")
        blocks = list(ex.find_blocks(open(second, "rb").read()))
        assert blocks[0][1]["text"] == b"hi\n"

    print("extract_log_block: OK (format, checksum, CLI, length)")


if __name__ == "__main__":
    main()
