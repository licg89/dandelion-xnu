#!/usr/bin/env python3
"""Static consistency check for the three lists that decide what ships.

A module only exists in the firmware when all three agree, and they are edited in
three different files:

    dandelion.dsc  [Components]   -> it gets compiled
    Include/DXE.inc               -> it is placed in the firmware volume
    Include/APRIORI.inc           -> it is dispatched in this order

Getting one of them wrong fails quietly: the build succeeds and the module is
simply absent (CORRECTIONS C13).  This script reads the three lists plus the FDF,
cross-checks them, and verifies that every module that lives in this repository
actually exists on disk.

    python tools/check_modules.py
    python tools/check_modules.py --tree /mnt/d/.../uefi-a04br3   # also check upstream INFs

Exit code is 0 when every list agrees and every file exists.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.join(os.path.dirname(HERE), "Platforms", "Xiaomi", "dandelionPkg")

INF_RE = re.compile(r"^\s*INF\s+(\S+)\s*$")


def read_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def collect(path, keep=lambda line: True):
    """Return the INF paths listed in a DSC/FDF/inc file."""
    out = []
    for line in read_lines(path):
        stripped = line.split("#", 1)[0].rstrip()
        match = INF_RE.match(stripped)
        if match and keep(match.group(1)):
            out.append(match.group(1))
    return out


# Bare path form: this DSC lists components as "dandelionPkg/.../X.inf" with no
# "INF" keyword (that form is for FDF lists).  Accept both so the parser does not
# silently report zero components -- which it did the first time, and a checker
# that reports nothing is worse than no checker.
BARE_INF_RE = re.compile(r"^\s*([A-Za-z0-9_./+-]+\.inf)\s*$")


def dsc_components(path):
    """Components listed in the DSC's [Components] section."""
    inside = False
    out = []
    for line in read_lines(path):
        stripped = line.split("#", 1)[0].rstrip()
        if stripped.startswith("["):
            inside = stripped.lower().startswith("[components]")
            continue
        if not inside:
            continue
        match = INF_RE.match(stripped) or BARE_INF_RE.match(stripped)
        if match:
            out.append(match.group(1))
    return out


def resolve(inf, tree):
    """Map an INF path as written in the DSC/FDF to a file on disk, or None."""
    # Paths are relative to the workspace roots the build uses: dandelionPkg/...,
    # MediaTekPkg/..., MdePkg/... and so on.  Only this repository's own modules
    # are resolvable without a build tree.
    if inf.startswith("dandelionPkg/"):
        return os.path.join(PKG, inf[len("dandelionPkg/"):])
    if tree:
        # Everything else lives in the upstream tree under Silicon/ or Mu_Basecore/.
        for base in ("Platforms/Xiaomi", "Silicon/MediaTek", "Silicon/Silicium", "Mu_Basecore"):
            candidate = os.path.join(tree, base, inf)
            if os.path.exists(candidate):
                return candidate
        for base in os.listdir(tree):
            candidate = os.path.join(tree, base, inf)
            if os.path.exists(candidate):
                return candidate
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", help="upstream EDK2 tree, to resolve non-dandelion INFs")
    args = ap.parse_args()

    dsc = os.path.join(PKG, "dandelion.dsc")
    fdf = os.path.join(PKG, "dandelion.fdf")
    apriori = os.path.join(PKG, "Include", "APRIORI.inc")
    dxe = os.path.join(PKG, "Include", "DXE.inc")

    for path in (dsc, fdf, apriori, dxe):
        if not os.path.exists(path):
            sys.exit("missing %s" % path)

    components = dsc_components(dsc)
    fv = collect(fdf)
    apriori_mods = collect(apriori)
    dxe_mods = collect(dxe)

    problems = []

    # 1. Everything from *this* package that is placed in the FV must also be
    #    compiled.  Upstream modules come from the package DSC includes
    #    (MT6765Pkg.dsc.inc, SiliciumPkg.dsc.inc, ...), so their absence from
    #    dandelion.dsc's own [Components] means nothing.
    for mod in sorted(set(fv) | set(apriori_mods) | set(dxe_mods)):
        if mod.startswith("dandelionPkg/") and mod not in components:
            problems.append("in an FV list but not in [Components]: %s" % mod)

    # 2. Everything compiled that belongs to this package should be in the FV.
    for mod in sorted(components):
        if not mod.startswith("dandelionPkg/"):
            continue
        if mod not in fv and mod not in dxe_mods and mod not in apriori_mods:
            problems.append(
                "compiled but placed in no FV list (it will be built and never shipped): %s" % mod
            )

    # 3. APRIORI only orders what is in the FV.
    for mod in sorted(set(apriori_mods)):
        if mod not in fv and mod not in dxe_mods:
            problems.append("ordered in APRIORI but placed in no FV list: %s" % mod)

    # 4. Every module file must exist.
    for mod in sorted(set(components) | set(fv) | set(apriori_mods) | set(dxe_mods)):
        path = resolve(mod, args.tree)
        if path is None:
            if args.tree:
                problems.append("cannot resolve %s anywhere" % mod)
            continue
        if not os.path.exists(path):
            problems.append("listed but the file does not exist: %s" % mod)

    print("dandelionPkg modules")
    print("  [Components] : %d" % len([m for m in components if m.startswith("dandelionPkg/")]))
    print("  FDF files    : %d" % len([m for m in fv if m.startswith("dandelionPkg/")]))
    print("  DXE.inc      : %d" % len([m for m in dxe_mods if m.startswith("dandelionPkg/")]))
    print("  APRIORI.inc  : %d" % len([m for m in apriori_mods if m.startswith("dandelionPkg/")]))
    print("")
    for mod in sorted({m for m in components if m.startswith("dandelionPkg/")}):
        marks = []
        marks.append("build" if mod in components else "-")
        marks.append("fv" if (mod in fv or mod in dxe_mods or mod in apriori_mods) else "-")
        marks.append("order" if mod in apriori_mods else "-")
        print("  %-58s %s" % (mod, " ".join(marks)))

    if problems:
        print("")
        for problem in problems:
            print("  !! %s" % problem)
        sys.exit("%d consistency problem(s)" % len(problems))

    print("")
    print("check_modules: OK (all lists agree, all files present)")


if __name__ == "__main__":
    main()
