/* Dandelion platform test payload - entry stub
 *
 * The shim copies the payload to 0x60000000 and jumps here with:
 *   x0 = physical address of struct xnu_boot_args
 *   EL1h, MMU off, DAIF all masked
 *
 * Only the file image (text/rodata/data) is copied, so .bss has to be
 * cleared here before entering C.
 */
	.section .text.entry, "ax"
	.globl _start
_start:
	msr	daifset, #0xf

	adrp	x1, __bss_start
	add	x1, x1, :lo12:__bss_start
	adrp	x2, __bss_end
	add	x2, x2, :lo12:__bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b

2:	bl	platform_test_main
3:	wfe
	b	3b
