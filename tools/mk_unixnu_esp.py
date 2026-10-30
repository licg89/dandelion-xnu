#!/usr/bin/env python3
"""Build the FAT volume UniXNU boots from, without mkfs.vfat and mtools.

`tools/mk_unixnu_esp.sh` is the reference, and it needs bash + mtools -- none of
which exists on a plain Windows workstation.  Rather than lose a bring-up round
to that, this writes the same layout in pure Python: FAT16, 4 KiB clusters,
15,000 KiB, label UNIXNU, one file at the root (``\\UniXNU.efi`` -- the first
name PayloadBootDxe tries) and the rest under ``\\EFI\\UniXNU``:

    UniXNU.toml          DarwinBoot.efi        UniXNUDxe.efi
    UniXNURamDiskDxe.efi kernel.macho          kernel.macho.adt

``mach_kernel`` is copied as ``kernel.macho`` (the name DarwinBoot and UniXNU ask
for), exactly as the shell script does.

Every entry gets long-file-name records, so the names on disk are the names the
loaders ask for and the 8.3 aliases stay valid.

Layout inside the 'flashinfo' partition (agreed with bootshim/adt.c,
tools/mk_unixnu_esp.sh and PayloadBootDxe.c):

    0      .. 30000 sectors   this FAT volume       (15 MiB)
    30000  .. 30512           XNU's eMMC log channel
    30512  .. 30640           the firmware's log block

so the image is exactly 30000 sectors and writing it with seek=0 leaves the
logs alone.

usage:
    python tools/mk_unixnu_esp.py                    # -> out/unixnu-esp.img
    python tools/mk_unixnu_esp.py --out X --src DIR
    python tools/mk_unixnu_esp.py --check X          # list what is inside
"""
import argparse
import os
import struct
import sys

SECTOR = 512
TOTAL_SECTORS = 30000            # 15,360,000 bytes = the 15 MiB flashinfo window
SECTORS_PER_CLUSTER = 4          # 2 KiB: big enough for a directory, small enough that the cluster count stays in FAT16 range
RESERVED_SECTORS = 1
NUM_FATS = 2
ROOT_ENTRIES = 512
MEDIA = 0xF8
LABEL = b"UNIXNU     "
OEM = b"MSWIN4.1"
FSTYPE = b"FAT16   "
VOLUME_ID = 0x10291030

# 2026-10-30 00:00:00, so the image is byte-for-byte reproducible.
DOS_TIME = 0
DOS_DATE = ((2026 - 1980) << 9) | (10 << 5) | 30

ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LABEL = 0x08
ATTR_LFN = 0x0F

# destination path -> source file name in Resources/UniXNU
LAYOUT = [
    ("UniXNU.efi", "UniXNU.efi"),
    ("EFI/UniXNU/UniXNU.toml", "UniXNU.toml"),
    ("EFI/UniXNU/DarwinBoot.efi", "DarwinBoot.efi"),
    ("EFI/UniXNU/UniXNUDxe.efi", "UniXNUDxe.efi"),
    ("EFI/UniXNU/UniXNURamDiskDxe.efi", "UniXNURamDiskDxe.efi"),
    ("EFI/UniXNU/kernel.macho", "mach_kernel"),
    ("EFI/UniXNU/kernel.macho.adt", "kernel.macho.adt"),
]


def geometry():
    """Sectors per FAT and the cluster count that fits the image."""
    fat = 1
    while True:
        data = TOTAL_SECTORS - RESERVED_SECTORS - NUM_FATS * fat - (ROOT_ENTRIES * 32) // SECTOR
        clusters = data // SECTORS_PER_CLUSTER
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        if need == fat:
            return fat, clusters
        fat = need


def short_name(long_name, alias=None):
    """11-byte 8.3 name, space padded."""
    name = alias or long_name
    base, _, ext = name.upper().partition(".")
    return (base.ljust(8)[:8] + ext.ljust(3)[:3]).encode("ascii")


def lfn_checksum(short11):
    total = 0
    for byte in short11:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def lfn_records(long_name, short11):
    units = [ord(ch) for ch in long_name] + [0x0000]
    while len(units) % 13:
        units.append(0xFFFF)
    chunks = [units[i:i + 13] for i in range(0, len(units), 13)]
    checksum = lfn_checksum(short11)
    records = []
    for index in range(len(chunks) - 1, -1, -1):
        seq = index + 1
        if index == len(chunks) - 1:
            seq |= 0x40
        chunk = chunks[index]
        raw = struct.pack(
            "<B10sBBB12sH4s",
            seq,
            b"".join(struct.pack("<H", u) for u in chunk[0:5]),
            ATTR_LFN,
            0,
            checksum,
            b"".join(struct.pack("<H", u) for u in chunk[5:11]),
            0,
            b"".join(struct.pack("<H", u) for u in chunk[11:13]),
        )
        records.append(raw)
    return records


def entry(name, attr, cluster, size, alias=None, lfn=True, raw_short=None):
    """One directory entry, preceded by its long-name records."""
    short11 = raw_short or short_name(name, alias)
    records = lfn_records(name, short11) if lfn else []
    records.append(struct.pack(
        "<11sBBBHHHHHHHI",
        short11, attr, 0, 0,
        DOS_TIME, DOS_DATE, DOS_DATE,
        0, DOS_TIME, DOS_DATE,
        cluster & 0xFFFF, size))
    return records


class Image:
    def __init__(self):
        self.fat_sectors, self.clusters = geometry()
        self.data_start = RESERVED_SECTORS + NUM_FATS * self.fat_sectors + \
            (ROOT_ENTRIES * 32) // SECTOR
        self.buf = bytearray(TOTAL_SECTORS * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFF00 | MEDIA
        self.fat[1] = 0xFFFF
        self.next_cluster = 2
        self.dirs = {}          # path -> first cluster

    def cluster_size(self):
        return SECTORS_PER_CLUSTER * SECTOR

    def offset_of(self, cluster):
        return (self.data_start + (cluster - 2) * SECTORS_PER_CLUSTER) * SECTOR

    def alloc(self, count):
        if self.next_cluster + count - 1 > self.clusters + 1:
            raise SystemExit("image full at cluster %u of %u" % (self.next_cluster, self.clusters))
        first = self.next_cluster
        for _ in range(count):
            cluster = self.next_cluster
            self.next_cluster += 1
            self.fat[cluster] = cluster + 1
        self.fat[self.next_cluster - 1] = 0xFFFF
        return first

    def store(self, first, data):
        cluster = first
        done = 0
        while done < len(data):
            take = min(self.cluster_size(), len(data) - done)
            start = self.offset_of(cluster)
            self.buf[start:start + take] = data[done:done + take]
            done += take
            if done < len(data):
                cluster = self.fat[cluster]

    def store_file(self, data):
        count = max(1, (len(data) + self.cluster_size() - 1) // self.cluster_size())
        first = self.alloc(count)
        self.store(first, data)
        return first

    def store_dir(self, records, cluster, parent):
        block = bytearray()
        for record in entry(".", ATTR_DIRECTORY, cluster, 0, lfn=False,
                            raw_short=b".          "):
            block += record
        for record in entry("..", ATTR_DIRECTORY, parent, 0, lfn=False,
                            raw_short=b"..         "):
            block += record
        for record in records:
            if isinstance(record, (list, tuple)):
                for item in record:
                    block += item
            else:
                block += record
        if len(block) > self.cluster_size():
            raise SystemExit("directory %u does not fit one cluster" % cluster)
        self.store(cluster, bytes(block))

    def build(self, files):
        """files: list of (path relative to the root, bytes)."""
        # 1. the directory tree the file list implies
        paths = set()
        for path, _data in files:
            parts = path.split("/")
            for i in range(1, len(parts)):
                paths.add("/".join(parts[:i]))
        for path in sorted(paths, key=lambda p: (p.count("/"), p)):
            self.dirs[path] = self.alloc(1)

        # 2. the files, so their entries can carry the real cluster numbers
        placed = []
        for path, data in files:
            placed.append((path, data, self.store_file(data)))

        # 3. one directory record list per directory
        records = {path: [] for path in [""] + sorted(paths, key=lambda p: (p.count("/"), p))}
        for path in sorted(paths, key=lambda p: (p.count("/"), p)):
            parent, _, name = path.rpartition("/")
            records[parent].append(entry(name, ATTR_DIRECTORY, self.dirs[path], 0))
        for path, data, cluster in placed:
            parent, _, name = path.rpartition("/")
            records[parent].extend(entry(name, ATTR_ARCHIVE, cluster, len(data)))

        # 4. directories, then the fixed root area
        for path in sorted(paths, key=lambda p: (p.count("/"), p)):
            parent = path.rpartition("/")[0]
            parent_cluster = self.dirs[parent] if parent else 0
            self.store_dir(records[path], self.dirs[path], parent_cluster)

        root = bytearray(ROOT_ENTRIES * 32)
        root[0:32] = struct.pack("<11sBBBHHHHHHHI", LABEL, ATTR_LABEL, 0, 0,
                                 DOS_TIME, DOS_DATE, DOS_DATE, 0, DOS_TIME,
                                 DOS_DATE, 0, 0)
        pos = 32
        for record in records[""]:
            items = record if isinstance(record, (list, tuple)) else [record]
            for item in items:
                root[pos:pos + 32] = item
                pos += 32
        root_start = (RESERVED_SECTORS + NUM_FATS * self.fat_sectors) * SECTOR
        self.buf[root_start:root_start + len(root)] = root

        self.boot_sector()
        self.write_fats()
        return bytes(self.buf)

    def boot_sector(self):
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = OEM
        struct.pack_into("<HBHBHHBHHHII", bs, 11,
                         SECTOR, SECTORS_PER_CLUSTER, RESERVED_SECTORS, NUM_FATS,
                         ROOT_ENTRIES, TOTAL_SECTORS, MEDIA, self.fat_sectors,
                         32, 64, 0, 0)
        bs[36] = 0x80
        bs[38] = 0x29
        struct.pack_into("<I", bs, 39, VOLUME_ID)
        bs[43:54] = LABEL
        bs[54:62] = FSTYPE
        bs[510:512] = b"\x55\xAA"
        self.buf[0:SECTOR] = bs

    def write_fats(self):
        raw = bytearray(self.fat_sectors * SECTOR)
        for index, value in enumerate(self.fat):
            if index * 2 + 2 > len(raw):
                break
            struct.pack_into("<H", raw, index * 2, value)
        for copy in range(NUM_FATS):
            start = (RESERVED_SECTORS + copy * self.fat_sectors) * SECTOR
            self.buf[start:start + len(raw)] = raw


def collect(src):
    out = []
    for dest, source in LAYOUT:
        path = os.path.join(src, source)
        if not os.path.isfile(path):
            print("note: %s is missing, leaving %s out" % (path, dest))
            continue
        with open(path, "rb") as fh:
            out.append((dest, fh.read()))
    return out


def verify(path, files):
    """Read the image back the way a FAT driver would, from the spec.

    Deliberately a second implementation rather than a call into the writer: it
    walks the BPB, the FAT and the fixed root directory independently, so an
    offset or chain mistake in Image.build() shows up as a mismatch here.
    """
    with open(path, "rb") as fh:
        disk = fh.read()

    bps = struct.unpack_from("<H", disk, 11)[0]
    spc = disk[13]
    reserved = struct.unpack_from("<H", disk, 14)[0]
    fats = disk[16]
    root_entries = struct.unpack_from("<H", disk, 17)[0]
    fat_sectors = struct.unpack_from("<H", disk, 22)[0]
    cluster_bytes = bps * spc
    fat_start = reserved * bps
    root_start = (reserved + fats * fat_sectors) * bps
    data_start = root_start + root_entries * 32

    fat = []
    for index in range((fat_sectors * bps) // 2):
        fat.append(struct.unpack_from("<H", disk, fat_start + index * 2)[0])

    def chain(first):
        out = []
        cluster = first
        while 2 <= cluster < 0xFFF8:
            out.append(cluster)
            cluster = fat[cluster]
        return out

    def read(first, size):
        data = b""
        for cluster in chain(first):
            offset = data_start + (cluster - 2) * cluster_bytes
            data += disk[offset:offset + cluster_bytes]
        return data[:size]

    def entries(start, count):
        found = []
        lfn = ""
        for index in range(count):
            raw = disk[start + index * 32:start + index * 32 + 32]
            if raw[0] == 0x00:
                break
            if raw[0] == 0xE5:
                continue
            if raw[11] == ATTR_LFN:
                seq = raw[0] & 0x3F
                units = struct.unpack_from("<5H", raw, 1) + \
                    struct.unpack_from("<6H", raw, 14) + \
                    struct.unpack_from("<2H", raw, 28)
                text = "".join(chr(u) for u in units if u not in (0x0000, 0xFFFF))
                lfn = text + lfn
                continue
            if raw[11] & ATTR_LABEL:
                lfn = ""
                continue
            short = raw[0:11].decode("ascii", "replace")
            if lfn:
                name = lfn
            else:
                name = short[0:8].rstrip() + ("." + short[8:11].rstrip() if short[8:11].strip() else "")
            volume = raw[11] == ATTR_DIRECTORY
            first_cluster = struct.unpack_from("<H", raw, 26)[0]
            size = struct.unpack_from("<I", raw, 28)[0]
            found.append((name, volume, first_cluster, size))
            lfn = ""
        return found

    root = entries(root_start, root_entries)

    def lookup(name):
        parts = name.split("/")
        table = root
        for index, part in enumerate(parts):
            match = [e for e in table if e[0].upper() == part.upper()]
            if not match:
                raise SystemExit("verify: %s not found (at %s)" % (name, part))
            entry_name, is_dir, first, size = match[0]
            if index == len(parts) - 1:
                return read(first, size) if not is_dir else b""
            table = entries(data_start + (first - 2) * cluster_bytes, cluster_bytes // 32)
        return b""

    bad = 0
    for dest, payload in files:
        got = lookup(dest)
        if got != payload:
            bad += 1
            print("verify: MISMATCH %s (%u vs %u bytes)" % (dest, len(got), len(payload)))
        else:
            print("verify: ok %-32s %9u bytes" % (dest, len(got)))
    if bad:
        raise SystemExit("verify: %u files differ" % bad)
    print("verify: %u files identical, FAT%u, %u-byte clusters" %
          (len(files), 16, cluster_bytes))
def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default=os.path.join(repo, "out", "unixnu-esp.img"))
    parser.add_argument("--src", default=os.path.join(repo, "Resources", "UniXNU"))
    parser.add_argument("--check", metavar="IMG", help="verify an existing image against --src and exit")
    args = parser.parse_args()

    if args.check:
        files = collect(args.src)
        verify(args.check, files)
        return

    files = collect(args.src)
    image = Image()
    data = image.build(files)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "wb") as fh:
        fh.write(data)

    print("wrote %s: %u bytes, %u sectors, %u clusters of %u bytes, FAT is %u sectors"
          % (args.out, len(data), len(data) // SECTOR, image.clusters,
             image.cluster_size(), image.fat_sectors))
    for dest, payload in files:
        print("  %-32s %9u bytes" % (dest, len(payload)))
    print("""
flash it (the partition is 16 MiB, this image is 30000 sectors, so the log area
after it is untouched):

  fastboot flash flashinfo %s
""" % args.out)


if __name__ == "__main__":
    main()
