#!/usr/bin/env bash
# Run the *real* CI build line for MACHINE=dandelion KERNEL_CONFIG=DEVELOPMENT
# on a Linux host (WSL), and report exactly where it stops.
#
# It stops. On this machine (Windows + WSL2 Ubuntu, clang 21) the build gets as
# far as building XNU's own build tools (SETUP/*) and then dies, because those
# tools are macOS programs: XNU compiles and runs them during the build, so a
# native Linux copy must be ported first.  After that, MIG and ld64/cctools are
# still missing.  docs/LOCAL-BUILD.md has the full account with error text.
#
# This script exists so the claim stays reproducible: run it, read the log.
#
# env:
#   WORK        scratch root           (default $HOME/xnu-local)
#   XNU_SRC     tree to snapshot       (default /mnt/d/aios/xnu)
#   AVPL        availability.pl        (default this repo's tools/ci/availability.pl)
#   JOBS        make -j                (default nproc)
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
WORK=${WORK:-$HOME/xnu-local}
XNU_SRC=${XNU_SRC:-/mnt/d/aios/xnu}
AVPL=${AVPL:-$REPO/tools/ci/availability.pl}
JOBS=${JOBS:-$(nproc)}
SRC=$WORK/src
SDK=$WORK/sdk/iPhoneOS16.5.sdk
BIN=$WORK/bin
OUT=$WORK/build
LOG=$WORK/build.log

[ -f "$XNU_SRC/Makefile" ] || { echo "try-build-xnu: no XNU tree at $XNU_SRC" >&2; exit 2; }
[ -f "$AVPL" ] || { echo "try-build-xnu: no availability.pl at $AVPL" >&2; exit 2; }

# 1. Snapshot the tree.  The build writes generated headers into OBJROOT, but
#    keeping the source tree pristine means a failed experiment cannot confuse
#    the checkout that CI builds from.
if [ ! -d "$SRC/bsd" ]; then
	echo "== snapshot $XNU_SRC -> $SRC"
	mkdir -p "$SRC"
	(cd "$XNU_SRC" && tar -cf - --exclude=.git .) | (cd "$SRC" && tar -xf -) || exit 1
fi

# 2. Local portability fixes, snapshot only.
#    bsd/sys/make_symbol_aliasing.sh starts with "#!/bin/bash -"; macOS bash 3.2
#    tolerates the stray "-", Linux bash 5.x does not.  The checkout may also
#    have CRLF endings on that line, hence the ".*".
sed -i '1s|^#! /bin/bash -.*$|#!/bin/bash|' "$SRC/bsd/sys/make_symbol_aliasing.sh"

# 3. Tool discovery shim.
XC_SDK_DIR=$SDK XC_BIN_DIR=$BIN WORK=$WORK bash "$HERE/xcrun-shim.sh" || exit 1
export PATH=$BIN:$PATH
export XNU_AVAILABILITY_PL=$AVPL
mkdir -p "$OUT/obj" "$OUT/sym" "$OUT/dst"

# 4. Host facts the macOS makedefs read from sysctl/sw_vers, passed as make
#    variables instead (they are all plain "?=" or ":=" assignments, so a
#    command-line value wins).
HOSTMEM=$(awk '/MemTotal/ {printf "%d", $2 * 1024}' /proc/meminfo)

# 5. The CI invocation, verbatim from tools/ci/build-xnu.sh (the "make build"
#    step), plus the host overrides from step 4.
echo "== make build (MACHINE=DANDELION KERNEL_CONFIG=DEVELOPMENT)"
make -C "$SRC" build \
	SDKROOT=iphoneos \
	KERNEL_SDKROOT="$SDK" \
	ARCH_CONFIGS=ARM64 \
	MACHINE_CONFIGS=DANDELION \
	KERNEL_CONFIGS=DEVELOPMENT \
	ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
	LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY \
	BUILD_LTO=0 \
	BOUND_CHECKS=0 \
	BUILD_WERROR=0 \
	MAKEJOBS=-j"$JOBS" \
	OBJROOT="$OUT/obj" \
	SYMROOT="$OUT/sym" \
	DSTROOT="$OUT/dst" \
	XCRUN="$BIN/xcrun" \
	VERBOSE=YES \
	MEMORY_SIZE="$HOSTMEM" \
	SYSCTL_HW_PHYSICALCPU="$JOBS" \
	SYSCTL_HW_LOGICALCPU="$JOBS" \
	HOST_OS_VERSION=14.0 \
	>"$LOG" 2>&1
rc=$?

echo "== make exit: $rc  (log: $LOG, $(wc -l <"$LOG") lines)"
echo "== where it stopped:"
grep -E 'error:|Error [0-9]|\*\*\*' "$LOG" | grep -v 'xcrun: error' | tail -12
echo "== tools the build asked for and did not get:"
grep -oE 'unable to find utility "[^"]+"' "$LOG" | sort -u | sed 's/^/  /'
exit $rc
