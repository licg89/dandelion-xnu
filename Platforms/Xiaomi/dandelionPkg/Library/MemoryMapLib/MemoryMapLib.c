/**
  Memory map for the Xiaomi Redmi 9A ("dandelion", MediaTek MT6762G / MT6765).

  Based on Platforms/Xiaomi/lancelotPkg/Library/MemoryMapLib/MemoryMapLib.c
  (MT6768 / MT6769T). Deviations, with provenance:

    [MEASURED] DDR window starts at 0x48000000 (384 MiB), not 0x40000000
               (dandelion shim probe; docs/UEFI-DANDELION-PLAN.md section 2).
               UEFI Stack / UEFI FD / DXE Heap follow the base.
    [DT]       GIC Redistributors at 0x0C100000 / 2 MiB. mt6765.dts:
               reg = <0 0x0c000000 0 0x40000>,   <- distributor
                     <0 0x0c100000 0 0x200000>,  <- redistributor
               Android dmesg: "GICv3: CPU0: found redistributor 0 region
               0:0x000000000c100000". lancelot says 0x0C040000 - WRONG here.
               This table is what ArmGicV3 / the DXE memory map walk uses; the
               PcdGicRedistributorsBase in MT6765Pkg.dsc.inc and this entry
               must agree.
    [DT]       MSDC Top-0 (msdc0_top) at 0x11CD0000 and MSDC Top-1 at
               0x11C90000 - note that the two are SWAPPED relative to MT6768
               (lancelot: top-0 0x11C90000, top-1 0x11CD0000). Confirmed twice:
               mt6765.dts ("msdc0_top@11cd0000", "msdc1_top@11c90000") and LK
               platform/mt6765/include/platform/mt_reg_base.h
               (MSDC0_TOP_BASE 0x11CD0000, MSDC1_TOP_BASE 0x11C90000).

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/MemoryMapLib.h>

STATIC
EFI_MEMORY_REGION_DESCRIPTOR
gMemoryDescriptor[] = {
  // Name, Address, Length, HobOption, ResourceType, ResourceAttribute, MemoryType, ArmAttribute

  // DDR Regions
  //
  // [MEASURED] window base 0x48000000 (see header). The first three entries are
  //            the ones this port actually depends on; the reserved holes below
  //            are [INHERITED] from lancelot (MT6768) and have NOT been
  //            re-derived from dandelion's DT reserved-memory nodes.
  //            TODO(MT6765): re-derive before any of this is flashed.
  //            "UEFI FD" here must equal [uefi_fd].base in
  //            Resources/Configs/dandelion.toml (the shim copies the FD there),
  //            and "DXE Heap" must start at that base + FD size.
  {"UEFI Stack",         0x48000000, 0x00040000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"UEFI FD",            0x48880000, 0x00200000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
  {"DXE Heap",           0x48A80000, 0x03C00000, AddMem, SYS_MEM, SYS_MEM_CAP, Conv,   WRITE_BACK},
  {"ATF Reserved",       0x4CE00000, 0x00060000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"TEE Reserved",       0x70000000, 0x05000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"AP MD C SMEM",       0x76000000, 0x00C00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"SPM Reserved",       0x77FF0000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"AP MD NC SMEM",      0x78000000, 0x01000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"Display Reserved",   0x7D9B0000, 0x02250000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH}, // [INHERITED]
  {"TEE SecMem",         0x7FC00000, 0x00200000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"SSPM Reserved",      0x7FF00000, 0x000C0000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"SCP Reserved",       0x9F900000, 0x00600000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"CCCI Tag Mem",       0x9FFEF000, 0x00010000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"MD Mem Usage",       0xAE000000, 0x10000000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"TEE Log Reserved",   0xBFC00000, 0x00020000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"ATF Log Reserved",   0xBFE00000, 0x00040000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]
  {"ATF Ramdump",        0xBFF80000, 0x00080000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_BACK}, // [INHERITED]

  // Register Regions
  //
  // All of these are shared with MT6768 EXCEPT the two GIC and the two MSDC-top
  // entries, which are called out above.
  {"GIC Distributor",    0x0C000000, 0x00040000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // [DT]    same as lancelot
  {"GIC Redistributors", 0x0C100000, 0x00200000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // [DT]    lancelot: 0x0C040000
  {"Top Ck Gen",         0x10000000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Infra Cfg AO",       0x10001000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"Pinctrl",            0x10005000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"WatchDog Timer",     0x10007000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"AP Mixed",           0x1000C000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"PMIC Wrapper",       0x1000D000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"EMI",                0x10219000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSDC-0",             0x11230000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSDC-1",             0x11240000, 0x00010000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE},
  {"MSDC Top-0",         0x11CD0000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // [DT]    lancelot: 0x11C90000
  {"MSDC Top-1",         0x11C90000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}, // [DT]    lancelot: 0x11CD0000
  {"Display OVL",        0x1400B000, 0x00001000, AddDev, MMAP_IO, UNCACHEABLE, MmIO,   NS_DEVICE}  // [INHERITED] TODO(MT6765): verify OVL offset
};

VOID
GetMemoryMap (
  OUT EFI_MEMORY_REGION_DESCRIPTOR **MemoryDescriptor,
  OUT UINT8                         *MemoryDescriptorCount)
{
  // Pass Data
  *MemoryDescriptor      = gMemoryDescriptor;
  *MemoryDescriptorCount = ARRAY_SIZE (gMemoryDescriptor);
}
