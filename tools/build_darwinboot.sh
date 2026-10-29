#!/usr/bin/env bash
# Build DarwinBoot.efi from https://github.com/UniXNU/DarwinBoot on Linux.
#
# DarwinBoot is the piece between UniXNU and XNU: UniXNU hands it a boot entry's
# arguments, DarwinBoot copies them into XNU's command line, prints
# "DARWINBOOT: command line:", paints a green bar and branches to the kernel.
# It is an ordinary EDK2 package with its own DarwinBoot.dsc, so unlike the
# kernel it does not need macOS.
#
# Usage (inside WSL):
#   bash tools/build_darwinboot.sh
#   UNIXNU_EDK2=/root/other-tree bash tools/build_darwinboot.sh
set -euo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${DARWINBOOT_WORK:-/root/DarwinBoot}
OUT=${DARWINBOOT_OUT:-/root/DarwinBoot-out}
EDK=${UNIXNU_EDK2:-/root/uefi-a04br3/Mu_Basecore}
SRC=${DARWINBOOT_SRC_REPO:-https://github.com/UniXNU/DarwinBoot.git}
# DarwinBoot.inf depends on UniXNUPkg.dec (the loader protocol GUID lives there),
# so the UniXNU checkout has to be on the package path as well.
UNIXNU_WORK=${UNIXNU_WORK:-/root/UniXNU}
UNIXNU_SRC=${UNIXNU_SRC_REPO:-https://github.com/UniXNU/UniXNU.git}

if [ ! -d "$WORK/.git" ]; then
  echo "== cloning $SRC -> $WORK"
  git clone --depth 1 "$SRC" "$WORK"
fi

if [ ! -f "$UNIXNU_WORK/package/UniXNUPkg/UniXNUPkg.dec" ]; then
  echo "== cloning $UNIXNU_SRC -> $UNIXNU_WORK (for UniXNUPkg.dec)"
  git clone --depth 1 "$UNIXNU_SRC" "$UNIXNU_WORK"
fi

#
# Our patch over upstream: make AllocateWindow's placement diagnostic also go to
# ConOut, because UniXNU redraws its menu the moment the entry returns and wipes
# the framebuffer rows it is drawn on.  ConOut is what reaches the flash log.
#
python3 "$REPO/tools/patch_darwinboot.py" "$WORK"

if [ ! -x "$EDK/BaseTools/Source/C/bin/GenFw" ]; then
  echo "== building GenFw and GenSec (a full BaseTools make needs the brotli submodule)"
  make -C "$EDK/BaseTools/Source/C" -j"$(nproc)" GenFw GenSec
fi

export WORKSPACE="$OUT"
export PACKAGES_PATH="$WORK:$UNIXNU_WORK/package:$EDK"
export EDK_TOOLS_PATH="$EDK/BaseTools"
export CONF_PATH="$OUT/Build/Conf"
export PYTHON_COMMAND=${PYTHON_COMMAND:-python3}
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH"
export CLANG_BIN=${CLANG_BIN:-$(dirname "$(command -v clang)")/}

mkdir -p "$CONF_PATH"
for f in target tools_def build_rule; do
  cp "$EDK_TOOLS_PATH/Conf/$f.template" "$CONF_PATH/$f.txt"
done

cd "$WORK"
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b DEBUG -t CLANGPDB -n "$(nproc)"

mkdir -p "$REPO/Resources/UniXNU"
find "$OUT" -name 'DarwinBoot.efi' -exec cp -f {} "$REPO/Resources/UniXNU/" \;
ls -l "$REPO/Resources/UniXNU/DarwinBoot.efi"
