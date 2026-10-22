#!/usr/bin/env python3
"""Create a minimal test payload for dandelion shim.

This creates a fake XNU kernel payload that the shim can load and jump to.
The test kernel just loops forever (wfe), so we can verify the handoff works.
"""
import struct
import sys
import os

# Payload magic
MAGIC = b"DNDLXNU0"

# XNU kernel config (matches dandelion)
PHYS_BASE = 0x60000000
MEM_SIZE  = 0x18000000   # 384MB managed window
VIRT_BASE = 0xffffff8007004000  # Standard ARM64 link base
ENTRY_VA  = VIRT_BASE + 0x100000  # Entry at 1MB into kernel

def make_test_kernel():
    """Create a minimal AArch64 kernel that loops forever."""
    # Entry at offset 0x100000 (1MB) - matches ENTRY_VA offset
    code = bytearray(0x100000 + 0x1000)  # 1MB + 4KB
    # Place loop instruction at entry offset
    code[0x100000:0x100004] = b"\x00\x00\x00\x14"  # b 0 (infinite loop)
    return bytes(code)

def make_payload(kernel_code):
    """Package kernel code into the dandelion payload format."""
    # Segment: one segment for the kernel code
    seg_physical = PHYS_BASE
    seg_memory_size = len(kernel_code)
    seg_file_offset = 0  # relative to data_offset
    seg_file_size = len(kernel_code)

    # Header
    header_size = 8 + 4 + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 8 + 8 + 608  # magic+ver+segs+...+cmdline
    # Actually let me use the struct layout from handoff.h
    # struct xnu_payload { magic[8], version, segment_count, virt_base, phys_base,
    #                      mem_size, entry_va, kernel_end_pa, segments_offset,
    #                      data_offset, total_size, command_line[608] }
    # = 8 + 4 + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 8 + 8 + 608 = 688 bytes

    segments_offset = 688  # right after header
    data_offset = segments_offset + 1 * 32  # one segment descriptor (32 bytes)
    # Align data_offset to 8 bytes
    if data_offset % 8:
        data_offset += 8 - (data_offset % 8)

    # kernel_end_pa must equal aligned(end of all segments)
    seg_end_aligned = (seg_physical + seg_memory_size + 0x3FFF) & ~0x3FFF
    kernel_end_pa = seg_end_aligned

    total_size = data_offset + seg_file_size

    # Build header
    payload = b""
    payload += MAGIC                          # magic[8]
    payload += struct.pack("<I", 1)           # version
    payload += struct.pack("<I", 1)           # segment_count
    payload += struct.pack("<Q", VIRT_BASE)   # virt_base
    payload += struct.pack("<Q", PHYS_BASE)   # phys_base
    payload += struct.pack("<Q", MEM_SIZE)    # mem_size
    payload += struct.pack("<Q", ENTRY_VA)    # entry_va
    payload += struct.pack("<Q", kernel_end_pa)  # kernel_end_pa (aligned!)
    payload += struct.pack("<Q", segments_offset)  # segments_offset
    payload += struct.pack("<Q", data_offset)      # data_offset
    payload += struct.pack("<Q", total_size)       # total_size

    # Command line (608 bytes, null-terminated)
    cmdline = b"-v serial=3 debug=0x14e keepsyms=1 cpus=1 -noprogress"
    cmdline += b"\x00" * (608 - len(cmdline))
    payload += cmdline

    assert len(payload) == segments_offset, f"Header size mismatch: {len(payload)} != {segments_offset}"

    # Segment descriptor
    # file_offset is ABSOLUTE offset into the payload file
    seg_file_offset = data_offset  # kernel data starts right at data_offset
    payload += struct.pack("<Q", seg_physical)     # physical
    payload += struct.pack("<Q", seg_memory_size)   # memory_size
    payload += struct.pack("<Q", seg_file_offset)   # file_offset (absolute!)
    payload += struct.pack("<Q", seg_file_size)     # file_size

    assert len(payload) == data_offset, f"Data offset mismatch: {len(payload)} != {data_offset}"

    # Kernel data
    payload += kernel_code

    assert len(payload) == total_size, f"Total size mismatch: {len(payload)} != {total_size}"

    return payload

def main():
    out_dir = os.path.join(os.path.dirname(__file__), "out")
    os.makedirs(out_dir, exist_ok=True)

    kernel = make_test_kernel()
    payload = make_payload(kernel)

    out_path = os.path.join(out_dir, "test-payload.bin")
    with open(out_path, "wb") as f:
        f.write(payload)

    print(f"Test payload created: {out_path}")
    print(f"  Size: {len(payload)} bytes")
    print(f"  Kernel: {len(kernel)} bytes")
    print(f"  Entry VA: 0x{ENTRY_VA:016x}")
    print(f"  Entry PA: 0x{PHYS_BASE + 0x100000:08x}")
    print(f"  Phys base: 0x{PHYS_BASE:08x}")
    print(f"  Mem size: 0x{MEM_SIZE:08x}")

    # Now create a boot.img with this payload as the ramdisk
    # We need to gzip the shim kernel and append DTB, then use the payload as ramdisk
    print("\nTo test: replace ramdisk in boot.img with this payload")

if __name__ == "__main__":
    main()
