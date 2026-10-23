/*
 * Exception vectors for the Dandelion trap test payload.
 *
 * Installed with VBAR_EL1 while running EL1h with the MMU off.  The table is
 * the architectural 16-entry layout (0x800 bytes, 2 KiB aligned):
 *
 *   0x000  Current EL with SP_EL0, synchronous
 *   0x200  Current EL with SP_ELx, synchronous   <- we run here
 *   0x280  Current EL with SP_ELx, IRQ           <- timer PPI
 *   0x400  Lower EL, AArch64
 *
 * Only the two entries we actually exercise do real work; everything else
 * reports and halts so a stray exception during bring-up is visible on the
 * framebuffer instead of resetting silently.
 */

	.section .text.vectors, "ax"
	.balign 2048
	.globl	payload_vectors
payload_vectors:
	/* Current EL with SP_EL0 */
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected

	/* Current EL with SP_ELx */
	.balign	128
	b	payload_sync_entry
	.balign	128
	b	payload_irq_entry
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected

	/* Lower EL using AArch64 */
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected

	/* Lower EL using AArch32 */
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected
	.balign	128
	b	payload_unexpected

/*
 * Frame: 176 bytes, 16-byte aligned, x0-x18 + x29/x30.  Callee-saved
 * registers x19-x28 are preserved by the C handlers.
 */
	.macro	SAVE_FRAME
	sub	sp, sp, #176
	stp	x0, x1, [sp, #0]
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	str	x18, [sp, #144]
	stp	x29, x30, [sp, #152]
	.endm

	.macro	RESTORE_FRAME
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldr	x18, [sp, #144]
	ldp	x29, x30, [sp, #152]
	add	sp, sp, #176
	.endm

payload_sync_entry:
	SAVE_FRAME
	mrs	x0, esr_el1
	mrs	x1, elr_el1
	mrs	x2, far_el1
	mrs	x3, spsr_el1
	bl	payload_handle_sync
	/* x0 = address to resume at, or 0 to keep ELR_EL1. */
	cbz	x0, 1f
	msr	elr_el1, x0
1:
	RESTORE_FRAME
	eret

payload_irq_entry:
	SAVE_FRAME
	bl	payload_handle_irq
	RESTORE_FRAME
	eret

payload_unexpected:
	mrs	x0, esr_el1
	mrs	x1, elr_el1
	mrs	x2, far_el1
	mrs	x3, spsr_el1
	bl	payload_handle_unexpected
2:	wfe
	b	2b

/* void payload_install_vectors(void) */
	.globl	payload_install_vectors
payload_install_vectors:
	adrp	x0, payload_vectors
	add	x0, x0, :lo12:payload_vectors
	msr	vbar_el1, x0
	isb
	ret
