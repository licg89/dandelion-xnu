#!/usr/bin/env python3
"""Switch XNU's arm64 timer from the virtual timer to the physical one.

Why: on dandelion the interrupt that LK/ATF leaves to EL1 non-secure is PPI 30, the
*physical* timer.  Proof, from Android running on the same phone:

    $ grep arch_timer /proc/interrupts
      3:  ...  GICv3  30 Level  arch_timer

XNU's arm64 code arms CNTV (the virtual timer, PPI 27) and deliberately masks CNTP.
Against this GIC that shows up exactly as measured on the device:

    ispender0=0                     nothing pending in the Group 1 NS view
    igroupr0=0                      PPI 27 is not a Group 1 NS interrupt
    gicd ctlr 10 -> 10              non-secure cannot open Group 1 at the GICD
    hppir1=3ff                      so no tick, and every deadline sleep blocks

XNU runs at EL1, like Linux, so it should use the same timer Linux uses.

Edits, all in the kernel:
  osfmk/arm64/machine_routines.c  set_tval / get_tval / ml_get_timer_pending and
                                  _enable_virtual_timer move to CNTP; CNTV is masked
  pexpert/pexpert/arm64/DANDELION.h  DANDELION_TIMER_IRQ 27 -> 30
  pexpert/arm/pe_fiq.c            the late-init self-test arms CNTP so that it tests
                                  the timer the kernel actually uses

Run inside the patched XNU tree, then generate the patch:

    py -3.13 tools/make_cntp_timer_patch.py D:\\aios\\xnu
"""
import os
import sys

EDITS = {
    "osfmk/arm64/machine_routines.c": [
        (
            '\t\t__builtin_arm_wsr64("CNTV_TVAL_EL0", (uint64_t)dec_value);',
            '\t\t/* dandelion: PPI 30 (CNTP) is the timer interrupt non-secure owns. */\n'
            '\t\t__builtin_arm_wsr64("CNTP_TVAL_EL0", (uint64_t)dec_value);',
        ),
        (
            '\t\twide_val = __builtin_arm_rsr64("CNTV_TVAL_EL0");',
            '\t\twide_val = __builtin_arm_rsr64("CNTP_TVAL_EL0");',
        ),
        (
            '\tuint64_t cntv_ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");\n'
            '\treturn ((cntv_ctl & CNTV_CTL_EL0_ISTATUS) != 0) ? TRUE : FALSE;',
            '\tuint64_t cntp_ctl = __builtin_arm_rsr64("CNTP_CTL_EL0");\n'
            '\treturn ((cntp_ctl & CNTP_CTL_EL0_ISTATUS) != 0) ? TRUE : FALSE;',
        ),
        (
            '\tuint64_t cntvctl = CNTV_CTL_EL0_ENABLE; /* One wants to use 32 bits, but "mrs" prefers it this way */\n'
            '\n'
            '\t__builtin_arm_wsr64("CNTV_CTL_EL0", cntvctl);\n'
            '\t/* disable the physical timer as a precaution, as its registers reset to architecturally unknown values */\n'
            '\t__builtin_arm_wsr64("CNTP_CTL_EL0", CNTP_CTL_EL0_IMASKED);',
            '\t/*\n'
            '\t * dandelion: the timer interrupt left to EL1 non-secure on this platform is\n'
            '\t * PPI 30 -- the physical timer -- which is the one Linux uses here too\n'
            '\t * (/proc/interrupts: "GICv3 30 Level arch_timer").  The virtual timer\'s PPI\n'
            '\t * 27 is not a Group 1 NS interrupt on this GIC, so arming CNTV produces a\n'
            '\t * pending local interrupt that nobody ever receives: every deadline sleep\n'
            '\t * then blocks forever.  Arm CNTP and mask CNTV instead.\n'
            '\t */\n'
            '\tuint64_t cntpctl = CNTP_CTL_EL0_ENABLE;\n'
            '\n'
            '\t__builtin_arm_wsr64("CNTP_CTL_EL0", cntpctl);\n'
            '\t__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);',
        ),
    ],
    "pexpert/pexpert/arm64/DANDELION.h": [
        (
            "#define DANDELION_TIMER_IRQ       27",
            "#define DANDELION_TIMER_IRQ       30   /* CNTP: what Linux uses on this GIC */",
        ),
    ],
    "pexpert/arm/pe_fiq.c": [
        (
            '\t\t\t__builtin_arm_wsr64("CNTV_TVAL_EL0", freq / 100);\n'
            '\t\t\t__builtin_arm_wsr64("CNTV_CTL_EL0", 1);',
            '\t\t\t__builtin_arm_wsr64("CNTP_TVAL_EL0", freq / 100);\n'
            '\t\t\t__builtin_arm_wsr64("CNTP_CTL_EL0", 1);',
        ),
    ],
}


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else r"D:\aios\xnu"
    total = 0
    for relative, edits in EDITS.items():
        path = os.path.join(root, relative.replace("/", os.sep))
        if not os.path.isfile(path):
            sys.exit("missing: " + path)
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read()
        for old, new in edits:
            if new in text:
                continue
            if old not in text:
                sys.exit("anchor missing in %s:\n%r" % (relative, old[:120]))
            text = text.replace(old, new, 1)
            total += 1
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
    print("applied %d edit(s) under %s" % (total, root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
