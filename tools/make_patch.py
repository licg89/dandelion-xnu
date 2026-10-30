#!/usr/bin/env python3
"""Assemble a minimal patch from snapshot/current pairs, with git-style headers.

The XNU tree in this repository is only ever reproduced in CI by applying
patches/xnu/*.patch in filename order.  A change therefore has to be expressed as a
patch whose context matches the tree *after* every earlier patch -- which is exactly
what the local patched tree looks like.  Diffing the local tree against its upstream
commit would instead produce the cumulative diff and fail to apply.

So: snapshot the file before editing, edit it in the tree, then diff snapshot ->
current.  --no-index gives a clean unified diff for a file that is not tracked at
that path; the headers are rewritten to a/<relpath> and b/<relpath> so git apply
accepts it at the top of the tree.

    py -3.13 tools/make_patch.py out.patch \\
        osfmk/arm64/machine_routines.c=D:/aios/artifacts/snap/osfmk_arm64_machine_routines.c \\
        ...
"""
import os
import subprocess
import sys


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = sys.argv[1]
    chunks = []

    for spec in sys.argv[2:]:
        if "=" not in spec:
            sys.exit("bad spec %r, want relpath=snapshot" % spec)
        relative, snapshot = spec.split("=", 1)
        if not os.path.isfile(snapshot):
            sys.exit("missing snapshot " + snapshot)

        result = subprocess.run(
            ["git", "diff", "--no-index", "--no-color", "--unified=3", snapshot, relative],
            capture_output=True, text=True, cwd=os.getcwd())
        if result.returncode not in (0, 1):
            sys.exit("git diff failed for %s: %s" % (relative, result.stderr.strip()))
        body = result.stdout
        if not body.strip():
            print("no change for " + relative)
            continue

        lines = body.split("\n")
        # Rewrite: "diff --git a/<snap> b/<rel>", "--- a/<snap>", "+++ b/<rel>"
        rewritten = []
        for line in lines:
            if line.startswith("diff --git "):
                rewritten.append("diff --git a/%s b/%s" % (relative, relative))
            elif line.startswith("--- "):
                rewritten.append("--- a/%s" % relative)
            elif line.startswith("+++ "):
                rewritten.append("+++ b/%s" % relative)
            else:
                rewritten.append(line)
        chunks.append("\n".join(rewritten).rstrip("\n") + "\n")

    if not chunks:
        sys.exit("nothing to write")

    with open(out, "w", encoding="utf-8", newline="") as fh:
        fh.write("".join(chunks))
    print("wrote %s (%d file(s))" % (out, len(chunks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
