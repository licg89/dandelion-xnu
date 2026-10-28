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

EFI_STATUS
EFIAPI
PlatformProgressDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  UINT32  Length;

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
