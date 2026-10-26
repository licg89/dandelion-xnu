# mouseos XNU-on-MediaTek code study

Source-level study of the four mouseos repositories, aimed at a *different* MediaTek port:
**MT6762G / Redmi 9A, LK bootloader, no UART pads, shim at EL1 with MMU off.**

All quotations below are verbatim from the cloned trees (line numbers are from the snapshot named
below; only `…` marks an elision). Everything is treated as read-only data.

## Provenance

| Repo | Commit | Date | URL |
|---|---|---|---|
| `xnu_bootshim_benesse_a04br3` | `f82c3b85e26bb5a2be8045776a5af78215d35982` | 2026-10-22 | https://github.com/mouseos/xnu_bootshim_benesse_a04br3 |
| `xnu_device_benesse_a04br3` | `a23d3b9775fe5d24f91c715257421cee61d1e621` | 2026-10-23 | https://github.com/mouseos/xnu_device_benesse_a04br3 |
| `xnu_kernel_benesse_a04br3` | `f5d43bf1677955e7e367545b9bbbd6449776cdda` | 2026-10-23 | https://github.com/mouseos/xnu_kernel_benesse_a04br3 |
| `NativeMsdc` | `e718d5d7bde7f6986cd9a7b425c82c50a4dcc3b8` | 2026-10-26 | https://github.com/mouseos/NativeMsdc |

Cloned shallow into `D:\aios\research\refs\<repo>`. Paths in this document are relative to those
directories unless prefixed. Two non-mouseos local trees are cited **only** in §3.5 for MT6762G
facts and are marked as such: `refs\mt6765-vendor\` (MediaTek MT6765 vendor kernel/DTS) and
`refs\lk-svoboda18\` (MTK LK with an mt6765 platform).

Line counts of every file actually quoted: `shim.c` 150, `start.S` 110, `handoff.c` 338,
`adt.c` 205, `fdt.c` 129, `pe_a04br3_gic.c` 108, `pe_a04br3_watchdog.c` 23, `pe_serial.c` 1242,
`pe_kprintf.c` 181, `arm_vm_init.c` 2498, `video_console.c` 3298, `iso_font.c` ~310,
`a04br3_msdc.c` 793, `NativeMsdc.cpp` 1500, `NativeMsdc.h` 599.

## Verdict in one table

| Question | Answer |
|---|---|
| How do bytes get out? | One **polled 8250-style UART at PA `0x11005000`**, LK's baud rate inherited. Nothing else. |
| RAM log buffer read back next boot? | **No.** The mechanism exists in XNU (`PLATFORM_PANIC_LOG_PADDR`) and the port **explicitly disables** it (`PLATFORM_PANIC_LOG_DISABLED`). |
| Framebuffer console with font blitter? | **In the tree, compiled, and *not used*** except behind `a04br3.display_probe=1`. The blitter is Apple's stock `video_console.c` + 8×16 `iso_font.c`. |
| USB? | Userland only (`/dev/a04boot` + `0e8d:20ff` gadget). Useless for a kernel that dies before PBS/bsd_init. |
| Does the shim touch GIC? | **No GIC code at all** in the shim. Zero hits for `GIC`/`gicd`/`GICR`/`ICC_` in the whole bootshim repo. |
| GIC version | MT8167 = **GICv2 / GIC-400** (`arm,gic-400`, GICD `0x10310000`, GICC `0x1032f000`). MT6762G = **GICv3** (see §3.5). |
| Group-register wall? | Yes — dodged by never writing `GICD_IGROUPRn`; they enable only **Group 1** through `GICC_CTLR` and leave Group 0 to ATF. |
| Does the port reach userspace? | It reaches a **BSD PID 1** on a RAMDisk/HFS root (documented hardware milestone). It does **not** claim a working OS, Apple launchd GUI, or SpringBoard. |
| What to steal for no-UART? | The **eMMC PIO block path + persistent-store ABI** (NativeMsdc), the **boot_args Video → video_console 8×16 blitter** path, and the **panic-log slot** they turned off. |

---

# 1. LOGGING

## 1.1 The only early channel: a polled UART at physical `0x11005000`

`xnu_bootshim_benesse_a04br3/shim.c:13-34` — *entire* output implementation of the shim:

```c
#define UART_BASE ((uintptr_t)0x11005000)
#define UART_LSR (UART_BASE + 0x14)
#define UART_TX_READY (1u << 5)

static void putc_uart(char c)
{
    /* Preserve the UART setup inherited from LK, including its baud rate. */
    for (unsigned int n = 0; n < 1000000; n++) {
        if (*(volatile uint32_t *)UART_LSR & UART_TX_READY) {
            *(volatile uint32_t *)UART_BASE = (uint8_t)c;
            return;
        }
    }
}

static void puts_uart(const char *s)
{
    while (*s) {
        if (*s == '\n') putc_uart('\r');
        putc_uart(*s++);
    }
}
```

Facts that matter for a port with no UART pads:

* **No initialisation at all** — no divisor, no `LCR`, no clock gate, no pin mux. The comment is the
  design: LK already did it. The device repo records the inherited rate:
  `xnu_device_benesse_a04br3/board.json:6-7`
  `"uart_physical_address": "0x11005000", "uart_baud_inherited_from_lk": 921600`.
* **Bounded spin, then silently drop the byte** (`n < 1000000`). A dead/deaf UART costs ~1e6 reads
  per character and produces no output — it never hangs, and it never reports failure. Every
  `value()`/`puts_uart()` call therefore succeeds from the caller's point of view whether or not the
  board has a UART pin.
* Every 8-bit write to `UART_BASE` is the THR (`0x11005000 + 0`); the LSR bit `1u<<5` is THRE.

The same file wires three callbacks that the loader code uses for tracing
(`shim.c:47-48`):

```c
void handoff_trace(const char *label, uint64_t v) { value(label, v); }
void handoff_message(const char *message) { puts_uart(message); }
```

## 1.2 Kernel side: `kprintf` → `PE_kputc` → `uart_putc` → `mtk_uart_transmit_data`

`xnu_kernel_benesse_a04br3/pexpert/arm/pe_kprintf.c:57-72`:

```c
__startup_func
static void
PE_init_kprintf(void)
{
	if (serial_init()) {
		PE_kputc = serial_putc_crlf;
	} else {
		/**
		 * If serial failed to initialize then fall back to using the console,
		 * and assume the console is using the video console (because clearly
		 * serial doesn't work).
		 */
		PE_kputc = console_write_unbuffered;
	}
}
STARTUP(KPRINTF, STARTUP_RANK_FIRST, PE_init_kprintf);
```

`pe_kprintf.c:156-163`:

```c
static void
serial_putc_crlf(char c)
{
	if (c == '\n') {
		uart_putc('\r');
	}
	uart_putc(c);
}
```

`pexpert/arm/pe_serial.c:1062-1073` — the multiplexer that every byte passes through, and note the
polling behaviour:

```c
	while (fns != NULL) {
		if (force || serial_do_transmit) {
			while (!fns->transmit_ready()) {
				if (!uart_hibernation) {
					if (!poll && irq_available_and_ready(fns)) {
						serial_wait_for_interrupt(fns);
					} else {
						serial_poll();
					}
				}
			}
			fns->transmit_data((uint8_t)c);
		}
		fns = fns->next;
	}
```

`pe_serial.c:684-724` — the MT8167 driver, added as a new `#if defined(A04BR3)` block at the bottom
of the file. This is the whole driver:

```c
#if defined(A04BR3)
/* MT8167 UART: 16550-compatible registers at 32-bit spacing. Retain LK's
 * clock, divisors and 921600 baud line setup; polling needs no IRQ driver.
 */
static vm_offset_t mtk_uart_base;
static unsigned int mtk_uart_transmit_ready(void)
{
	return (*(volatile uint32_t *)(mtk_uart_base + 0x14) & 0x20) != 0;
}
static void mtk_uart_transmit_data(uint8_t c)
{
	*(volatile uint32_t *)mtk_uart_base = c;
}
…
static void mtk_uart_init(void)
{
	extern void a04br3_early_uart_handoff(vm_offset_t);
	a04br3_early_uart_handoff(mtk_uart_base);
	const char *p = "\r\nXNU_A04BR3_UART_MAPPED\r\n";
	while (*p) {
		unsigned int remaining = 0xffff;
		while (!mtk_uart_transmit_ready() && --remaining) { }
		if (!remaining) { return; }
		mtk_uart_transmit_data((uint8_t)*p++);
	}
}
SECURITY_READ_ONLY_LATE(static struct pe_serial_functions) mtk_uart_serial_functions = {
	.init = mtk_uart_init,
	.transmit_ready = mtk_uart_transmit_ready,
	.transmit_data = mtk_uart_transmit_data,
	.receive_ready = mtk_uart_receive_ready,
	.receive_data = mtk_uart_receive_data,
	.device = SERIAL_MTK_UART
};
#endif
```

`pe_serial.c:822-830` — how it is bound. Note the **hard-coded sanity check** on `reg` *and* on the
SoC base; a port cannot reuse this verbatim:

```c
#if defined(A04BR3)
	if (SecureDTFindEntry("name", "uart0", &entryP) == kSuccess &&
	    SecureDTGetProperty(entryP, "reg", (void const **)&reg_prop, &prop_size) == kSuccess &&
	    prop_size == 2 * sizeof(uintptr_t) && *reg_prop == 0x01005000 &&
	    *(reg_prop + 1) == 0x1000 && soc_base == 0x10000000) {
		mtk_uart_base = ml_io_map(soc_base + *reg_prop, *(reg_prop + 1));
		if (mtk_uart_base) { register_serial_functions(&mtk_uart_serial_functions); }
	}
#endif
```

Two consequences:

1. `has_irq` is never set for `SERIAL_MTK_UART` (no `interrupts` property is read), so
   `irq_available_and_ready()` is false forever and **every kernel byte is emitted by polling**.
2. `serial_init()` returns 0 when `gPESF == NULL` (`pe_serial.c:953-955`), which is exactly the
   condition that makes `PE_kputc = console_write_unbuffered` — i.e. **removing the UART makes the
   kernel fall back to the framebuffer console automatically**. That single line is the most
   valuable sentence in the file for a no-UART port.

Gate on `kprintf` itself — `pe_kprintf.c:20-23` and `41-43`:

```c
// disable_serial_output disables kprintf() *and* unbuffered panic output.
SECURITY_READ_ONLY_LATE(bool) disable_serial_output = true;
…
	if (debug_boot_arg & DB_KPRT) {
		disable_serial_output = false;
	}
```

The device's command line supplies it (`board.json:26`):
`"-v serial=3 debug=0x14e keepsyms=1 cpus=1 -noprogress"` — `0x14e` contains `DB_KPRT` (`0x08`),
so `kprintf` is live.

## 1.3 Before any C code runs: the kernel writes the UART from assembly

`xnu_kernel_benesse_a04br3/osfmk/arm64/start.s:499-525` (inside `start_first_cpu`, i.e. the very
first instructions of XNU, `x0` = boot_args, MMU off):

```asm
#if defined(A04BR3)
	/* First real kernel-entry evidence, before stack/MMU initialization.
	 * Preserve x0 (boot_args) and the inherited MediaTek UART configuration.
	 */
	msr DAIFSet, #0xf
	adrp x9, L_a04br3_entry_message@page
	add x9, x9, L_a04br3_entry_message@pageoff
	mov x10, #0x5000
	movk x10, #0x1100, lsl #16
L_a04br3_entry_char:
	ldrb w11, [x9], #1
	cbz w11, L_a04br3_entry_done
	mov w13, #0xffff
L_a04br3_entry_wait:
	ldr w12, [x10, #0x14]
	tbnz w12, #5, L_a04br3_entry_send
	subs w13, w13, #1
	b.ne L_a04br3_entry_wait
	b L_a04br3_entry_done
L_a04br3_entry_send:
	str w11, [x10]
	b L_a04br3_entry_char
L_a04br3_entry_message:
	.asciz "\r\nXNU_A04BR3_KERNEL_ENTRY\r\n"
```

This is a self-contained 20-line "did the jump land?" probe that works with MMU off, no stack, no
C runtime. **Copy this shape** for any first-marker mechanism you choose; on a padless board the
same 20 lines become the first `store` into your reserved log page.

## 1.4 Keeping the log alive across the TTBR0→TTBR1 switch

This is subtle and well solved. `osfmk/arm64/arm_vm_init.c:1815-1835`:

```c
#if defined(A04BR3)
/* Adapted from ios-sweet patch 0012: retain early diagnostics across TTBR0
 * teardown. MT8167 uses a Device UART block instead of SWEET's framebuffer. */
#define A04BR3_EARLY_UART_VA (LOW_GLOBAL_BASE_ADDRESS + ARM_TT_L2_SIZE)
static boolean_t a04br3_uart_final;
static boolean_t a04br3_uart_panicking;
static vm_offset_t a04br3_uart_mapped;

static void
a04br3_vm_trace(const char *text, boolean_t final_mapping)
{
	volatile uint32_t *uart = (volatile uint32_t *)(uintptr_t)
	    (final_mapping ? (a04br3_uart_mapped ? a04br3_uart_mapped :
	    A04BR3_EARLY_UART_VA + 0x01005000) : 0x11005000);
	while (*text) {
		unsigned int remaining = 0xffff;
		while (!(uart[5] & 0x20) && --remaining) { }
		if (!remaining) { return; }
		uart[0] = (uint8_t)*text++;
	}
}
```

Three phases in one function: physical `0x11005000` while the low globals are still identity-ish,
then `A04BR3_EARLY_UART_VA + 0x01005000` after the temporary TTBR1 block exists, then
`a04br3_uart_mapped` once `ml_io_map` owns a real mapping.

The block alias itself, `arm_vm_init.c:1882-1897`:

```c
static void
a04br3_early_map_final(void)
{
	const vm_map_address_t va = A04BR3_EARLY_UART_VA;
	assert(va + ARM_TT_L2_SIZE <= gVirtBase);
	tt_entry_t l1 = cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];
	assert((l1 & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) ==
	    (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE));
	tt_entry_t *l2 = (tt_entry_t *)(phystokv(l1 & ARM_TTE_TABLE_MASK));
	tt_entry_t *slot = &l2[(va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT];
	assert(*slot == ARM_TTE_EMPTY);
	*slot = 0x10000000ULL | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK |
	    ARM_TTE_BLOCK_ATTRINDX(CACHE_ATTRINDX_DISABLE) |
	    ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX;
	__asm__ volatile("dsb ishst" ::: "memory");
}
```

One L2 **block** covers the whole device window: `A04BR3.h:17` sets `__ARM_16K_PG__ 1`, and
`osfmk/arm64/proc_reg.h:1247` gives `ARM_16K_TT_L2_SIZE 0x0000000002000000ULL` (32 MiB). So
`0x10000000 .. 0x11FFFFFF` is mapped by that single entry — which is why `alias + 0x01005000` reaches
the UART at `0x11005000`. (With 4 KiB pages an L2 block is 2 MiB and this would *not* work; the 16 KiB
granule is load-bearing here.)

Attributes: index `CACHE_ATTRINDX_DISABLE` (device-nGnRnE for the UART — correct), `AF`, `PNX`
(privileged no-execute), `NX`.

Release on handoff, `arm_vm_init.c:1859-1880`:

```c
/* Once normal serial owns a VM-allocated mapping, release the temporary
 * block before randomized zone/metadata claims can use the same VA. */
void a04br3_early_uart_handoff(vm_offset_t uart);
void
a04br3_early_uart_handoff(vm_offset_t uart)
{
	if (!uart || !a04br3_uart_final) { return; }
	boolean_t release_alias = (a04br3_uart_mapped == 0);
	a04br3_uart_mapped = uart;
	if (release_alias) {
		const vm_map_address_t va = A04BR3_EARLY_UART_VA;
		tt_entry_t l1 = cpu_tte[(va & ARM_TT_L1_INDEX_MASK) >> ARM_TT_L1_SHIFT];
		tt_entry_t *l2 = (tt_entry_t *)(phystokv(l1 & ARM_TTE_TABLE_MASK));
		tt_entry_t *slot = &l2[(va & ARM_TT_L2_INDEX_MASK) >> ARM_TT_L2_SHIFT];
		assert((*slot & (ARM_TTE_VALID | ARM_TTE_TYPE_MASK)) ==
		    (ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK));
		*slot = ARM_TTE_EMPTY;
		__asm__ volatile("dsb ishst" ::: "memory");
		flush_mmu_tlb();
		a04br3_vm_trace("XNU_UART_BOOTSTRAP_ALIAS_RELEASED\r\n", TRUE);
	}
}
```

And the boot-order anchors (`osfmk/arm/arm_init.c`): `543 arm_vm_init(...)` →
`598 PE_create_console()` → `611 PE_init_platform(TRUE, …)` → `634 PE_init_cpu()` →
`677 machine_startup(args)`. Note **console before GIC**: the kernel is printing before any
interrupt controller exists, which is why the console *must* be polled.

## 1.5 RAM log buffer read back on the next boot: **not present, and deliberately disabled**

The port has **no** cross-boot log. The strongest evidence is the negative:

`xnu_kernel_benesse_a04br3/pexpert/pexpert/arm64/A04BR3.h:26-29`:

```c
/* No PAN, LSE, PAC, GICv3, Apple SMP or Apple implementation registers. */
#ifndef ASSEMBLER
#define PLATFORM_PANIC_LOG_DISABLED
#endif
```

That macro disables the exact mechanism a UART-less port needs. XNU's implementation is right there
in `pexpert/arm/pe_init.c:93-99`:

```c
static void
check_for_panic_log(void)
{
#ifdef PLATFORM_PANIC_LOG_PADDR
	gPanicBase = ml_io_map_wcomb(PLATFORM_PANIC_LOG_PADDR, PLATFORM_PANIC_LOG_SIZE);
	panic_text_len = PLATFORM_PANIC_LOG_SIZE - sizeof(struct embedded_panic_header);
	gPanicSize = PLATFORM_PANIC_LOG_SIZE;
#else
```

…and the "previous boot wrote a panic here" handshake, `pe_init.c:158-179`:

```c
	panic_info = (struct embedded_panic_header *)gPanicBase;

	/* Check if a shared memory console is running in the panic buffer */
	if (panic_info->eph_magic == 'SHMC') {
		panic_console_available = TRUE;
		return;
	}
…
	/*
	 * Check to see if a panic (FUNK) is in VRAM from the last time
	 */
	if (panic_info->eph_magic == EMBEDDED_PANIC_MAGIC) {
		printf("iBoot didn't extract panic log from previous session crash, this is bad\n");
	}

	/* Clear panic region */
	bzero((void *)gPanicBase, gPanicSize);
```

The working example they did **not** copy is the Raspberry Pi target in the same tree,
`pexpert/pexpert/arm64/BCM2837.h:63-64`:

```c
#define PLATFORM_PANIC_LOG_PADDR        0x3c0fc000
#define PLATFORM_PANIC_LOG_SIZE         16384        // 16kb
```

So: two `#define`s plus a reserved DRAM hole give you a log that survives a watchdog reset and is
read back by the *next* kernel (not by the shim). The A04BR3 port chose "no persistent log" and paid
for it with a UART.

`check_for_panic_log()` is called from `PE_create_console()`, `pe_init.c:506-520` — the same function
that would enable the framebuffer console:

```c
void
PE_create_console(void)
{
	/*
	 * Check the head of VRAM for a panic log saved on last panic.
	 * Do this before the VRAM is trashed.
	 */
	check_for_panic_log();

	if (PE_state.video.v_display) {
		PE_initialize_console(&PE_state.video, kPEGraphicsMode);
	} else {
		PE_initialize_console(&PE_state.video, kPETextMode);
	}
}
```

## 1.6 Framebuffer console with a font blitter: present, compiled, and *off*

`config/MASTER:730-733` (the base config every A04BR3 build derives from):

```
# Console options
options		SERIAL_CONSOLE	# bi-directional serial over UART
options		VIDEO_CONSOLE	# uni-directional output over framebuffer
```

`VIDEO_CONSOLE` is therefore built, but `PE_state.video.v_display` is 0 unless `boot_args.Video` is
populated. The port only populates it **on request**, from the loader,
`xnu_bootshim_benesse_a04br3/handoff.c:13-37`:

```c
/* LK scanout geometry follows the working A04BR3 EDK2 DisplayInitDxe.
 * Publish it only for the explicit physical-display test and only when
 * FDT protects the complete four-buffer scanout buffer from managed RAM. */
int handoff_video_setup(struct xnu_video *video, const struct fdt_info *fdt,
                        const struct xnu_payload *p)
{
    *video = (struct xnu_video){0};
    const char flag[] = "a04br3.display_probe=1";
    int enabled = 0;
…
    if (!enabled) return 0;
    const uint64_t base = 0xbe9c0000, bytes = 800ULL * 1280 * 4 * 4;
    int reserved = 0;
    for (unsigned i = 0; i < fdt->reserved_count; i++) {
        if (inside(base, bytes, fdt->reserved[i].base, fdt->reserved[i].size)) reserved = 1;
    }
    if (!reserved) return 0;
    *video = (struct xnu_video){base, 1, 800 * 4, 800, 1280, 32};
    return 1;
}
```

and the "is it really on the panel?" paint, `handoff.c:312-327`:

```c
    if (handoff_video_setup(&ba.video, fdt, p)) {
        /* Small RGB bands on all four LK buffers identify this test without
         * reprogramming clocks, panel, DSI or overlay registers. */
        for (unsigned page = 0; page < 4; page++) {
            volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)
                (ba.video.base + page * 800UL * 1280 * 4);
            for (unsigned y = 0; y < 24; y++) {
                for (unsigned x = 0; x < 800; x++) {
                    pixels[y * 800 + x] = x < 267 ? 0xffff0000 :
                        x < 534 ? 0xff00ff00 : 0xff0000ff;
                }
            }
        }
        __asm__ __volatile__("dsb sy" ::: "memory");
        handoff_trace("XNU_LK_FRAMEBUFFER", ba.video.base);
    }
```

Note `pixels[y * 800 + x]` is a *pixel* stride of 800 against `rowbytes = 800*4 = 3200`, i.e. the
band painter silently assumes `rowbytes == width*4`. That is true here and is the assumption to
re-check on any other panel.

### The blitter implementation details (asked for explicitly)

Font table — `osfmk/console/iso_font.c:47` and `:308-309`:

```c
unsigned char iso_font[256 * 16] = {
…
#define ISO_CHAR_WIDTH  8
#define ISO_CHAR_HEIGHT 16
```

So: **8×16, 1 bit per pixel, 16 bytes per glyph, glyph = 16 consecutive bytes, bit 0 = leftmost
pixel**, table compiled in (`iso_font.c` is `#include`d by `video_console.c:106`, not linked
separately — one 4 KiB blob, no external font file, no RAM font).

Glyph expansion — `video_console.c:1378-1409`:

```c
static void
vc_render_char(unsigned char ch, unsigned char *renderptr, short newdepth)
{
	union {
		unsigned char  *charptr;
		unsigned short *shortptr;
		uint32_t  *longptr;
	} current;      /* current place in rendered font, multiple types. */
	unsigned char *theChar; /* current char in iso_font */
	int line;

	current.charptr = renderptr;
	theChar = iso_font + (ch * ISO_CHAR_HEIGHT);

	for (line = 0; line < ISO_CHAR_HEIGHT; line++) {
		unsigned char mask = 1;
		do {
			switch (newdepth) {
			case 8:
				*current.charptr++ = (*theChar & mask) ? 0xFF : 0;
				break;
			case 16:
				*current.shortptr++ = (*theChar & mask) ? 0xFFFF : 0;
				break;

			case 30:
			case 32:
				*current.longptr++ = (*theChar & mask) ? 0xFFFFFFFF : 0;
				break;
			}
			mask <<= 1;
		} while (mask); /* while the single bit drops to the right */
```

Each glyph is pre-expanded once into a per-depth render buffer and then blitted as opaque
`0x00000000 / 0xFFFFFFFF` runs (`vc_rendered_font`, `video_console.c:1284-1294`), with colour applied
at paint time (`vc_paint_char` / `vc_blit_rect_32`). No alpha, no gamma, no anti-aliasing.

Geometry and `stride` handling — `video_console.c:2777-2789`:

```c
static void
vc_initialize(__unused struct vc_info * vinfo_p)
{
	vinfo.v_rows = vinfo.v_height / ISO_CHAR_HEIGHT;
	vinfo.v_columns = vinfo.v_width / ISO_CHAR_WIDTH;
	vinfo.v_rowscanbytes = ((vinfo.v_depth + 7) / 8) * vinfo.v_width;
```

For this board: `v_rows = 1280/16 = 80`, `v_columns = 800/8 = 100`, `v_rowscanbytes = 4*800 = 3200`
and `v_rowbytes` (from `boot_args.Video.v_rowBytes`) `= 3200` — so **stride == visible bytes, no
padding**, which is precisely why the scroll code works unmodified.

Scrolling — `video_console.c:1734-1766` (down; up is symmetric):

```c
static void
vc_scroll_down(int num, unsigned int scrreg_top, unsigned int scrreg_bottom)
{
	uint32_t *from, *to, linelongs, i, line, rowline, rowscanline;

	if (!vinfo.v_depth) {
		return;
	}

	linelongs = vinfo.v_rowbytes * (ISO_CHAR_HEIGHT >> 2);
	rowline = vinfo.v_rowbytes >> 2;
	rowscanline = vinfo.v_rowscanbytes >> 2;

	to = (uint32_t *) vinfo.v_baseaddr + (linelongs * scrreg_bottom)
	    - (rowline - rowscanline);
	from = to - (linelongs * num);  /* handle multiple line scroll (Michel Pollet) */

	i = (scrreg_bottom - scrreg_top) - num;

	while (i-- > 0) {
		for (line = 0; line < ISO_CHAR_HEIGHT; line++) {
			/*
			 * Only copy what is displayed
			 */
			video_scroll_down(from,
			    (from - (vinfo.v_rowscanbytes >> 2)),
			    to);

			from -= rowline;
			to -= rowline;
		}
	}
}
```

Mechanism: **memmove of whole scanlines through a `video_scroll_*` callback, 16 rows at a time,
using the stride (`v_rowbytes`) for addressing and the visible width (`v_rowscanbytes`) for the byte
count** — this is what makes it survive a padded panel. `gc_scroll_up/down` (VT100 region aware,
`video_console.c:1012-1191`) drives it. There is no hardware pan, no dirty-rect, no double buffer:
each scroll is `(rows-1)*16` line copies of 3200 B ≈ 4 MiB per full scroll at 800×1280×32.

The 8×16 grid also means **80 rows × 100 columns** of usable text on this panel — a full panic dump
fits on ~30 lines of it.

**For the MT6762G port:** this whole path is SoC-agnostic. If LK already leaves a lit panel with a
known framebuffer, you need only (a) `boot_args.Video = {base, display=1, rowBytes, width, height,
depth}`, (b) the framebuffer excluded from managed RAM (the shim already does that check), and (c)
`PE_state.video` populated. Then a kernel with **no UART at all** still prints, because of
`PE_kputc = console_write_unbuffered` in §1.2.

## 1.7 USB: not a boot-time log

`xnu_device_benesse_a04br3/USB-CONSOLE.md:5-9`:

```
The fixed kernel source must include the MT8167 gadget implementation. UART remains available.
The private board interface is `0e8d:20ff`, product `A04BR3 XNU USB Console`.
This uses the existing board debug identity; it is not a production USB-ID allocation.
```

and `USB-CONSOLE.md:20-24`:

```
The shell provides `help`, `pwd`, `cd`, `ls`, `cat`, `ps`, and `exec /absolute/program`.
It is a source-built command shell; it does not require a donor `/bin/sh`.
…
`smoke` checks native commands and reads SystemVersion.plist.
```

This is a **userland** console (`/dev/a04boot` staging, `tools/usb_console.py` requires "Python 3,
PyUSB and libusb" and waits for `--ready-wait`). It needs PBS/`bsd_init`, a working `/dev`, and the
gadget driver. It cannot observe a kernel that dies at MMU-enable or in `PE_init_cpu`.

## 1.8 Summary of channels, and what a padless board is left with

| Channel | Exists in the port? | Works before `bsd_init`? | Survives watchdog reset? |
|---|---|---|---|
| Polled UART `0x11005000` | Yes, the only one | Yes (shim, asm, kernel) | No (you must capture live) |
| Framebuffer + 8×16 blitter | In tree, compiled, opt-in flag | Yes (from `PE_create_console`) | No (visible only while powered) |
| Persistent panic log in DRAM | **Disabled by `PLATFORM_PANIC_LOG_DISABLED`** | Yes, if enabled | **Yes** |
| eMMC store (`NativeMsdc`) | Yes (userland + kernel) | After MSDC probe | **Yes** |
| USB console | Yes, userland | No | n/a |

Practical read for MT6762G: enable `PLATFORM_PANIC_LOG_PADDR`, keep the framebuffer console as the
*display* for it, and use eMMC for logs that must outlive a battery pull. None of the three requires
a UART pad.

---

# 2. BOOT CHAIN — what the shim does before the jump

## 2.1 Entry, stacks, vectors, BSS

`xnu_bootshim_benesse_a04br3/start.S:1-52`:

```asm
/* SPDX-License-Identifier: MIT
 * AArch64 Image framing follows dipa-studio/ios-sweet (see CREDITS.md).
 * This entry supports the observed MT8167 EL2 handoff and EL1 diagnostics.
 */
.section .text.head, "ax"
.global _head
_head:
    b _start
    .long 0
    .quad 0
    .quad _end
    .quad 0xa
    .quad 0, 0, 0
    .ascii "ARM\x64"
    .long 0
.text
.global _start
_start:
    msr daifset, #0xf
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mov x22, x3
    mov x23, sp
    msr spsel, #1
    adr x1, _stack_top
    mov sp, x1
    adr x1, vectors
    mrs x2, CurrentEL
    cmp x2, #8
    b.eq 1f
    cmp x2, #4
    b.ne halt
    msr vbar_el1, x1
    b 2f
1:  msr vbar_el2, x1
2:  isb
    adr x1, _bss_start
    adr x2, _bss_end
3:  cmp x1, x2
    b.hs 4f
    str xzr, [x1], #8
    b 3b
4:  mov x0, x19
    mov x1, x20
    mov x2, x21
    mov x3, x22
    mov x4, x23
    bl shim_main
```

Notes: DAIF masked immediately; `spsel #1` so the C code runs on `SP_ELx` even if LK handed over on
`SP_EL0`; `VBAR_EL1` installed for the EL1 case and `VBAR_EL2` for the EL2 case (**both** paths
supported — this is the part that transfers directly to an EL1-only port); BSS cleared by hand;
`_stack_top` is 16 KiB above `_bss_end` (`shim.ld:13` `. += 0x4000;`).

Exception reporting is trivial and reusable — `shim.c:55-71` prints `vector`, `CurrentEL`, `ESR/ELR/FAR`
for whichever EL faulted, then `wfe`.

## 2.2 The EL2→EL1 drop

`start.S:54-86`:

```asm
/* Enter the EL1h state required by XNU start_first_cpu; x0 retains the FDT.
 * Called only with EL2 MMU and caches off, as checked by shim_main.
 */
.global enter_el1
enter_el1:
    mov x1, #(1 << 31)
    msr hcr_el2, x1
    mov x1, #3
    msr cnthctl_el2, x1
    msr cntvoff_el2, xzr
    mov x1, #0x33ff
    msr cptr_el2, x1
    msr hstr_el2, xzr
    msr vttbr_el2, xzr
    mov x1, #0x0800
    movk x1, #0x30d0, lsl #16
    msr sctlr_el1, x1
    adr x1, vectors
    msr vbar_el1, x1
    adr x1, _stack_top
    msr sp_el1, x1
    adr x1, el1_start
    msr elr_el2, x1
    mov x1, #0x3c5
    msr spsr_el2, x1
    dsb sy
    isb
    eret
```

Register-by-register, because this is the exact contract XNU's `_start` expects:

| Write | Value | Meaning |
|---|---|---|
| `HCR_EL2` | `1<<31` | `RW=1` → EL1 is AArch64. Everything else zero: no traps, **no `IMO`/`FMO`/`AMO`** (so no virtual IRQ/FIQ is injected into EL1 — see §3). |
| `CNTHCTL_EL2` | `3` | `EL1PCTEN|EL1PCEN` → EL1 may read the physical counter and program the physical timer. |
| `CNTVOFF_EL2` | `0` | virtual counter == physical counter. |
| `CPTR_EL2` | `0x33ff` | FP/SIMD (TFP), CPACR (TCPAC) and the AMU/PMU/TRBE/ZCR/GT traps disabled → EL1 owns FP, needed before `-mgeneral-regs-only` code hands over. |
| `HSTR_EL2` | `0` | no CP15 trapping. |
| `VTTBR_EL2` | `0` | no stage-2 translation (HCR.VM is 0 anyway). |
| `SCTLR_EL1` | `0x30d00800` | `M=0, C=0, I=0` — **MMU off, D-cache off, I-cache off**; bits 11,20,22,23,28,29 = RES1. |
| `SPSR_EL2` | `0x3c5` | `D,A,I,F` masked, `M=0b0101` = EL1h (uses `SP_EL1`). |
| `ELR_EL2` | `el1_start` | continues in `shim_el1`. |

Result for the new port: **the shim arrives at XNU exactly the way the Redmi 9A already arrives**
(EL1h, MMU/caches off, DAIF masked) — this path is the one to reuse verbatim, and the EL2 path can be
deleted if LK is known to enter at EL1.

## 2.3 The pre-conditions it refuses to violate

`shim.c:95-104`:

```c
    if (el == 8) {
        /* Do not discard live mappings or dirty cache data inherited from LK. */
        if (READ_SYSREG(SCTLR_EL2) & ((1u << 0) | (1u << 2) | (1u << 12))) {
            puts_uart("EL1_TRANSITION_REFUSED_MMU_OR_CACHE_ON\n");
            stop();
        }
        value("HCR_EL2_before", READ_SYSREG(HCR_EL2));
        value("CNTHCTL_EL2_before", READ_SYSREG(CNTHCTL_EL2));
        puts_uart("ENTER_EL1\n");
        enter_el1(x0);
    }
```

and again, immediately before the actual jump, in `handoff.c:265-269`:

```c
    uint64_t el, sctlr, timebase;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(timebase));
    if (el != 4 || (sctlr & 0x1005) || timebase > 0xffffffff) return "XNU_HANDOFF_CPU_STATE_REJECTED";
```

`0x1005` = `M|C|I` (`bit0|bit2|bit12`). So the invariant is enforced twice: EL1 only, MMU/D-cache/
I-cache **off**, and `CNTFRQ_EL0 ≤ 0xffffffff` (XNU stores it in a 32-bit ADT property). The refusal
strings (`EL1_TRANSITION_REFUSED_MMU_OR_CACHE_ON`, `XNU_HANDOFF_CPU_STATE_REJECTED`) are how a
UART-less operator would learn *why* nothing happened — keep them, and route them to the
framebuffer/log instead.

## 2.4 GIC setup: **there is none in the shim**

The question was specific — `GICD_CTLR`, `GICD_IGROUPR`, `GICR_*`, before or after the MMU. Answer:

* **The shim touches no GIC register of any kind.** A full-text search of the bootshim repository for
  `GIC|gicd|GICD|GICR|gicr|icc_|ICENABLER|IPRIORITYR|IGROUPR|GICv2|GICv3` returns **zero matches**
  (only the *descriptive* GIC node it writes into the ADT, §2.8).
* All GIC programming happens in the **kernel, long after the MMU is on**:
  `osfmk/arm/arm_init.c:634 PE_init_cpu()` → `pexpert/arm/pe_identify_machine.c:443-451`
  `PE_init_cpu() { … pe_init_fiq(); }` → `pexpert/arm/pe_fiq.c:173-180`
  `pe_init_fiq() { #if defined(A04BR3) extern void a04br3_gic_init(void); a04br3_gic_init(); #endif }`
  → `pexpert/arm/pe_a04br3_gic.c:28`, which obtains its MMIO through `ml_io_map()` and therefore
  runs with the MMU on, under TTBR1, at `PE_init_cpu` (line 634 of `arm_init.c`), i.e. **after
  `arm_vm_init` (543) and after `PE_create_console` (598)**.
* The shim does not even hand over a GIC base in a register — it publishes the GIC as an ADT node and
  the kernel re-reads it from the device tree (`pe_a04br3_gic.c:34-41`).

So on this port the answer to "did they hit the group-register wall before or after the MMU" is:
*they never wrote a group register at all, and the MMIO they did write was mapped by the kernel after
the MMU was up.* See §3 for what they wrote and why.

## 2.5 Timer: none programmed; only published

The shim reads the counter and frequency for diagnostics and passes the frequency to XNU through the
device tree. `shim.c:90-91` and `shim.c:122-124`:

```c
    value("CNTFRQ_EL0", READ_SYSREG(CNTFRQ_EL0));
    value("CNTVCT_EL0", READ_SYSREG(CNTVCT_EL0));
…
    value("SCTLR_EL1", READ_SYSREG(SCTLR_EL1));
    value("CNTFRQ_EL0", READ_SYSREG(CNTFRQ_EL0));
    value("CNTVCT_EL0", READ_SYSREG(CNTVCT_EL0));
```

`adt.c:142-149` publishes it per CPU:

```c
    for (unsigned i = 0; i < cpus; i++) {
        char name[] = "cpu0"; name[3] += i;
        node(&w, 8, 0);
        text(&w, "name", name); text(&w, "device_type", "cpu");
        prop(&w, "compatible", compatible_cpu, sizeof(compatible_cpu)); word(&w, "reg", i);
        text(&w, "state", i ? "stopped" : "running"); word(&w, "timebase-frequency", timebase);
        word(&w, "cpu-id", i); word(&w, "cluster-id", 0);
    }
```

**No `CNTP_*`/`CNTV_*` comparator is programmed, no `CNTFRQ` is written, no `CNTVOFF` setup beyond
zero.** The only timer-adjacent write in the entire shim is `CNTHCTL_EL2 = 3` in §2.2. XNU's
`ARM_ARCH_TIMER` support (`A04BR3.h:11`) then takes over.

## 2.6 Watchdog: untouched by the shim, killed late by the kernel

Shim, `shim.c:110-111`:

```c
    /* Leave the inherited watchdog untouched. A later reset is possible. */
    stop();
```

and `README.md:14`: *"It performs no storage writes or watchdog changes."*

The kernel disables it only after all CPUs are up, `iokit/Kernel/arm/A04BR3CPU.cpp:141-143`:

```cpp
    /* MSI/storage remain serialized by their existing controller locks. */
    a04br3_watchdog_quiesce();
    ml_cpu_init_completed();
```

`pexpert/arm/pe_a04br3_watchdog.c:12-23` — the whole function:

```c
void a04br3_watchdog_quiesce(void)
{
    vm_offset_t base = ml_io_map(0x10007000, 0x1000);
    if (!base) { panic("A04BR3 WDT: mapping failed"); }
    volatile uint32_t *mode = (volatile uint32_t *)base;
    uint32_t before = *mode;
    *mode = (before & ~1u) | 0x22000000u;
    __builtin_arm_dsb(DSB_SY);
    uint32_t after = *mode;
    kprintf("A04BR3 WDT: late quiesce mode %08x -> %08x\n", before, after);
    if (after & 1u) { panic("A04BR3 WDT: enable bit did not clear"); }
}
```

Reading the code literally: WDT base `0x10007000`, register 0 (`MODE`), clear bit 0 (enable) while
OR-ing in `0x22000000` — the value MTK's `WDT_MODE` requires as a "key" field for the write to be
accepted. (That `0x22000000` key is the same constant MediaTek's Linux `mtk_wdt.c` uses; I state it
as an inference from the value and the write-then-read-back pattern, not as something the repo
documents.) The check `if (after & 1u) panic(...)` — **read the register back and refuse to continue
silently** — is the reusable part, plus the note that *this is much too late*: everything between the
shim's `stop()` and CPU bring-up runs on LK's inherited watchdog timer.

For MT6762G this function is plausibly identical (same `0x10007000` top-level WDT block in the
MT6765 family), but it must be moved **into the shim** if you want the log to survive, which is the
opposite of what this port chose.

## 2.7 Cache maintenance

Exactly one sequence, immediately before the jump — `handoff.c:333-336`:

```c
    handoff_message("XNU_HANDOFF_JUMP\n");
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
    ((void (*)(uint64_t))(uintptr_t)plan.entry_pa)(plan.args_pa);
    return "XNU_ENTRY_RETURNED";
```

`ic iallu` + `dsb` + `isb`, **no `dc civac`, no `dc cvau`, no `dc ivac`** — deliberate, because the
D-cache must be off at this point (§2.3). The shim's own data (the copied segments, ADT, boot_args)
was written with the cache off, so it is already in DRAM.

The contrasting case (cache **on**, so the driver must clean) is the MSDC DMA path,
`bsd/dev/a04br3_msdc.c:91-98`:

```c
static void dma_cache_sync(vm_offset_t address,unsigned bytes)
{
    unsigned line=4u<<((__builtin_arm_rsr64("CTR_EL0")>>16)&15u);
    vm_offset_t end=address+bytes;
    for(vm_offset_t p=address&~((vm_offset_t)line-1);p<end;p+=line)
        __asm__ volatile("dc civac, %0"::"r"(p):"memory");
    __asm__ volatile("dsb sy":::"memory");
}
```

## 2.8 What it actually hands to XNU

`handoff.c:306-332` (the boot_args build):

```c
    struct xnu_boot_args ba;
    zero_bytes((uintptr_t)&ba, sizeof(ba));
    ba.revision = ba.version = 2;
    ba.virt_base = p->virt_base; ba.phys_base = p->phys_base;
    ba.mem_size = ba.mem_size_actual = p->mem_size;
    ba.top_of_kernel_data = plan.top;
    if (handoff_video_setup(&ba.video, fdt, p)) {
…
    ba.device_tree = p->virt_base + (plan.adt_pa - p->phys_base);
    ba.device_tree_length = (uint32_t)adt_size;
    for (unsigned i = 0; i < XNU_CMDLINE; i++) ba.command_line[i] = p->command_line[i];
    copy_bytes(plan.args_pa, &ba, sizeof(ba));
```

Before that, entropy and a versioned payload check — `handoff.c:270-281`:

```c
    unsigned char seed[64];
    handoff_message("XNU_TRNG_BEGIN\n");
    int rng_status = mt8167_random_seed(seed, (uint32_t)timebase);
    if (rng_status) {
        handoff_trace("XNU_TRNG_ERROR", (uint64_t)(int64_t)rng_status);
        return "XNU_BOOT_ENTROPY_UNAVAILABLE";
    }
```

The ADT (Apple Device Tree) is generated from scratch in `adt.c` — this is the piece that makes XNU
enumerate *any* hardware. `adt.c:150-166`:

```c
    node(&w, 6, 2 + display + (p->version == 6));
    text(&w, "name", "arm-io"); text(&w, "device_type", "soc");
    text(&w, "compatible", "mediatek,mt8167");
    uint64_t io_ranges[3] = {0, 0x10000000, 0x10000000};
    prop(&w, "ranges", io_ranges, sizeof(io_ranges));
    word(&w, "#address-cells", 2); word(&w, "#size-cells", 2);
    node(&w, 4, 0);
    text(&w, "name", "uart0"); text(&w, "device_type", "serial");
    text(&w, "compatible", "mediatek,mt6577-uart");
    uint64_t uart[2] = {0x01005000, 0x1000}; prop(&w, "reg", uart, sizeof(uart));
    node(&w, 5, 0);
    text(&w, "name", "interrupt-controller");
    text(&w, "compatible", "arm,gic-400");
    text(&w, "interrupt-controller", "master");
    uint64_t gic[4] = {0x00310000, 0x1000, 0x0032f000, 0x2000};
    prop(&w, "reg", gic, sizeof(gic));
    word(&w, "virtual-timer-intid", 27);
```

**All ADT `reg` values are SoC-relative to `0x10000000`** (`ranges = {0, 0x10000000, 0x10000000}`).
See the §3.5 landmine: MT6765's own DTS uses absolute addresses.

## 2.9 Build recipe (the "link at zero" trick)

`build.sh:8-10,17-22`:

```sh
flags=(-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align
       -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
       -mcpu=cortex-a35 -Wall -Wextra -Werror)
…
if "${prefix}readelf" -r "$out/shim.o" "$out/fdt.o" "$out/adt.o" "$out/handoff.o" "$out/rng.o" | grep -q 'R_AARCH64_ABS'; then
    echo 'Absolute C relocation is incompatible with a relocated shim' >&2; exit 1
fi
"${prefix}gcc" "${flags[@]}" -static -T "$src/shim.ld" -Wl,--build-id=none \
    -Wl,-N "$out/start.o" … -o "$out/shim.elf"
```

with `shim.ld:4` `. = 0;`. `-fno-pic` + link at 0 + an explicit `R_AARCH64_ABS` rejection is a clean
way to get position-independence for a payload whose load address LK decides: every `adrp/add` pair
becomes a PC-relative offset. `-mgeneral-regs-only` matters because the shim runs before it has
proven FP/SIMD is usable (the kernel only later sets `CPTR_EL2` so EL1 owns FP). `-mstrict-align`
matters because `SCTLR_EL1.A` is 0.

---

# 3. THE GIC QUESTION

## 3.1 Platform: GICv2 / GIC-400

Three independent confirmations in the sources:

1. The shim's ADT node: `arm,gic-400`, distributor `soc+0x310000` = `0x10310000`, CPU interface
   `soc+0x32f000` = `0x1032f000`, `virtual-timer-intid 27` (`adt.c:160-166`, quoted in §2.8).
2. `pe_a04br3_gic.c:1-5`:

```c
/* SPDX-License-Identifier: MIT
 * MT8167 non-secure GICv2 boot-CPU timer/SGI bring-up.
 * Register semantics: Arm GICv2 and existing A04BR3 EDK2 GICv2 implementation.
 * Secure Group 0 ownership is left to ATF. No GICv3 system registers are used.
 */
```

3. `A04BR3.h:26`: *"No PAN, LSE, PAC, GICv3, Apple SMP or Apple implementation registers."*

## 3.2 Every GIC register they write

`pexpert/arm/pe_a04br3_gic.c:28-51`:

```c
void a04br3_gic_init(void)
{
    if (!distributor) {
        DTEntry node;
        const void *property;
        unsigned int bytes;
        if (SecureDTLookupEntry(NULL, "/arm-io/interrupt-controller", &node) != kSuccess ||
            SecureDTGetProperty(node, "reg", &property, &bytes) != kSuccess || bytes != 32) {
            panic("A04BR3 GICv2: missing device-tree register ranges");
        }
        const uint64_t *reg = property;
        vm_offset_t soc = pe_arm_get_soc_base_phys();
        distributor = ml_io_map(soc + reg[0], reg[1]);
        cpu_interface = ml_io_map(soc + reg[2], reg[3]);
        if (!distributor || !cpu_interface) { panic("A04BR3 GICv2: mapping failed"); }
        /* Disable inherited non-secure SPIs until their device drivers exist.
         * Non-secure accesses cannot change Secure Group 0 enable bits. */
        unsigned groups = (read_reg(distributor, 4) & 31) + 1;
        for (unsigned group = 1; group < groups; group++) {
            write_reg(distributor, 0x180 + group * 4, UINT32_MAX);
        }
        kprintf("A04BR3 GICv2: mapped, timer INTID %u, self SGI %u\n",
            A04BR3_VTIMER_INTID, A04BR3_IPI_SGI);
    }
```

`pe_a04br3_gic.c:52-77` (continuation — the actual enable):

```c
    /* Read the banked GICv2 CPU target bit; do not assume logical IDs map
     * to distributor target bits. Publish before enabling secondary IPIs. */
    unsigned logical = cpu_number();
    uint32_t target = read_reg(distributor, 0x800) & 0xff;
    if (logical >= MAX_CPUS || !target || (target & (target - 1)))
        panic("A04BR3 GICv2: invalid CPU target mask");
    __atomic_store_n(&cpu_targets[logical], target, __ATOMIC_RELEASE);
    /* Banked SGI/PPI state for the boot CPU. Keep secure routing unchanged. */
    write_reg(distributor, 0x180, UINT32_MAX);
    write_reg(distributor, 0x280, 1u << A04BR3_VTIMER_INTID);
    unsigned offsets[2] = { A04BR3_VTIMER_INTID, A04BR3_IPI_SGI };
    for (unsigned i = 0; i < 2; i++) {
        unsigned offset = 0x400 + (offsets[i] & ~3u);
        unsigned shift = (offsets[i] & 3u) * 8;
        uint32_t priority = read_reg(distributor, offset);
        write_reg(distributor, offset, (priority & ~(0xffu << shift)) | (0xa0u << shift));
    }
    write_reg(distributor, 0x100, (1u << A04BR3_VTIMER_INTID) | (1u << A04BR3_IPI_SGI));
    write_reg(cpu_interface, 4, 0xff); /* GICC_PMR */
    write_reg(cpu_interface, 8, 0);    /* GICC_BPR */
    /* Non-secure CTLR bit 0 enables Group 1. Disable split EOI mode (bit 9). */
    write_reg(cpu_interface, 0, (read_reg(cpu_interface, 0) & ~(1u << 9)) | 1u);
    write_reg(distributor, 0, read_reg(distributor, 0) | 1u);
    __builtin_arm_dsb(DSB_SY);
    __builtin_arm_isb(ISB_SY);
}
```

Decoded (my architectural mapping of the raw offsets, which the code only comments for some):

| Offset | GICv2 register | What they do |
|---|---|---|
| `GICD + 0x004` | `GICD_TYPER` | read `ITLinesNumber` → number of 32-IRQ groups |
| `GICD + 0x180 + 4n` | `GICD_ICENABLERn` | **disable** all SPIs in groups 1..N |
| `GICD + 0x180` | `GICD_ICENABLER0` | disable all SGIs/PPIs (write `UINT32_MAX`) |
| `GICD + 0x280` | `GICD_ICPENDR0` | clear pending for INTID 27 |
| `GICD + 0x400` | `GICD_IPRIORITYR` | set INTID 27 and the IPI SGI to priority `0xa0` |
| `GICD + 0x100` | `GICD_ISENABLER0` | **enable** only INTID 27 and the IPI SGI |
| `GICD + 0x800` | `GICD_ITARGETSR0` | read the banked, read-only "which CPU am I" byte |
| `GICD + 0x000` | `GICD_CTLR` | `|= 1` (EnableGrp1) |
| `GICC + 0x004` | `GICC_PMR` | `0xff` |
| `GICC + 0x008` | `GICC_BPR` | `0` |
| `GICC + 0x000` | `GICC_CTLR` | clear bit 9 (`EOImodeNS`), set bit 0 (EnableGrp1) |
| `GICD + 0xF00` | `GICD_SGIR` | send SGI (see below) |

Runtime helpers, `pe_a04br3_gic.c:78-107`:

```c
uint32_t a04br3_gic_acknowledge(void)
{
    uint32_t iar = read_reg(cpu_interface, 0x0c);
    if ((iar & 0x3ff) == A04BR3_IPI_SGI) { __atomic_add_fetch(&ipi_count, 1, __ATOMIC_RELAXED); }
    if ((iar & 0x3ff) == A04BR3_VTIMER_INTID) { __atomic_add_fetch(&timer_count, 1, __ATOMIC_RELAXED); }
    return iar;
}
void a04br3_gic_complete(uint32_t iar)
{
    __builtin_arm_dsb(DSB_ISH);
    write_reg(cpu_interface, 0x10, iar); /* Preserve IAR source CPU for SGIs. */
    __builtin_arm_isb(ISB_SY);
}
…
void a04br3_gic_signal_cpu(unsigned logical)
{
…
    write_reg(distributor, 0xf00, (target << 16) | A04BR3_IPI_SGI);
…
}
void a04br3_gic_signal_self(void)
{
    __builtin_arm_dsb(DSB_ISHST);
    write_reg(distributor, 0xf00, (2u << 24) | A04BR3_IPI_SGI);
    __builtin_arm_isb(ISB_SY);
}
```

`(2u << 24)` is `GICD_SGIR.TargetListFilter = 0b10` = "this CPU only" — the correct way to self-IPI
without needing the target list.

## 3.3 The wall: they never write `GICD_IGROUPRn`

Three comments, verbatim, are the whole story:

* `GICD_IGROUPRn` (offset `0x080`) **appears nowhere in the tree.** Confirmed by grep for `IGROUPR`
  across the whole kernel repo: the only hits are Apple's GICv3 path in `pe_fiq.c` and the constants
  in `VMAPPLE.h`.
* `pe_a04br3_gic.c:3-4`:
  > `* Secure Group 0 ownership is left to ATF. No GICv3 system registers are used.`
* `pe_a04br3_gic.c:43-45`:
  > `/* Disable inherited non-secure SPIs until their device drivers exist.`
  > ` * Non-secure accesses cannot change Secure Group 0 enable bits. */`
* `pe_a04br3_gic.c:72-74`:
  > `/* Non-secure CTLR bit 0 enables Group 1. Disable split EOI mode (bit 9). */`

So yes — same class of wall, and the workaround is architectural rather than a register poke: with
the GIC in a two-world (security-enabled) configuration, **NS cannot re-assign an INTID to a group**;
`GICD_IGROUPRn` bits that belong to Group 0 are effectively read-only from NS, and Group-0 enable
bits in `GICD_ISENABLERn` cannot be changed from NS either. The only lever left to non-secure code is:

1. `GICC_CTLR.EnableGrp1` (bit 0) — enable Group 1 signalling to the CPU interface, and
2. `GICD_CTLR.EnableGrp1` (bit 0) — forward Group-1 interrupts from the distributor,
3. plus the routing decision that already happened in ATF/EL3 (`SCR_EL3.IRQ/FIQ`, `HCR_EL2.IMO/FMO`).

They take that lever and nothing else, and consequently they *disable* inherited SPIs rather than
re-group them (`write_reg(distributor, 0x180 + group * 4, UINT32_MAX)`) — a "quiet the machine until
drivers exist" policy that is itself worth copying.

Note also `HCR_EL2 = 1u<<31` only (§2.2): with `IMO=0, FMO=0` no virtual interrupt is injected; all
this Group-1 traffic arrives as a physical IRQ at EL1, which is what XNU's `pe_fiq.c`/`PE_init_cpu`
path expects on a non-virtualised board.

## 3.4 The GICv3 code is present but compiled out — and it *is* the wall

`pexpert/arm/pe_fiq.c:15` and `:144-170`:

```c
#if HAS_GIC_V3
…
	// Configure timers and legacy FIQ to be group 0
	gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);

	// Enable PPI 27
	gicr_write32(GICR_ISENABLER0, (1 << 27));

	// Enable system register access
	uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");
	icc_sre |= ICC_SRE_SRE;
	__builtin_arm_wsr64("ICC_SRE_EL1", icc_sre);
	__builtin_arm_isb(ISB_SY);

	// Set priority masks and binary point for group 0
	__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);

	// Set EOI mode of this processor
	uint64_t icc_ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
	icc_ctlr &= ~ICC_CTLR_EOIMODE;
	__builtin_arm_wsr64("ICC_CTLR_EL1", icc_ctlr);

	// Enable the forwarding of the vtimer interrupt
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
	gicd_ctlr |= GICD_CTLR_ENABLEGRP0;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
	__builtin_arm_isb(ISB_SY);
}
#else
void
pe_init_fiq()
{
#if defined(A04BR3)
	extern void a04br3_gic_init(void);
	a04br3_gic_init();
#endif
}
#endif
```

`HAS_GIC_V3` is defined **only** by `pexpert/pexpert/arm64/VMAPPLE.h:40`; A04BR3 does not define it,
so this whole Group-0/GICR block is dead code for the MediaTek build. That is fortunate, because it
is exactly the code that would fail on a security-enabled GICv3: `GICR_IGROUPR0`, `GICD_CTLR =
…ENABLEGRP0` and `ICC_IGRPEN0_EL1` are Group-0 knobs that non-secure software cannot use. If a port
copies Apple's file wholesale it will write three registers that silently do nothing (or fault), and
then wonder why INTID 27 never arrives.

## 3.5 MT6762G is GICv3 — the v2 code does not transfer, the *rules* do

Local evidence (not mouseos — `refs\mt6765-vendor`, `refs\lk-svoboda18`):

`refs\mt6765-vendor\arch\arm64\boot\dts\mediatek\mt6765.dts:356-365`:

```
gic: interrupt-controller@0c000000 {
    compatible = "arm,gic-v3";
    #interrupt-cells = <3>;
    #address-cells = <2>;
    #size-cells = <2>;
    #redistributor-regions = <1>;
    interrupt-parent = <&gic>;
    interrupt-controller;
    reg = <0 0x0c000000 0 0x40000>, // distributor
          <0 0x0c100000 0 0x200000>, // redistributor
          <0 0x10200a80 0 0x50>; // intpol
    interrupts = <GIC_PPI 9 IRQ_TYPE_LEVEL_HIGH>;
```

`refs\lk-svoboda18\platform\mt6765\rules.mk:526` → `MTK_GIC_VERSION := v3`, and
`refs\lk-svoboda18\dev\gic\rules.mk` selects `mt_gic_v3.o` on that value.
`refs\lk-svoboda18\dev\gic\mt_gic_v3.c:266` (LK's own bring-up):

```c
DRV_WriteReg32(GIC_DIST_BASE + GIC_DIST_CTRL, GICD_CTLR_ARE | GICD_CTLR_ENGRP1S | GICD_CTLR_ENGRP1NS);
```

with `mt_gic_icc_sre_write(0x01)` and `mt_gic_icc_igrpen1_write(0x01)` earlier in the same file
(`:170-172`).

What that means concretely for the port:

* **Do not** reuse the offsets in §3.2. There is no `GICC_*` frame; the CPU interface is
  `ICC_*_EL1` system registers, per-CPU state is in a redistributor (`GICR_*`) that must be woken
  (`GICR_WAKER.ProcessorSleep = 0`, then poll `ChildrenAsleep`), and SGIs are sent with
  `ICC_SGI1R_EL1`, not `GICD_SGIR`.
* **Do** reuse the strategy: `ICC_SRE_EL1.SRE = 1` first; `ICC_PMR_EL1 = 0xff`;
  `ICC_CTLR_EL1.EOImode = 0`; enable **Group 1 NS only**
  (`GICD_CTLR.EnableGrp1NS` + `ICC_IGRPEN1_EL1 = 1`); never touch `GICR_IGROUPR0`,
  `GICD_IGROUPRn`, `\*_IGRPEN0`, or `EnableGrp0` from NS.
* The **INTID is portable**: 27 is the EL1 virtual timer PPI on both platforms, so the ADT property
  `virtual-timer-intid = 27` (`adt.c:166`) carries over unchanged.
* **Landmine (address convention):** mouseos adds the SoC base to DT `reg` values —
  `pe_serial.c:827 ml_io_map(soc_base + *reg_prop, …)`, `pe_a04br3_gic.c:40 ml_io_map(soc + reg[0], reg[1])`,
  `pe_fiq.c:107 ml_io_map(soc_base_phys + gicd_base_prop, …)`. The MT6765 DTS `reg` values are
  **absolute** (`0x0c000000`, `0x11230000`). On MT6762G, `soc_base + reg` yields `0x1c000000`-class
  garbage. Whatever FDT/ADT the port consumes, decide the convention once and assert it.
* MSDC0 differs too: `refs\mt6765-vendor\...\mt6765.dts` has `msdc0:msdc@11230000` (size `0x10000`,
  `GIC_SPI 79`), where MT8167 uses `0x11120000` (`a04br3_msdc.c:727`). Re-derive from your DTS.

---

# 4. NativeMsdc and `a04br3_msdc.c`

Two artifacts, same register knowledge:

* `xnu_kernel_benesse_a04br3/bsd/dev/a04br3_msdc.c` (793 lines) — in-tree BSD block/char driver,
  `bdevsw`/`cdevsw`, plus a boot-update staging cdev. **This one provably read and wrote the eMMC of
  the device** (`NativeMsdc/README.md:7-8`: *"The register sequences it uses read and wrote the eMMC
  of the TAB-A04-BR3 in the earlier a04br3 XNU port."*).
* `NativeMsdc/NativeMsdc.cpp` (1500) + `NativeMsdc.h` (599) — IOKit `IOBlockStorageDevice`
  re-implementation by the same author, with the register/command logic factored into a header that
  has **no IOKit, no MMIO and no allocation** so it can be host-tested.
  `NativeMsdc.h:3-10`:

```c
// NativeMsdc: MT8167 MSDC register map, eMMC command encoding and the pure userdata layout logic.
//
// No IOKit, no MMIO and no allocation here, so the same code is used by the kernel driver and by host tests.
// The includer provides uint8_t/uint32_t/uint64_t, size_t, memcpy, memset, memcmp and strcmp.
//
// Register usage and command sequences come from the a04br3 XNU port (bsd/dev/a04br3_msdc.c and
// a04br3_msdc_dma.h of github.com/mouseos/xnu_kernel_benesse_a04br3), which read and wrote the eMMC of the
// TAB-A04-BR3 with them, and from the MSDC driver of the a04br3 EDK II port it was adapted from.
```

## 4.1 API surface

In-kernel (`bsd/dev/a04br3_msdc.c`) — every function is `static` except the exported hooks:

```c
static uint32_t rd(unsigned r);
static void     wr(unsigned r, uint32_t v);                       /* + dsb sy */
static uint64_t deadline(void);                                    /* now + 1 s in ticks */
static int      clear_wait(unsigned reg, uint32_t mask);
static int      command(unsigned opcode, uint32_t argument, int data, uint32_t *response);
static int      dma_read(uint32_t argument, uint8_t *buffer, unsigned blocks);
static int      read_blocks(unsigned opcode, uint32_t argument, uint8_t *buffer, unsigned blocks);
static int      read_block(unsigned opcode, uint32_t argument, uint8_t *buffer);
static int      write_blocks_raw(uint32_t block, const uint8_t *buffer, unsigned blocks);
static int      write_block_raw(uint32_t block, const uint8_t buffer[512]);
static int      flush_card(void);
static int      flush_card_force(void);
static int      read_disk_sector(uint32_t block, uint8_t sector[512]);
int  a04br3_emmc_rootdev(dev_t *dev);
void a04br3_msdc_probe(void);
```

Plus the device-switch entry points `disk_open/close/strategy/ioctl/read/psize`,
`boot_open/close/read/write/ioctl/commit`, and `nvram_read_bytes` /
`a04br3_wlan_read_calibration` as alternative consumers.

Standalone (`NativeMsdc.cpp`), the equivalents with a `host` prefix and an RAII owner:

```cpp
uint64_t deadlineAfter (uint64_t nanoseconds);
IOReturn waitClear (unsigned offset, uint32_t mask);
void     cacheCleanInvalidate (const void *start, size_t length);
IOReturn hostCommand (unsigned opcode, uint32_t argument, Data data, uint32_t *response);
IOReturn hostWaitTransfer (void);                      // CMD13 until state == 4
IOReturn hostPrepareData (uint32_t blocks);            // BLOCKS + FIFO clear
IOReturn hostReadPio (unsigned opcode, uint32_t argument, uint8_t *buffer, uint32_t blocks);
IOReturn hostReadDma (uint32_t argument, uint8_t *buffer, uint32_t blocks);
IOReturn hostWrite (uint32_t sector, const uint8_t *buffer, uint32_t blocks);
IOReturn hostFlush (void);
IOReturn transfer (uint64_t sector, uint32_t blocks, uint8_t *buffer, bool write);
class    HostSession { /* mask INTEN, force PIO, restore on scope exit */ };
bool     hostBringUp (IOService *provider, bool useDma, const char **why);
bool     hostFindLayout (const char **why);            // GPT + userdata + manifest
```

and the `extern "C"` durable-store ABI (`NativeMsdc.cpp:1414-1480`):

```cpp
extern "C" unsigned native_msdc_store_mode (void);
extern "C" int      native_msdc_store_open (uint64_t blocks, uint64_t *firstSector, const char **why);
extern "C" int      native_msdc_store_read (uint64_t relative, uint32_t blocks, void *destination);
extern "C" int      native_msdc_store_write (uint64_t relative, uint32_t blocks, const void *source);
extern "C" int      native_msdc_store_sync (void);
extern "C" unsigned native_msdc_store_gpt_check (void);
```

Pure helpers in `NativeMsdc.h` (host-testable, zero dependencies): `commandWord()`, `busyMask()`,
`cardInTransfer()`, `gptCrc32()`, `gptCheckHeader()`, `gptEntryUsed()`, `gptEntryNamed()`,
`gptFindUserdata()`, `parseManifest()`, `buildRootPlan()`, `storeWindow()`, `dmaChecksum()`,
`dmaDescriptors()`, `decodeBusMode()`.

## 4.2 Command encoding — the reusable bit-packer

`NativeMsdc.h:88-111`:

```c
// SDC_CMD: opcode [5:0], response type [9:7] (1 = R1, 7 = R1b), STOP bit 14, data type [12:11]
// (1 single, 2 multiple), write bit 13, block length [27:16].
constexpr uint32_t commandWord (unsigned opcode, Data data)
{
  uint32_t word = opcode | ((opcode == kCmdSwitch || opcode == kCmdStop ? 7u : 1u) << 7);
  if (opcode == kCmdStop) word |= 1u << 14;
  if (data != Data::None) {
    bool multi = data == Data::ReadMulti || data == Data::WriteMulti;
    word |= (multi ? 2u : 1u) << 11;
    word |= uint32_t (kSector) << 16;
    if (data == Data::WriteSingle || data == Data::WriteMulti) word |= 1u << 13;
  }
  return word;
}
static_assert (commandWord (kCmdReadMulti, Data::ReadMulti) == (18u | (1u << 7) | (2u << 11) | (512u << 16)), "CMD18");
static_assert (commandWord (kCmdWriteSingle, Data::WriteSingle) ==
               (24u | (1u << 7) | (1u << 11) | (1u << 13) | (512u << 16)), "CMD24");
static_assert (commandWord (kCmdStop, Data::None) == (12u | (7u << 7) | (1u << 14)), "CMD12");

// CMD12 and CMD13 may run while the data line is busy; every other command waits for the whole SDC.
constexpr uint32_t busyMask (unsigned opcode)
{
  return opcode == kCmdStop || opcode == kCmdStatus ? kStsCmdBusy : kStsSdcBusy;
}

constexpr bool cardInTransfer (uint32_t status)
{
  return (status & (1u << 8)) && ((status >> 9) & 15u) == 4u;
}
```

The `static_assert`s are the safety net: if you retarget and the encoding is wrong, it fails at
compile time, not on the phone. The in-kernel original packs the same fields inline
(`a04br3_msdc.c:69-71`) — uglier, same result:

```c
    wr(CMD,opcode|((opcode==6 || opcode==12 ? 7u : 1u)<<7)|
        (opcode==12 ? (1u<<14) : 0)|
        (data ? ((data>=3 ? 2u : 1u)<<11)|(512u<<16)|(data==2 || data==4 ? (1u<<13) : 0) : 0));
```

## 4.3 Issuing a command, and the polling loop shape

`a04br3_msdc.c:56-85`:

```c
static uint64_t deadline(void) {
    uint64_t ticks; nanoseconds_to_absolutetime(1000000000ULL, &ticks);
    return mach_absolute_time()+ticks;
}
static int clear_wait(unsigned reg, uint32_t mask) {
    uint64_t end=deadline();
    do { if (!(rd(reg)&mask)) return 0; } while (mach_absolute_time()<end);
    return ETIMEDOUT;
}
static int command(unsigned opcode, uint32_t argument, int data, uint32_t *response) {
    int e=clear_wait(STS, opcode==13 || opcode==12 ? 2 : 1);
    if (e) return e;
    wr(INTR,rd(INTR)); wr(ARG,argument);
    wr(CMD,opcode|…);
    uint64_t end=deadline();
    do {
        uint32_t status=rd(INTR)&(CMD_DONE|CMD_ERR);
        if (status) {
            wr(INTR,status);
            if (status&CMD_ERR) return EIO;
            *response=rd(RESP);
            /* R1 error bits, not the current-state/ready indicators. */
            if (*response&0xfdffa080u) return EIO;
            return 0;
        }
    } while (mach_absolute_time()<end);
    return ETIMEDOUT;
}
```

Three polling idioms are used throughout, and they are the whole concurrency model:

1. **`clear_wait(reg, mask)`** — bounded wait for a register bit to clear (busy bits).
2. **`do { … } while (mach_absolute_time() < end)`** — 1 s per command/data phase
   (`kCommandTimeoutNs = 1000000000ULL`, `NativeMsdc.cpp:103`).
3. **CMD13 until state 4** — 5 s for post-programming (`kBusyTimeoutNs = 5000000000ULL`),
   `NativeMsdc.cpp:202-213`:

```cpp
// SEND_STATUS until the card is back in the transfer state (after programming, or an R1b command).
IOReturn hostWaitTransfer (void)
{
  uint64_t end = deadlineAfter (kBusyTimeoutNs);
  do {
    uint32_t response = 0;
    IOReturn result = hostCommand (kCmdStatus, kStatusArgument, Data::None, &response);
    if (result != kIOReturnSuccess) return result;
    if (cardInTransfer (response)) return kIOReturnSuccess;
  } while (mach_absolute_time () < end);
  return kIOReturnTimeout;
}
```

No interrupts are ever used: the driver masks `MSDC_INTEN` and polls `MSDC_INT` itself.

## 4.4 PIO vs DMA

**PIO is the default and the only writes path.** DMA is opt-in, read-only, and only after a byte-for-byte
self-test against PIO. `a04br3_msdc.c:766-782`:

```c
    unsigned use_dma=0;
    if(!error && PE_parse_boot_argn("a04br3.msdc_dma",&use_dma,sizeof(use_dma)) && use_dma==1){
        kern_return_t kr=kmem_alloc_contig(kernel_map,&dma_area,PAGE_SIZE+65536,
            PAGE_MASK,(ppnum_t)(0xffffffffULL>>PAGE_SHIFT),0,KMA_ZERO,VM_KERN_MEMORY_BSD);
        …
        uint8_t reference[2048],result[2048];
        de=read_blocks(17,0x40,reference,4);
        if(!de)de=dma_read(0x40,result,4);
        if(!de && memcmp(reference,result,sizeof(reference)))de=EIO;
        if(!de)dma_enabled=1;
```

PIO read — FIFO drain loop, 32-bit words then bytes, `dsb` only per burst,
`a04br3_msdc.c:155-178`:

```c
    uint64_t end=deadline(); unsigned offset=0, total=blocks*512; int done=0;
    do {
        uint32_t ints=rd(INTR)&(DATA_DONE|DATA_ERR);
        if (ints) {
            wr(INTR,ints);
            if (ints&DATA_ERR) { e=EIO; goto recover; }
            if (ints&DATA_DONE) done=1;
        }
        unsigned available=rd(FIFO)&255u;
        while (available && offset<total) {
            if (available>=4 && total-offset>=4) {
                uint32_t word=rd(RX);
                memcpy(buffer+offset,&word,4);offset+=4;available-=4;
            } else {
                buffer[offset++]=*(volatile uint8_t *)(msdc+RX);available--;
            }
        }
        if (done && offset==total) return 0;
    } while (mach_absolute_time()<end);
```

PIO write — fill the FIFO to its 128-byte depth, then one `dsb`, `a04br3_msdc.c:198-222`:

```c
    uint64_t end=deadline();unsigned offset=0,total=blocks*512;int done=0;
    do {
        uint32_t ints=rd(INTR)&(DATA_DONE|DATA_ERR);
        if (ints) { … }
        unsigned used=(rd(FIFO)>>16)&255u;
        unsigned available=used<=128 ? 128-used : 0;
        while (available>=4 && offset<total) {
            uint32_t word;memcpy(&word,buffer+offset,4);
            *(volatile uint32_t *)(msdc+TX)=word;
            offset+=4;available-=4;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        if (done && offset==total) {
            do {
                error=command(13,1u<<16,0,&response);
                if (error) return error;
                if ((response&(1u<<8)) && ((response>>9)&15u)==4) return 0;
            } while (mach_absolute_time()<end);
            error=ETIMEDOUT;goto recover;
        }
    } while (mach_absolute_time()<end);
```

`kMaxBlocks = 128` → **64 KiB per command**, matching `DKIOCGETMAXBLOCKCOUNTREAD`
(`a04br3_msdc.c:137-138`).

DMA is one GPD pair + one BD, all below 4 GiB, checksummed (`NativeMsdc.h:323-357`, quoted in the
next section) — and it **quarantines the transport** if the engine will not stop
(`NativeMsdc.cpp:322-330`):

```cpp
  wr (kDmaCtrl, rd (kDmaCtrl) | kDmaCtrlStop);
  if (waitClear (kDmaCfg, kDmaCfgActive) != kIOReturnSuccess) {
    gHost.poisoned = true;
    gHost.dmaEnabled = false;
    native_msdc_dma = 3;
    native_msdc_failure = "DMA engine did not stop";
    IOLog ("NativeMsdc: DMA engine did not stop; transport quarantined\n");
    return kIOReturnNotReady;
  }
```

with the in-kernel equivalent going further and panicking
(`a04br3_msdc.c:126-128`: *"Never recycle a buffer while hardware may still write to it."*).

## 4.5 Card initialisation: **there is none — LK did it**

This is the single most important structural fact of the driver, and it is stated twice:

`a04br3_msdc.c:730-731`:

```c
    /* No reset: retain LK's card clock, voltage, bus width and sampling. */
    wr(INTEN,0);wr(CFG,cfg|(1u<<3));
```

`NativeMsdc/README.md:33-39`:

> The boot firmware leaves the card initialized in the transfer state (RCA 1). The driver keeps the
> inherited clock, bus width, voltage and sampling, and never resets the controller. For each request
> it masks the controller interrupts and selects PIO, then restores both. Commands are polled.
>
> * Start: SEND_STATUS must report the transfer state; EXT_CSD gives the capacity (SEC_COUNT), the cache state
>   (CACHE_CTRL) and must show the user area selected (PARTITION_ACCESS 0).

So the CMD sequence at bring-up is only **13 → 8 → 17**, `NativeMsdc.cpp:622-641`:

```cpp
  HostSession session;
  uint32_t response = 0;
  if (hostCommand (kCmdStatus, kStatusArgument, Data::None, &response) != kIOReturnSuccess || !cardInTransfer (response)) {
    *why = "card not in the transfer state";
    return false;
  }
  native_msdc_step = 2;
  uint8_t *extCsd = gHost.bounce;
  if (hostReadPio (kCmdSendExtCsd, 0, extCsd, 1) != kIOReturnSuccess) {
    *why = "EXT_CSD read failed";
    return false;
  }
  gHost.diskSectors = le32 (extCsd + 212);
  gHost.cache = extCsd[33] & 1;
  IOLog ("NativeMsdc: EXT_CSD rev %u, %u sectors, cache %u, partition access %u\n", extCsd[192], gHost.diskSectors,
         gHost.cache, extCsd[179] & 7);
  if (!gHost.diskSectors || (extCsd[179] & 7)) {
    *why = "EXT_CSD: no capacity or a boot/RPMB partition selected";
    return false;
  }
```

and the in-kernel probe, `a04br3_msdc.c:724-753`:

```c
void a04br3_msdc_probe(void) {
    unsigned enabled=0;
    if (!PE_parse_boot_argn("a04br3.msdc_probe",&enabled,sizeof(enabled)) || !enabled) return;
    msdc=ml_io_map(0x11120000,0x1000);
    uint32_t cfg=rd(CFG),inten=rd(INTEN),response=0;
    printf("A04BR3 MSDC0: inherited CFG=0x%08x STS=0x%08x\n",cfg,rd(STS));
    /* No reset: retain LK's card clock, voltage, bus width and sampling. */
    wr(INTEN,0);wr(CFG,cfg|(1u<<3));
    int error=command(13,1u<<16,0,&response);
    if (!error && ((response>>9)&15u)!=4u) error=ENXIO;
    uint8_t sector[512];uint32_t sectors=0;
    if (!error) error=read_block(8,0,sector);
    if (!error) {
        sectors=le32(sector+212);
        cache_enabled=sector[33]&1u;
        printf("A04BR3 MSDC0: EXT_CSD rev=%u sectors=%u partition_access=%u\n",
            sector[192],sectors,sector[179]&7u);
        if (!sectors || (sector[179]&7u)) error=ENXIO;
    }
    uint32_t locations[2]={1,0x1c01};int found=0;
    for (unsigned i=0;!error && i<2;i++) {
        error=read_block(17,locations[i],sector);
        if (error) break;
        if (memcmp(sector,"EFI PART",8)) continue;
        unsigned size=le32(sector+12);
        if (size<92 || size>512 || le64(sector+24)!=locations[i] ||
            le32(sector+16)!=header_crc(sector,size)) { error=EINVAL;break; }
        …
```

**Explicitly absent:** CMD0 (GO_IDLE), CMD1 (SEND_OP_COND), CMD2 (ALL_SEND_CID), CMD3 (SET_RELATIVE_ADDR),
CMD7 (SELECT_CARD), CMD9 (SEND_CSD), CMD16 (SET_BLOCKLEN), any clock/divider write, any
`MSDC_CFG` clock-mode change, any pad/tuning change, any controller reset. `RCA` is assumed to be 1
(`NativeMsdc.h:74-75`: `constexpr uint32_t kRca = 1; // the relative card address LK assigns to the eMMC`).
**The port inherits a live card from LK, full stop.** If LK on MT6762G leaves the card in a different
state (or you want the driver to run after LK re-inits), this is what breaks first.

## 4.6 What can be lifted for a durable log — exactly

The persistent-store path is already the "durable log" primitive, minus the log:

`NativeMsdc.cpp:1378-1411`:

```cpp
// ---- Persistent store window (NativeRamBlock's overlay, see PersistOverlay.h) --------------------------------------
// 4096-byte blocks, relative to the window. Mode 1 only reads; mode 2 also writes. Writes return when the blocks are
// on the medium (written, then the card cache flushed), so the overlay needs no separate flush.
namespace {
uint64_t gStoreFirstSector, gStoreBlocks;
…
bool storeTransfer (uint64_t relative, uint32_t blocks, uint8_t *data, bool write)
{
  if (!gStoreBlocks || !data || !blocks || relative >= gStoreBlocks || blocks > gStoreBlocks - relative) return false;
  constexpr uint32_t perTransfer = uint32_t (kMaxBlocks / kSectorsPerStoreBlock);
  while (blocks) {
    uint32_t chunk = blocks > perTransfer ? perTransfer : blocks;
    HostSession session;
    if (transfer (gStoreFirstSector + relative * kSectorsPerStoreBlock, uint32_t (chunk * kSectorsPerStoreBlock), data,
                  write) != kIOReturnSuccess)
      return false;
    data += chunk * kStoreBlock;
    relative += chunk;
    blocks -= chunk;
  }
  return true;
}
```

and the write path's durability contract, `NativeMsdc.cpp:1454-1468`:

```cpp
extern "C" int native_msdc_store_write (uint64_t relative, uint32_t blocks, const void *source)
{
  if (storeMode () != 2) return 0;
  // transfer() only reads from the buffer on a write.
  if (!storeTransfer (relative, blocks, static_cast<uint8_t *> (const_cast<void *> (source)), true)) return 0;
  HostSession session;
  return hostFlush () == kIOReturnSuccess;
}

extern "C" int native_msdc_store_sync (void)
{
  if (storeMode () != 2 || !gStoreBlocks || gHost.poisoned) return 0;
  HostSession session;
  return hostFlush () == kIOReturnSuccess;
}
```

`hostFlush` is CMD6 `FLUSH_CACHE` + CMD13 poll (`NativeMsdc.cpp:396-404`), mirroring
`a04br3_msdc.c:269-281`:

```c
static int flush_card_force(void) {
    uint32_t response;
    if (!cache_enabled) return 0;
    int error=command(6,(3u<<24)|(32u<<16)|(1u<<8),0,&response);
    if (error) return error;
    uint64_t end=deadline();
    do {
        error=command(13,1u<<16,0,&response);
        if (error) return error;
        if ((response&(1u<<8)) && ((response>>9)&15u)==4) return 0;
    } while (mach_absolute_time()<end);
    return ETIMEDOUT;
}
```

**Lift-list for a logger (ranked by how little you must understand):**

1. `command()` / `hostCommand()` + `commandWord()` + `busyMask()` + `clear_wait()` / `waitClear()`
   — ~60 lines, complete, no dependencies.
2. `setup` bits: `wr(INTEN,0); wr(CFG, cfg|PIO)` around the transfer, restore after (the
   `HostSession` RAII class, `NativeMsdc.cpp:406-427`) — protects you against the inherited
   controller state and is 20 lines.
3. `write_blocks_raw()` / `hostWrite()` — CMD23+CMD25 for ≤128 blocks, CMD24 for one, FIFO depth
   wait, `dsb`, then CMD13-until-state-4. This is your log append.
4. `flush_card_force()` / `hostFlush()` — CMD6 `FLUSH_CACHE`
   (`(3u<<24)|(32u<<16)|(1u<<8)` = write byte, EXT_CSD[32], value 1) + CMD13. Without this a log
   line is not durable across a power cut.
5. `read_blocks()` / `hostReadPio()` — for reading the log back (e.g. from a recovery boot).
6. The GPT bit **only if you need to place the log in a partition**: `header_crc()` /
   `gptCheckHeader()` / `gptFindUserdata()` — but for a log ring you can simply hard-code a
   sector range that the GPT does not use, and skip the partition logic entirely.
7. `storeWindow()`/`native_msdc_store_*` — the pre-shaped "reserve N MiB at the end of userdata,
   serve 4096-byte blocks, write returns after flush" API. Adopt the *shape*, replace the manifest
   logic.
8. `dmaDescriptors()` + `dmaChecksum()` (`NativeMsdc.h:323-357`) if you later want DMA reads:

```c
struct DmaGpd { uint32_t info, next, pointer, length, argument, blocks, command; };
struct DmaBd { uint32_t info, next, pointer, length; };
static_assert (sizeof (DmaGpd) == 28, "GPD layout");
static_assert (sizeof (DmaBd) == 16, "BD layout");

static inline uint8_t dmaChecksum (const void *descriptor)
{
  const uint8_t *bytes = static_cast<const uint8_t *> (descriptor);
  unsigned sum = 0;
  for (unsigned i = 0; i < 16; ++i) sum += bytes[i];
  return uint8_t (255u - sum);
}
…
  bd->info = 1;                                  // last BD
  bd->pointer = uint32_t (bufferPhys);
  bd->length = bytes;
  bd->info |= uint32_t (dmaChecksum (bd)) << 8;
  gpd[0].info = 3;                               // hardware owned, BD present
  gpd[0].next = uint32_t (gpdPhys + sizeof (DmaGpd));
  gpd[0].pointer = uint32_t (bdPhys);
  gpd[0].info |= uint32_t (dmaChecksum (gpd)) << 8;
```

**Do not lift:** the SHNGROOT manifest parser, the `A04RPOL1` VerifiedRoot policy region
(`NativeMsdc.h:359-390`), the `boot_*` staging cdev with SHA-256 commit/verify
(`a04br3_msdc.c:362-389`), the 30-second APFS window publication wait, `NATIVE_MSDC_UFS_STORE_ABI`
aliasing. All of that is about *root filesystem authenticity*, which a logger does not need and
which will cost you a day to unwind.

**Also do not lift the base address.** `msdc=ml_io_map(0x11120000,0x1000)` is MT8167; MT6765's MSDC0
is `0x11230000` (§3.5), and its window is `0x10000` per the vendor DTS.

---

# 5. WHAT THEY ACTUALLY ACHIEVE

## 5.1 The authors' own status line, in all three repos

Identical banner, first line of each README
(`xnu_bootshim_benesse_a04br3/README.md:1`, `xnu_kernel_benesse_a04br3/README.md:1`,
`xnu_device_benesse_a04br3/README.md:1`):

> **Work in progress / 作業中:** Experimental XNU port for Benesse TAB-A04-BR3 (MT8167). XNU entry has
> been observed, but the device currently watchdog-reboots; a working OS is not available.

Device repo, `README.md:5-9`:

> The current stage targets entry into a source-built XNU kernel. Initial kernel entry has been
> observed on hardware; the previous image then faulted after MMU enable. This revision corrects the
> 39-bit virtual layout and still needs hardware verification.

Shim repo, `README.md:16-19` and `:36-37`:

> The EL2 and EL1/FDT diagnostic stages were verified on TAB-A04-BR3. The new kernel handoff has host
> validation only and awaits hardware verification.
> …
> A successful host build is not a successful device boot.
> The inherited watchdog may reset the device after the diagnostic stops.

## 5.2 What *is* documented as reaching hardware

`xnu_device_benesse_a04br3/README.md:186-192` — the one dated milestone:

> Hardware milestone (2026-10-20): kernel `89fea0f`, bootshim `556aa15`, device `41dc5bc` mounted the
> generated HFS+ RAMDisk on TAB-A04-BR3, executed the diagnostic PID 1 and printed the regular file
> `/etc/a04br3-stage`. Periodic console output continued without an observed panic. This verifies the
> diagnostic root path, not Apple launchd or an iOS or macOS environment. The next image optionally
> probes inherited MSDC0 using `--msdc-probe`; eMMC operation is not yet hardware verified.

So the achieved state is: **LK → shim (EL2→EL1, FDT, ADT, boot_args, jump) → XNU entry → `arm_vm_init`
→ `PE_create_console` → `PE_init_cpu` (GIC) → `bsd_init` → BSD PID 1 → read a file from an HFS+
RAMDisk.** That is a *system*, but not a usable OS: no Apple launchd, no BSD init scripts, no GUI.

The scope of "diagnostic PID 1" is spelled out, `README.md:168-174`:

> Minimal userspace bring-up: add `--bringup-init` … `rd=md0` selects Apple's built-in memory device
> and mockfs exposes it as `/sbin/launchd`. This PID 1 prints a banner and periodic heartbeat through
> `/dev/console`; it does not provide an iOS or macOS environment.

## 5.3 The further stages are scaffolding, honestly labelled

The device README (629 lines) and kernel README (490 lines) describe launchd, keybag, dyld, WMT/WLAN/
GPU firmware, native MSDC root, touch, HID — but almost every paragraph ends in a disclaimer. Two
representative ones:

`README.md:257-265`:

> For a bounded Apple launchd PID 1 bring-up, pass `--pid1 apple` … This minimal image does not
> contain the complete iOS boot-task executables or daemon configuration and is not a full iOS root.

`xnu_kernel_benesse_a04br3/README.md:486-490`:

> Setup and SpringBoard aborts now include longer bounded stacks and first-abort heap exception text.
> The opt-in platform-root MAC stand-in grants only the work-interval scheduling privilege to platform
> binaries; …

That last sentence implies SpringBoard/Setup *started and aborted* at some point — but nothing claims
a usable GUI, and the banner at line 1 still says "a working OS is not available".

## 5.4 The storage piece specifically

`NativeMsdc/README.md:7-10`:

> Current state: compiles against the xnu-10063.141.1 headers; not yet booted. The register sequences
> it uses read and wrote the eMMC of the TAB-A04-BR3 in the earlier a04br3 XNU port.

So: the *sequences* are hardware-proven (in `a04br3_msdc.c`), the *new driver* is not booted. For a
port, `a04br3_msdc.c` is the trustworthy reference and `NativeMsdc.cpp` is the clean re-expression.

## 5.5 Is there a "waiting for root device" stage?

Yes, structurally: XNU's `bsd_init`/`vfs_mountroot` needs either `rd=` (RAMDisk, `md0`) or a block
device. The device repo shows both worked around:

* `rd=md0` + mockfs (`README.md:171-174`), and
* `rd=emmcroot0` + `a04br3_emmc_rootdev()` (`a04br3_msdc.c:685-689`):

```c
int a04br3_emmc_rootdev(dev_t *dev) {
    if (!root_sectors || !root_device) return ENXIO;
    *dev=root_device;return 0;
}
```

and `README.md:194-198`:

> Persistent diagnostic root: build the same generated HFS image, write it to `userdata`, and package
> the source kernel with `--emmc-root` instead of a RAMDisk option. This selects `rd=emmcroot0` and
> the inherited MSDC0 probe.

They never quote a literal "waiting for root device" log line. The watchdog loop is the shim's
`stop()` (`shim.c:50-53`, `for (;;) __asm__ volatile("wfe")`) after any refusal, plus the kernel's
`PE_init_cpu`-time panics.

---

# 6. TOP 5 THINGS TO COPY (ranked for a port with **no UART**)

### 1. The persistent log slot — re-enable what they disabled
**From:** `xnu_kernel_benesse_a04br3/pexpert/arm/pe_init.c:93-180` (`check_for_panic_log`),
`pexpert/pexpert/arm64/BCM2837.h:63-64` (working `#define` example), and the deletion of
`PLATFORM_PANIC_LOG_DISABLED` from `pexpert/pexpert/arm64/A04BR3.h:28`.
**Why:** it is the only mechanism in this entire codebase that puts bytes somewhere a **later boot**
can read them, and it is already wired into XNU's panic path and `PE_create_console()`. Two
`#define`s (`PLATFORM_PANIC_LOG_PADDR`, `PLATFORM_PANIC_LOG_SIZE`) plus an FDT/ADT reserved region and
you have a cross-watchdog log with zero new code. The A04BR3 port disabled it and thereby made
itself UART-dependent. Copy the *enablement*, not the disablement. Your reader can be LK (it already
has a font blitter) or the next kernel.

### 2. `boot_args.Video` → Apple's 8×16 framebuffer console
**From:** `xnu_bootshim_benesse_a04br3/handoff.c:16-37` (`handoff_video_setup`) and `:312-327` (the
probe paint); consumer side `pexpert/arm/pe_init.c:506-520`, `osfmk/console/video_console.c`,
`osfmk/console/iso_font.c`.
**Why:** it is the only *live* console available without a UART, it is already compiled into this
kernel config (`config/MASTER:733`), and it needs nothing from the SoC beyond a lit panel and its
geometry. The port's `PE_kputc` fallback (`pe_kprintf.c:63-70`) hands it to you automatically when
`serial_init()` returns 0 — which is precisely your situation. Concretely reusable numbers:
8×16 glyphs from a built-in 4 KiB table, 1 bpp MSB-first, rows = `height/16`, columns = `width/8`,
`v_rowscanbytes = ((depth+7)/8)*width` versus `v_rowbytes` from the boot args, and scroll = memmove
of scanlines using the stride. Verify one thing before trusting it: `rowbytes == width*4` on your
panel (it is on this one, which is why the hard-coded `pixels[y*800+x]` works).

### 3. The MSDC PIO block path + flush, i.e. a durable log on eMMC
**From:** `xnu_kernel_benesse_a04br3/bsd/dev/a04br3_msdc.c` — `command()` (65), `read_blocks()` (139),
`write_blocks_raw()` (185), `flush_card_force()` (269); clean re-expression in
`NativeMsdc/NativeMsdc.cpp` — `hostCommand` (182), `hostReadPio` (224), `hostWrite` (346),
`hostFlush` (396), `HostSession` (406); encoding helpers in `NativeMsdc/NativeMsdc.h:88-116`.
**Why:** a log that survives a battery pull. ~200 lines, PIO only (no DMA, no clock programming, no
card init, no GPT, no root policy), proven to have written the eMMC on real hardware. The `static_assert`s
on `commandWord()` catch retargeting mistakes at compile time. It depends on **LK having already
initialised the card** (RCA 1, transfer state) — which is true of your LK bootloader too, but verify
it and keep the `command(13)` / `command(8)` sanity gates that refuse to proceed when the state is
wrong.

### 4. The GIC strategy — but port it to GICv3
**From:** `xnu_kernel_benesse_a04br3/pexpert/arm/pe_a04br3_gic.c` (whole file, 108 lines) and the
dead GICv3 branch in `pexpert/arm/pe_fiq.c:144-170`.
**Why:** not for the offsets (MT6762G is GICv3, §3.5), but for the four rules it encodes: never write
`*_IGROUPR*`/`EnableGrp0` from non-secure; enable only Group 1 (`GICC_CTLR` for v2 →
`GICD_CTLR.EnableGrp1NS` + `ICC_IGRPEN1_EL1` for v3); set `ICC_SRE_EL1.SRE` before any `ICC_*`; and
*drain/disable* inherited SPIs rather than re-group them, with a log line saying you did. It also
shows the exact order (PMR/BPR → CTLR → `dsb`/`isb`) and that GIC init belongs **after** the MMU and
after the console — put it at `PE_init_cpu`, not in the shim. And it documents the wall you will
otherwise spend a day on: with ATF owning Group 0, your virtual-timer INTID 27 arrival depends on
what EL3/HCR handed you, not on anything you can write.

### 5. The shim skeleton itself: EL1 entry contract, refusal gates, link-at-zero build, watchdog
**From:** `xnu_bootshim_benesse_a04br3/start.S:18-86` (entry, vectors, `enter_el1`, and especially the
plain-EL1 path), `shim.c:95-112` and `handoff.c:265-269` (the `CurrentEL`/`SCTLR`/`CNTFRQ` gates),
`build.sh:8-22` + `shim.ld` (link at 0, `-fno-pic`, `-mgeneral-regs-only`, `R_AARCH64_ABS` rejection),
`fdt.c`/`fdt.h` (a 129-line, allocation-free FDT reader), `adt.c` (an ADT writer), and — separately —
`pexpert/arm/pe_a04br3_watchdog.c:12-23`.
**Why:** your shim already runs at EL1 with the MMU off, which is *exactly* the state this code
validates and hands over from, so the `enter_el1` half is dead weight and everything else transfers
directly. The refusal gates are worth more than they look: each one turns "black screen forever" into
a named marker string, and *those markers are what you will route to the framebuffer/log instead of
the UART*. The watchdog function is 12 lines of `ml_io_map(0x10007000)` + `MODE &= ~1 | 0x22000000` +
read-back-and-panic — but move it **into the shim** if you want a reset-free log window, because at
`A04BR3CPU::startAll()` it is far too late.

---

# 7. Traps and caveats for the MT6762G port

1. **`soc_base + reg` is not universal.** mouseos adds the SoC base to every DT `reg`
   (`pe_serial.c:827`, `pe_a04br3_gic.c:40-41`, `pe_fiq.c:107`, `adt.c` publishes SoC-relative
   values). MT6765's DTS uses absolute addresses (`0x0c000000`, `0x11230000`). Pick one convention
   and assert it with a `panic` the way `pe_serial.c` asserts `soc_base == 0x10000000`.
2. **16 KiB page granule is load-bearing** for the UART-alias trick (`ARM_TT_L2_SIZE` = 32 MiB).
   Also `A04BR3.h` forces `#error` for `ARM_LARGE_MEMORY` and for a link address outside the 39-bit
   TTBR1 range. If you keep 16 KiB pages, those invariants still hold; if you go 4 KiB, recompute the
   alias arithmetic.
3. **`bootopt=64S3,32N2,64N2` at Android-header offset `0x40`** — this is a hard LK/preloader
   requirement, documented from failure, `xnu_device_benesse_a04br3/README.md:74-79`:
   > `bootopt=64S3,32N2,64N2` starts at Android header offset `0x40`. MT8167 preloader parses these
   > fixed bytes before LK loads the image. Removing or moving this prefix selects secure 32-bit
   > defaults and can crash LK at its ATF-log SMC, before any shim code executes. The builder rejects
   > that layout.
   Redmi 9A is also MTK preloader + LK; expect the same rule (with an MT6765-specific value).
4. **The shim's own log is lost unless you leave something behind.** Everything they print is gone on
   reset. If the goal is "no UART", decide *up front* where the shim's output goes (framebuffer
   painted by the shim itself, or a reserved DRAM page that LK/the kernel drains) — adding it later
   means re-testing every refusal path.
5. **`v_rowscanbytes` vs `v_rowbytes`** — the vt blitter handles padding correctly, but the port's
   own `handoff_video_setup` paints with a hard-coded pixel stride. If your panel has a padded
   stride, the RGB probe bands will be skewed diagonally and you will misdiagnose it as a wrong base
   address.
6. **`debug=` must contain `DB_KPRT`** or `kprintf` is silently dead even with a working console
   (`pe_kprintf.c:41-43`). On a padless board this is one more reason the only output you see is
   `console_write_unbuffered`.
7. **MSDC0 base and window size differ** (`0x11120000`/4 KiB vs `0x11230000`/64 KiB). Also
   `kMaxBlocks = 128` (64 KiB per command) is a *driver* policy matching `DKIOCGETMAXBLOCKCOUNTREAD`,
   not a hardware limit.

---

## Appendix: files worth opening first

| Purpose | File | Lines |
|---|---|---|
| Shim entry, EL2→EL1, vectors | `xnu_bootshim_benesse_a04br3/start.S` | 110 |
| Shim diagnostics + UART | `xnu_bootshim_benesse_a04br3/shim.c` | 150 |
| Payload validate / copy / boot_args / jump | `xnu_bootshim_benesse_a04br3/handoff.c` | 338 |
| ADT (Apple device tree) writer | `xnu_bootshim_benesse_a04br3/adt.c` | 205 |
| Minimal FDT reader | `xnu_bootshim_benesse_a04br3/fdt.c`+`.h` | 129+17 |
| Build recipe (link at 0) | `xnu_bootshim_benesse_a04br3/build.sh`, `shim.ld` | 35, 18 |
| GICv2 bring-up (the wall) | `xnu_kernel_benesse_a04br3/pexpert/arm/pe_a04br3_gic.c` | 108 |
| GICv3 path (compiled out) | `xnu_kernel_benesse_a04br3/pexpert/arm/pe_fiq.c` | 181 |
| UART driver + kprintf plumbing | `pe_serial.c` 684-724/822-830, `pe_kprintf.c` | 1242, 181 |
| UART alias across MMU switch | `osfmk/arm64/arm_vm_init.c` 1815-1898, 2228-2250 | 2498 |
| Persistent panic log (disabled) | `pexpert/arm/pe_init.c` 93-180, `pexpert/pexpert/arm64/A04BR3.h` | 865, 30 |
| Framebuffer console + 8×16 blitter | `osfmk/console/video_console.c`, `iso_font.c` | 3298, ~310 |
| eMMC PIO driver (hardware-proven) | `bsd/dev/a04br3_msdc.c` | 793 |
| eMMC register/command header (clean) | `NativeMsdc/NativeMsdc.h` | 599 |
| eMMC driver, IOKit shell | `NativeMsdc/NativeMsdc.cpp` | 1500 |
| Watchdog quiesce | `pexpert/arm/pe_a04br3_watchdog.c` | 23 |
| CPU bring-up / IPI / late watchdog | `iokit/Kernel/arm/A04BR3CPU.cpp` | 212 |
