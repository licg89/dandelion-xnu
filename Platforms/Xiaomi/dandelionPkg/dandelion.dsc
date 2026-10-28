##
#  Xiaomi Redmi 9A ("dandelion", MediaTek MT6762G / MT6765) platform.
#
#  Derived from Platforms/Xiaomi/lancelotPkg/lancelot.dsc (MT6768Pkg / MT6769T).
#  Deviations, all deliberate:
#
#   1. !include MT6765Pkg/MT6765Pkg.dsc.inc instead of MT6768Pkg's, and
#      SOC_TYPE = 1 (the MT6762G payload in MT6765Pkg; MT6765Pkg's SOC_TYPE 1
#      is MT6762G, NOT upstream MT6768Pkg's MT6769T).
#   2. PcdSystemMemoryBase / stack base 0x48000000 (shim-measured; lancelot
#      uses 0x40000000).
#   3. MemoryMapLib from dandelionPkg: GICR 0x0C100000, MSDC Top-0 0x11CD0000.
#      The GICR fix has to happen in BOTH places (the PCD in MT6765Pkg.dsc.inc
#      and this map) - see MT6765Pkg/README.md section 2.
#   4. SMBIOS / frame buffer / ACPI identity changed to the Redmi 9A.
#
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = dandelion
  PLATFORM_GUID                  = 57BB9BE1-18F9-4891-9C06-1F52BC21B57D
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/dandelionPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = dandelionPkg/dandelion.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = MT6765 (Helio P22)
  # 1 = MT6762G (Helio G25)   <-- dandelion
  #
  SOC_TYPE                       = 1

!include MT6765Pkg/MT6765Pkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  # [MEASURED] The dandelion shim sees a 384 MiB window from 0x48000000
  #            (docs/UEFI-DANDELION-PLAN.md section 2). lancelot (MT6768) uses
  #            0x40000000; do not copy that value.
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x48000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x48000000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Redmi 9A"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"dandelion"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Redmi_9A_dandelion"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Redmi 9A"

  #
  # Simple Frame Buffer
  #
  # 720x1600, NT36525B MIPI DSI (docs/UEFI-DANDELION-PLAN.md section 2).
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|720
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1600
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Storage
  #
  gMediaTekPkgTokenSpaceGuid.PcdStorageIsEMMC|TRUE

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|dandelionPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # Input Libraries
  #
  KeypadDeviceLib|dandelionPkg/Library/KeypadDeviceLib/KeypadDeviceLib.inf

[Components]
  #
  # ACPI Tables
  #
  # Lives in Silicium-ACPI/Platforms/Xiaomi/dandelion (PackagesPath entry
  # "Silicium-ACPI/Platforms/Xiaomi").
  #
#  dandelion/AcpiTables.inf   # TODO: needs Silicium-ACPI/Platforms/Xiaomi/dandelion

  #
  # Input
  #
  SiliciumPkg/Drivers/KeypadDxe/KeypadDxe.inf
  SiliciumPkg/Drivers/KeypadDeviceDxe/KeypadDeviceDxe.inf

  #
  # M2 progress beacon (re-arms the watchdog once DXE dispatch works).
  # dandelion has no UART, so this is how "reached DXE" becomes observable.
  # Temporary: see the comment in the driver.
  #
  dandelionPkg/Drivers/PlatformProgressDxe/PlatformProgressDxe.inf
  dandelionPkg/Drivers/DandelionConsoleDxe/DandelionConsoleDxe.inf

  #
  # M4 storage beacon: [Depex] gEfiSimpleFileSystemProtocolGuid, so it only
  # runs once FAT has a volume.  Its line on the panel is the M4 criterion.
  #
  dandelionPkg/Drivers/StorageBeaconDxe/StorageBeaconDxe.inf
