# drivers/ — display and touch groundwork for dandelion

**Status: untested skeleton code. Nothing in this directory has run on the phone,
and nothing in it is compiled into the kernel.** It exists so that the register
maps, the panel arithmetic and the bring-up order are written down in the same
tree as the plans that explain them, and so that a later session starts from
code that has been reviewed rather than from a blank page.

| file | what it is |
| --- | --- |
| `dandelion_display.h` | DSI0 / MIPI TX register offsets, panel parameters, GPIOs, the bus interface |
| `dandelion_display.c` | timing arithmetic, DSI state read-back, framebuffer fill, panel DCS sequence |
| `dandelion_touch.h` | SPI3, GPIOs, Novatek addresses, the transport interface, the report types |
| `dandelion_touch.c` | Novatek register access, reset sequences, identification, one report |

The plans are `docs/DISPLAY-PLAN.md` and `docs/TOUCH-PLAN.md`; every constant in
the code is cited there to a file, a log line or a named upstream project.

## Why these are not in the build

`config/`, `patches/`, `pexpert/`, `iokit/` and `tools/ci/` belong to the patch
series owner. Adding a driver to the XNU build is a change to
`Resources/XnuPatches/0001-xnu-dandelion-port.patch` (or a new patch) plus a
`files` entry, and it has to be ordered against the rest of the series — that is
a decision for whoever owns the series, not for this directory. Until then these
files are compiled by nothing.

## How to check them without a phone

There is no build product to compare against, so the checks are:

1. **Syntax.** Either a freestanding clang target:
   ```
   clang --target=aarch64-none-elf -ffreestanding -nostdlib -fsyntax-only \
         -std=c99 drivers/dandelion_display.c drivers/dandelion_touch.c
   ```
   or, when no C compiler is available, a parse with pycparser after stripping
   `#include`/`#define` lines and supplying `stdint.h` typedefs. The second is a
   grammar check only — it proves nothing about types or behaviour.
2. **Arithmetic.** The three pure functions
   (`dandelion_dsi_horizontal_wc`, `dandelion_dsi_phy_timcon`,
   `dandelion_dsi_pll_div`) must reproduce the tables in
   `docs/DISPLAY-PLAN.md` section 3 exactly. If a constant is changed in one
   place it has to be changed in both.
3. **On the phone.** Ladder steps D0 and T3 in the two plans are the first
   things that can be tested at all, and both of them are read-only.

## What these files deliberately do not do

- They do not program the display into a state LK has not already produced.
  LK leaves DSI0 running in video mode, and the kernel console already draws into
  the buffer it scans out; the first useful step is to *read that state back*,
  not to overwrite it.
- They do not guess. Where a value is unknown the code says so and returns
  `..._ERR_UNIMPLEMENTED` or leaves a `TODO` with the exact register or command
  that would settle it.

## Provenance

- Register offsets and PHY formulas: MediaTek LK, `platform/mt6765/` and
  `platform/mt6765/include/platform/` (see the header of each file).
- Panel parameters, DCS init sequence and the Novatek protocol constants: the
  Xiaomi vendor kernel for this device, `dandelion-q-oss` @ `8974b91`. That tree
  is GPL-2.0. No code was copied from it — the constants are facts about the
  chip, and each one is attributed in a comment — but anything that ends up in a
  distributed binary should be reviewed by whoever owns licensing.
- Structural reference: `UniXNU/ShengNovatekTouch` (Apache-2.0), which targets
  the NT36532E, a different chip in the same family. Its memory map and its
  raw-matrix decoding do not apply here; its layering does.
