#!/usr/bin/env python3
"""Add the emmc-log node to the dandelion ADT, and re-lay-out flashinfo around it.

Why: the kernel's console on this board is eMMC, not a UART.
patches/xnu/native-emmc-serial.patch looks the channel up by name --

    SecureDTFindEntry("name", "emmc-log", &entryP)
    SecureDTGetProperty(entryP, "log-lba", ...)
    SecureDTGetProperty(entryP, "log-sectors", ...)
    SecureDTGetProperty(entryP, "clock-div", ...)

-- so the ADT has to describe it, and nothing else does.  The LBA is absolute on
purpose: the kernel is told where to write instead of being made to parse the GPT
(docs/UNIXNU-EMMC-CONSOLE.md 3.3).

Layout inside flashinfo (32768 sectors), which this script and
tools/mk_unixnu_esp.sh and PayloadBootDxe.c now all agree on:

    0      .. 30000   FAT volume: \\UniXNU.efi, \\EFI\\UniXNU\\*, kernel.macho
    30000  .. 30512   XNU's log channel, 512 sectors = 256 KiB
    30512  .. 30640   the firmware's own log block, 128 sectors

clock-div is 26 because that is what DandelionEmmcDxe calibrates to on this board
(not the 260 bootshim uses); the kernel inherits the controller as firmware left it.

    python3 tools/patch_adt_emmc_log.py
Idempotent.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADT = os.path.join(REPO, "bootshim", "adt.c")
ESP = os.path.join(REPO, "tools", "mk_unixnu_esp.sh")
BOOT = os.path.join(REPO, "Platforms", "Xiaomi", "dandelionPkg", "Drivers",
                    "PayloadBootDxe", "PayloadBootDxe.c")

DEFINES = '''/*
 * Where XNU's own console writes.
 *
 * patches/xnu/native-emmc-serial.patch looks for an ADT node named "emmc-log"
 * and reads log-lba / log-sectors / clock-div from it.  The LBA is absolute on
 * purpose: the kernel is told where to write instead of being made to parse the
 * GPT, which halves the code and the risk (docs/UNIXNU-EMMC-CONSOLE.md 3.3).
 *
 * flashinfo starts at LBA 122109919 on this device (measured, docs/EMMC-LOG.md).
 * Layout inside it, agreed with tools/mk_unixnu_esp.sh and PayloadBootDxe:
 *
 *     0      .. 30000   FAT volume: \\UniXNU.efi, \\EFI\\UniXNU\\*, kernel.macho
 *     30000  .. 30512   XNU log channel, 512 sectors = 256 KiB   <- here
 *     30512  .. 30640   the firmware's own log block, 128 sectors
 *
 * clock-div is 26, which is what DandelionEmmcDxe calibrates to on this board,
 * not the 260 bootshim uses: the kernel inherits the controller as firmware
 * left it.
 */
#define ADT_EMMC_LOG_LBA      (122109919u + 30000u)
#define ADT_EMMC_LOG_SECTORS  512u
#define ADT_EMMC_LOG_CLKDIV   26u

'''

NODE = '''
	/*
	 * /arm-io/emmc-log -- the console channel on a board with no UART.  Four
	 * properties: name plus the three the kernel reads.
	 */
	w_node(&w, 4, 0);
	w_text(&w, "name", "emmc-log");
	uint32_t emmc_log_lba = ADT_EMMC_LOG_LBA;
	uint32_t emmc_log_sectors = ADT_EMMC_LOG_SECTORS;
	uint32_t emmc_log_div = ADT_EMMC_LOG_CLKDIV;
	w_word(&w, "log-lba", emmc_log_lba);
	w_word(&w, "log-sectors", emmc_log_sectors);
	w_word(&w, "clock-div", emmc_log_div);
'''

UART_TAIL = ('\tuint64_t uart_reg[2] = { ADT_UART_BASE - ADT_GIC_BASE, 0x1000 };\n'
             '\tw_prop(&w, "reg", uart_reg, sizeof(uart_reg));\n')


def patch_adt():
    with open(ADT, "r", encoding="utf-8") as fh:
        text = fh.read()
    changed = 0

    if "ADT_EMMC_LOG_LBA" not in text:
        anchor = "size_t dandelion_make_adt(void *buffer, size_t capacity,"
        if anchor not in text:
            sys.exit("adt.c: function anchor missing")
        text = text.replace(anchor, DEFINES + anchor, 1)
        changed += 1

    if "/arm-io node: 6 properties, 3 children" in text:
        text = text.replace("/arm-io node: 6 properties, 3 children",
                            "/arm-io node: 6 properties, 4 children", 1)
        text = text.replace("\tw_node(&w, 6, 3);", "\tw_node(&w, 6, 4);", 1)
        changed += 1

    if '"name", "emmc-log"' not in text:
        if UART_TAIL not in text:
            sys.exit("adt.c: uart0 tail anchor missing")
        text = text.replace(UART_TAIL, UART_TAIL + NODE, 1)
        changed += 1

    if changed:
        with open(ADT, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
    return changed


def patch_file(path, old, new, what):
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    if new in text:
        return 0
    if old not in text:
        sys.exit("%s: anchor missing (%s)" % (path, what))
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write(text.replace(old, new, 1))
    return 1


def main():
    changed = patch_adt()
    changed += patch_file(ESP, "SIZE_KIB=${ESP_SIZE_KIB:-16256}",
                          "SIZE_KIB=${ESP_SIZE_KIB:-15000}", "esp size")
    changed += patch_file(BOOT, "#define LOG_PARTITION_OFFSET   32512",
                          "#define LOG_PARTITION_OFFSET   30512", "log offset")
    print("patched %d site(s)" % changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
