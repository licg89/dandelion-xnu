#!/usr/bin/env python3
"""Publish the eMMC hand-off record XNU's console looks for, from the firmware.

Why: dandelion has no UART, so the kernel's kprintf needs somewhere to go.
patches/xnu/dandelion-early-trace.patch implements that channel, and it does NOT
take its address from the device tree -- it reads a fixed record out of physical
memory:

    handoff = (struct dandelion_emmc_handoff *)DANDELION_EMMC_HANDOFF_PA;
    if (handoff->magic != DANDELION_EMMC_HANDOFF_MAGIC ||
        handoff->ok != 1 || handoff->sectors < 2) {
            printf ("eMMC log: bad handoff, channel disabled\\n");
            return;
    }

with, from pexpert/pexpert/arm64/DANDELION.h:

    struct dandelion_emmc_handoff {
        uint32_t magic;      /* 0x444D4844 "DHMD" */
        uint32_t ok;         /* 1: controller staged, region verified */
        uint32_t lba;        /* first sector of the log block */
        uint32_t sectors;    /* sectors reserved */
        uint32_t clock_div;  /* CKDIV the shim left programmed */
        uint32_t reserved;
    };
    #define DANDELION_EMMC_HANDOFF_PA    0x5FD00000ULL
    #define DANDELION_EMMC_HANDOFF_MAGIC 0x444D4844U

Nothing wrote it, which is why sector 30000 of flashinfo stayed all zeros while
the firmware log happily reached "hand-off to the kernel".

The kernel then checks two more things, and both are worth reporting on the same
line because either one silently disables the channel:

    dl_emmc_rd(0x114) != 0x20170426   -> "MSDC not where it should be"
    MSDC_CFG.CKSTB clear              -> "no MSDC clock"

0x5FD00000 sits inside the 561 MiB 'DRAM Low Conv' region this platform declares,
so the kernel's identity map covers it.

    python3 tools/patch_emmc_handoff.py
Idempotent.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT = os.path.join(REPO, "Platforms", "Xiaomi", "dandelionPkg", "Drivers",
                    "PayloadBootDxe", "PayloadBootDxe.c")

DEFINES = '''//
// The eMMC hand-off record XNU's console looks for.
//
// patches/xnu/dandelion-early-trace.patch reads a fixed record out of physical
// memory rather than from the device tree, and refuses the channel unless the
// magic and the "ok" flag are right:
//
//     handoff = (struct dandelion_emmc_handoff *)DANDELION_EMMC_HANDOFF_PA;
//     if (handoff->magic != DANDELION_EMMC_HANDOFF_MAGIC || handoff->ok != 1 ||
//         handoff->sectors < 2) { printf ("bad handoff, channel disabled"); ... }
//
// The layout is from pexpert/pexpert/arm64/DANDELION.h.  Nothing wrote it before,
// which is why the kernel's log region stayed all zeros while the firmware log
// reached "hand-off to the kernel" without complaint.
//
#define EMMC_HANDOFF_PA       0x5FD00000ULL
#define EMMC_HANDOFF_MAGIC    0x444D4844U   /* "DHMD" */
#define EMMC_LOG_LBA_OFFSET   30000u        /* sectors into flashinfo */
#define EMMC_LOG_SECTORS      512u
#define EMMC_LOG_CLKDIV       26u           /* what DandelionEmmcDxe calibrated */
#define EMMC_MSDC_BASE        0x11230000ULL
#define EMMC_MSDC_VERSION_OFS 0x114u
#define EMMC_MSDC_VERSION_WANT 0x20170426u

typedef struct {
  UINT32  Magic;
  UINT32  Ok;
  UINT32  Lba;
  UINT32  Sectors;
  UINT32  ClockDiv;
  UINT32  Reserved;
} EMMC_HANDOFF;

/**
  Publish the hand-off record, and report the two register checks the kernel makes.

  Both of the kernel's remaining refusals are silent unless they print through the
  channel they just disabled, so the firmware says them out loud instead: the MSDC
  version register has to read 0x20170426, and MSDC_CFG.CKSTB has to be set.
**/
STATIC
VOID
PublishEmmcHandoff (
  VOID
  )
{
  EFI_BLOCK_IO_PROTOCOL  *BlockIo;
  EFI_LBA                Start;
  EFI_LBA                Last;
  EMMC_HANDOFF           *Handoff;
  UINT32                 Version;
  UINT32                 Cfg;

  if (!FindFlashInfo (&BlockIo, &Start, &Last)) {
    return;
  }

  Version = *(volatile UINT32 *)(UINTN)(EMMC_MSDC_BASE + EMMC_MSDC_VERSION_OFS);
  Cfg     = *(volatile UINT32 *)(UINTN)(EMMC_MSDC_BASE + 0x00);

  Handoff           = (EMMC_HANDOFF *)(UINTN)EMMC_HANDOFF_PA;
  Handoff->Magic     = EMMC_HANDOFF_MAGIC;
  Handoff->Ok        = 1;
  Handoff->Lba       = (UINT32)(Start + EMMC_LOG_LBA_OFFSET);
  Handoff->Sectors   = EMMC_LOG_SECTORS;
  Handoff->ClockDiv  = EMMC_LOG_CLKDIV;
  Handoff->Reserved  = 0;

  DEBUG ((
    DEBUG_ERROR,
    "PayloadBoot: emmc handoff lba %lu sectors %u div %u; msdc ver 0x%08x (want 0x%08x) cfg 0x%08x ckstb %u\\n",
    (UINT64)(Start + EMMC_LOG_LBA_OFFSET),
    EMMC_LOG_SECTORS,
    EMMC_LOG_CLKDIV,
    Version,
    EMMC_MSDC_VERSION_WANT,
    Cfg,
    (Cfg >> 7) & 1u
    ));
}

'''


def main():
    with open(BOOT, "r", encoding="utf-8") as fh:
        text = fh.read()
    changed = 0

    if "PublishEmmcHandoff" not in text:
        anchor = "STATIC\nVOID\nFreezeConsole ("
        if anchor not in text:
            sys.exit("FreezeConsole anchor missing")
        text = text.replace(anchor, DEFINES + anchor, 1)
        changed += 1

    if "PublishEmmcHandoff ();" not in text:
        # call it just before the first flush, so the line lands in that flush
        anchor = "  if (!EFI_ERROR (FlushLogToFlash ())) {\n    Say (L\"PayloadBoot: firmware log written to flash\\r\\n\");\n  }"
        if anchor not in text:
            sys.exit("first-flush anchor missing")
        text = text.replace(
            anchor,
            "  PublishEmmcHandoff ();\n\n" + anchor, 1)
        changed += 1

    if changed:
        with open(BOOT, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
    print("patched %d site(s)" % changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
