#!/usr/bin/env python3
"""Verify the patch series applies to a pristine tree, and that it says what it should.

Run inside WSL, where the build tree lives:

    wsl -d Ubuntu -e bash -lc 'python3 /mnt/d/aios/dandelion-xnu/tools/check_patches.py'

Why: `tools/build-local.sh` already fails when a patch does not apply, but a patch
can apply cleanly and still lose the thing it existed for -- that is exactly how
the PIO read rewrite disappeared once (a hand edit in the tree, wiped by the next
build's `git checkout --`, see docs/HANDOVER-CURRENT.md §5.6).  This checks the
result of applying the series against a list of markers that must be present
afterwards, so "the patch is in the tree" is a fact rather than an assumption.

Exit code 0 means: every patch applied in order and every marker was found.
"""
import os
import subprocess
import sys

WORK = os.environ.get("UEFI_TREE", os.path.expanduser("~/uefi-a04br3"))
HERE = os.path.dirname(os.path.abspath(__file__))
PATCHDIR = os.path.join(os.path.dirname(HERE), "Resources", "MuPatches")

MSDC = "Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.c"
MSDC_INF = "Silicon/MediaTek/MediaTekPkg/Drivers/MsdcDxe/MsdcDxe.inf"

# Markers that must be in the patched tree.  Each one corresponds to a decision
# documented in docs/ANALYSIS-EMMC-DATA-PATH.md; if it disappears, that decision
# is gone from the firmware even though the build still succeeds.
EXPECT = {
    MSDC: [
        ("panel readout", "MsdcPanelValue"),
        ("panel readout: failing values parked", "MsdcPanelValue (0, IntStatus)"),
        ("PIO read rewrite", "ddn pio-read FAIL"),
        ("pin-level sampling", "PsOr  |= PsNow"),
        ("DMA read path", "MsdcDmaRead"),
        ("DMA: descriptors cleaned", "WriteBackInvalidateDataCacheRange"),
        ("stale interrupt clear", "MsdcWrite (Private, MSDC_INT, 0xFFFFFFFF)"),
        ("card timing forced back", "EMMCSwitch (PassThru, 3, 185, 0, 0)"),
        ("card bus forced to 1-bit", "EMMCSwitch (PassThru, 3, 183, 0, 0)"),
        ("data timeout pinned", "0xff << 24"),
        ("signal path registers", "MSDC_PAD_TUNE0"),
        ("response captured", "ddn data cmd=%u r1=%08x"),
        ("fatal R1 aborts the transfer", "refused, r1 has error bits"),
    ],
    MSDC_INF: [
        ("cache maintenance for DMA", "CacheMaintenanceLib"),
    ],
}


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, **kwargs)


def main():
    if not os.path.isdir(WORK):
        sys.exit("build tree not found: %s (set UEFI_TREE)" % WORK)

    patches = sorted(
        os.path.join(PATCHDIR, name)
        for name in os.listdir(PATCHDIR)
        if name.endswith(".patch")
    )
    if not patches:
        sys.exit("no patches in %s" % PATCHDIR)

    targets = set()
    for patch in patches:
        with open(patch, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("+++ b/"):
                    targets.add(line[len("+++ b/"):].strip())

    print("patch series: %d patch(es), %d target file(s)" % (len(patches), len(targets)))
    for target in sorted(targets):
        print("  target %s" % target)

    tracked = [t for t in sorted(targets) if run(["git", "-C", WORK, "ls-files", "--error-unmatch", t]).returncode == 0]
    untracked = sorted(targets - set(tracked))
    if untracked:
        sys.exit("not tracked upstream, cannot reset: %s" % ", ".join(untracked))

    status = run(["git", "-C", WORK, "checkout", "--"] + tracked)
    if status.returncode != 0:
        sys.exit("git checkout failed: %s" % status.stderr)

    for patch in patches:
        result = run(["git", "-C", WORK, "apply", "-p1", "--ignore-whitespace", patch])
        name = os.path.basename(patch)
        if result.returncode != 0:
            sys.exit("FAILED to apply %s\n%s" % (name, result.stderr))
        print("  applied %s" % name)

    problems = 0
    for rel, markers in EXPECT.items():
        path = os.path.join(WORK, rel)
        if not os.path.exists(path):
            print("  !! %s does not exist" % rel)
            problems += 1
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        for label, marker in markers:
            if marker not in text:
                print("  !! missing from %s: %s (%r)" % (rel, label, marker))
                problems += 1
        print("  checked %d marker(s) in %s" % (len(markers), rel))

    if problems:
        sys.exit("%d marker(s) missing -- the patch series no longer says what it should" % problems)

    print("check_patches: OK (series applies and every marker is present)")


if __name__ == "__main__":
    main()
