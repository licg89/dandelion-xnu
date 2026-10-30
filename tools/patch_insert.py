#!/usr/bin/env python3
"""Insert lines into a unified-diff patch, keeping every hunk header honest.

Why: the kernel is built in CI from patches/xnu/*.patch, so every experiment means
editing a patch file rather than a source tree.  Hand-editing is how a patch silently
stops applying: the @@ -a,b +c,d @@ counts go stale and `git apply` rejects the whole
file, or worse, `patch` applies it with fuzz and the experiment measures something
else.  This does the arithmetic instead.

Additions only (+ lines), which is all a diagnostic needs:

  * inside hunk i, the + count grows by N
  * every later hunk's + start shifts by N
  * - side is untouched

Self-checks after editing: every hunk's declared counts must equal the number of
lines it actually carries.

Usage, from the repository root:

    py -3.13 tools/patch_insert.py patches/xnu/dandelion-gic-mode.patch \\
        --after 'printf("[dl] gic late: hppir1=%llx\\n",' \\
        --insert 'printf("[dl] gic late: hppir0=%llx\\n",' \\
        --insert '    (unsigned long long)__builtin_arm_rsr64("ICC_HPPIR0_EL1"));'

--after matches a line by substring (the line is kept as-is); every --insert line is
added after it, prefixed with '+'.  Repeat --after/--insert pairs by invoking more
than once; the tool is not idempotent by itself, so check the result.
"""
import argparse
import re
import sys

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$")


def parse(lines):
    """Return [(header_index, minus_start, minus_count, plus_start, plus_count, end_index)]."""
    hunks = []
    for index, line in enumerate(lines):
        match = HUNK.match(line)
        if not match:
            continue
        minus_start = int(match.group(1))
        minus_count = int(match.group(2)) if match.group(2) is not None else 1
        plus_start = int(match.group(3))
        plus_count = int(match.group(4)) if match.group(4) is not None else 1
        end = len(lines)
        for later in range(index + 1, len(lines)):
            if HUNK.match(lines[later]) or lines[later].startswith("diff --git "):
                end = later
                break
        hunks.append([index, minus_start, minus_count, plus_start, plus_count, end])
    return hunks


def check(lines, hunks):
    for (head, ms, mc, ps, pc, end) in hunks:
        body = lines[head + 1:end]
        minus = sum(1 for line in body if line.startswith("-") and not line.startswith("---"))
        plus = sum(1 for line in body if line.startswith("+") and not line.startswith("+++"))
        context = sum(1 for line in body if line.startswith(" ") or line == "\n")
        if minus + context < mc:
            return "hunk at line %d declares %d '-' but carries %d" % (head + 1, mc, minus + context)
        if plus + context < pc:
            return "hunk at line %d declares %d '+' but carries %d" % (head + 1, pc, plus + context)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("patch")
    ap.add_argument("--after", required=True)
    ap.add_argument("--insert", action="append", default=None,
                    help="one line to add; repeat for several")
    ap.add_argument("--insert-file", default=None,
                    help="a file whose lines are added verbatim (use this when the code has "
                         "quotes, tabs or blank lines -- the shell cannot be trusted with them)")
    args = ap.parse_args()

    with open(args.patch, "r", encoding="utf-8") as fh:
        lines = fh.read().split("\n")

    hunks = parse(lines)
    if not hunks:
        sys.exit("no hunks in %s" % args.patch)

    # Find the marker line inside some hunk body.
    target = None
    for (head, ms, mc, ps, pc, end) in hunks:
        for index in range(head + 1, end):
            if args.after in lines[index] and lines[index].startswith(("+", " ", "-")):
                target = index
                break
        if target is not None:
            break
    if target is None:
        sys.exit("no line matching %r inside a hunk" % args.after)

    # Which hunk holds it, and how many later hunks need their + start moved.
    holder = None
    for number, hunk in enumerate(hunks):
        if hunk[0] < target < hunk[5]:
            holder = number
            break
    if holder is None:
        sys.exit("marker line %d is outside every hunk" % (target + 1))

    if args.insert_file:
        with open(args.insert_file, "r", encoding="utf-8") as fh:
            body = fh.read().rstrip("\n").split("\n")
    elif args.insert:
        body = args.insert
    else:
        sys.exit("nothing to insert: pass --insert or --insert-file")
    added = ["+" + text for text in body]
    lines[target + 1:target + 1] = added
    count = len(added)

    # Recompute headers from scratch off the new line positions.
    hunks = parse(lines)
    for number, hunk in enumerate(hunks):
        head, ms, mc, ps, pc, end = hunk
        if number == holder:
            pc += count
        elif number > holder:
            ps += count
        body = lines[head + 1:end]
        minus = sum(1 for line in body if line.startswith("-") and not line.startswith("---"))
        plus = sum(1 for line in body if line.startswith("+") and not line.startswith("+++"))
        context = sum(1 for line in body if line.startswith(" ") or line == "\n")
        mc = minus + context
        pc = plus + context
        lines[head] = "@@ -%d,%d +%d,%d @@" % (ms, mc, ps, pc)

    problem = check(lines, parse(lines))
    if problem:
        sys.exit("self-check failed: " + problem)

    with open(args.patch, "w", encoding="utf-8", newline="") as fh:
        fh.write("\n".join(lines))
    print("inserted %d line(s) after line %d; %d hunk(s) rewritten" % (count, target + 1, len(hunks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
