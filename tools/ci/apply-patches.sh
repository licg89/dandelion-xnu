#!/usr/bin/env bash
# Apply the source patches this project needs on top of a pristine XNU tree.
#
# These are build-environment and 4 KiB page fixes that cannot live in a
# separate file.  They are applied for every machine configuration, not just
# Dandelion.
#
# usage: tools/ci/apply-patches.sh <path-to-xnu-src>
set -euo pipefail

SRC=${1:?usage: apply-patches.sh <path-to-xnu-src>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

if [ ! -f "$SRC/Makefile" ] || [ ! -d "$SRC/osfmk" ]; then
	echo "apply-patches.sh: $SRC does not look like an XNU tree" >&2
	exit 1
fi

shopt -s nullglob
for patch in "$HERE"/patches/xnu/*.patch; do
	name=$(basename "$patch")
	if git -C "$SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
		echo "already applied: $name"
		continue
	fi
	if ! git -C "$SRC" apply --check "$patch" >/dev/null 2>&1; then
		echo "apply-patches.sh: $name does not apply to this tree" >&2
		git -C "$SRC" apply --check "$patch" || true
		exit 1
	fi
	git -C "$SRC" apply "$patch"
	echo "applied: $name"
done
