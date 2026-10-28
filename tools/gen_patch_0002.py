#!/usr/bin/env python3
"""Regenerate Resources/MuPatches/0002-MsdcDxe-dandelion-panel-markers.patch.

Why this is a script and not a hand-written patch: the patch is a 300-line diff
against upstream MsdcDxe.c, and every hand edit of it has eventually been lost
(the build resets the tree with `git checkout --` and then re-applies the patch
series, so an edit that only exists in the tree survives exactly one build).

Run inside WSL (the build tree lives on ext4):

    wsl -d Ubuntu -e bash -lc 'python3 /mnt/d/aios/dandelion-xnu/tools/gen_patch_0002.py'

Overrides:
    UEFI_TREE   path to the uefi-a04br3 checkout   (default ~/uefi-a04br3)
    MU_PATCHES  output directory                   (default <repo>/Resources/MuPatches)

The script resets the file to its upstream content, applies patch 0001, applies
every edit below, and writes the resulting diff as patch 0002.  It is therefore
idempotent: run it as often as you like.
"""
import os
import subprocess
import sys

WORK = os.environ.get("UEFI_TREE", os.path.expanduser("~/uefi-a04br3"))
HERE = os.path.dirname(os.path.abspath(__file__))
PATCHDIR = os.environ.get("MU_PATCHES", os.path.join(os.path.dirname(HERE), "Resources", "MuPatches"))
REL = "Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c"
PATCH0001 = os.path.join(PATCHDIR, "0001-MediaTekPkg-MsdcDxe-mt6765-bringup.patch")
BASE = os.path.join("/tmp", "MsdcDxe.base.c")
OUT = os.path.join(PATCHDIR, "0002-MsdcDxe-dandelion-panel-markers.patch")

path = os.path.join(WORK, REL)

# Regenerate the 0001-applied baseline from the tree's own git object, so this
# script is self-contained (WSL may have recycled /tmp since the last run).
subprocess.run(["git", "-C", WORK, "checkout", "--", REL], check=True)
subprocess.run(["git", "-C", WORK, "apply", "-p1", PATCH0001], check=True)
subprocess.run(["cp", path, BASE], check=True)

text = open(path, "r", encoding="utf-8", errors="surrogateescape").read()
base = open(BASE, "r", encoding="utf-8", errors="surrogateescape").read()

if text != base:
    sys.exit("baseline snapshot does not match the working file")


# Sanity: the anchors and identifiers this patch depends on.
# Sanity: the anchors and identifiers this patch depends on.  UnknownCard and
# EmmcCard come from MsdcDxe.h, so only the .c-side anchors are checked here.
for needle in ("for (UINTN i = 0; i < gPlatformInfo.NumberOfHosts; i++)", "Private->SdInfo.CardType = EmmcCard"):
    if needle not in text:
        sys.exit("anchor missing: %r" % needle)


def _matches(src, anchor):
    return [i for i, line in enumerate(src.split("\n")) if line.strip() == anchor.strip()]


def insert_after(src, anchor, addition, count=1, skip=0):
    """Insert `addition` after the `count`-th match of `anchor`, plus `skip` lines."""
    lines = src.split("\n")
    hits = _matches(src, anchor)
    if len(hits) < count:
        sys.exit("anchor not found %d time(s): %r (found %d)" % (count, anchor, len(hits)))
    lines.insert(hits[count - 1] + 1 + skip, addition.rstrip("\n"))
    return "\n".join(lines)


def insert_before(src, anchor, addition, count=1):
    """Insert `addition` before the `count`-th match of `anchor`."""
    lines = src.split("\n")
    hits = _matches(src, anchor)
    if len(hits) < count:
        sys.exit("anchor not found %d time(s): %r (found %d)" % (count, anchor, len(hits)))
    lines.insert(hits[count - 1], addition.rstrip("\n"))
    return "\n".join(lines)


EXTERN = '''
//
// dandelion: panel markers for the card-identification path.
//
// The SoC library (MT6765Pkg/Library/MsdcImplLib) owns a framebuffer strip of
// 80x80 cells that records how far eMMC bring-up got, and exports exactly one
// symbol for the markers that have to live in this upstream file.  There is no
// library header, so the prototype is declared here.
//
extern VOID MsdcTrace (UINTN Stage, UINT32 Colour);

//
// dandelion: persistent numeric readout on the panel (see
// MsdcPanelValue in MT6765Pkg/Library/MsdcImplLib).  Console text scrolls
// away before a photo can be taken; these values do not.
//
extern VOID MsdcPanelValue (UINTN Slot, UINT32 Value);
'''

text = insert_after(text, '#include "MsdcDxe.h"', EXTERN)

# 8: both msdc0 memory regions resolved -- without this the host silently
#    `continue`s and no later marker can ever appear.  Only host 0 paints it:
#    the loop also walks MSDC-1 (the SD slot), and with one shared cell the
#    second host simply repainted host 0's result (measured 2026-10-29: the
#    cell came out red, i.e. the i != 0 branch, while host 0 was fine).
text = insert_after(
    text,
    "Private->TopMmioReg = Region.Address;",
    "    if (i == 0) {\n"
    "      MsdcTrace (8, 0x0000FF00);\n"
    "    }",
)

# 9: controller out of reset (MsdcReset polls MSDC_CFG_RST with no timeout).
text = insert_after(text, "MsdcReset (Private);", "  MsdcTrace (9, 0x00FFFF00);")

# 10: a command actually reached the bus during identification.  Gated on
#     UnknownCard so later EmmcDxe/BlockIo traffic does not repaint the cell.
# 18..21: which command the *driver model consumer* (EmmcDxe) is sending once
#     identification is done -- 18 CMD9 SEND_CSD, 19 CMD7 SELECT, 20 CMD10
#     SEND_CID, 21 CMD8 SEND_EXT_CSD.  EmmcDxe's DiscoverAllPartitions sends
#     CMD13, CMD7(0), CMD13, CMD9, CMD10, CMD7(1), CMD8+512B read, so the set
#     of cells that light up tells us exactly which step it died on.
text = insert_after(
    text,
    "MsdcWrite (Private, SDC_CMD, RawCmd);",
    "  if (Private->SdInfo.CardType == UnknownCard) {\n"
    "    MsdcTrace (10, 0x00FF00FF);\n"
    "  } else {\n"
    "    switch (CommandBlk->CommandIndex) {\n"
    "      case 7:  MsdcTrace (19, 0x0000FFFF); break;\n"
    "      case 8:  MsdcTrace (21, 0x00FFFFFF); break;\n"
    "      case 9:  MsdcTrace (18, 0x00FFFF00); break;\n"
    "      case 10: MsdcTrace (20, 0x00FF8000); break;\n"
    "      default: break;\n"
    "    }\n"
    "  }",
)

# 22: an inbound data transfer (the 512-byte ext-CSD read) finished -- green if
#     the PIO read returned success, red if it did not.  A "zeroed" ext CSD that
#     still reports success is invisible here, but ExtCsdRev < 5 would then make
#     EmmcDxe return EFI_UNSUPPORTED, which is one of the two ways to end up with
#     no block device after a successful card identification.
text = insert_after(
    text,
    "Status = MsdcPioRead (Private, Packet->InDataBuffer, Packet->InTransferLength);",
    "      if (Private->SdInfo.CardType != UnknownCard) {\n"
    "        MsdcTrace (22, EFI_ERROR (Status) ? 0x00FF0000 : 0x0000FF00);\n"
    "      }",
)

# 23: a command timed out / reported an error after identification.  This is the
#     "EmmcDxe sent something the card did not answer" cell.
text = insert_after(
    text,
    "Status = MsdcPollInterrupts (Private, MSDC_INT_CMDSTS, MSDC_INT_CMDRDY);",
    "  if (EFI_ERROR (Status) && (Private->SdInfo.CardType != UnknownCard)) {\n"
    "    MsdcTrace (23, 0x00FF0000);\n"
    "    DEBUG ((DEBUG_ERROR, \"dandelion cmd %u failed: status=0x%x\\n\", CommandBlk->CommandIndex, Status));\n"
    "  }",
)

# Data-transfer clock.  Measured 2026-10-29: after CMD2/CMD3/CMD9/CMD10 succeed
# (all command-only, at the identification clock), the first operation that
# moves DATA -- EmmcDxe's ext-CSD read, CMD8 + 512 bytes -- fails.  That is the
# classic "commands fine, data path not" signature and the cheapest thing to
# rule out is the 50 MHz bus clock that CardSetBusMode leaves behind, because at
# that rate the read relies on sampling/tuning values we have not verified.
# 26 MHz is the default-speed eMMC ceiling and is the rate everything up to this
# point already works at.
OLD_MCLK = "MsdcSetMclk(Private, 50 * 1000 * 1000);"
NEW_MCLK = ("  //\n"
            "  // dandelion: stay at default-speed 26 MHz instead of 50 MHz.\n"
            "  // 50 MHz is where the DATA path (not the command path) started failing.\n"
            "  //\n"
            "  MsdcSetMclk(Private, 26 * 1000 * 1000);")
if OLD_MCLK not in text:
    sys.exit("clock line not found")
text = text.replace(OLD_MCLK, NEW_MCLK)

# Do not put the card into high-speed mode.
#
# Measured 2026-10-29: every command-only step succeeds (CMD0/1/2/3/9/10/13, and
# CMD8 is accepted), but the first operation that moves data -- EmmcDxe's
# ext-CSD read, CMD8 + 512 bytes -- fails in MsdcPioRead.  CMD6(HS_TIMING=1)
# makes the CARD switch to high speed, but the HOST timing (MSDC_IOCON,
# pad delay/tuning registers) has to change with it and we have never verified
# that half.  Default speed at 26 MHz is the state in which everything else on
# this board already works, so keep the card there until data reads are known
# good; speed is irrelevant for reading one FAT partition.
OLD_HS = "    Status = EMMCSwitch(PassThru, 3, 185, 1, 0);"
NEW_HS = ("    //\n"
          "    // dandelion: skip HS_TIMING=1.  The card would switch to high speed\n"
          "    // while the host timing stays at default, which is the most likely\n"
          "    // reason a data read fails where every command succeeds.\n"
          "    //\n"
          "    Status = EFI_SUCCESS;")
if OLD_HS not in text:
    sys.exit("EMMCSwitch HS_TIMING line not found")
text = text.replace(OLD_HS, NEW_HS)

# ---------------------------------------------------------------------------
# MsdcPioRead: the actual reason a 512-byte read fails.
#
# Three defects against LK's msdc_pio_read() (lk-svoboda18, common/storage/mmc/
# msdc.c:1335-1393), any one of which turns a good transfer into an error:
#
#  1. It only wrote-1-to-clears XFER_COMPL.  Every other latched bit (the data
#     error bits among them) stays set, so a stale DATTMO/ACMDCRCERR from an
#     earlier command is read back as a data error here.
#  2. It required the FIFO to report at least 128 bytes (MSDC_FIFO_SIZE) before
#     draining anything, while the hardware threshold is 64.  On a 512-byte read
#     the count never reaches 128, nothing is ever drained, RemainSize stays at
#     512, and when XFER_COMPL finally arrives the function returns EFI_ABORTED
#     via the "Data not fully read!" branch.  That is exactly the symptom seen on
#     hardware: every command works, the first data transfer fails.
#  3. It treats XFER_COMPL as "the FIFO is empty".  It is not: up to a
#     threshold's worth of bytes can still be queued.
#
# Also drops the ASSERT (FALSE) on leftover bytes -- in a DEBUG build that is a
# CpuDeadLoop, i.e. an unexplained freeze.
#
# The replacement drains whatever the FIFO currently reports, four bytes at a
# time, and only finishes when the transfer is both complete and fully consumed.
OLD_PIO = """  UINT32 IntStatus, ChunkSize, RemainSize, RxBytes;
  UINT8 *ByteBuffer = (UINT8 *)Buffer;

  RemainSize = BufferLength;

  MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_DATSTS);

  while (TRUE) {
    MsdcRead (Private, MSDC_INT, &IntStatus);

    if (IntStatus & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_ACMDTMO | MSDC_INT_ACMDCRCERR)) {
      return MsdcIntTrackError (Private, MSDC_INT_DATSTS, IntStatus);
    }

    ChunkSize = RemainSize > MSDC_FIFO_SIZE ? MSDC_FIFO_SIZE : RemainSize;
    MsdcFifoRxBytes (Private, &RxBytes);

    if (RemainSize == 0 && RxBytes) {
      ASSERT (FALSE);
    }

    if (RxBytes >= ChunkSize) {
      MsdcFifoRead (Private, ByteBuffer, ChunkSize);
      ByteBuffer += ChunkSize;
      RemainSize -= ChunkSize;
    }

    if (IntStatus & MSDC_INT_XFER_COMPL) {
      MsdcWrite (Private, MSDC_INT, IntStatus & MSDC_INT_XFER_COMPL);

      if (RemainSize) {
        DEBUG ((DEBUG_ERROR, "MsdcDxe: Data not fully read!\\n"));
        return EFI_ABORTED;
      }

      break;
    }
  }

  return EFI_SUCCESS;
}"""

NEW_PIO = """  UINT32  IntStatus, RemainSize, RxBytes, Words;
  UINT32  Spins;
  UINT8   *ByteBuffer = (UINT8 *)Buffer;

  RemainSize = BufferLength;
  Spins      = 0;

  MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_DATSTS);

  while (TRUE) {
    MsdcRead (Private, MSDC_INT, &IntStatus);

    //
    // Errors are checked before anything else, and every bit that was latched
    // is cleared after handling, so the next command cannot inherit them.
    //
    if (IntStatus & (MSDC_INT_DATTMO | MSDC_INT_DATCRCERR | MSDC_INT_ACMDTMO | MSDC_INT_ACMDCRCERR)) {
      UINT32  DbgCfg, DbgFifo, DbgSts, DbgDcrc;

      //
      // The panel is the only debug channel this board has: print the evidence
      // rather than guessing which of the four error bits fired.  Two lines so
      // nothing wraps at 90 columns.
      //
      MsdcRead (Private, MSDC_CFG, &DbgCfg);
      MsdcRead (Private, MSDC_FIFOCS, &DbgFifo);
      MsdcRead (Private, SDC_STS, &DbgSts);
      MsdcRead (Private, SDC_DATCRC_STS, &DbgDcrc);
      DEBUG ((
        DEBUG_ERROR,
        "ddn pio-read FAIL int=%08x left=%u\\n",
        IntStatus,
        RemainSize
        ));
      DEBUG ((
        DEBUG_ERROR,
        "ddn  cfg=%08x fifo=%08x sts=%08x dcrc=%08x\\n",
        DbgCfg,
        DbgFifo,
        DbgSts,
        DbgDcrc
        ));

      // MSDC_PS: the pin levels the controller sees right now.  DAT0 is bit 0;
      // if the card is driving a data block at least one DAT bit must have
      // moved, so an unchanged/zero PS means nothing arrived on the pads at all
      // (wiring or card state) rather than a sampling problem.
      {
        UINT32  DbgPs;
        MsdcRead (Private, MSDC_PS, &DbgPs);
        DEBUG ((DEBUG_ERROR, "ddn  ps=%08x\\n", DbgPs));
      }
      //
      // Also park the numbers in the fixed readout band.  The console keeps
      // only its last 68 lines and EmmcDxe/BdsDxe print more than that
      // afterwards, so text alone cannot be relied on to still be on screen by
      // the time the panel is photographed.
      //
      {
        UINT32  DbgCmd, DbgSdc, DbgBlk;

        MsdcRead (Private, SDC_CMD, &DbgCmd);
        MsdcRead (Private, SDC_CFG, &DbgSdc);
        MsdcRead (Private, SDC_BLK_NUM, &DbgBlk);
        MsdcPanelValue (0, IntStatus);
        MsdcPanelValue (1, DbgCfg);
        MsdcPanelValue (2, DbgFifo);
        MsdcPanelValue (3, DbgSts);
        MsdcPanelValue (4, DbgDcrc);
        MsdcPanelValue (5, RemainSize);
        MsdcPanelValue (7, DbgCmd);
        MsdcPanelValue (8, DbgSdc);
        MsdcPanelValue (9, DbgBlk);
      }

      MsdcWrite (Private, MSDC_INT, IntStatus);
      return MsdcIntTrackError (Private, MSDC_INT_DATSTS, IntStatus);
    }

    //
    // Drain whatever the FIFO reports right now -- no threshold assumption.
    // The old code waited for 128 bytes (MSDC_FIFO_SIZE) while the hardware
    // reports at most the threshold (64), so it never drained at all.
    //
    MsdcFifoRxBytes (Private, &RxBytes);
    Words = RxBytes / 4;
    while ((Words > 0) && (RemainSize >= 4)) {
      MsdcFifoRead (Private, ByteBuffer, 4);
      ByteBuffer += 4;
      RemainSize -= 4;
      Words--;
    }

    //
    // Tail: fewer than four bytes left in the buffer but the FIFO still holds
    // data.  Read them one at a time so nothing is left behind.
    //
    while ((RxBytes >= 4) && (RemainSize > 0) && (RemainSize < 4)) {
      MsdcFifoRead (Private, ByteBuffer, RemainSize);
      ByteBuffer += RemainSize;
      RemainSize  = 0;
    }

    if (IntStatus & MSDC_INT_XFER_COMPL) {
      MsdcWrite (Private, MSDC_INT, IntStatus);
      if (RemainSize == 0) {
        break;
      }
      //
      // Card finished but bytes are still queued: keep looping to drain them.
      // Only give up if the FIFO stops moving entirely.
      //
    } else {
      MsdcWrite (Private, MSDC_INT, IntStatus);
    }

    if (RemainSize == 0) {
      //
      // Everything the caller asked for has been read; the remaining FIFO bytes
      // (if any) belong to nothing we need, so stop here rather than spin.
      //
      break;
    }

    if (++Spins > 2000000) {
      DEBUG ((DEBUG_ERROR, "MsdcDxe: PIO read stalled with %u bytes left\\n", RemainSize));
      return EFI_TIMEOUT;
    }
  }

  //
  // One line per read would drown the panel; the first two are enough to show
  // that a transfer completed and how much was still sitting in the FIFO.
  //
  {
    STATIC UINT32  DbgOk;

    if (DbgOk < 2) {
      UINT32  DbgFifo;

      DbgOk++;
      MsdcRead (Private, MSDC_FIFOCS, &DbgFifo);
      DEBUG ((DEBUG_ERROR, "ddn pio-read ok len=%u fifo=%08x\\n", BufferLength, DbgFifo));
    }
  }

  return EFI_SUCCESS;
}"""

if OLD_PIO not in text:
    sys.exit("MsdcPioRead body not found")
text = text.replace(OLD_PIO, NEW_PIO)

# PATCH_BIT2: write the full family default before the field tweaks, in the same
# order LK does (msdc.c:1863 writes 0x14801803, then adjusts fields).  Writing
# only a few fields leaves the rest at whatever the bootloader left behind, and
# several of those bits are about data CRC/response timing.
OLD_PB1 = "  MsdcWrite (Private, MSDC_PATCH_BIT1, 0xFFFA0349);"
NEW_PB1 = ("  MsdcWrite (Private, MSDC_PATCH_BIT1, 0xFFFA0349);\n"
           "\n"
           "  //\n"
           "  // dandelion: PATCH_BIT2 was never initialised, only poked, so everything\n"
           "  // except the three fields below kept whatever the bootloader left there.\n"
           "  // 0x3489180F is LK's full post-initialisation value for MT6765\n"
           "  // (MSDC_PB2_DEFAULT 0x14801803 from msdc_io.h:317, then RESPWAIT=3,\n"
           "  // CFGCRCSTS, CRCSTSENSEL=1, RESPSTENSEL=1, DDR50SEL, POPENCNT=8,\n"
           "  // CFGRESPCNT=1, INTCRESPSEL=1, CFGCRCSTSCNT=1).  Those bits are response\n"
           "  // and data CRC sampling -- exactly what a data read depends on.\n"
           "  //\n"
           "  MsdcWrite (Private, MSDC_PATCH_BIT2, 0x3489180F);")
if OLD_PB1 not in text:
    sys.exit("PATCH_BIT1 line not found")
text = text.replace(OLD_PB1, NEW_PB1)

# Drop every latched interrupt bit before the command goes out.
#
# Reference: Platforms/Benesse/a04br3Pkg/Drivers/SdCardBlockIoDxe does exactly
# this (SdWrite32 (MSDC_INT, SdRead32 (MSDC_INT))) before every command, and it
# is the only MSDC implementation in this tree that is known to move data on
# real MT6765 hardware.  MSDC_INT is latched status: our driver only ever
# write-1-clears the single bit it is waiting for, so a data-error bit left over
# from an earlier command is still set when the next transfer reads the register
# -- which looks exactly like "the read failed instantly".
OLD_CLRINT = "MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_CMDSTS);"
NEW_CLRINT = (
    "MsdcClrBits (Private, MSDC_INTEN, MSDC_INT_CMDSTS);\n"
    "\n"
    "  {\n"
    "    UINT32  StaleInt;\n"
    "\n"
    "    //\n"
    "    // dandelion: clear whatever an earlier command left latched.\n"
    "    //\n"
    "    MsdcRead (Private, MSDC_INT, &StaleInt);\n"
    "    if (StaleInt != 0) {\n"
    "      MsdcWrite (Private, MSDC_INT, StaleInt);\n"
    "    }\n"
    "  }"
)
if text.count(OLD_CLRINT) != 1:
    sys.exit("interrupt-disable anchor appears %d times" % text.count(OLD_CLRINT))
text = text.replace(OLD_CLRINT, NEW_CLRINT)

# Re-assert PIO mode (and clear the FIFO) right before a data command.
#
# LK does this immediately before every PIO data phase (msdc.c:2625,
# mmc_core.c:561; vendor msdc_dma_off() sd.c:162/2931/3227).  Our driver sets
# MSDC_CFG_PIO once at init, *before* the soft reset that MsdcReset() performs,
# and never again -- so if the reset or the bootloader left the controller in
# DMA mode, the FIFO never pops and any data read times out at any clock.  That
# failure mode is identical at 26 and 50 MHz, which is what the panel shows.
OLD_BLKNUM = "    MsdcWrite (Private, SDC_BLK_NUM, BlkLen);"
NEW_BLKNUM = ("    MsdcWrite (Private, SDC_BLK_NUM, BlkLen);\n"
              "\n"
              "    //\n"
              "    // dandelion: re-assert PIO mode immediately before the command, and\n"
              "    // drop whatever is left in the FIFO from the previous transfer.\n"
              "    //\n"
              "    MsdcSetBits (Private, MSDC_CFG, MSDC_CFG_PIO);\n"
              "    MsdcSetBits (Private, MSDC_FIFOCS, MSDC_FIFOCS_CLR);")
if OLD_BLKNUM not in text:
    sys.exit("SDC_BLK_NUM line not found")
text = text.replace(OLD_BLKNUM, NEW_BLKNUM)




# 11: CMD1 answered and OCR bit31 is set (eMMC path only -- first occurrence).
lines = text.split("\n")
for idx, line in enumerate(lines):
    if line.strip() == "} while ((Ocr & BIT31) == 0);":
        lines.insert(idx + 1, "  MsdcTrace (11, 0x0000FFFF);")
        break
else:
    sys.exit("eMMC OCR loop anchor not found")
text = "\n".join(lines)

# 12: CMD2 + CMD3 done and CardType = EmmcCard, i.e. EmmcDxe can bind.
text = insert_after(
    text,
    "Private->SdInfo.CardType = EmmcCard;",
    "  MsdcTrace (12, 0x00FFFFFF);",
)

# 13: CardSetBusMode (CMD7 select + CMD6 HS_TIMING + CMD13 + 50 MHz switch).
#     This is the last thing EmmcIdentification does, and a failure here makes
#     InitMsdc skip the pass-thru install entirely -- which looks exactly like
#     "card identified but no block device".
text = insert_after(
    text,
    "Status = CardSetBusMode (Private, 1);",
    "  MsdcTrace (13, EFI_ERROR (Status) ? 0x00FF0000 : 0x0000FF00);",
)

# 14: reached the pass-thru install -- i.e. the detect loop succeeded.
#     Inserted BEFORE the call: the call spans several lines, and inserting
#     after its first line (as a first attempt did) breaks the argument list.
# 15: the detect loop failed (red), which is the other way to get no block device.
text = insert_before(
    text,
    "Status = gBS->InstallMultipleProtocolInterfaces (",
    "    MsdcTrace (14, 0x0000FFFF);\n",
)
text = insert_after(
    text,
    'DEBUG ((DEBUG_ERROR, "MsdcDxe: Failed to detect card! %r\\n", Status));',
    "      MsdcTrace (15, 0x00FF0000);",
)

# 16: MsdcBuildDevicePath was called -- this is the first thing a driver binding
#     on top of us does, so it distinguishes "EmmcDxe never looked at us" from
#     "EmmcDxe looked and then failed".
# 17: a pass-through command arrived AFTER identification, i.e. from EmmcDxe
#     rather than from MsdcDxe's own identification sequence.
text = insert_after(
    text,
    "if (This == NULL || DevicePath == NULL || Slot != 0) {",
    "  MsdcTrace (16, 0x00FFFF00);",
    skip=4,
)
text = insert_before(
    text,
    "return MsdcSendCmd (Private, Packet);",
    "  if (Private->SdInfo.CardType != UnknownCard) {\n"
    "    MsdcTrace (17, 0x0000FFFF);\n"
    "  }\n",
)


# ---------------------------------------------------------------------------
# The card's own answer to the data command, parked in readout slot 6.
#
# R1 bits 12:9 are CURRENT_STATE (4 = Tran, the only state in which a card will
# send data), bit 8 is READY_FOR_DATA, bit 22 is ILLEGAL_COMMAND.  Between them
# they separate "the card refused the command" from "the card went quiet", which
# is exactly the question a DATTMO with an empty FIFO leaves open.
text = insert_before(
    text,
    "      Status = MsdcPioRead (Private, Packet->InDataBuffer, Packet->InTransferLength);",
    "      MsdcPanelValue (6, SdMmcStatusBlk->Resp0);",
)

# ---------------------------------------------------------------------------
# Signal-integrity registers, as LK sets them.
#
# These are the registers that decide where the controller samples the DATA
# lines, and the original code wrote only MSDC_IOCON.  Everything else kept
# whatever the bootloader left behind, which is fine for commands (they work) and
# fatal for data.  Values and order from lk-svoboda18 msdc.c:1855-1875
# (msdc_init_hw), which is the same controller on the same SoC:
#
#   MSDC_PAD_TUNE0/1   0xf0 / 0xf4    RX delay line
#   MSDC_DAT_RDDLY0-3  0xf8..0x104    per-lane data read delay
#   SDC_FIFO_CFG b24/25               FIFO valid selects -- LK clears both, with
#                                     the comment that leaving them set breaks
#                                     the data path
OLD_IOCON = "  MsdcWrite (Private, MSDC_IOCON, 0);"
NEW_IOCON = (
    "  MsdcWrite (Private, MSDC_IOCON, 0);\n"
    "\n"
    "  //\n"
    "  // dandelion: the rest of LK's msdc_init_hw() signal path.\n"
    "  //\n"
    "  MsdcWrite (Private, 0xf0, 0);   // MSDC_PAD_TUNE0\n"
    "  MsdcWrite (Private, 0xf4, 0);   // MSDC_PAD_TUNE1\n"
    "  MsdcWrite (Private, 0xf8, 0);   // MSDC_DAT_RDDLY0\n"
    "  MsdcWrite (Private, 0xfc, 0);   // MSDC_DAT_RDDLY1\n"
    "  MsdcWrite (Private, 0x100, 0);  // MSDC_DAT_RDDLY2\n"
    "  MsdcWrite (Private, 0x104, 0);  // MSDC_DAT_RDDLY3\n"
    "  MsdcClrBits (Private, SDC_FIFO_CFG, (BIT24 | BIT25));"
)
if text.count(OLD_IOCON) != 1:
    sys.exit("MSDC_IOCON anchor appears %d times" % text.count(OLD_IOCON))
text = text.replace(OLD_IOCON, NEW_IOCON)

# ---------------------------------------------------------------------------
# SDC_CFG_SDIO is an SDIO-card bit and was set unconditionally ("otherwise cmd5
# won't work").  LK sets it only when the card really is SDIO; on this board
# host 0 is an eMMC and left set it changes how the controller treats the data
# phase -- the last place to look when every command is answered and the first
# data transfer times out.
OLD_SDIO = "  MsdcSetBits (Private, SDC_CFG, SDC_CFG_SDIO);"
NEW_SDIO = (
    "  //\n"
    "  // dandelion: only for SDIO hosts.  Host 0 on this board is the eMMC.\n"
    "  //\n"
    "  if (Private->Index != 0) {\n"
    "    MsdcSetBits (Private, SDC_CFG, SDC_CFG_SDIO);\n"
    "  } else {\n"
    "    MsdcClrBits (Private, SDC_CFG, SDC_CFG_SDIO);\n"
    "  }"
)
if text.count(OLD_SDIO) != 1:
    sys.exit("SDC_CFG_SDIO anchor appears %d times" % text.count(OLD_SDIO))
text = text.replace(OLD_SDIO, NEW_SDIO)

open(path, "w", encoding="utf-8", errors="surrogateescape", newline="").write(text)

# Emit the patch: 0001-applied state -> final state.
diff = subprocess.run(
    ["diff", "-u", "--label", "a/" + REL, "--label", "b/" + REL, BASE, path],
    capture_output=True, text=True,
)
if diff.returncode != 1:
    sys.exit("unexpected diff result: %d %s" % (diff.returncode, diff.stderr))
open(OUT, "w", encoding="utf-8", newline="\n").write(diff.stdout)
print("wrote %s (%d lines)" % (OUT, diff.stdout.count("\n")))
