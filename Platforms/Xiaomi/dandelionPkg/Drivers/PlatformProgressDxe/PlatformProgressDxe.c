/**
  M2 progress beacon, stage 3 of 3: proves that the DXE core and dispatcher
  are alive on dandelion.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Why this exists: dandelion has no UART, so "how long until the SoC resets"
  is the only channel the firmware has before any storage driver works. The
  ladder is

      ~10 s  -> the FD never executed (LK's watchdog is still armed)
      ~20 s  -> SEC entry ran (PlatformSecLib), died before PlatformPeim
      ~30 s  -> passed memory init (PlatformPeiLib), died before DXE dispatch
      ~2 s   -> this driver ran: DXE core + dispatcher + FV are working

  This driver deliberately leaves the watchdog ARMED at 2 s instead of
  disarming it, so that "reached DXE" is a positive, visible signal rather
  than silence. Flip BEACON_SECONDS to 0 to disarm instead once DXE is known
  to be reached - that is the first thing to change when this stops being a
  diagnostic build.
**/

#include <Uefi.h>

#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

//
// TOPRGU. Must stay equal to the "WatchDog Timer" entry in
// Platforms/Xiaomi/dandelionPkg/Library/MemoryMapLib/MemoryMapLib.c
// (0x10007000). Hardcoded because this driver is meant to be as independent
// as possible from the rest of the platform: if the memory map itself is
// wrong, the beacon still has to work.
//
#define TOPRGU_BASE         0x10007000

//
// Register layout and key values: see
// Silicon/MediaTek/MT6765Pkg/Library/PlatformSecLib/PlatformRegisters.h
// (verified against the MT6765 vendor kernel mtk_wdt.h / mtk_wdt_v2.c).
//
#define WDT_MODE_OFFSET     0x0000
#define WDT_LENGTH_OFFSET   0x0004
#define WDT_MODE_KEY        0x22000000
#define WDT_MODE_ENABLE     0x0001
#define WDT_MODE_EXTEN      0x0004
#define WDT_LENGTH_KEY      0x0008

#define BEACON_SECONDS      2

//
// M2 screen postcode, stage 3: white band under PEI's blue one.  If this band
// appears, the DXE core, the dispatcher and our FV all work -- which is the
// real M2 question, and it answers it without any console stack.
//
#define DANDELION_FB_BASE    0x7EC50000ULL
#define DANDELION_FB_STRIDE  2944
#define DANDELION_FB_WIDTH   720

#include "DandelionFont.h"

STATIC
VOID
DandelionPaintBand (
  UINT32  Color,
  UINTN   Top
  )
{
  volatile UINT32  *Row;
  UINTN            X;
  UINTN            Y;

  for (Y = Top; Y < Top + 32; Y++) {
    Row = (volatile UINT32 *)(UINTN)(DANDELION_FB_BASE + Y * DANDELION_FB_STRIDE);
    for (X = 0; X < DANDELION_FB_WIDTH; X++) {
      Row[X] = Color;
    }
  }
}

STATIC
VOID
DandelionFillRect (
  UINTN   Left,
  UINTN   Top,
  UINTN   Width,
  UINTN   Height,
  UINT32  Color
  )
{
  volatile UINT32  *Row;
  UINTN            X;
  UINTN            Y;

  for (Y = Top; Y < Top + Height; Y++) {
    Row = (volatile UINT32 *)(UINTN)(DANDELION_FB_BASE + Y * DANDELION_FB_STRIDE) + Left;
    for (X = 0; X < Width; X++) {
      Row[X] = Color;
    }
  }
}

STATIC
VOID
DandelionDrawString (
  UINTN        X,
  UINTN        Y,
  CONST CHAR8  *Text,
  UINT32       Fg,
  UINT32       Bg
  )
{
  volatile UINT32  *Row;
  UINTN            Column;
  UINTN            Line;
  UINT8            Bits;

  for ( ; *Text != '\0'; Text++, X += 8) {
    if ((*Text < 0x20) || (*Text > 0x7E)) {
      continue;
    }

    for (Line = 0; Line < 16; Line++) {
      Bits = mDandelionFont8x16[*Text - 0x20][Line];
      Row  = (volatile UINT32 *)(UINTN)(DANDELION_FB_BASE + (Y + Line) * DANDELION_FB_STRIDE) + X;
      for (Column = 0; Column < 8; Column++) {
        Row[Column] = (Bits & (0x80 >> Column)) ? Fg : Bg;
      }
    }
  }
}

EFI_STATUS
EFIAPI
PlatformProgressDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Length;

  //
  // Real text now: clear the top of the panel and write what we know.
  // There is no GOP and no ConSplitter on this board yet, so this is the same
  // direct framebuffer trick the bootshim uses -- and it means the DXE stage
  // can report state without any console stack at all.
  //
  DandelionFillRect (0, 0, DANDELION_FB_WIDTH, 200, 0x00000000);
  DandelionDrawString (16, 24,  "DANDELION UEFI", 0x00FFFFFF, 0x00000000);
  DandelionDrawString (16, 56,  "DXE OK", 0x0000FF00, 0x00000000);
  DandelionDrawString (16, 88,  (SystemTable->ConOut != NULL) ? "CONOUT YES" : "CONOUT NO", 0x00FFFF00, 0x00000000);
  DandelionDrawString (16, 120, "FD 40080000", 0x00AAAAAA, 0x00000000);
  DandelionPaintBand (0x00FFFFFF, 200);

  //
  // ConOut exists either way (ConSplitter always creates it), so the only real
  // test of "is there a working console on this board" is to write through it
  // and look at the panel.  If this text shows up, every later UEFI stage --
  // UniXNU.efi, DarwinBoot -- can print normally.
  //
  if (SystemTable->ConOut != NULL) {
    SystemTable->ConOut->OutputString (
      SystemTable->ConOut,
      L"\r\n\r\nCONOUT REACHES PANEL\r\n"
      );
  }

  // Disarm first: WDT_MODE with only the key clears ENABLE.
  MmioWrite32 (TOPRGU_BASE + WDT_MODE_OFFSET, WDT_MODE_KEY);

  // WDT_LENGTH[15:5] = seconds * 64, [4:0] = key.
  Length = (((UINT32)BEACON_SECONDS * 64) << 5) | WDT_LENGTH_KEY;
  MmioWrite32 (TOPRGU_BASE + WDT_LENGTH_OFFSET, Length);

  // Arm with external reset.
  MmioWrite32 (
    TOPRGU_BASE + WDT_MODE_OFFSET,
    WDT_MODE_KEY | WDT_MODE_EXTEN | WDT_MODE_ENABLE
    );

  return EFI_SUCCESS;
}
