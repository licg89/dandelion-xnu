#!/usr/bin/env python3
"""Read dandelion shim log from device memory via ADB.

The shim writes diagnostic output to a known offset in the framebuffer
region (0x7ec50000 + 15MB). After reboot, this script reads it back.

Usage:
  python3 read_log.py
"""
import subprocess
import struct
import sys

# Log location: near end of framebuffer region
LOG_OFFSET = 0x7ec50000 + (15 * 1024 * 1024)  # 15MB into framebuffer
LOG_SIZE = 8192  # 8KB log buffer

def read_mem_via_dd(offset, size):
    """Read memory using dd on device (requires root)."""
    # Try /proc/kcore first (if available)
    cmd = f"su -c 'dd if=/proc/kcore bs=1 skip={offset} count={size} 2>/dev/null'"
    try:
        result = subprocess.run(
            ["adb", "shell", cmd],
            capture_output=True, timeout=10
        )
        if result.returncode == 0 and len(result.stdout) > 0:
            return result.stdout
    except:
        pass

    # Try reading from pstore
    cmd = "su -c 'cat /sys/fs/pstore/console-ramoops-0 2>/dev/null'"
    try:
        result = subprocess.run(
            ["adb", "shell", cmd],
            capture_output=True, timeout=10
        )
        if result.returncode == 0 and len(result.stdout) > 0:
            return result.stdout
    except:
        pass

    return None

def read_log():
    """Read the shim log from the device."""
    print("Reading shim log from device...")

    # Method 1: Try pstore
    print("\n=== Method 1: pstore ===")
    cmd = "su -c 'cat /sys/fs/pstore/console-ramoops-0 2>/dev/null || echo NOT_FOUND'"
    result = subprocess.run(["adb", "shell", cmd], capture_output=True, text=True)
    if result.stdout.strip() and result.stdout.strip() != "NOT_FOUND":
        print(result.stdout)
        return

    # Method 2: Try reading framebuffer memory directly
    print("\n=== Method 2: Framebuffer memory ===")
    # Use dd to read from the log offset
    cmd = f"su -c 'dd if=/dev/block/by-name/boot bs=1 skip=0 count=64 2>/dev/null | xxd'"
    # This won't work for RAM, but let's try /proc/iomem approach

    # Method 3: Write a tiny kernel module approach
    print("\n=== Method 3: Via dmesg ===")
    cmd = "su -c 'dmesg | grep -i dandelion || echo NO_LOG_FOUND'"
    result = subprocess.run(["adb", "shell", cmd], capture_output=True, text=True)
    if result.stdout.strip() and "NO_LOG_FOUND" not in result.stdout:
        print(result.stdout)
        return

    print("\nNo log found. The shim might not have written to a readable location.")
    print("Try: adb shell su -c 'cat /sys/fs/pstore/console-ramoops-0'")

if __name__ == "__main__":
    read_log()
