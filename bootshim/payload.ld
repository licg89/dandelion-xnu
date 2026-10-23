/* Dandelion platform test payload
 *
 * Must be linked at the address the shim copies it to; the payload is not
 * relocated at runtime, so absolute addresses in .data are valid.
 */
ENTRY(_start)
SECTIONS
{
	. = 0x60000000;
	.text : {
		*(.text.entry)
		*(.text*)
	}
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.got : { *(.got*) }
	. = ALIGN(16);
	__bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
	}
	. = ALIGN(16);
	__bss_end = .;

	/*
	 * The shim only copies the file image and rounds the copy size up to a
	 * page.  Pad the file to a page boundary so the zeroed .bss region is
	 * always covered by that rounded-up copy.
	 */
	. = ALIGN(0x4000);
	.tail : { LONG(0) }
	_end = .;
}
