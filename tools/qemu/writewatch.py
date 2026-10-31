"""Catch the writer of the write-after-free, automatically.  gdb Python.

The phone can only tell us where the boot stopped; the crash moves every boot
and XNU's own detector (zalloc_validate_element) fires at *allocation* time, so
the writer is long gone by then.  A watchpoint on the freed element fires on
the write itself, and the backtrace at that moment is the answer.

    tools/qemu/run.sh --gdb                        # -S, waiting for us
    gdb-multiarch -x tools/qemu/writewatch.py

    # or attach to a boot already in progress:
    tools/qemu/run.sh --seconds 900 --ring &
    gdb-multiarch -ex "target remote :1234" -x tools/qemu/writewatch.py

What it does:

  1. breaks on kfree_ext(void *kheap_or_kt_view, x1 = data, x2 = size) --
     osfmk/kern/kalloc.c:2728, the one free path every kalloc_type element goes
     through;
  2. for elements of the size class we care about (16 bytes: the
     "shared.kalloc.16" the phone reports), finishes the free and then sets a
     hardware watchpoint on the element;
  3. on a hit, prints the writer's backtrace, the registers and the element's
     current contents, and stops so the session can stay there.

A hit is not automatically the bug: zalloc may simply have handed the same
address out again, and then the "writer" is an ordinary constructor.  The
backtrace is what tells the two apart, and `set_max_watch` in the file below is
how much noise you are willing to trade for the search.  Booting with
`--after bsd_init` (the default) only arms the trap once bsd_init() runs, which
is where the phone's corruption starts to bite.

Everything here is read-only with respect to the guest: no inferior function
calls (QEMU's gdbstub does not support them), only breakpoints, watchpoints and
memory reads.
"""
import collections
import os
import sys
import traceback

import gdb

# --- knobs -----------------------------------------------------------------
SIZE_CLASS = int(os.environ.get("WATCH_SIZE", "16"))
MAX_WATCH = int(os.environ.get("WATCH_MAX", "2"))
ARM_AFTER = os.environ.get("WATCH_AFTER", "bsd_init")   # "" arms immediately
LOG = os.environ.get("WATCH_LOG", "tools/qemu/out/writewatch.log")

armed = not ARM_AFTER
hits = 0
live = collections.OrderedDict()        # element address -> watchpoint


def log(message):
    print(message)
    try:
        os.makedirs(os.path.dirname(LOG), exist_ok=True)
        with open(LOG, "a", encoding="utf-8") as handle:
            handle.write(message + "\n")
    except OSError:
        pass


def arm_hook():
    """Arm the trap once the boot reaches a milestone (bsd_init by default).

    The kfree_ext breakpoint is not installed before that: every hit costs a
    round trip through the gdbstub, and the boot makes thousands of frees
    before bsd_init, which would turn a minutes-long boot into an hour-long one.
    """
    class Arm(gdb.Breakpoint):
        def __init__(self, where):
            # Hardware, not software: the shim copies mach_kernel into place
            # after QEMU starts, so a software breakpoint written into RAM
            # before that is overwritten by the copy and never fires.
            super().__init__(where, gdb.BP_HARDWARE_BREAKPOINT, internal=True)

        def stop(self):
            global armed
            armed = True
            log("[writewatch] armed at %s" % ARM_AFTER)
            FreeHook()
            self.delete()
            return False                    # do not stop the boot

    try:
        Arm(ARM_AFTER)
        log("[writewatch] will arm at %s" % ARM_AFTER)
    except gdb.error as error:
        log("[writewatch] cannot break at %s (%s); arming now"
            % (ARM_AFTER, error))
        globals()["armed"] = True


class WatchHit(gdb.Breakpoint):
    """A write into an element after it was freed."""

    def __init__(self, spec, element):
        super().__init__(spec, gdb.BP_WATCHPOINT, internal=False)
        self.element = element

    def stop(self):
        global hits
        hits += 1
        log("\n=== write-after-free? element 0x%x (hit %d) ==="
            % (self.element, hits))
        try:
            value = int(gdb.parse_and_eval(
                "*(unsigned long *)0x%x" % self.element))
            log("value now 0x%016x" % value)
        except gdb.error:
            pass
        for command in ("bt 20", "info registers pc lr sp x0 x1 x2 x3"):
            try:
                log(gdb.execute(command, to_string=True).rstrip())
            except gdb.error as error:
                log("(%s: %s)" % (command, error))
        log("The frame above is the writer.  `continue` resumes the boot;")
        log("`detach` leaves the guest running; ringdump.py reads the ring.")
        return True


class FreeHook(gdb.Breakpoint):
    """kfree_ext: keep the element, let the free finish, then watch it."""

    def __init__(self):
        super().__init__("kfree_ext", gdb.BP_HARDWARE_BREAKPOINT, internal=True)

    def stop(self):
        if not armed:
            return False
        try:
            size = int(gdb.parse_and_eval("(unsigned long)$x2"))
            element = int(gdb.parse_and_eval("(unsigned long)$x1"))
        except gdb.error:
            return False
        if size != SIZE_CLASS or element == 0:
            return False
        if element in live:                 # already watching this one
            return False
        # Step out of kfree_ext first: the free itself writes into the element
        # (free-list linkage), which would fire the watchpoint immediately.
        try:
            gdb.execute("finish", to_string=True)
        except gdb.error:
            return False
        try:
            hook = WatchHit("*(unsigned long *)0x%x" % element, element)
        except gdb.error as error:
            log("[writewatch] watchpoint refused for 0x%x: %s" % (element, error))
            return False
        live[element] = hook
        while len(live) > MAX_WATCH:
            old_address, old_hook = live.popitem(last=False)
            try:
                old_hook.delete()
            except gdb.error:
                pass
            log("[writewatch] dropped the watchpoint on 0x%x" % old_address)
        log("[writewatch] watching 0x%x (%d live)" % (element, len(live)))
        return False                        # do not stop the boot


def main():
    print("[writewatch] size class %d, at most %d watchpoints, log %s"
          % (SIZE_CLASS, MAX_WATCH, LOG))
    try:
        gdb.execute("set pagination off")
        gdb.execute("set confirm off")
        gdb.execute("target remote :%s" % os.environ.get("WATCH_PORT", "1234"))
    except gdb.error as error:
        print("[writewatch] cannot attach: %s" % error)
        print("[writewatch] start a run with tools/qemu/run.sh --gdb first")
        return 1
    arm_hook()
    if armed:
        FreeHook()
    print("[writewatch] running; the boot is not paused unless something hits")
    try:
        gdb.execute("continue")
    except gdb.error:
        traceback.print_exc()
    print("[writewatch] %d hit(s), %d watchpoint(s) still live"
          % (hits, len(live)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
