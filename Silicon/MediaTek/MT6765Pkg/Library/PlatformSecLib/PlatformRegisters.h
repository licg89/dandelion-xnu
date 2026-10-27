#ifndef _PLATFORM_REGISTERS_H_
#define _PLATFORM_REGISTERS_H_

//
// Display OVL Registers
//
// [INHERITED] From MT6768Pkg, unchanged.
// TODO(MT6765): verify against dandelion's display hardware before trusting it.
//   The Display OVL block base comes from the platform MemoryMapLib entry
//   "Display OVL"; the only value that has to be measured is the *offset*.
//   On lancelot (MT6768) the map places OVL at 0x1400B000 inside the DDP
//   window at 0x14000000. dandelion's vendor DT only describes the DDP block
//   (ddp@14000000), not the OVL sub-block, so what must be measured is:
//     1. the OVL base inside the DDP window (probe +0x0000..+0x2000 for the
//        OVL "DISP_OVL" register signature, or read it from LK's
//        platform/mt6765 display tables if present), and
//     2. whether the constant-blend enable bit is really BIT28 of
//        OVL_PITCH_OFFSET(n) on MT6765 as it is on MT6768.
//   If either is wrong the failure mode is benign (no blending / no display),
//   it does not hang the boot.
//
#define OVL_PITCH_OFFSET(x)           (0x44 + 0x20 * (x))

//
// Display OVL Bits
//
// [INHERITED] Not verified on MT6765 - see the TODO above.
//
#define OVL_CONST_BLEND                BIT28

//
// WatchDog Timer Values
//
// [VERIFIED] WDT_MODE_KEY = 0x22000000, from the MT6765 vendor LK
//   (platform/mt6765/include/platform/mtk_wdt.h: MTK_WDT_MODE_KEY) and the
//   vendor kernel mtk_wdt_v2.c. Every write to WDT_MODE must OR this key in or
//   the write is ignored. Writing only the key with ENABLE (bit 0) clear -
//   which is what this value does, since the whole register is overwritten
//   with 0x22000000 - also disables the watchdog. Same on MT6768.
//
#define WDT_MODE_KEY                   0x22000000

#endif /* _PLATFORM_REGISTERS_H_ */
