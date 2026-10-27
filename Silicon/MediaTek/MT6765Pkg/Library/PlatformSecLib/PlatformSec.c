/**
  Copyright (c) 2011-2012, ARM Limited. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent

  Derived from MT6768Pkg/Library/PlatformSecLib/PlatformSec.c.
  The code is unchanged; only the notes below are new.

  What this does on MT6765/MT6762G (both looked up through the platform
  MemoryMapLib, this library contains no absolute addresses):

    DisableWatchDogTimer()  - "WatchDog Timer" region = TOPRGU at 0x10007000.
                              WDT_MODE_KEY (0x22000000) is confirmed against the
                              vendor LK source for MT6765
                              (platform/mt6765/include/platform/mtk_wdt.h,
                              MTK_WDT_MODE_KEY) and against the vendor kernel
                              driver mtk_wdt_v2.c. On entry from LK the watchdog
                              is armed (dual mode, IRQ + ext reset, ~10 s), so
                              this call is not optional.

    EnableConstantBlending() - "Display OVL" region. See PlatformRegisters.h:
                              the OVL register offsets below are inherited from
                              MT6768 and are NOT verified for dandelion's DDP.
**/

#include <Library/IoLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PlatformSecLib.h>

#include "PlatformRegisters.h"

VOID
DisableWatchDogTimer ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR WDogRegion;

  // Locate "WatchDog Timer" Memory Region
  Status = LocateMemoryRegionByName ("WatchDog Timer", &WDogRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Disable WatchDog Timer
  MmioWrite32 (WDogRegion.Address, WDT_MODE_KEY);
}

VOID
EnableConstantBlending ()
{
  EFI_STATUS                   Status;
  EFI_MEMORY_REGION_DESCRIPTOR OvlRegion;

  // Locate "Display OVL" Memory Region
  Status = LocateMemoryRegionByName ("Display OVL", &OvlRegion);
  if (EFI_ERROR (Status)) {
    return;
  }

  // Enable Constant Blending
  MmioOr32 (OvlRegion.Address + OVL_PITCH_OFFSET (0), OVL_CONST_BLEND);
}

VOID
PlatformInitialize ()
{
  // Disable WatchDog Timer
  DisableWatchDogTimer ();

  // Enable Constant Blending
  EnableConstantBlending ();
}
