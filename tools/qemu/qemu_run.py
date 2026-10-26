#!/usr/bin/env python3
"""Run the Dandelion boot shim (and optionally XNU) under QEMU.

The phone has no accessible serial port, so both the shim and the kernel write
their diagnostics into memory:

  * the shim mirrors its framebuffer console to QEMU's PL011 when it is built
    with -DQEMU_VIRT (build.sh xnu-qemu), so its log arrives on the serial
    line;
  * XNU appends everything it prints to the trace buffer at 0x5fc00000 (see
    osfmk/kern/printf.c); this tool pulls that buffer out of guest RAM with
    the QEMU monitor and prints it as text.

That makes a full boot observable without the hardware:

    python3 tools/qemu/qemu_run.py --gic 3 --seconds 45
    python3 tools/qemu/qemu_run.py --gic 2 --seconds 20

Both GIC models are exercised: 'gic-version=3' is a GIC-500 (single security
state, so Group 0/FIQ is usable), 'gic-version=2' is a GIC-400 with the CPU
interface at 0x08010000.  Neither is the phone, but the register offsets,
security-state handling and timer delivery logic are the same code the kernel
uses (pexpert/pexpert/arm64/dandelion_gic.h).
"""

import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

TRACE_PA = 0x5FC00000
TRACE_SIZE = 0x40000
TRACE_MAGIC = 0x58545831

QEMU = "qemu-system-aarch64"


def start_qemu(image, initrd, gic, memory, cpus, monitor_sock, serial_log,
               extra):
    args = [
        QEMU,
        "-M", "virt,virtualization=on,gic-version=%d" % gic,
        "-cpu", "cortex-a53",
        "-m", str(memory),
        "-smp", str(cpus),
        "-kernel", image,
        "-display", "none",
        "-no-reboot",
        "-no-shutdown",
        "-serial", "file:%s" % serial_log,
        "-monitor", "unix:%s,server,nowait" % monitor_sock,
    ]
    if initrd:
        args += ["-initrd", initrd]
    args += extra
    return subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)


def monitor(path, commands, wait=10.0):
    """Send HMP commands to the QEMU monitor socket, return its output."""
    deadline = time.time() + wait
    while time.time() < deadline:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(path)
            break
        except OSError:
            time.sleep(0.2)
    else:
        raise SystemExit("qemu_run: monitor socket %s never appeared" % path)

    sock.settimeout(5.0)
    try:
        sock.recv(4096)                     # banner
    except socket.timeout:
        pass

    replies = []
    for command in commands:
        sock.sendall((command + "\n").encode())
        time.sleep(0.4)
        try:
            replies.append(sock.recv(65536).decode("utf-8", "replace"))
        except socket.timeout:
            replies.append("")
    sock.close()
    return "".join(replies)


def read_trace(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 8:
        return None
    magic, length = struct.unpack_from("<II", data, 0)
    if magic != TRACE_MAGIC:
        return None
    text = data[8:8 + min(length, len(data) - 8)]
    return text.split(b"\0", 1)[0].decode("utf-8", "replace")


def main():
    here = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", default=os.path.join(here, "bootshim", "out", "Image"),
                        help="shim image (build.sh xnu-qemu)")
    parser.add_argument("--initrd", help="XNU payload from mkxnu.py")
    parser.add_argument("--gic", type=int, choices=(2, 3), default=3,
                        help="QEMU GIC model (default 3)")
    parser.add_argument("--seconds", type=float, default=30.0,
                        help="how long to let the guest run")
    parser.add_argument("--memory", type=int, default=2048, help="guest MiB")
    parser.add_argument("--cpus", type=int, default=1)
    parser.add_argument("--serial-out", default=None,
                        help="where to keep the serial log (default: temp)")
    parser.add_argument("--trace-out", default=None,
                        help="where to keep the raw trace buffer")
    parser.add_argument("--quiet", action="store_true",
                        help="only print the XNU trace")
    parser.add_argument("qemu_args", nargs="*", help="extra QEMU arguments")
    args = parser.parse_args()

    tmpdir = tempfile.mkdtemp(prefix="dandelion-qemu-")
    monitor_sock = os.path.join(tmpdir, "mon.sock")
    serial_log = args.serial_out or os.path.join(tmpdir, "serial.log")
    trace_bin = args.trace_out or os.path.join(tmpdir, "trace.bin")

    process = start_qemu(args.image, args.initrd, args.gic, args.memory,
                         args.cpus, monitor_sock, serial_log, args.qemu_args)
    try:
        time.sleep(args.seconds)
        if process.poll() is not None:
            sys.stderr.write("qemu_run: QEMU exited before the dump was taken "
                             "(%d); shorten --seconds or drop -no-reboot\n"
                             % process.returncode)
        else:
            commands = ["pmemsave 0x%x 0x%x %s"
                        % (TRACE_PA, TRACE_SIZE, trace_bin)]
            try:
                reply = monitor(monitor_sock, commands)
            except SystemExit as error:
                sys.stderr.write("qemu_run: %s\n" % error)
                reply = ""
            if "could not" in reply or "unknown" in reply.lower():
                sys.stderr.write("qemu_run: monitor said: %s\n" % reply.strip())
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()

    shim_log = ""
    if os.path.exists(serial_log):
        with open(serial_log, "r", encoding="utf-8", errors="replace") as handle:
            shim_log = handle.read().replace("\r", "")

    trace = read_trace(trace_bin) if os.path.exists(trace_bin) else None

    if not args.quiet:
        print("=== shim (serial) ===")
        print(shim_log.rstrip())
        print()
    print("=== XNU trace (0x%x) ===" % TRACE_PA)
    if trace:
        print(trace.rstrip())
    else:
        print("(no trace: the kernel did not run, or it did not print)")
    print()
    print("=== files ===")
    print("serial: %s" % serial_log)
    print("trace:  %s" % trace_bin)
    return 0


if __name__ == "__main__":
    sys.exit(main())
