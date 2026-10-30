#!/usr/bin/env python3
"""Rebuild the patched XNU tree from upstream + patches/xnu/*.patch, offline.

Why this exists: the kernel is only ever built in CI, so a patch that does not
apply, or that applies but injects something the compiler rejects, costs a
45-minute round trip.  Both have happened:

  * 5dfa9b6/cefd3c7/1466671 failed with
      pe_fiq.c:301:1: error: use of undeclared identifier '\ufeff'
    -- a stray UTF-8 BOM on a line of the patch body, which `git apply`
    faithfully copied into the source file.  Three pushes were spent on it.

So before pushing, materialise the series here:

    py -3.13 tools/ci/check_series.py                 # verify only
    py -3.13 tools/ci/check_series.py --emit-to D:/aios/xnu
        rewrite the target files of the real tree with the series result,
        which makes the tree "upstream + series" again and lets
        tools/make_patch.py diff a snapshot against a truthful tree.

What it checks:
  1. every patch applies in filename order (the same order apply-patches.sh
     uses), against pristine content taken from git;
  2. no patch injects a byte that has no business being in C source:
     U+FEFF (BOM), other zero-width or control characters;
  3. with --emit-to, the tree's files really do equal the series result
     (rather than a stale mix of manual edits and older patches).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PATCH_DIR = os.path.join(REPO, "patches", "xnu")
DEFAULT_TREE = "D:/aios/xnu"

# Characters that must never be *added* by a patch to a C family source file.
# U+FEFF is the one that actually cost three CI rounds: a patch line of "+﻿"
# put a BOM into pe_fiq.c and clang rejected the file with
#   error: use of undeclared identifier '\ufeff'
SUSPECT = {
    "\ufeff": "U+FEFF byte order mark",
    "\u200b": "U+200B zero width space",
    "\u200e": "U+200E left-to-right mark",
    "\u200f": "U+200F right-to-left mark",
    "\u2060": "U+2060 word joiner",
}

# Harmless in a comment, present in upstream sources; reported, never fatal.
HARMLESS = {"\u00a0": "U+00A0 no-break space"}

# Board files are not in a patch: tools/ci/apply-board.sh copies them in.
BOARD_FILES = [
    "pexpert/pexpert/arm64/DANDELION.h",
    "pexpert/pexpert/arm64/dandelion_gic.h",
    "iokit/Kernel/arm/DandelionPlatformExpert.cpp",
    "config/MASTER.arm64.dandelion",
]

TARGET = re.compile(r"^\+\+\+ b/(.+)$")


def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, text=True, **kwargs)


def patch_list():
    names = sorted(n for n in os.listdir(PATCH_DIR) if n.endswith(".patch"))
    if not names:
        sys.exit("no patches in " + PATCH_DIR)
    return [os.path.join(PATCH_DIR, n) for n in names]


def targets_of(patch_path):
    found = []
    with open(patch_path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            match = TARGET.match(line.rstrip("\n"))
            if match:
                found.append(match.group(1))
    return found


def upstream_content(tree, path):
    """Pristine content of `path` from the tree's git object store.

    None means the file does not exist upstream: the series creates it, and
    `git apply` must be allowed to do that itself.
    """
    exists = subprocess.run(["git", "-C", tree, "cat-file", "-e", "HEAD:" + path],
                            capture_output=True)
    if exists.returncode != 0:
        return None
    result = subprocess.run(["git", "-C", tree, "show", "HEAD:" + path],
                            capture_output=True)
    if result.returncode != 0:
        sys.exit("cannot read %s from %s: %s" % (
            path, tree, result.stderr.decode("utf-8", "replace").strip()))
    return result.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", default=os.environ.get("XNU_TREE", DEFAULT_TREE),
                    help="pristine XNU checkout to take upstream files from")
    ap.add_argument("--emit-to", default=None,
                    help="write the series result into this tree (normally --tree)")
    ap.add_argument("--work", default=None, help="scratch directory (default: temp)")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir(os.path.join(args.tree, ".git")):
        sys.exit("not a git checkout: " + args.tree)

    patches = patch_list()
    targets = []
    for patch in patches:
        for path in targets_of(patch):
            if path not in targets:
                targets.append(path)
    if not args.quiet:
        print("series: %d patch(es), %d file(s)" % (len(patches), len(targets)))

    work = args.work or tempfile.mkdtemp(prefix="xnuseries-")
    if os.path.isdir(work):
        shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work, exist_ok=True)

    for path in targets:
        content = upstream_content(args.tree, path)
        if content is None:
            if not args.quiet:
                print("  new file (created by the series): %s" % path)
            continue
        dest = os.path.join(work, path.replace("/", os.sep))
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as fh:
            fh.write(content)
    if not args.quiet:
        print("pristine copies in %s" % work)

    for patch in patches:
        name = os.path.basename(patch)
        # Mirror tools/ci/apply-patches.sh exactly, including its idempotency
        # rule: a patch whose added lines are already in the tree (because an
        # earlier patch carried the same hunk) is skipped, not failed.  This
        # is how non-apple-no-root-panic.patch coexists with
        # non-apple-bsd-init-trace.patch in CI.
        reverse = run(["git", "apply", "--reverse", "--check", patch], cwd=work)
        if reverse.returncode == 0:
            if not args.quiet:
                print("  already applied: %s" % name)
            continue
        check = run(["git", "apply", "-p1", "--whitespace=nowarn",
                     "--unsafe-paths", patch], cwd=work)
        if check.returncode != 0:
            print("FAILED to apply %s" % name)
            print(check.stderr.strip() or check.stdout.strip())
            return 1
        if not args.quiet:
            print("  applied %s" % name)

    problems = 0
    warnings = 0

    # 1. What the patches *add* is what ends up in the compiled source, so lint
    #    the added lines of every patch directly: this catches the BOM that
    #    clang rejected without depending on where the hunk landed.
    for patch in patches:
        name = os.path.basename(patch)
        with open(patch, "r", encoding="utf-8", errors="replace") as fh:
            for line_number, line in enumerate(fh, 1):
                if not line.startswith("+") or line.startswith("+++"):
                    continue
                for char, label in SUSPECT.items():
                    if char in line:
                        print("  SUSPECT %s:%d: %s" % (name, line_number, label))
                        print("     %r" % line[:120])
                        problems += 1
                for char, label in HARMLESS.items():
                    if char in line:
                        warnings += 1
                        if args.verbose:
                            print("  note %s:%d: %s" % (name, line_number, label))

    # 2. Board files are copied in, not patched, and are compiled just the same.
    for relative in BOARD_FILES:
        full = os.path.join(REPO, relative.replace("/", os.sep))
        if not os.path.isfile(full):
            continue
        with open(full, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
        for line_number, line in enumerate(text.splitlines(), 1):
            for char, label in SUSPECT.items():
                if char in line:
                    print("  SUSPECT %s:%d: %s" % (relative, line_number, label))
                    problems += 1

    if problems:
        print("series applies, but %d suspicious byte(s) would be compiled" % problems)
        return 1

    if args.emit_to:
        root = args.emit_to
        if not os.path.isdir(os.path.join(root, ".git")):
            sys.exit("--emit-to is not a git checkout: " + root)
        for path in targets:
            src = os.path.join(work, path.replace("/", os.sep))
            dst = os.path.join(root, path.replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(src, "rb") as fh:
                data = fh.read()
            with open(dst, "wb") as fh:
                fh.write(data)
            if not args.quiet:
                print("  emitted %s" % path)

    print("check_series: OK (%d patch(es) apply in order, no suspicious bytes)" % len(patches))
    if not args.work and not args.emit_to:
        shutil.rmtree(work, ignore_errors=True)
    elif not args.work:
        shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
