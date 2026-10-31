/*
 * Link the shim at 0x40080000, the address QEMU's virt machine uses for a raw
 * AArch64 Image, so nothing it needs collides with:
 *
 *   0x40080000  this shim (a few tens of KiB)
 *   0x48000000  mach_kernel staged raw by -device loader  (15 MiB)
 *   0x4D004000  the kernel itself, copied here by shim_main (the window
 *               DarwinBoot picks on the phone: 256 MiB from 0x4D004000)
 *   0x5FC00000  XNU's in-memory trace ring   (printf.c, DANDELION)
 *   0x7EC50000  the framebuffer text console (printf.c, DANDELION)
 */
ENTRY(_start)
SECTIONS
{
	. = 0x40080000;
	.text : { *(.text.boot) *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : {
		__bss_start = .;
		*(.bss*) *(COMMON)
		. = ALIGN(16);
		. = . + 0x10000;		/* 64 KiB of stack */
		__stack_top = .;
		__bss_end = .;
	}
	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
