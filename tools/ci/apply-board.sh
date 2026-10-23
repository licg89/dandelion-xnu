#!/usr/bin/env bash
# Copy the Dandelion board files into a pristine XNU tree.
#
# The repository only carries the files that differ from Apple's release:
#   config/MASTER.arm64.dandelion        machine configuration
#   pexpert/pexpert/arm64/DANDELION.h    per-board definitions
#   iokit/Kernel/arm/*.cpp               board support
#
# Source patches live in patches/xnu/ and are applied by apply-patches.sh.
#
# usage: tools/ci/apply-board.sh <path-to-xnu-src>
set -euo pipefail

SRC=${1:?usage: apply-board.sh <path-to-xnu-src>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

if [ ! -f "$SRC/Makefile" ] || [ ! -d "$SRC/osfmk" ]; then
	echo "apply-board.sh: $SRC does not look like an XNU tree" >&2
	exit 1
fi

echo "== copying Dandelion board files into $SRC =="
cp -v "$HERE/config/MASTER.arm64.dandelion"                 "$SRC/config/"
cp -v "$HERE/pexpert/pexpert/arm64/DANDELION.h"             "$SRC/pexpert/pexpert/arm64/"
cp -v "$HERE/iokit/Kernel/arm/DandelionPlatformExpert.cpp"  "$SRC/iokit/Kernel/arm/"
