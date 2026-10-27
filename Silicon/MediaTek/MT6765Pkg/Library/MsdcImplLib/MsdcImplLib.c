/**
  MT6765 / MT6762G MSDC (eMMC / SD) glue.

  Derived from MT6768Pkg/Library/MsdcImplLib/MsdcImplLib.c. The code is
  unchanged; everything below the header is the upstream logic plus notes.

  ---------------------------------------------------------------------------
  Where the MSDC0 base address lives (it is not in this file)

  [VERIFIED] MSDC0 controller = 0x11230000 on MT6765/MT6762G. Three sources:
    - vendor DT: msdc0:msdc@11230000 { reg = <0 0x11230000 0 0x10000> }
    - LK: platform/mt6765/include/platform/mt_reg_base.h MSDC0_BASE
    - dandelion shim: bootshim/msdc.c (#define MSDC0_BASE 0x11230000)

  This is the SAME value lancelot (MT6768) uses, so "MSDC base" is *not* a
  difference between the two SoCs. And this library does not contain it
  anyway: MsdcDxe locates the "MSDC-0" memory region through the platform
  MemoryMapLib. The memory map is the single source of truth; if it says
  something else, this library will drive the wrong block.

  TODO(MT6765): one MSDC-adjacent memory map entry DOES differ from lancelot.
  Dandelion's msdc0_top (the HS200/HS400 delay-line tuning block) is at
  0x11CD0000 (vendor DT msdc0_top@11cd0000), while lancelot's MemoryMapLib
  maps "MSDC Top-0" at 0x11C90000. A dandelion platform must use 0x11CD0000.
  Nothing in this library or in MT6765Pkg reads it (MsdcPadTuneReg and
  TuningStep below are plain register offsets), so this is a platform task,
  not a package task.

  ---------------------------------------------------------------------------
  Clock side

  [VERIFIED] The clock gates and the top-level mux this file asks for by name
  are bit-identical between MT6768Pkg's ClockImplLib and the MT6765 vendor
  kernel (drivers/clk/mediatek/clk-mt6765.c):

    name (ClockImplLib)   register                      MT6765 source
    INFRA_MSDC0           0x88 set / 0x8c clr / 0x94 sta, bit 2
                          GATE_IFR3(CLK_IFR_MSDC0, ..., 2)
    INFRA_MSDC1           same block, bit 4
                          GATE_IFR3(CLK_IFR_MSDC1, ..., 4)
    INFRA_FAES_FDE        0xa4 / 0xa8 / 0xac, bit 29
                          GATE_IFR4(CLK_IFR_FAES_FDE, ..., 29)
    INFRA_MSDC0_SRC       0xc0 / 0xc4 / 0xc8, bit 9
                          GATE_IFR5(CLK_IFR_MSDC0_SRC, ..., 9)
    INFRA_MSDC1_SRC       same block, bit 10
    INFRA_AP_MSDC0        same block, bit 7
    INFRA_MD_MSDC0        same block, bit 8
    TOP_MSDC50_0_SEL      CLK_CFG_3 = 0x70, set 0x74, clr 0x78, shift 8, width 3
    TOP_MSDC50_0_HCLK_SEL same, shift 0, width 2
    TOP_MSDC30_1_SEL      same, shift 16, width 3

  So the GetId()/GetFrequency()/SetEnable() calls below land on the right
  registers on MT6765, provided the ClockImplLib is the one in this package.

  [VERIFIED] All three parent tables match MT6768Pkg entry for entry, MSDCPLL
  in slot 1: msdc5hclk_parents (4 entries), msdc50_0_parents (8) and
  msdc30_1_parents (8) are identical arrays on MT6765 (clk-mt6765.c lines
  473-500). So unlike the apmixed PLL block, the MSDC clock path needs no
  SOC_TYPE branch anywhere.

  ---------------------------------------------------------------------------
  GPIO side

  [VERIFIED] MT6765's eMMC pin group is GPIO122..133: the MT6765 LK explicitly
  skips them in its generic GPIO init
  (platform/mt6765/mt_gpio_init.c:229, "skip msdc pins init GPIO122~133"),
  and the vendor kernel drives exactly MSDC0_GPIO_MODE0 (pins 120..127) and
  MODE1 (pins 128..135) for this block. The pin numbers used in InitGpio()
  below (122,123,124,125,126,127,128,129,130,132) all fall inside that range,
  so they are at least self-consistent for MT6765.

  TODO(MT6765): the individual signal-to-pin assignment (which of 122..133 is
  CMD/CLK/DAT0..7/DS/RSTB) was NOT confirmed from MT6765 source - the
  inherited MT6768 assignment is plausible but unproven. If eMMC init hangs
  with CMD line idle, this is the first thing to check: read
  Pinctrl+0x3F0 and Pinctrl+0x400 after InitGpio() and compare against the
  vendor kernel's msdc_set_pin_mode() result
  (ComboA/mt6765/msdc_cust.c, which sets MODE0[25:8] and MODE1[23:0] to
  0x222222 or 0x111111 depending on MSDC0_GPIO_MODE_TRAP bit 13,
  i.e. Pinctrl+0x6F0 & (0x3 << 13)).

  ---------------------------------------------------------------------------
  Regulator side (blocking for real hardware)

  TODO(MT6765): PowerControl() calls the PMIC protocol with the names
  "ldo_vemc", "ldo_vmch", "ldo_vmc". Those names resolve inside the shared
  MediaTekPkg/Drivers/MT6358PmicDxe driver, whose register table is MT6358's.
  Dandelion's PMIC is MT6357 and the addresses differ (see
  PmicWrapperImplLib.c). Until an MT6357 driver replaces it, PowerControl()
  on a dandelion build will poke the wrong PMIC registers - which on an eMMC
  path means the flash may be left unpowered or, worse, another rail is
  touched. This is the single biggest blocker between "MT6765Pkg builds" and
  "MT6765Pkg reads a block".

  ---------------------------------------------------------------------------
  Platform configuration values (gPlatformInfo)

  [INHERITED] All of these come from MT6768Pkg unverified:
    NumberOfHosts = 2      - MT6765 also has msdc0+msdc1 (vendor DT), fine.
    UseTop        = TRUE   - needs the msdc0_top base to be mapped (0x11CD0000
                             above) or every pad-tune write goes nowhere.
    MsdcPadTuneReg = 0xf0  - OFFSET_MSDC_PAD_TUNE0 in the vendor msdc_reg.h,
                             verified to exist at 0xf0 for MT6765.
    TuningStep    = {32,64} - board/timing specific, unverified on dandelion.
    AsyncFifo/BusyCheck/StopClkFix/EnhanceRx - MT6768-class quirks; unverified.
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/MsdcImplLib.h>

#include <Protocol/MtkGpio.h>
#include <Protocol/MtkClock.h>
#include <Protocol/MtkPmic.h>

MSDC_PLATFORM_INFO gPlatformInfo = {
  .NumberOfHosts = 2,
  .UseTop = TRUE,
  .MsdcPadTuneReg = 0xf0,
  .TuningStep = {32, 64},
  .AsyncFifo = TRUE,
  .BusyCheck = TRUE,
  .StopClkFix = TRUE,
  .EnhanceRx = TRUE
};

STATIC MTK_GPIO_PROTOCOL *mGpio = NULL;
STATIC MTK_CLOCK_PROTOCOL *mClock = NULL;
STATIC MTK_PMIC_PROTOCOL *mPmic = NULL;

VOID
GetSourceClockRate (
  UINT32 Index,
  UINTN *Hz)
{
  EFI_STATUS Status;
  UINT32 ClockId;

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "TOP_MSDC50_0" : "TOP_MSDC30_1", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Get Clock Frequency
  Status = mClock->GetFrequency(ClockId, Hz);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Frequency! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }
}

VOID
SourceClockControl (
  UINT32 Index,
  BOOLEAN Enable)
{
  EFI_STATUS Status;
  UINT32 ClockId;

  if (Index == 0) {
    // Get Clock Id
    Status = mClock->GetId("INFRA_FAES_FDE", &ClockId);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
      ASSERT_EFI_ERROR (Status);
    }

    // Enable Clock
    Status = mClock->SetEnable(ClockId, Enable);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
      ASSERT_EFI_ERROR (Status);
    }
  }

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "INFRA_MSDC0_SRC" : "INFRA_MSDC1_SRC", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Enable Clock
  Status = mClock->SetEnable(ClockId, Enable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }
}

VOID
ClockControl (
  UINT32 Index,
  BOOLEAN Enable)
{
  EFI_STATUS Status;
  UINT32 ClockId;

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "TOP_MSDC50_0" : "TOP_MSDC30_1", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Enable Clock
  Status = mClock->SetEnable(ClockId, Enable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Get Clock Id
  Status = mClock->GetId(Index == 0 ? "INFRA_MSDC0" : "INFRA_MSDC1", &ClockId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Get Clock Id! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Enable Clock
  Status = mClock->SetEnable(ClockId, Enable);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Enable Clock! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }
}

VOID
PowerControl (
  UINT32 Index,
  BOOLEAN Enable)
{
  EFI_STATUS Status;

  // TODO(MT6765): these regulator names resolve to MT6358 register addresses
  // inside MediaTekPkg/Drivers/MT6358PmicDxe. Dandelion's PMIC is MT6357, so
  // this is wrong on real hardware until that driver is forked. See the
  // header comment.
  if (Index == 0 && FixedPcdGetBool(PcdStorageIsEMMC)) {
    // Enable VEMC LDO
    Status = mPmic->RegulatorSetEnable("ldo_vemc", Enable);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Enable LDO VEMC! Status = %r\n", Status));
      ASSERT_EFI_ERROR (Status);
    }
  } else {
    // Enable VMCH LDO
    Status = mPmic->RegulatorSetEnable("ldo_vmch", Enable);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Enable LDO VMCH! Status = %r\n", Status));
      ASSERT_EFI_ERROR (Status);
    }

    // Enable VMC LDO
    Status = mPmic->RegulatorSetEnable("ldo_vmc", Enable);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failed to Enable LDO VMC! Status = %r\n", Status));
      ASSERT_EFI_ERROR (Status);
    }
  }
}

VOID
InitGpio (
  UINT32 Index)
{
#if SOC_TYPE == 1
  //
  // MT6765 / MT6762G: the pin *range* is verified (GPIO122..133, MT6765 LK
  // mt_gpio_init.c:229), the individual signal assignment is inherited from
  // MT6768 and unproven. See the header for how to check it on silicon.
  //
  DEBUG ((DEBUG_INFO, "MT6765: MSDC0 GPIO pin assignment inherited from MT6768, unverified\n"));
#endif

  if (Index == 0 && FixedPcdGetBool(PcdStorageIsEMMC)) {
    // Setup EMMC GPIO
    mGpio->SetMode (123, 1); // DAT0
    mGpio->SetMode (128, 1); // DAT1
    mGpio->SetMode (125, 1); // DAT2
    mGpio->SetMode (132, 1); // DAT3
    mGpio->SetMode (126, 1); // DAT4
    mGpio->SetMode (129, 1); // DAT5
    mGpio->SetMode (127, 1); // DAT6
    mGpio->SetMode (130, 1); // DAT7
    mGpio->SetMode (122, 1); // CMD
    mGpio->SetMode (124, 1); // CLK
  }
  else {
    // Setup SDCard GPIO
    // TODO(MT6765): SD pins (161..171 here) unverified - dandelion has no SD
    // slot wired for our use; the vendor DT does not describe one, so this
    // path is dead code on this board.
    mGpio->SetMode (161, 1); // DAT0
    mGpio->SetMode (162, 1); // DAT1
    mGpio->SetMode (163, 1); // DAT2
    mGpio->SetMode (164, 1); // DAT3
    mGpio->SetMode (170, 1); // CMD
    mGpio->SetMode (171, 1); // CLK
  }
}

EFI_STATUS
EFIAPI
MsdcLibConstructor (VOID)
{
  EFI_STATUS Status;

  // Locate Gpio Protocol
  Status = gBS->LocateProtocol (&gMediaTekGpioProtocolGuid, NULL, (VOID **)&mGpio);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Locate GPIO Protocol! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Locate Clock Protocol
  Status = gBS->LocateProtocol (&gMediaTekClockProtocolGuid, NULL, (VOID **)&mClock);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Locate Clock Protocol! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Locate PMIC Protocol
  Status = gBS->LocateProtocol (&gMediaTekPmicProtocolGuid, NULL, (VOID **)&mPmic);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Locate PMIC Protocol! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Set VMCH LDO Voltage
  // TODO(MT6765): 3300000 mV is the MT6358 SD/eMMC I/O voltage. On MT6357 the
  // VOSEL encoding differs (mask/shift), so the value written here would map
  // to a different voltage once an MT6357 driver is in place - re-check.
  Status = mPmic->RegulatorSetVoltage("ldo_vmch", 3300000);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Set VMCH LDO Voltage! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  // Set VMC LDO Voltage
  Status = mPmic->RegulatorSetVoltage("ldo_vmc", 3300000);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to Set VMC LDO Voltage! Status = %r\n", Status));
    ASSERT_EFI_ERROR (Status);
  }

  return Status;
}
