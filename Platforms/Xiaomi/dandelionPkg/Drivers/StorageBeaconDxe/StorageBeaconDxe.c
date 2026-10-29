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
// side changes.  Read-only here, see ProbeMsdcRegisters().
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

#define SDC_STS_BUSY       (1u << 0)
#define SDC_CMD_STOP_CMD   (1u << 14)
#define SDC_CMD_RSP_SHIFT  7
#define MSDC_INT_CMDRDY    (1u << 8)
#define MSDC_INT_CMDTMO    (1u << 9)

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
**/
STATIC
VOID
PaintDots (
  IN UINTN   Top,
  IN UINTN   Count,
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            Index;
  UINTN            Left;
  UINTN            X;
  UINTN            Y;

  if (Count > 24) {
    Count = 24;
  }

  for (Index = 0; Index < Count; Index++) {
    Left = 8 + Index * 28;
    for (Y = Top; Y < Top + 20; Y++) {
      Row = (volatile UINT32 *)(UINTN)(BEACON_FB_BASE + (UINT64)Y * BEACON_FB_STRIDE) + Left;
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
  Paint one 32-bit value as 32 squares, bit 0 on the left, in groups of eight.

  Survives BDS clearing the console, and is readable off a photo without any
  text rendering.  A cleared bit is dim grey rather than black so the strip is
  visible even when the value is zero -- "all zero" is itself the evidence we
  are looking for.
**/
STATIC
VOID
PaintBits (
  IN UINTN   Top,
  IN UINT32  Value,
  IN UINT32  Colour
  )
{
  volatile UINT32  *Row;
  UINTN            Bit;
  UINTN            Left;
  UINTN            X;
  UINTN            Y;

  for (Bit = 0; Bit < 32; Bit++) {
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
  Read-only probe of the MSDC0 controller registers.

  Evidence this answers (2026-10-29, IMG_5727):

      ddn data: cmd 8 failed: status=0x12      <-- EFI_TIMEOUT
      ddn data: cmd 8 r1=00000900 len=512      <-- but R1 says TRAN (state 4)
      ddn data: stall int=00000000             <-- no interrupt bit at all

  A command that reports timeout yet leaves a valid R1 in RESP0 means the
  completion is not being observed.  Since the controller's own data timeout
  would set DATTMIO regardless, an all-zero INT suggests the status register is
  not the one being polled, is masked, or the block is clock-gated.

  Nothing here writes to the controller: a read cannot change card state, start
  a transfer, or clear an interrupt (INT is read, not written back).  Cell 20
  is painted first, so a photo ending at cell 20 means the MMIO read itself
  hung the bus -- which would prove the base address or the clock is wrong.
**/
STATIC
VOID
ProbeMsdcRegisters (
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  volatile UINT32  *Regs;
  UINT32           Cfg;
  UINT32           IntStatus;
  UINT32           IntEnable;
  UINT32           Resp0;
  CHAR16           Text[10];

  StageCell (20, 0x0000FFFF);

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

  PaintBits (1132, Cfg,       0x00FFFFFF);
  PaintBits (1143, IntEnable, 0x00FFFF00);

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

  PaintBits (1158, Cfg,       0x00FFFFFF);
  PaintBits (1169, IntEnable, 0x00FFFF00);

  //
  // The eMMC is on H0 and its R1 said CURRENT_STATE = 5 (DATA): the card began
  // sending a block and nobody finished or stopped the transfer, so every later
  // command times out.  Issue CMD12 (STOP_TRANSMISSION) once, bounded, to put
  // the card back into TRAN so the next driver that touches it starts from a
  // sane state.  Everything is bounded: a wedged controller must not hang here.
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

    PaintBits (1180, SdCfg, 0x0000FFFF);
  }

  StageCell (21, 0x0000FF00);
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

  //
  // We are dispatched, therefore the pass-through protocol exists.
  //
  PaintBand (0x008000FF);
  StageCell (6, 0x00FFFF00);
  Say (SystemTable, L"\r\nMSDC PASSTHRU\r\n");

  //
  // Read (never write) the controller registers before doing anything else, so
  // the numbers describe the state MsdcDxe left behind.
  //
  ProbeMsdcRegisters (SystemTable);

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

  PaintDots (1190, BlockDevices, 0x008000FF);

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

  PaintDots (1220, Volumes, 0x0000FF00);

  //
  // Deliberately keep running with the connected handles in place: BDS and
  // UniXNU.efi later read gST->ConOut / the same handles, and leaving them
  // connected is what a normal platform looks like after ConnectAll.
  //
  return EFI_SUCCESS;
}
// (dual-host probe: build trigger)
