#!/usr/bin/env bash
# Overlay this repository onto a pristine XNU source tree.
#
# The repository only carries the files that differ from Apple's release:
#   config/MASTER.arm64.dandelion        machine configuration
#   pexpert/pexpert/arm64/DANDELION.h    per-board definitions
#   iokit/Kernel/arm/*.cpp               board support
#   patches/xnu/*.patch                  source changes that cannot live in
#                                        a separate file (4 KiB page ABI)
#
# usage: tools/ci/apply-overlay.sh <path-to-xnu-src>
set -euo pipefail

SRC=${1:?usage: apply-overlay.sh <path-to-xnu-src>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

if [ ! -f "$SRC/Makefile" ] || [ ! -d "$SRC/osfmk" ]; then
	echo "apply-overlay.sh: $SRC does not look like an XNU tree" >&2
	exit 1
fi

echo "== copying board files into $SRC =="
cp -v "$HERE/config/MASTER.arm64.dandelion"          "$SRC/config/"
cp -v "$HERE/pexpert/pexpert/arm64/DANDELION.h"      "$SRC/pexpert/pexpert/arm64/"
cp -v "$HERE/iokit/Kernel/arm/DandelionPlatformExpert.cpp" "$SRC/iokit/Kernel/arm/"

echo "== applying source patches =="
shopt -s nullglob
for patch in "$HERE"/patches/xnu/*.patch; do
	if git -C "$SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
		echo "already applied: $(basename "$patch")"
		continue
	fi
	git -C "$SRC" apply --check "$patch"
	git -C "$SRC" apply "$patch"
	echo "applied: $(basename "$patch")"
done
