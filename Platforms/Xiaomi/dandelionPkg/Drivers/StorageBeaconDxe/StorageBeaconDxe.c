/**
  M4 storage beacon: connect the eMMC stack, then report how far it got.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Why this driver has to *connect*, not just report (2026-10-29):

  MsdcDxe installs gEfiSdMmcPassThruProtocolGuid and EmmcDxe is a UEFI_DRIVER
  whose DriverBinding sits on top of it, but nothing in this firmware calls
  gBS->ConnectController for that handle.  Normally BDS does that for the whole
  platform (EfiBootManagerConnectAll) -- and on this board BDS either never gets
  to run or is not to be trusted yet, so the pass-thru handle stayed unbound and
  no block device ever appeared.  Measured on hardware: MsdcDxe's own markers
  showed CMD2/CMD3 succeeding (card identified, EmmcDxe-visible subtype), yet
  neither BlockIo nor SimpleFileSystem ever appeared.

  So this driver, dispatched the moment the pass-thru protocol is installed,
  does the connect sweep itself: for every handle, ConnectController with
  Recursive = TRUE.  That starts EmmcDxe (-> BlockIo), then PartitionDxe and
  FatPkg (-> SimpleFileSystem), which is exactly the order BDS would have used.

  Reporting is the same framebuffer cell strip MsdcImplLib uses, because ConOut
  does not exist yet at this point:

      cell 6 yellow  this driver ran, i.e. the pass-thru protocol is installed
      cell 4 purple  at least one BlockIo handle exists
      cell 5 green   a SimpleFileSystem mounted

  ---------------------------------------------------------------------------
  The power probe (2026-10-29, section 5 of docs/RESUME-1029.md)

  Everything above assumes the card answers.  It does not, and the first
  explanation tried was "it has no power" -- LK used to leave the eMMC rails on,
  but MsdcImplLib's PowerControl() is deliberately gutted (the in-tree PMIC
  driver is MT6358's, dandelion carries an MT6357).  This driver measured it
  instead of assuming it:

      P_before   MSDC_PS, read as the very first load this driver performs
      V          MT6357 LDO_VEMC/VMC/VIO18 CON0 + VEMC_ANA_CON0 via the wrapper
      P_after    MSDC_PS again, after V and a short settle

  Hardware, 2026-10-29 (IMG_5738): P = 0x81FF0002 both times, VEMC CON0 = 0x0001.
  With the real MSDC_PS layout (DAT at bits 23..16, CMD at bit 24) that means
  DAT = 0xFF, CMD = 1, card present, rail enabled -- so "no power" is refuted,
  and the next question is whether the card can hear the controller at all.
  That is what ProbeCardAndMux() answers: a hand-sent CMD1 with the pin state
  sampled in a tight loop, then the same ping in the other pin-mux mode.

  Reading table: docs/POWER-BEFORE-AFTER.md.  Deliberately no stage cells: 16..23
  are painted by MsdcDxe/EmmcDxe, and this driver runs after them, so a cell
  here would erase the M4 evidence.
**/

#include <Uefi.h>

#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/BlockIo.h>
#include <Protocol/SimpleFileSystem.h>

#define BEACON_FB_BASE     0x7EC50000ULL
#define BEACON_FB_STRIDE   2944
#define BEACON_FB_PIXELS   736
// Same three-row strip as MsdcImplLib: y 1400 / 1448 / 1496, cells 80x44 on an
// 88-pixel pitch, stages 0..7 in row A, 8..15 in row B, 16..23 in row C.
#define BEACON_STAGE_SIZE  80
#define BEACON_STAGE_HIGH  44
#define BEACON_STAGE_PITCH 88
#define BEACON_STAGE_LEFT  8
#define BEACON_ROW0_TOP    1400
#define BEACON_ROW1_TOP    1448
#define BEACON_ROW2_TOP    1496

//
// MSDC0 register window (LK: platform/mt6765/include/platform/mt_reg_base.h,
// and bootshim/msdc.c uses the same value).  Offsets follow the MTK MSDC block;
// the upstream MsdcDxe header has the same ones -- keep them in sync if either
// side changes.  Read-only here, see ProbeMsdcLeftovers().
//
#define MSDC0_MMIO_BASE    0x11230000ULL
#define MSDC1_MMIO_BASE    0x11240000ULL
#define MSDC_CFG_OFS       0x00
#define MSDC_PS_OFS        0x08
#define MSDC_INT_OFS       0x0C
#define MSDC_INTEN_OFS     0x10
#define SDC_CFG_OFS        0x30
#define SDC_CMD_OFS        0x34
#define SDC_ARG_OFS        0x38
#define SDC_STS_OFS        0x3C
#define SDC_RESP0_OFS      0x40
#define SDC_BLK_NUM_OFS    0x50
//
// Pad configuration (mtk-sd.c: MSDC_PAD_CTL0 = 0xE0, MSDC_PAD_CTL1 = 0xE4).
// Read-only here: if these came back zero the pads would not be driving at all,
// which would explain a command that never reaches the card.
//
#define MSDC_PAD_CTL0_OFS  0xE0
#define MSDC_PAD_CTL1_OFS  0xE4

#define SDC_STS_BUSY       (1u << 0)
#define SDC_STS_CMDBUSY    (1u << 1)
#define SDC_CMD_STOP_CMD   (1u << 14)
#define SDC_CMD_RSP_SHIFT  7
#define MSDC_INT_CMDRDY    (1u << 8)
#define MSDC_INT_CMDTMO    (1u << 9)

//
// MSDC_PS is pin state, not controller state, and the field layout is NOT
// "DAT in the low byte" -- that misreading is what produced the "the card has
// no power" conclusion in docs/RESUME-1029.md section 3.  The vendor header
// (ComboA/msdc_reg.h, and the same masks in mtk-sd.c upstream) says:
//
//   BIT(0)      CDEN          card-detect enable          (RW)
//   BIT(1)      CDSTS         card-detect status          (R)
//   BIT(15:12)  CDDEBOUNCE                                (RW)
//   BIT(23:16)  DAT[7:0]      data lines, one bit each    (R)
//   BIT(24)     CMD                                       (R)
//   BIT(31)     WP            write protect               (R)
//
// Measured on this board: PS = 0x81FF0002 -> DAT = 0xFF, CMD = 1, WP = 1,
// CDSTS = 1: a powered card sitting on a clean, pulled-up idle bus.
//
#define MSDC_PS_DAT_SHIFT  16
#define MSDC_PS_DAT_MASK   0xFFu
#define MSDC_PS_CMD_BIT    (1u << 24)
#define MSDC_PS_CDSTS_BIT  (1u << 1)
#define MSDC_PS_WP_BIT     (1u << 31)

//
// Pinctrl / GPIO (MT6765): base 0x10005000 (GpioImplLib.c gPlatformInfo:
// ModeOffset 0x300, DataInOffset 0x200, 8 pins per register, 4 bits per pin).
// Pins 122..133 are the eMMC0 group: mode register 0x3F0 covers 120..127 and
// 0x400 covers 128..135, which is exactly the vendor's MSDC0_GPIO_MODE0/1.
//
#define PINCTRL_MMIO_BASE    0x10005000ULL
#define PINCTRL_DIN_OFS      0x200
#define PINCTRL_MODE_OFS     0x300
#define PINCTRL_MODE_STRIDE  0x10
#define PINCTRL_STRAP_ADDR   0x100056F0ULL
#define PINCTRL_STRAP_BIT    13
#define EMMC_PIN_FIRST       122
#define EMMC_PIN_LAST        133

//
// SDC_CMD response type field (MsdcDxe.c switch on ResponseType):
// R1/R5/R6/R7 = 1, R2 = 2, R3 = 3, R4 = 4, R1b = 7, shifted left by 7.
//
#define SDC_CMD_RSP_R1      1u
#define SDC_CMD_RSP_R3      3u
//
// CMD1 argument: high-capacity support (bit 30) and the 3.2-3.4 V window,
// i.e. what an eMMC answers with OCR bit 31 set.  Only used to see whether the
// card reacts at all; the exact value does not matter for that.
//
#define CMD1_OCR            0x40FF8080u

//
// PMIC wrapper (PWRAP) at 0x1000D000 on MT6765/MT6762G: vendor DT
// pwrap@1000d000, LK PWRAP_BASE.  The offsets and the WACS2 encoding below are
// the ones PmicWrapperImplLib.c selects for SOC_TYPE == 1 and the ones
// MediaTekPkg's PmicWrapperDxe uses (upstream repository), so this is the same
// transaction the PMIC driver itself issues -- just with a ceiling on it.
//
#define PWRAP_MMIO_BASE        0x1000D000ULL
#define PWRAP_INIT_DONE2_OFS   0x0A0
#define PWRAP_WACS2_CMD_OFS    0xC20
#define PWRAP_WACS2_RDATA_OFS  0xC24
#define PWRAP_WACS2_VLDCLR_OFS 0xC28
#define PWRAP_FSM_SHIFT        16
#define PWRAP_FSM_MASK         0x7
#define PWRAP_FSM_IDLE         0
#define PWRAP_FSM_WF_VLDCLR    6
#define PWRAP_RETRIES          200
#define PWRAP_RETRY_STALL_US   50   // 200 x 50 us = 10 ms per transaction, hard cap

//
// MT6357 regulator addresses.  dandelion carries an MT6357, NOT the MT6358 the
// in-tree PMIC driver was written for, and the two maps do not overlap:
//
//   rail             MT6357 (used here)  MT6358 (what MT6358PmicDxe pokes)
//   ldo_vemc.Con0    0x1988              0x1B1C
//   ldo_vmc.Con0     0x1AB0              0x1CC4
//   ldo_vmch.Con0    0x1AC4              0x1CD8
//
// Sources: LK platform/mt6765/include/platform/upmu_hw.h (MT6357 map), mainline
// drivers/regulator/mt6357-regulator.c with include/linux/mfd/mt6357/
// registers.h, and the vendor kernel's power_gs_6357 golden-setting tables.
//
// Where each field lives -- this cost a wrong reading once, so it is written
// down: mainline's MT6357_LDO() takes (enreg, vosel, vosel_mask), and the table
// entry is
//
//   MT6357_LDO("ldo-vemc", VEMC, vemc_voltages,
//              MT6357_LDO_VEMC_CON0, MT6357_VEMC_ANA_CON0, 0x700)
//
// so **CON0 bit 0 = enable** and **VOSEL bits 10..8 live in ANA_CON0 (0x1C20)**,
// not in CON0.  Reading VOSEL out of CON0 prints "vosel=0 0mV", which is
// meaningless.  vemc_voltages[] maps VOSEL 2/3/5 to 2.9/3.0/3.3 V.
//
#define MT6357_LDO_VIO18_CON0   0x1958
#define MT6357_LDO_VEMC_CON0    0x1988
#define MT6357_LDO_VMC_CON0     0x1AB0
#define MT6357_VEMC_ANA_CON0    0x1C20
#define MT6357_VMC_ANA_CON0     0x1C3C
#define MT6357_LDO_ENABLE_BIT   (1u << 0)
#define MT6357_VEMC_VOSEL_SHIFT 8
#define MT6357_VEMC_VOSEL_MASK  0x7

//
// Panel rows: directly under the two status banners (1100 and 1140, 30 px
// tall) and above the numeric readout that starts at 1244.  Being outside
// DandelionConsoleDxe's text area (rows 0..67), these rows survive BDS's screen
// resets, which the console text does not.
//
#define BEACON_STRIP_PS_BEFORE 1172   // idle pin state, first read
#define BEACON_STRIP_CMD_AND   1186   // PS bits that stayed high during CMD1
#define BEACON_STRIP_CMD_OR    1200   // PS bits ever high during CMD1
#define BEACON_VERDICT_LEFT    400
#define BEACON_VERDICT_TOP     1197
#define BEACON_VERDICT_W       160
#define BEACON_VERDICT_H       12
#define BEACON_DOTS_TOP        1216
#define BEACON_DOTS_VOLUMES_X  380

//
// MT6357 ldo_vemc voltage table (mainline mt6357-regulator.c, vemc_voltages[]),
// indexed by VOSEL.  Zero means "not a documented step".
//
STATIC CONST UINT16  mVemcMillivolts[8] = { 0, 0, 2900, 3000, 0, 3300, 0, 0 };

/**
  Repaint the status band (y 40..47, full width).

  Same strip that DandelionConsoleDxe (magenta) and the MSDC stages
  (yellow/orange/cyan) use, so the colour on screen at reset is the last stage
  reached.  Purple = block device, green = filesystem.
**/
STATIC
VOID
PaintBand (
  IN UINT32  Colour
  )
{
  volatile UINT32  *Band;
  UINTN            Y;
  UINTN            X;

  for (Y = 40; Y < 48; Y++) {
    Band = (volatile UINT32 *)(UINTN)(BEACON_FB_BASE + (UINT64)Y * BEACON_FB_STRIDE);
    for (X = 0; X < BEACON_FB_PIXELS; X++) {
      Band[X] = Colour;
    }
  }
}

/**
  Paint one of the stage cells at the bottom of the panel.

  Same strip MsdcImplLib paints (eight 80x80 cells on an 88-pixel pitch at
  y 1400, plus a shorter second row at y 1488); this driver owns cells 4, 5 and 6
  so the whole eMMC chain reports into one row of cells.
**/
STATIC
VOID
StageCell (
  IN UINTN   Stage,
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            Left;
  UINTN            Top;
  UINTN            X;
  UINTN            Y;

  if (Stage > 23) {
    return;
  }

  if (Stage < 8) {
    Top = BEACON_ROW0_TOP;
  } else if (Stage < 16) {
    Top = BEACON_ROW1_TOP;
  } else {
    Top = BEACON_ROW2_TOP;
  }

  Left = BEACON_STAGE_LEFT + (Stage & 7) * BEACON_STAGE_PITCH;
  for (Y = Top; Y < Top + BEACON_STAGE_HIGH; Y++) {
    Row = (volatile UINT32 *)(UINTN)(BEACON_FB_BASE + (UINT64)Y * BEACON_FB_STRIDE) + Left;
    for (X = 0; X < BEACON_STAGE_SIZE; X++) {
      Row[X] = Colour;
    }
  }
}

/**
  Paint a full-width banner below the text area.

  y 1100 and up is outside DandelionConsoleDxe's text area (rows 0..67), so
  ClearScreen/ScrollUp/SetMode -- and therefore BDS's own console reset -- cannot
  erase it.  That matters: by the time BDS prints "No Operating System Found",
  everything it printed has scrolled or been cleared, but this banner stays.
**/
STATIC
VOID
PaintBanner (
  IN UINTN   Top,
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            X;
  UINTN            Y;

  for (Y = Top; Y < Top + 30; Y++) {
    Row = (volatile UINT32 *)(UINTN)(BEACON_FB_BASE + (UINT64)Y * BEACON_FB_STRIDE);
    for (X = 0; X < BEACON_FB_PIXELS; X++) {
      Row[X] = Colour;
    }
  }
}

/**
  Paint `Count` small squares in a row -- a number you can count in a photograph.

  Why: the text console is unusable as a measurement channel here (BDS clears it,
  and small text on this panel photographs badly).  A row of 20x20 squares is
  countable from an arm's length away.

  `Left` exists so two independent counts can share one row: block devices from
  x 8 (blue) and mounted volumes from x 380 (green).  They used to be two rows,
  which cost the space the power probe now needs.
**/
STATIC
VOID
PaintDots (
  IN UINTN   Top,
  IN UINTN   Left,
  IN UINTN   Count,
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            Index;
  UINTN            X;
  UINTN            Y;

  if (Count > 12) {
    Count = 12;
  }

  for (Index = 0; Index < Count; Index++) {
    for (Y = Top; Y < Top + 20; Y++) {
      Row = (volatile UINT32 *)(UINTN)(BEACON_FB_BASE + (UINT64)Y * BEACON_FB_STRIDE) + Left + Index * 28;
      for (X = 0; X < 20; X++) {
        Row[X] = Colour;
      }
    }
  }
}

STATIC
VOID
Say (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN CHAR16            *Text
  )
{
  if ((SystemTable != NULL) && (SystemTable->ConOut != NULL)) {
    SystemTable->ConOut->OutputString (SystemTable->ConOut, Text);
  }
}

/**
  Format a UINT32 as eight upper-case hex digits.
**/
STATIC
VOID
HexString (
  IN  UINT32  Value,
  OUT CHAR16  *Out
  )
{
  STATIC CONST CHAR16  Digits[] = L"0123456789ABCDEF";
  UINTN                Index;

  for (Index = 0; Index < 8; Index++) {
    Out[Index] = Digits[(Value >> ((7 - Index) * 4)) & 0xF];
  }

  Out[8] = L'\0';
}

/**
  Print "labelXXXXXXXX" in one call, so the value follows its label even if the
  console interrupts between OutputString calls (it does not, but a reader of
  the photo cannot tell the difference, and one call is one line of code).
**/
STATIC
VOID
SayHex (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN CHAR16            *Label,
  IN UINT32            Value
  )
{
  CHAR16  Text[10];

  HexString (Value, Text);
  Say (SystemTable, Label);
  Say (SystemTable, Text);
}

/**
  Print a decimal number (stays inside 10 digits; every caller passes a small
  value such as a bit, a VOSEL index or a millivolt figure).
**/
STATIC
VOID
SayDec (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN UINTN             Value
  )
{
  CHAR16  Text[12];
  UINTN   Index;

  Index        = 11;
  Text[Index]  = L'\0';
  do {
    Text[--Index] = (CHAR16)(L'0' + (Value % 10));
    Value        /= 10;
  } while (Value != 0);

  Say (SystemTable, &Text[Index]);
}

/**
  Paint a solid rectangle -- used for the power verdict next to the VEMC strip.
  Kept separate from PaintBand/PaintBanner because those span the whole width.
**/
STATIC
VOID
PaintPatch (
  IN UINTN   Left,
  IN UINTN   Top,
  IN UINTN   Width,
  IN UINTN   Height,
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            X;
  UINTN            Y;

  for (Y = Top; Y < Top + Height; Y++) {
    Row = (volatile UINT32 *)(UINTN)(BEACON_FB_BASE + (UINT64)Y * BEACON_FB_STRIDE) + Left;
    for (X = 0; X < Width; X++) {
      Row[X] = Colour;
    }
  }
}

/**
  Paint a value as a row of squares, bit 0 on the left, in groups of eight.

  Survives BDS clearing the console, and is readable off a photo without any
  text rendering.  A cleared bit is dim grey rather than black so the row is
  visible even when the value is zero -- "all zero" is itself the evidence we
  are looking for.  `Count` is 32 for a whole MSDC register and 16 for a PMIC
  half-word, whose upper bits are always zero and would just be noise.
**/
STATIC
VOID
PaintBitStrip (
  IN UINTN   Top,
  IN UINTN   Count,
  IN UINT32  Value,
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            Bit;
  UINTN            Left;
  UINTN            X;
  UINTN            Y;

  if (Count > 32) {
    Count = 32;
  }

  for (Bit = 0; Bit < Count; Bit++) {
    Left = 8 + Bit * 21 + (Bit >> 3) * 6;
    for (Y = Top; Y < Top + 9; Y++) {
      Row = (volatile UINT32 *)(UINTN)(BEACON_FB_BASE + (UINT64)Y * BEACON_FB_STRIDE) + Left;
      for (X = 0; X < 9; X++) {
        Row[X] = ((Value >> Bit) & 1) ? Colour : 0x00202020;
      }
    }
  }
}

/**
  Read one MSDC0 register.  Everything in this file that touches the controller
  is a load, except the single bounded CMD12 in ProbeMsdcLeftovers().
**/
STATIC
UINT32
Msdc0Read32 (
  IN UINTN  Offset
  )
{
  return *(volatile UINT32 *)(UINTN)(MSDC0_MMIO_BASE + Offset);
}

/**
  Read one 16-bit PMIC register through the PMIC wrapper.

  Same transaction PmicWrapperDxe issues -- wait for the WACS2 FSM to be idle,
  write the half-word address into WACS2_CMD, wait for WF_VLDCLR, take the low
  half of WACS2_RDATA, acknowledge with WACS2_VLDCLR -- but bounded.  The
  upstream wait is an unbounded while(), and a wedged wrapper would then hang
  the boot with nothing on the panel, which is the one failure this board
  cannot report.

  Returns FALSE (and 0xFFFFFFFF in *Value) if the transaction does not complete
  within 10 ms, so "the PMIC window is wrong" stays distinguishable from "the
  register reads as zero".
**/
STATIC
BOOLEAN
PmicRead16 (
  IN  UINT32  Address,
  OUT UINT32  *Value
  )
{
  volatile UINT32  *Wrap;
  UINT32           Rdata;
  UINTN            Tries;

  *Value = 0xFFFFFFFF;

  Wrap = (volatile UINT32 *)(UINTN)PWRAP_MMIO_BASE;

  for (Tries = 0; Tries < PWRAP_RETRIES; Tries++) {
    if (((Wrap[PWRAP_WACS2_RDATA_OFS / 4] >> PWRAP_FSM_SHIFT) & PWRAP_FSM_MASK) == PWRAP_FSM_IDLE) {
      break;
    }

    gBS->Stall (PWRAP_RETRY_STALL_US);
  }

  if (Tries == PWRAP_RETRIES) {
    return FALSE;
  }

  //
  // ArbCapabilities = FALSE encoding (PmicWrapperImplLib.c, SOC_TYPE 1 = this
  // SoC): a read command is the byte address shifted right by one, in the
  // address field, with the write bit clear.
  //
  Wrap[PWRAP_WACS2_CMD_OFS / 4] = (Address >> 1) << 16;

  for (Tries = 0; Tries < PWRAP_RETRIES; Tries++) {
    Rdata = Wrap[PWRAP_WACS2_RDATA_OFS / 4];
    if (((Rdata >> PWRAP_FSM_SHIFT) & PWRAP_FSM_MASK) == PWRAP_FSM_WF_VLDCLR) {
      Wrap[PWRAP_WACS2_VLDCLR_OFS / 4] = 1;
      *Value                           = Rdata & 0xFFFF;
      return TRUE;
    }

    gBS->Stall (PWRAP_RETRY_STALL_US);
  }

  return FALSE;
}

/**
  The power probe: P_before, V, P_after -- in that order (docs/RESUME-1029.md
  section 5), plus the pin-mux state and the GPIO input levels of the eMMC pins.

  Measured on hardware 2026-10-29 (IMG_5738) and the reason this file now
  interprets MSDC_PS differently: P_before = P_after = 0x81FF0002, which with
  the real field layout means DAT7..0 = 0xFF, CMD = 1, WP = 1, CDSTS = 1, and
  MT6357 LDO_VEMC_CON0 = 0x0001 (enable set).  So the card is powered, present
  and sitting on a clean pulled-up bus -- "no power" is refuted, and the verdict
  below says so instead of looking for a rail that was never off.

  Read-only with respect to power: nothing here writes a PMIC register.
**/
STATIC
VOID
ProbePower (
  IN  EFI_SYSTEM_TABLE  *SystemTable,
  OUT UINT32            *VerdictColour
  )
{
  UINT32  PsBefore;
  UINT32  PsAfter;
  UINT32  Vemc;
  UINT32  VemcAna;
  UINT32  Vmc;
  UINT32  Vio18;
  UINT32  WrapInit;
  UINT32  Failures;
  UINT32  Vosel;
  UINT32  Millivolts;
  UINT32  Verdict;

  //
  // 1. P_before -- the first load this driver performs.  Nothing else in here
  // runs before it, and that is on purpose: a paint or a Say would be time the
  // hardware has to settle, and the point is to sample the state MsdcDxe left
  // behind.
  //
  PsBefore = Msdc0Read32 (MSDC_PS_OFS);

  //
  // The first strip is also the "I got this far" marker, and it is painted
  // right after the read rather than before it: a photo that shows this row
  // but neither of the rows painted later means the driver died in the PMIC
  // access, which is the only blocking transaction in here.
  //
  // Note what this does *not* do: it paints no stage cell.  Cells 16..23 all
  // belong to MsdcDxe/EmmcDxe (20 = CMD10, 21 = CMD8/ext-CSD, 22 = data-read
  // verdict, 23 = command failed), and this driver runs after them -- painting
  // a cell here would erase the M4 evidence the photo is for.
  //
  PaintBitStrip (BEACON_STRIP_PS_BEFORE, 32, PsBefore, 0x0000FFFF);
  PaintBand (0x008000FF);
  StageCell (6, 0x00FFFF00);
  Say (SystemTable, L"\r\nMSDC PASSTHRU\r\n");

  //
  // 2. V.  INIT_DONE2 bit 0 is the wrapper's own "WACS2 usable" flag, read
  // first so that "wrapper not initialised" is not reported as "registers read
  // back as zero".
  //
  Failures = 0;
  WrapInit = *(volatile UINT32 *)(UINTN)(PWRAP_MMIO_BASE + PWRAP_INIT_DONE2_OFS);

  if (!PmicRead16 (MT6357_LDO_VEMC_CON0, &Vemc)) {
    Failures++;
  }

  if (!PmicRead16 (MT6357_LDO_VMC_CON0, &Vmc)) {
    Failures++;
  }

  if (!PmicRead16 (MT6357_LDO_VIO18_CON0, &Vio18)) {
    Failures++;
  }

  if (!PmicRead16 (MT6357_VEMC_ANA_CON0, &VemcAna)) {
    Failures++;
  }

  //
  // 3. P_after, after a short settle (a rail that is ramping would show up as
  // a difference between the two readings).
  //
  gBS->Stall (10 * 1000);
  PsAfter = Msdc0Read32 (MSDC_PS_OFS);

  //
  // Text first: it is the readable version and the one that reaches the flash
  // log.  The strips are the backstop when the photo is bad.
  //
  SayHex (SystemTable, L"PS before=", PsBefore);
  SayHex (SystemTable, L" after=", PsAfter);
  SayHex (SystemTable, L" DAT=", (PsAfter >> MSDC_PS_DAT_SHIFT) & MSDC_PS_DAT_MASK);
  Say (SystemTable, L" CMD=");
  SayDec (SystemTable, (PsAfter & MSDC_PS_CMD_BIT) ? 1 : 0);
  Say (SystemTable, L" CD=");
  SayDec (SystemTable, (PsAfter & MSDC_PS_CDSTS_BIT) ? 1 : 0);
  Say (SystemTable, L"\r\n");

  SayHex (SystemTable, L"PWRAP init=", WrapInit & 0x1);
  SayHex (SystemTable, L" failed=", Failures);
  Say (SystemTable, L"\r\n");

  SayHex (SystemTable, L"VEMC=", Vemc);
  Say (SystemTable, L" en=");
  SayDec (SystemTable, (Vemc & MT6357_LDO_ENABLE_BIT) ? 1 : 0);

  if (VemcAna != 0xFFFFFFFF) {
    Vosel      = (VemcAna >> MT6357_VEMC_VOSEL_SHIFT) & MT6357_VEMC_VOSEL_MASK;
    Millivolts = mVemcMillivolts[Vosel];
    SayHex (SystemTable, L" ANA=", VemcAna);
    Say (SystemTable, L" vosel=");
    SayDec (SystemTable, Vosel);
    Say (SystemTable, L" ");
    SayDec (SystemTable, Millivolts);
    Say (SystemTable, L"mV");
  }

  Say (SystemTable, L"\r\n");

  SayHex (SystemTable, L"VMC=", Vmc);
  SayHex (SystemTable, L" VIO18=", Vio18);
  Say (SystemTable, L"\r\n");

  //
  // The verdict, in the order the questions were asked: wrapper alive, rail
  // enabled, bus pulled high.  It goes in two places -- a patch beside the
  // strips and the y 40 band -- because this panel's top rows are exactly where
  // photographs go bad.
  //
  if (Failures != 0) {
    Verdict = 0x00FF00FF;
    Say (SystemTable, L"BUS NO-WRAP\r\n");
  } else if ((Vemc & MT6357_LDO_ENABLE_BIT) == 0) {
    Verdict = 0x00FF0000;
    Say (SystemTable, L"BUS RAIL-OFF\r\n");
  } else if (((PsAfter >> MSDC_PS_DAT_SHIFT) & MSDC_PS_DAT_MASK) != MSDC_PS_DAT_MASK) {
    Verdict = 0x00FF8000;
    Say (SystemTable, L"BUS DAT-LOW\r\n");
  } else if ((PsAfter & MSDC_PS_CMD_BIT) == 0) {
    Verdict = 0x00FF8000;
    Say (SystemTable, L"BUS CMD-LOW\r\n");
  } else {
    Verdict = 0x0000FF00;
    Say (SystemTable, L"BUS OK\r\n");
  }

  PaintPatch (BEACON_VERDICT_LEFT, BEACON_VERDICT_TOP, BEACON_VERDICT_W, BEACON_VERDICT_H, Verdict);

  *VerdictColour = Verdict;
}

/**
  Which pinctrl register holds the 4-bit mode field of one pin.  GpioImplLib.c:
  ModeOffset 0x300, one register per eight pins, 4 bits per pin -- which is how
  0x300 + (122/8)*0x10 lands on the vendor's MSDC0_GPIO_MODE0 (0x3F0).
**/
STATIC
UINTN
EmmcModeRegister (
  IN UINTN  Pin
  )
{
  return PINCTRL_MODE_OFS + (Pin / 8) * PINCTRL_MODE_STRIDE;
}

STATIC
UINT32
PinctrlRead32 (
  IN UINTN  Offset
  )
{
  return *(volatile UINT32 *)(UINTN)(PINCTRL_MMIO_BASE + Offset);
}

/**
  Read the pad input level of GPIO122..133, packed as bit0 = pin 122.

  The data registers are NOT laid out like the mode registers: GpioDxe.c uses
  `PIN_OFFSET(pin) = (pin/32)*0x10` and `bit = pin%32` for direction, output and
  input, and `(pin/8)*0x10` with four bits per pin only for the mode registers.
  Reading DIN with the mode formula (as the first version of this probe did)
  lands on the wrong register and always returns 0 -- which is exactly what the
  panel showed (IMG_5743: `DIN 122..133=00000000` while PS said the same lines
  were high).
**/
STATIC
UINT32
ReadEmmcPinDin (VOID)
{
  UINTN   Pin;
  UINT32  Din;

  Din = 0;
  for (Pin = EMMC_PIN_FIRST; Pin <= EMMC_PIN_LAST; Pin++) {
    if (((PinctrlRead32 (PINCTRL_DIN_OFS + (Pin / 32) * PINCTRL_MODE_STRIDE) >> (Pin % 32)) & 1) != 0) {
      Din |= (1u << (Pin - EMMC_PIN_FIRST));
    }
  }

  return Din;
}

/**
  Write the eMMC pin group's mode the way the vendor does it: one plain 32-bit
  store per register, whole field at once, instead of per-pin read-modify-write.

  Why this matters: GpioDxe.c's GpioSetMode() writes to `Offset + 0x4` and
  `Offset + 0x8`, i.e. it treats the mode register as a set/reset alias pair and
  clears with a 3-bit mask (0x7) even though a field is four bits.  On this SoC
  that left pins 122/123 reading back as mode 0xF while 124..127 read 2 -- see
  IMG_5743's `m0=2222FF00`.  The vendor's msdc_set_pin_mode() simply stores
  0x22222200 (or 0x11111100) into 0x3F0 and 0x222222 (0x111111) into 0x400.
**/
STATIC
VOID
WriteEmmcPinMode (
  IN UINT32  Mode
  )
{
  UINT32  Pattern;
  UINT32  M0;
  UINT32  M1;

  //
  // 0x3F0 covers GPIO120..127: bits 7..0 are 120/121 and must be preserved.
  // 0x400 covers GPIO128..135: bits 31..24 are 134/135 and must be preserved.
  //
  Pattern = (Mode & 0xFu) * 0x111111u;

  M0 = PinctrlRead32 (EmmcModeRegister (EMMC_PIN_FIRST));
  M0 = (M0 & 0x000000FFu) | (Pattern << 8);
  *(volatile UINT32 *)(UINTN)(PINCTRL_MMIO_BASE + EmmcModeRegister (EMMC_PIN_FIRST)) = M0;

  M1 = PinctrlRead32 (EmmcModeRegister (128));
  M1 = (M1 & 0xFF000000u) | Pattern;
  *(volatile UINT32 *)(UINTN)(PINCTRL_MMIO_BASE + EmmcModeRegister (128)) = M1;
}

//
// One CMD1 ping's evidence.  Kept in a struct because each field answers a
// different question and they have to stay together in the log.
//
typedef struct {
  BOOLEAN    Answered;   // CMDRDY seen: the card replied
  UINT32     IntStatus;  // MSDC_INT after the command
  UINT32     Resp0;      // R3/OCR
  UINT32     PsAnd;      // MSDC_PS bits held high during the command
  UINT32     PsOr;       // MSDC_PS bits ever high during the command
  UINT32     CmdLow;     // samples where CMD (bit 24) read 0
  UINT32     DinAnd;     // eMMC pins held high during the command
  UINT32     DinOr;      // eMMC pins ever high during the command
} CARD_PING_RESULT;

/**
  Send one CMD1 (SEND_OP_COND) by hand and sample the bus while it is on the wire.

  Why hand-rolled: the question is no longer "what did the driver do" but "can
  this card hear this controller at all".  CMD1 needs only CMD and CLK, has a
  response (R3), and works in every card state, so it is the cheapest possible
  ping.

  The sampling loop has no Stall on purpose: the command phase is 48 clock
  cycles, about 120 us at 400 kHz, and a 1 ms poll steps straight over it.  The
  loop counts how often CMD read low, which is the answer to "did this
  controller drive the command line at all" -- a number the card's silence
  cannot give.  It also ANDs/ORs the GPIO input word, so a clock line that is
  actually toggling shows up as a bit that is 0 in the AND and 1 in the OR.

  Interrupts are cleared before the command because MSDC_INT is W1C: a stale
  CMDTMO from MsdcDxe would otherwise be read as this command's result.
**/
STATIC
CARD_PING_RESULT
CardPing (
  IN EFI_SYSTEM_TABLE  *SystemTable,
  IN CHAR16            *Tag
  )
{
  volatile UINT32  *Regs;
  CARD_PING_RESULT  Result;
  UINT32            Ps;
  UINT32            Din;
  UINTN             Tries;

  Result.Answered  = FALSE;
  Result.IntStatus = 0;
  Result.Resp0     = 0;
  Result.PsAnd     = 0xFFFFFFFF;
  Result.PsOr      = 0;
  Result.CmdLow    = 0;
  Result.DinAnd    = 0xFFFFFFFF;
  Result.DinOr     = 0;

  Regs = (volatile UINT32 *)(UINTN)MSDC0_MMIO_BASE;

  //
  // Wait for the controller to be idle.  A command issued while SDC_STS says
  // busy is dropped by the hardware, which would look like "no answer".
  //
  for (Tries = 0; Tries < 2000; Tries++) {
    if ((Regs[SDC_STS_OFS / 4] & (SDC_STS_BUSY | SDC_STS_CMDBUSY)) == 0) {
      break;
    }

    gBS->Stall (100);
  }

  Regs[MSDC_INT_OFS / 4] = 0xFFFFFFFF;
  Regs[SDC_ARG_OFS / 4]  = CMD1_OCR;
  Regs[SDC_CMD_OFS / 4]  = (1u | (SDC_CMD_RSP_R3 << SDC_CMD_RSP_SHIFT));

  for (Tries = 0; Tries < 20000; Tries++) {
    Ps = Regs[MSDC_PS_OFS / 4];
    if ((Ps & MSDC_PS_CMD_BIT) == 0) {
      Result.CmdLow++;
    }

    Result.PsAnd &= Ps;
    Result.PsOr  |= Ps;

    Din           = ReadEmmcPinDin ();
    Result.DinAnd &= Din;
    Result.DinOr  |= Din;
  }

  for (Tries = 0; Tries < 2000; Tries++) {
    Result.IntStatus = Regs[MSDC_INT_OFS / 4];
    if ((Result.IntStatus & (MSDC_INT_CMDRDY | MSDC_INT_CMDTMO)) != 0) {
      break;
    }

    gBS->Stall (1000);
  }

  Result.IntStatus = Regs[MSDC_INT_OFS / 4];
  Result.Resp0     = Regs[SDC_RESP0_OFS / 4];
  Regs[MSDC_INT_OFS / 4] = Result.IntStatus;

  Result.Answered = ((Result.IntStatus & MSDC_INT_CMDRDY) != 0);

  SayHex (SystemTable, Tag, Result.IntStatus);
  SayHex (SystemTable, L" R=", Result.Resp0);
  Say (SystemTable, L" cl=");
  SayDec (SystemTable, Result.CmdLow);
  SayHex (SystemTable, L" dA=", Result.DinAnd);
  SayHex (SystemTable, L" dO=", Result.DinOr);
  Say (SystemTable, L"\r\n");

  return Result;
}

/**
  Ask the card whether it is listening, and try the pin-mux modes explicitly.

  What IMG_5743 established: the strap-selected mode left pins 122/123 in a
  state the driver never intended (`m0=2222FF00`), the GPIO input read was
  landing on the wrong register, and the CMD line never moved during the
  command.  So this version (a) writes the mux the vendor's way and prints what
  it reads back, and (b) counts CMD-low samples instead of relying on a photo of
  a 9-pixel square.

  Order: current state -> mode 2 -> mode 1.  Whichever answers is kept, because
  the connect sweep that follows starts EmmcDxe and the boot may as well finish
  what it started.
**/
STATIC
VOID
ProbeCardAndMux (
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32            Strap;
  UINT32            Mode;
  UINT32            Trial;
  CARD_PING_RESULT  Ping;
  BOOLEAN           Answered;

  Strap = *(volatile UINT32 *)(UINTN)PINCTRL_STRAP_ADDR;
  Mode  = ((Strap & (1u << PINCTRL_STRAP_BIT)) != 0) ? 1 : 2;

  SayHex (SystemTable, L"MUX strap=", Strap);
  Say (SystemTable, L" mode=");
  SayDec (SystemTable, Mode);
  SayHex (SystemTable, L" m0=", PinctrlRead32 (EmmcModeRegister (EMMC_PIN_FIRST)));
  SayHex (SystemTable, L" m1=", PinctrlRead32 (EmmcModeRegister (128)));
  Say (SystemTable, L"\r\n");

  SayHex (SystemTable, L"DIN 122..133=", ReadEmmcPinDin ());
  Say (SystemTable, L"\r\n");

  SayHex (SystemTable, L"PAD0=", Msdc0Read32 (MSDC_PAD_CTL0_OFS));
  SayHex (SystemTable, L" PAD1=", Msdc0Read32 (MSDC_PAD_CTL1_OFS));
  Say (SystemTable, L"\r\n");

  //
  // Ping in whatever state MsdcDxe left behind.  This is the baseline.
  //
  Ping = CardPing (SystemTable, L"PING a I=");
  PaintBitStrip (BEACON_STRIP_CMD_AND, 32, Ping.PsAnd, 0x00FFFFFF);
  PaintBitStrip (BEACON_STRIP_CMD_OR,  32, Ping.PsOr,  0x0000FFFF);
  Answered = Ping.Answered;

  //
  // Then try the two modes explicitly, printing the readback each time so the
  // photo says whether the write took effect at all.
  //
  for (Trial = 0; (Trial < 2) && !Answered; Trial++) {
    UINT32  TryMode;

    TryMode = (Trial == 0) ? 2 : 1;
    WriteEmmcPinMode (TryMode);

    Say (SystemTable, L"SET mode=");
    SayDec (SystemTable, TryMode);
    SayHex (SystemTable, L" m0=", PinctrlRead32 (EmmcModeRegister (EMMC_PIN_FIRST)));
    SayHex (SystemTable, L" m1=", PinctrlRead32 (EmmcModeRegister (128)));
    Say (SystemTable, L"\r\n");

    Ping = CardPing (SystemTable, (TryMode == 2) ? L"PING b I=" : L"PING c I=");
    PaintBitStrip (BEACON_STRIP_CMD_AND, 32, Ping.PsAnd, 0x00FFFFFF);
    PaintBitStrip (BEACON_STRIP_CMD_OR,  32, Ping.PsOr,  0x0000FFFF);
    Answered = Ping.Answered;
  }

  if (Answered) {
    Say (SystemTable, L"MUX ANSWERED - mode kept\r\n");
    return;
  }

  //
  // Nothing answered.  Put the strap-selected mode back and say so, so the rest
  // of the boot (and the next session) starts from the documented state.
  //
  WriteEmmcPinMode (Mode);
  Say (SystemTable, L"MUX restored to ");
  SayDec (SystemTable, Mode);
  Say (SystemTable, L"\r\n");
}

/**
  Read-only probe of what MsdcDxe left in the MSDC0 controller, plus one
  bounded CMD12 to put a card stuck mid-transfer back into TRAN.

  Evidence this answers (2026-10-29, IMG_5727):

      ddn data: cmd 8 failed: status=0x12      <-- EFI_TIMEOUT
      ddn data: cmd 8 r1=00000900 len=512      <-- but R1 says TRAN (state 4)
      ddn data: stall int=00000000             <-- no interrupt bit at all

  A command that reports timeout yet leaves a valid R1 in RESP0 means the
  completion is not being observed.  Since the controller's own data timeout
  would set DATTMIO regardless, an all-zero INT suggests the status register is
  not the one being polled, is masked, or the block is clock-gated.

  Everything else here is read-only: a read cannot change card state, start a
  transfer, or clear an interrupt (INT is read, not written back).  The one
  write is the bounded CMD12 at the end.

  No stage cells either.  The previous version painted cells 20 and 21 as
  "probe started" / "probe finished", but those numbers are MsdcDxe's CMD10 and
  CMD8 (SEND_EXT_CSD) markers -- and this driver runs *after* MsdcDxe, so the
  paint erased the very evidence the cells exist for.  Text only.

  Graphics belong to ProbePower(), whose strips also carry their own
  "reached this point" information.
**/
STATIC
VOID
ProbeMsdcLeftovers (
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  volatile UINT32  *Regs;
  UINT32           Cfg;
  UINT32           IntStatus;
  UINT32           IntEnable;
  UINT32           Resp0;
  CHAR16           Text[10];

  //
  // Read BOTH controllers.  A host that is not in use keeps its reset values:
  // MSDC_CFG without MODE|CCKPD|PIO, and MSDC_INTEN at the reset mask.  The
  // previous single-host snapshot read CFG=0x02000000 (no BIT3), which is
  // exactly the signature of "this is not the host driving the eMMC".
  //
  Regs      = (volatile UINT32 *)(UINTN)MSDC0_MMIO_BASE;
  Cfg       = Regs[MSDC_CFG_OFS / 4];
  IntStatus = Regs[MSDC_INT_OFS / 4];
  IntEnable = Regs[MSDC_INTEN_OFS / 4];
  Resp0     = Regs[SDC_RESP0_OFS / 4];

  Say (SystemTable, L"H0 C=");
  HexString (Cfg, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L" E=");
  HexString (IntEnable, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L" I=");
  HexString (IntStatus, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L" R=");
  HexString (Resp0, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L"\r\n");

  Regs      = (volatile UINT32 *)(UINTN)MSDC1_MMIO_BASE;
  Cfg       = Regs[MSDC_CFG_OFS / 4];
  IntStatus = Regs[MSDC_INT_OFS / 4];
  IntEnable = Regs[MSDC_INTEN_OFS / 4];
  Resp0     = Regs[SDC_RESP0_OFS / 4];

  Say (SystemTable, L"H1 C=");
  HexString (Cfg, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L" E=");
  HexString (IntEnable, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L" I=");
  HexString (IntStatus, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L" R=");
  HexString (Resp0, Text);
  Say (SystemTable, Text);
  Say (SystemTable, L"\r\n");

  //
  // The eMMC is on H0.  An earlier round read R1 = 0xB00 (CURRENT_STATE = 5,
  // DATA) and that turned out to be LK's leftover register value, so this CMD12
  // is now best effort rather than a repair: if the card really is mid-transfer
  // it puts it back into TRAN, and if it is not, it is one bounded command that
  // times out and is ignored.  Everything here is bounded; a wedged controller
  // must not hang the beacon.
  //
  {
    UINT32  SdCfg;
    UINT32  Ps;
    UINT32  BlkNum;
    UINT32  Sts;
    UINT32  IntAfter;
    UINT32  RespAfter;
    UINTN   Tries;

    Regs   = (volatile UINT32 *)(UINTN)MSDC0_MMIO_BASE;
    SdCfg  = Regs[SDC_CFG_OFS / 4];
    Ps     = Regs[MSDC_PS_OFS / 4];
    BlkNum = Regs[SDC_BLK_NUM_OFS / 4];

    Say (SystemTable, L"H0 S=");
    HexString (SdCfg, Text);
    Say (SystemTable, Text);
    Say (SystemTable, L" P=");
    HexString (Ps, Text);
    Say (SystemTable, Text);
    Say (SystemTable, L" N=");
    HexString (BlkNum, Text);
    Say (SystemTable, Text);
    Say (SystemTable, L"\r\n");

    for (Tries = 0; Tries < 2000; Tries++) {
      Sts = Regs[SDC_STS_OFS / 4];
      if ((Sts & SDC_STS_BUSY) == 0) {
        break;
      }

      gBS->Stall (100);
    }

    Regs[SDC_ARG_OFS / 4] = 0;
    Regs[SDC_CMD_OFS / 4] = (12u | (7u << SDC_CMD_RSP_SHIFT) | SDC_CMD_STOP_CMD);

    for (Tries = 0; Tries < 2000; Tries++) {
      IntAfter = Regs[MSDC_INT_OFS / 4];
      if ((IntAfter & (MSDC_INT_CMDRDY | MSDC_INT_CMDTMO)) != 0) {
        break;
      }

      gBS->Stall (1000);
    }

    IntAfter  = Regs[MSDC_INT_OFS / 4];
    RespAfter = Regs[SDC_RESP0_OFS / 4];
    Regs[MSDC_INT_OFS / 4] = IntAfter;

    Say (SystemTable, L"CMD12 I=");
    HexString (IntAfter, Text);
    Say (SystemTable, Text);
    Say (SystemTable, L" R=");
    HexString (RespAfter, Text);
    Say (SystemTable, Text);
    Say (SystemTable, L"\r\n");
  }
}

/**
  Connect every handle in the system, recursively -- what BDS would have done.

  Three passes with a short stall between them, because each pass can create
  handles that the next one needs: pass 1 binds EmmcDxe (BlockIo appears),
  pass 2 binds PartitionDxe (child handles appear), pass 3 binds FatPkg
  (SimpleFileSystem appears).
**/
STATIC
VOID
ConnectEverything (VOID)
{
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  UINTN       Pass;

  for (Pass = 0; Pass < 3; Pass++) {
    Handles = NULL;
    Count   = 0;
    if (EFI_ERROR (gBS->LocateHandleBuffer (AllHandles, NULL, NULL, &Count, &Handles))) {
      return;
    }

    for (Index = 0; Index < Count; Index++) {
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    }

    gBS->FreePool (Handles);
    gBS->Stall (100 * 1000);
  }
}

STATIC
UINTN
CountProtocol (
  IN EFI_GUID  *Protocol
  )
{
  EFI_HANDLE  *Handles;
  UINTN       Count;

  Handles = NULL;
  Count   = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Handles))) {
    return 0;
  }

  gBS->FreePool (Handles);
  return Count;
}

EFI_STATUS
EFIAPI
StorageBeaconDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HANDLE  *Passthru;
  UINTN       PassthruCount;
  UINTN       BlockDevices;
  UINTN       Volumes;
  UINTN       Count;
  UINTN       Index;
  EFI_HANDLE  *Handles;
  UINT32      PowerVerdict;

  PowerVerdict = 0x00FF00FF;

  //
  // We are dispatched, therefore the pass-through protocol exists.
  //
  // This call comes first and does the band, cell 6 and the header itself: it
  // reads MSDC_PS before it draws a single pixel, because P_before has to
  // describe the state MsdcDxe left behind, not the state after we started
  // touching the panel.  See ProbePower().
  //
  ProbePower (SystemTable, &PowerVerdict);

  //
  // Then the older register snapshot.  Read-only apart from a bounded CMD12,
  // and text-only now: the graphics band belongs to the probes above.
  //
  ProbeMsdcLeftovers (SystemTable);

  //
  // Last diagnostic: ping the card (CMD1) and, if it stays silent, try the
  // other pin-mux mode.  This runs before the connect sweep on purpose -- if a
  // mode change wakes the card up, EmmcDxe gets a working bus in this same boot.
  //
  ProbeCardAndMux (SystemTable);

  Passthru      = NULL;
  PassthruCount = 0;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSdMmcPassThruProtocolGuid, NULL, &PassthruCount, &Passthru))) {
    PassthruCount = 0;
  } else {
    gBS->FreePool (Passthru);
  }

  ConnectEverything ();

  BlockDevices = CountProtocol (&gEfiBlockIoProtocolGuid);
  if (BlockDevices > 0) {
    PaintBand (0x008000FF);
    StageCell (4, 0x008000FF);
    PaintBanner (1100, 0x008000FF);
    Say (SystemTable, L"BLOCK OK\r\n");
  } else {
    StageCell (7, 0x00FF0000);
    PaintBanner (1100, 0x00FF0000);
    Say (SystemTable, L"BLOCK NONE\r\n");
  }

  PaintDots (BEACON_DOTS_TOP, 8, BlockDevices, 0x008000FF);

  //
  // Count mounted filesystems: OpenVolume is what proves a real FAT, not just a
  // SimpleFileSystem handle.
  //
  Volumes = 0;
  Count   = 0;
  Handles = NULL;
  if (!EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles))) {
    for (Index = 0; Index < Count; Index++) {
      EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
      EFI_FILE_PROTOCOL                *Root;

      Fs   = NULL;
      Root = NULL;
      if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs))) {
        continue;
      }

      if (!EFI_ERROR (Fs->OpenVolume (Fs, &Root))) {
        Volumes++;
        Root->Close (Root);
      }
    }

    gBS->FreePool (Handles);
  }

  if (Volumes > 0) {
    PaintBand (0x0000FF00);
    StageCell (5, 0x0000FF00);
    PaintBanner (1140, 0x0000FF00);
    Say (SystemTable, L"FAT OK\r\n");
  } else {
    PaintBanner (1140, 0x00FF0000);
    Say (SystemTable, L"FAT NONE\r\n");
  }

  PaintDots (BEACON_DOTS_TOP, BEACON_DOTS_VOLUMES_X, Volumes, 0x0000FF00);

  //
  // The y 40 band is the one channel that survives everything, so it now
  // carries this round's answer: why storage is blocked.  A filesystem wins
  // over it (green means M4 is done); otherwise the colour is the power
  // verdict from ProbePower().
  //
  if (Volumes == 0) {
    PaintBand (PowerVerdict);
  }

  //
  // Deliberately keep running with the connected handles in place: BDS and
  // UniXNU.efi later read gST->ConOut / the same handles, and leaving them
  // connected is what a normal platform looks like after ConnectAll.
  //
  return EFI_SUCCESS;
}
// (power probe: P_before / MT6357 VEMC / P_after)
