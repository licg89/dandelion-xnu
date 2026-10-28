#!/usr/bin/env python3
"""Check that every relative link and code-reference path in docs/ and README resolves.

Documentation in this repository is dense and cross-referenced on purpose: a
handover document that points at a file which no longer exists wastes exactly the
time it was written to save.  This walks every markdown file at the top level and
in docs/, checks markdown links like [x](y.md) and `path/like/this.py`
references, and reports the ones that do not exist.

    python tools/check_docs.py

Exit code 0 when every reference resolves.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LINK_RE = re.compile(r"\[[^\]]*\]\(([^)#\s]+)(?:#[^)]*)?\)")
# `backtick/paths` that look like files: contain a slash and an extension.
CODE_RE = re.compile(r"`([A-Za-z0-9_./+-]*/[A-Za-z0-9_.+-]+\.[A-Za-z0-9]+)`")

SKIP_PREFIX = ("http://", "https://", "mailto:", "#")


def markdown_files():
    files = []
    for base in (ROOT, os.path.join(ROOT, "docs")):
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            if name.endswith(".md"):
                files.append(os.path.join(base, name))
    return files


# Documents that describe a past state on purpose carry this marker near the top.
# Their references are allowed to rot: rewriting a historical snapshot so it
# matches today would destroy the thing it is for.
HISTORICAL_MARKER = "历史文档"


def is_historical(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        head = [next(fh, "") for _ in range(12)]
    return any(HISTORICAL_MARKER in line for line in head)


def check_reference(source, target, problems):
    if target.startswith(SKIP_PREFIX):
        return
    # A reference may be relative to the file, or to the repository root.
    for base in (os.path.dirname(source), ROOT):
        if os.path.exists(os.path.normpath(os.path.join(base, target))):
            return
    problems.append((source, target))


def main():
    problems = []
    seen = 0

    skipped = []
    for path in markdown_files():
        if is_historical(path):
            skipped.append(path)
            continue

        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()

        for match in LINK_RE.finditer(text):
            seen += 1
            check_reference(path, match.group(1).strip(), problems)

        for match in CODE_RE.finditer(text):
            candidate = match.group(1)
            # Only path-looking strings that start with a known top-level dir; the
            # docs also mention plenty of upstream paths that live in the build tree.
            if candidate.startswith(("tools/", "docs/", "Platforms/", "Resources/", "artifacts/", "bootshim/", "Silicon/")):
                seen += 1
                check_reference(path, candidate, problems)

    rel = lambda p: os.path.relpath(p, ROOT)
    if problems:
        for source, target in problems:
            print("  !! %s -> %s" % (rel(source), target))
        sys.exit("%d broken reference(s) out of %d checked" % (len(problems), seen))

    print(
        "check_docs: OK (%d references checked in %d markdown files, %d historical skipped)"
        % (seen, len(markdown_files()) - len(skipped), len(skipped))
    )


if __name__ == "__main__":
    main()
