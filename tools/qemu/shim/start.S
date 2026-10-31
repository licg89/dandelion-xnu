/*
 * Dandelion XNU boot shim for QEMU's aarch64 "virt" machine.
 *
 * QEMU loads this ELF with -kernel, places its PT_LOAD segments at their
 * physical addresses and jumps to _start with the MMU, caches and interrupts
 * off, at EL1 (or EL2 when the machine was given virtualization=on).
 *
 * XNU's start.s expects to be entered the way a device boot loader enters it:
 *
 *   x0 = physical address of struct boot_args (see pexpert/arm64/boot.h)
 *   MMU off, caches off, DAIF masked, SPLX/SP = SPx
 *
 * so all this file has to do is make the CPU state look like that and call
 * shim_main(), which does the real work in C.
 *
 * Why the register values below are the ones they are: they are the ones the
 * phone's firmware log reports at the same point of the hand-off
 * (CurrentEL 1, SCTLR_EL1 0x30D0180D, TCR_EL1 0x280803518) -- except that the
 * shim leaves SCTLR_EL1 *clear*, because the kernel enters with translation
 * off and switches it on itself in start.s.
 */

	.section ".text.boot", "ax"
	.globl _start
_start:
	/* No exceptions of any kind until the kernel installs its vectors. */
	msr	daifset, #0xf
	isb

	/* Which exception level did QEMU start us at? */
	mrs	x1, currentel
	lsr	x1, x1, #2
	cmp	x1, #2
	b.ne	1f

	/*
	 * EL2.  Drop to EL1h with DAIF masked, and hand the generic timer to
	 * EL1: without CNTHCTL_EL2.EL1PCTEN the kernel's CNTP accesses trap to
	 * EL2 and the first tick setup dies with an "unknown exception".
	 */
	adr	x1, el1_entry
	msr	elr_el2, x1
	mov	x1, #0x3c5			/* DAIF masked, EL1h, AArch64 */
	msr	spsr_el2, x1
	msr	hcr_el2, xzr			/* no traps, no stage-2 */
	mov	x1, #3				/* EL1PCTEN | EL1PCEN */
	msr	cnthctl_el2, x1
	msr	cntvoff_el2, xzr
	isb
	eret

1:
	/* Already EL1: fall through with a real branch, eret is not needed. */
	b	el1_entry

	.globl el1_entry
el1_entry:
	/* Translation, caches and alignment checking off: identity by default. */
	msr	sctlr_el1, xzr
	isb

	/* FP/SIMD usable at EL1 and EL0: the kernel turns NEON on later. */
	mov	x1, #(3 << 20)			/* CPACR_EL1.FPEN = 0b11 */
	msr	cpacr_el1, x1
	isb

	/*
	 * TCR_EL1 as the firmware log reports it.  XNU overwrites this from its
	 * own sysreg_restore table in start.s, so this is documentation of the
	 * expected hand-off state, not a functional requirement.
	 */
	movz	x1, #0x3518
	movk	x1, #0x8080, lsl #16
	movk	x1, #0x2, lsl #32
	msr	tcr_el1, x1

	/* MAIR attr0 = normal write-back, attr1 = device-nGnRnE (kernel rewrites). */
	movz	x1, #0x00ff
	msr	mair_el1, x1
	isb

	/* Stack: the shim's own, in .bss, below the kernel window. */
	adrp	x1, __stack_top
	add	x1, x1, :lo12:__stack_top
	mov	sp, x1

	bl	shim_main

	/* shim_main does not return; if it ever does, park where gdb can see it. */
2:
	wfi
	b	2b
