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
