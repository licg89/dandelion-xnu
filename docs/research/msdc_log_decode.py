#!/usr/bin/env python3
"""msdc_log_decode.py — decode the eMMC text-log ring described in mtk-msdc-pio.md §5.3.

Input is a raw dump of the `flashinfo` partition (or of just the header+ring region).

    adb exec-out su -c 'dd if=/dev/block/by-name/flashinfo bs=4096 count=512 2>/dev/null' > flashinfo.bin
    python msdc_log_decode.py flashinfo.bin                  # human readable
    python msdc_log_decode.py flashinfo.bin --json out.jsonl  # machine readable
    python msdc_log_decode.py flashinfo.bin --scan            # find the header anywhere

Design goals, in order: never crash on a torn/garbage image, never silently drop a good record,
never present a record that failed its CRC.  Run `--selftest` to build and decode a synthetic image.
"""
import argparse
import json
import struct
import sys
import zlib

MAGIC = b"MSDCLOG1"
HDR_BYTES = 4096
HDR_FMT = "<8sIIQQQQIIII"          # magic, ver, hdr_bytes, ring_off, ring_size, write_pos,
                                   # total_bytes, boot_count, hdr_crc, flags, rsvd
HDR_LEN = struct.calcsize(HDR_FMT)  # 64
HDR_CRC_OFF = 52                    # hdr_crc occupies [52,56)
HDR_CRC_LEN = 60                    # CRC32 over [0,60) with [52,56) zeroed
REC_HDR = 24
LEVELS = {0: "PANIC", 1: "ERROR", 2: "WARN", 3: "INFO", 4: "DEBUG"}
SRCS = {0: "shim", 1: "kernel", 2: "other"}


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


class Header:
    __slots__ = ("magic", "version", "hdr_bytes", "ring_off", "ring_size", "write_pos",
                 "total_bytes", "boot_count", "hdr_crc", "flags", "crc_ok", "at", "raw")

    def __repr__(self):
        return (f"v{self.version} boot={self.boot_count} ring=+0x{self.ring_off:x}"
                f"/0x{self.ring_size:x} pos=0x{self.write_pos:x} total={self.total_bytes}"
                f" crc={'ok' if self.crc_ok else 'BAD'} at=0x{self.at:x}")


def parse_header(buf, at):
    if len(buf) < at + HDR_LEN:
        return None
    f = struct.unpack_from(HDR_FMT, buf, at)
    if f[0] != MAGIC:
        return None
    h = Header()
    (h.magic, h.version, h.hdr_bytes, h.ring_off, h.ring_size, h.write_pos,
     h.total_bytes, h.boot_count, h.hdr_crc, h.flags) = f[:10]
    h.at = at
    raw = bytearray(buf[at:at + HDR_CRC_LEN])
    raw[HDR_CRC_OFF:HDR_CRC_OFF + 4] = b"\0\0\0\0"
    h.crc_ok = (zlib.crc32(bytes(raw)) & 0xFFFFFFFF) == h.hdr_crc
    return h


def find_header(buf, base):
    """Try `base`, then any 4096 boundary: makes a mis-based dump still decodable."""
    h = parse_header(buf, base)
    if h:
        return h
    for at in range(0, max(0, len(buf) - HDR_LEN), HDR_BYTES):
        h = parse_header(buf, at)
        if h:
            print(f"note: header found at 0x{at:x} instead of 0x{base:x}", file=sys.stderr)
            return h
    return None


def parse_records(buf, start, size):
    """Walk the ring, accept only CRC-valid records.  Returns (records, rejects)."""
    recs, rejects = [], 0
    end = start + size
    if size <= 0 or end > len(buf):
        size = max(0, len(buf) - start)
        end = start + size
    off = start
    zero8 = b"\0" * 8
    while off + REC_HDR <= end:
        if buf[off:off + 8] == zero8:
            break                            # unwritten tail: the ring ends here
        ln, flags, level, src, tsc, crc, seq = struct.unpack_from("<IHBBQII", buf, off)
        plausible = (ln != 0 and ln <= 4096 - REC_HDR and off + REC_HDR + ln <= end)
        if plausible:
            payload = buf[off + REC_HDR: off + REC_HDR + ln]
            head = bytearray(buf[off: off + 16])
            if (zlib.crc32(bytes(head) + payload) & 0xFFFFFFFF) == crc:
                recs.append({"off": off - start, "seq": seq, "tsc": tsc, "level": level,
                             "src": src, "flags": flags, "text": payload.decode("utf-8", "replace")})
                off += REC_HDR + ln
                off = (off + 7) & ~7
                continue
            # CRC failed but the length is believable: trust it for resync so the *next*
            # record still decodes (a torn tail must not hide what came before it).
            rejects += 1
            off += REC_HDR + ln
            off = (off + 7) & ~7
            continue
        # Unbelievable length: this is not a record boundary. Resync byte by byte is the only
        # safe move (records are 8-byte aligned, so 8 is a cheap compromise).
        rejects += 1
        off += 8
    return recs, rejects


def chronological(recs, write_pos, ring_size):
    """Physical order is oldest->newest *unless* the ring wrapped.

    With a single wrap, every record at/beyond write_pos predates every record below it.
    Sorting by (wrapped, off) recovers the true order without needing a global counter.
    """
    if ring_size <= 0:
        return recs
    wp = write_pos % ring_size
    return sorted(recs, key=lambda r: (0 if r["off"] >= wp else 1, r["off"]))


def fmt(rec, base_ns=None):
    lv = LEVELS.get(rec["level"], f"L{rec['level']}")
    src = SRCS.get(rec["src"], f"S{rec['src']}")
    ts = ""
    if rec["tsc"]:
        if base_ns is None:
            base_ns = min(r["tsc"] for r in [rec] if r["tsc"]) if rec["tsc"] else 0
        ts = f"{(rec['tsc'] - base_ns) / 1e9:12.6f} "
    tag = "[panic]" if rec["flags"] & 4 else ""
    return f"{ts}{lv:<5} {src:<6} {tag}{rec['text'].rstrip()}"


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="raw dump of the flashinfo partition")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0,
                    help="expected header offset (default 0 = dump starts at the partition); "
                         "if it does not parse, every 4096 boundary is scanned anyway")
    ap.add_argument("--json", metavar="PATH", help="also write one JSON object per record")
    ap.add_argument("--scan", action="store_true", help="search every 4096 boundary for the header")
    ap.add_argument("--selftest", action="store_true", help="build and decode a synthetic image")
    a = ap.parse_args(argv)

    if a.selftest:
        return selftest()
    if not a.image:
        ap.error("image required (or --selftest)")

    buf = open(a.image, "rb").read()
    h = find_header(buf, a.base)      # falls back to scanning every 4096 boundary
    if h is None:
        print("error: no MSDCLOG1 header (try --base, or check the dump offset)", file=sys.stderr)
        return 2
    print(f"header: {h}", file=sys.stderr)
    if not h.crc_ok:
        print("warning: header CRC mismatch; decoding the ring anyway", file=sys.stderr)

    recs, rejects = parse_records(buf, h.ring_off, h.ring_size)
    recs = chronological(recs, h.write_pos, h.ring_size)

    # de-duplicate: (seq) is per-boot monotonic, so seq collisions mean the same record seen twice
    seen, uniq = set(), []
    for r in recs:
        k = (r["seq"], r["off"])
        if k in seen:
            continue
        seen.add(k)
        uniq.append(r)

    if uniq:
        t0 = min(r["tsc"] for r in uniq if r["tsc"]) if any(r["tsc"] for r in uniq) else 0
        for r in uniq:
            print(fmt(r, t0))
    print(f"-- {len(uniq)} records, {rejects} rejected candidates, "
          f"{sum(len(r['text']) for r in uniq)} text bytes, boot #{h.boot_count}", file=sys.stderr)

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            for r in uniq:
                f.write(json.dumps({k: r[k] for k in ("seq", "tsc", "level", "src", "flags", "text")}) + "\n")
    return 0


def build_image(path, ring_size=1 << 20, nrec=40):
    """Write a synthetic image.  ring_size small enough => the shim's wrap path is exercised."""
    ring_off = 0x1000
    placed, pos, seq = [], 0, 0
    for i in range(nrec):
        text = f"record {i:03d} {'x' * (i % 37)}".encode()
        body = struct.pack("<IHBBQII", len(text), 0, 3 if i % 9 else 1, 0,
                           1_000_000 * (i + 1), 0, seq) + text
        if pos % ring_size + len(body) <= ring_size:      # never straddle the ring end
            placed.append((pos % ring_size, body))
        pos = (pos + len(body) + 7) & ~7                  # whole-ring-relative, wraps naturally
        seq += 1
    img = bytearray(max(ring_off + ring_size, 0x2000))
    for off_in_ring, body in placed:
        hdr, text = bytearray(body[:REC_HDR]), body[REC_HDR:]
        struct.pack_into("<I", hdr, 16, zlib.crc32(bytes(hdr[:16]) + text) & 0xFFFFFFFF)
        img[ring_off + off_in_ring: ring_off + off_in_ring + REC_HDR + len(text)] = bytes(hdr) + text
    write_pos = pos % ring_size
    hdr = bytearray(struct.pack(HDR_FMT, MAGIC, 1, HDR_BYTES, ring_off, ring_size,
                                write_pos, pos, 7, 0, 1, 0)[:HDR_LEN])
    raw = bytearray(hdr[:HDR_CRC_LEN])
    raw[HDR_CRC_OFF:HDR_CRC_OFF + 4] = b"\0\0\0\0"
    struct.pack_into("<I", hdr, HDR_CRC_OFF, zlib.crc32(bytes(raw)) & 0xFFFFFFFF)
    img[0:HDR_LEN] = bytes(hdr)
    with open(path, "wb") as f:
        f.write(img)
    return path


def selftest():
    import os
    import tempfile
    tmp = tempfile.gettempdir()

    # case 1: ring large enough that nothing is overwritten -> zero rejects expected
    p1 = os.path.join(tmp, "msdc_log_selftest_nowrap.bin")
    build_image(p1, ring_size=1 << 20, nrec=40)
    buf = open(p1, "rb").read()
    h = parse_header(buf, 0)
    assert h and h.crc_ok, "header should parse and CRC-check"
    recs, rejects = parse_records(buf, h.ring_off, h.ring_size)
    assert len(recs) == 40, f"expected 40 records, got {len(recs)}"
    assert rejects == 0, f"unexpected rejects in a non-wrapped ring: {rejects}"
    recs = chronological(recs, h.write_pos, h.ring_size)
    assert [r["seq"] for r in recs] == list(range(40)), "chronological order looks wrong"
    print(f"ok nowrap: {len(recs)} records, order 0..39, header {h}")

    # case 2: tiny ring -> wrap/overwrite; must not crash and must still yield records
    p2 = os.path.join(tmp, "msdc_log_selftest_wrap.bin")
    build_image(p2, ring_size=1024, nrec=60)
    buf2 = open(p2, "rb").read()
    h2 = parse_header(buf2, 0)
    assert h2 and h2.crc_ok
    r2, rej2 = parse_records(buf2, h2.ring_off, h2.ring_size)
    assert r2, "wrapped ring should still decode"
    r2 = chronological(r2, h2.write_pos, h2.ring_size)
    assert r2[-1]["seq"] == 59, f"newest record should be seq 59, got {r2[-1]['seq']}"
    print(f"ok wrap: {len(r2)} records survive, {rej2} rejects tolerated, newest seq {r2[-1]['seq']}")

    # case 3: one corrupted payload byte -> exactly one reject, everything else kept
    bad = bytearray(buf)
    bad[h.ring_off + REC_HDR + 1] ^= 0xFF
    r3, rej3 = parse_records(bytes(bad), h.ring_off, h.ring_size)
    assert rej3 == 1, f"expected exactly 1 reject, got {rej3}"
    assert len(r3) == 39, f"expected 39 good records, got {len(r3)}"
    print(f"ok corruption: {rej3} reject, {len(r3)} records kept (resync by trusted length)")

    # case 4: mis-based dump (starts at the ring, not at the partition) -> --base 0.5 MiB nonsense
    p4 = os.path.join(tmp, "msdc_log_selftest_offset.bin")
    open(p4, "wb").write(b"\xAA" * 8192 + buf)
    h4 = find_header(open(p4, "rb").read(), 0x100000)
    assert h4 and h4.at == 8192, f"scan should find the header at 8192, got {h4}"
    print(f"ok scan: header located at 0x{h4.at:x}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
