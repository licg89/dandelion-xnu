#!/usr/bin/env bash
# Build ONE XNU kernel configuration for this project on macOS.
#
# Environment (all optional):
#   XNU_SRC       XNU source tree            (default: $PWD/.xnu-src)
#   BUILD_ROOT    object/sym/dst root        (default: $PWD/build)
#   MACHINE       machine config name        (default: bcm2837, lower case)
#   KERNEL_CONFIG DEVELOPMENT/RELEASE/DEBUG  (default: DEVELOPMENT)
#
# The kernel ends up in $OBJROOT/${KERNEL_CONFIG}_ARM64_${MACHINE^^}/.
set -euo pipefail

ROOT=${BUILD_ROOT:-$PWD/build}
SRC=${XNU_SRC:-$PWD/.xnu-src}
MACHINE=${MACHINE:-bcm2837}
KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT}

MACHINE_UPPER=$(printf '%s' "$MACHINE" | tr '[:lower:]' '[:upper:]')
OUT="$ROOT/obj/${KERNEL_CONFIG}_ARM64_${MACHINE_UPPER}"

if [ ! -f "$SRC/Makefile" ]; then
	echo "build-xnu.sh: no XNU source at $SRC" >&2
	exit 1
fi

echo "== XNU ${XNU_TAG:-unknown} / $MACHINE / $KERNEL_CONFIG =="
# Note: no `| head -1` here; closing the pipe makes xcodebuild abort with
# SIGPIPE and fails the step.
clang --version
xcodebuild -version

mkdir -p "$ROOT/obj" "$ROOT/sym" "$ROOT/dst"

# XNU's bsd/sys/make_symbol_aliasing.sh needs availability.pl, which the
# iPhoneOS SDK does not ship.  patches/xnu/ci-availability-pl.patch makes it
# honour this variable; the script is a version-list stand-in (see the file).
AVAILABILITY_PL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/availability.pl"
chmod +x "$AVAILABILITY_PL"
export XNU_AVAILABILITY_PL="$AVAILABILITY_PL"

make -C "$SRC" build \
	SDKROOT=iphoneos \
	ARCH_CONFIGS=ARM64 \
	MACHINE_CONFIGS="$MACHINE_UPPER" \
	KERNEL_CONFIGS="$KERNEL_CONFIG" \
	ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
	LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY \
	BUILD_LTO=0 \
	BOUND_CHECKS=0 \
	BUILD_WERROR=0 \
	MAKEJOBS=-j"$(sysctl -n hw.ncpu)" \
	OBJROOT="$ROOT/obj" \
	SYMROOT="$ROOT/sym" \
	DSTROOT="$ROOT/dst"

echo "== produced =="
find "$OUT" -maxdepth 1 -type f -printf '%f %s\n' 2>/dev/null ||
	find "$OUT" -maxdepth 1 -type f -exec ls -l {} \;

# Copy the kernel under a stable name for the packaging steps.
if [ -f "$OUT/mach.${KERNEL_CONFIG,,}.${MACHINE}" ]; then
	cp "$OUT/mach.${KERNEL_CONFIG,,}.${MACHINE}" "$ROOT/mach_kernel"
elif [ -f "$OUT/mach.${KERNEL_CONFIG,,}.${MACHINE}.noctf" ]; then
	cp "$OUT/mach.${KERNEL_CONFIG,,}.${MACHINE}.noctf" "$ROOT/mach_kernel"
else
	echo "build-xnu.sh: kernel image not found in $OUT" >&2
	exit 1
fi
ls -l "$ROOT/mach_kernel"
