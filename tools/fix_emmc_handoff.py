#!/usr/bin/env python3
"""Fix the hand-off publish: reuse FlushLogToFlash's GPT scan, and call it after it.

The previous attempt added PublishEmmcHandoff() with its own FindFlashInfo(), and
the log line never appeared: that second GPT scan reports no flashinfo partition,
while FlushLogToFlash's inline scan in the same driver succeeds every boot (it
writes the firmware log to LBA 122142431).  Two implementations of the same
lookup, one of them wrong -- the fix is to keep only the working one and hand its
result out.

Changes, all idempotent:

  1. a static EFI_LBA mFlashInfoStart, set by FlushLogToFlash where it finds the
     partition;
  2. PublishEmmcHandoff derives the kernel's log LBA from it instead of scanning;
  3. the call moves to just after the first flush, which is the earliest point
     where mFlashInfoStart is known.  Its DEBUG line is then captured by the
     ExitBootServices flush -- that callback exists precisely to catch what is
     printed after the last heartbeat.

    python3 tools/fix_emmc_handoff.py
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOOT = os.path.join(REPO, "Platforms", "Xiaomi", "dandelionPkg", "Drivers",
                    "PayloadBootDxe", "PayloadBootDxe.c")


def sub(text, old, new, what):
    if new in text:
        return text, 0
    if old not in text:
        sys.exit("anchor missing (%s)" % what)
    return text.replace(old, new, 1), 1


def main():
    with open(BOOT, "r", encoding="utf-8") as fh:
        text = fh.read()
    changed = 0

    # 1. the static, and FlushLogToFlash recording what it found
    text, n = sub(text,
                  "#define EMMC_HANDOFF_PA       0x5FD00000ULL",
                  "//\n"
                  "// Where FlushLogToFlash found 'flashinfo', in whole-disk sectors.\n"
                  "// PublishEmmcHandoff uses it instead of scanning the GPT a second time:\n"
                  "// that second scan reported no partition while this one works every boot.\n"
                  "//\n"
                  "STATIC EFI_LBA  mFlashInfoStart;\n"
                  "#define EMMC_HANDOFF_PA       0x5FD00000ULL", "static")
    changed += n

    text, n = sub(text,
                  "    Target = (EFI_LBA)(FlashInfoStart + LOG_PARTITION_OFFSET);",
                  "    mFlashInfoStart = (EFI_LBA)FlashInfoStart;\n"
                  "    Target = (EFI_LBA)(FlashInfoStart + LOG_PARTITION_OFFSET);",
                  "record start")
    changed += n

    # 2. derive from the static instead of scanning
    text, n = sub(text,
                  "  if (!FindFlashInfo (&BlockIo, &Start, &Last)) {\n    return;\n  }",
                  "  if (mFlashInfoStart == 0) {\n    return;\n  }",
                  "no scan")
    changed += n

    text, n = sub(text,
                  "  Handoff->Lba       = (UINT32)(Start + EMMC_LOG_LBA_OFFSET);",
                  "  Handoff->Lba       = (UINT32)(mFlashInfoStart + EMMC_LOG_LBA_OFFSET);",
                  "lba from static")
    changed += n

    text, n = sub(text,
                  "    (UINT64)(Start + EMMC_LOG_LBA_OFFSET),",
                  "    (UINT64)(mFlashInfoStart + EMMC_LOG_LBA_OFFSET),",
                  "print lba")
    changed += n

    # unused locals in PublishEmmcHandoff after the change
    text, n = sub(text,
                  "  EFI_BLOCK_IO_PROTOCOL  *BlockIo;\n  EFI_LBA                Start;\n  EFI_LBA                Last;\n  EMMC_HANDOFF           *Handoff;",
                  "  EMMC_HANDOFF           *Handoff;",
                  "drop locals")
    changed += n

    # 3. move the call to after the first flush
    text, n = sub(text,
                  "  PublishEmmcHandoff ();\n\n  if (!EFI_ERROR (FlushLogToFlash ())) {",
                  "  if (!EFI_ERROR (FlushLogToFlash ())) {",
                  "remove early call")
    changed += n

    text, n = sub(text,
                  '    Say (L"PayloadBoot: firmware log written to flash\\r\\n");\n  }',
                  '    Say (L"PayloadBoot: firmware log written to flash\\r\\n");\n  }\n\n'
                  '  //\n'
                  '  // After the first flush, because it is what learns where flashinfo is.\n'
                  '  // The line it prints is captured by the ExitBootServices flush.\n'
                  '  //\n'
                  '  PublishEmmcHandoff ();',
                  "call after flush")
    changed += n

    if changed:
        with open(BOOT, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
    print("patched %d site(s)" % changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
