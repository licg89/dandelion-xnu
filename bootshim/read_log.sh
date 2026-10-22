#!/bin/bash
# Read dandelion shim log from device
# Usage: ./read_log.sh

echo "=== Dandelion Shim Log Reader ==="
echo ""

# Method 1: Read from pstore (kernel reads ramoops on boot)
echo "Reading from pstore..."
LOG=$(adb shell "su -c 'cat /sys/fs/pstore/console-ramoops-0 2>/dev/null'")

if [ -n "$LOG" ] && [ "$LOG" != "" ]; then
    echo "$LOG"
    echo ""
    echo "=== Log saved to shim_log.txt ==="
    echo "$LOG" > shim_log.txt
else
    echo "No log found in pstore."
    echo ""
    echo "The shim might not have written to the ramoops area."
    echo "Try checking if the shim ran: adb shell su -c 'dmesg | grep -i dandelion'"
fi
