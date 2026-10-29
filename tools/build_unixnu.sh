#!/usr/bin/env bash
# Build UniXNU.efi and friends from https://github.com/UniXNU/UniXNU on Linux.
#
# Why this is here: M6 was blocked on "external artefacts that do not exist", and
# that turned out to be wrong.  Only the *kernel* step needs macOS and Xcode;
# `package/UniXNUPkg` builds UniXNU.efi, UniXNUDxe.efi, UniXNURamDiskDxe.efi and
# UniXNULib with plain EDK2, on Linux, from public source.  DarwinBoot is a
# separate submodule and still has to be built.
#
# Requirements, all of which this machine already has:
#   * an edk2 / Project Mu checkout with MdePkg, BaseFdtLib/libfdt and
#     MipiSysTLib/mipisyst -- /root/uefi-a04br3/Mu_Basecore is one;
#   * GenFw and GenSec in that tree's BaseTools/Source/C/bin.  A full
#     `make -C BaseTools/Source/C` fails here on the missing brotli submodule
#     (BrotliCompress only), so build just the two that matter:
#         make -C $EDK2/BaseTools/Source/C -j4 GenFw GenSec
#
# Usage (inside WSL):
#   bash tools/build_unixnu.sh                    # uses the defaults below
#   UNIXNU_EDK2=/root/other-tree bash tools/build_unixnu.sh
#
# Results are copied to Resources/UniXNU/ so a checkout has them without the
# build.  Sources stay out of this repository: they are public and large.
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${UNIXNU_WORK:-/root/UniXNU}
EDK2=${UNIXNU_EDK2:-/root/uefi-a04br3/Mu_Basecore}
OUTDIR=${UNIXNU_BUILD_DIR:-/root/unixnu-out}
SRC=${UNIXNU_SRC_REPO:-https://github.com/UniXNU/UniXNU.git}

if [ ! -d "$WORK/.git" ]; then
  echo "== cloning $SRC -> $WORK"
  git clone --depth 1 "$SRC" "$WORK"
fi

if [ ! -x "$EDK2/BaseTools/Source/C/bin/GenFw" ]; then
  echo "== building GenFw and GenSec (see the note at the top of this script)"
  make -C "$EDK2/BaseTools/Source/C" -j"$(nproc)" GenFw GenSec
fi

mkdir -p "$OUTDIR"
UNIXNU_EDK2="$EDK2" UNIXNU_BUILD_DIR="$OUTDIR" bash "$WORK/package/build.sh" "$@"

mkdir -p "$REPO/Resources/UniXNU"
cp -f "$OUTDIR"/Build/Out/AARCH64-*/{UniXNU,UniXNUDxe,UniXNURamDiskDxe}.efi "$REPO/Resources/UniXNU/"
ls -l "$REPO/Resources/UniXNU/"
