#!/usr/bin/env bash
# Build ONE XNU kernel configuration for this project on macOS.
#
# Environment (all optional):
#   XNU_SRC             XNU source tree        (default: $PWD/.xnu-src)
#   BUILD_ROOT          object/sym/dst root    (default: $PWD/build)
#   MACHINE             machine config name    (default: bcm2837, lower case)
#   KERNEL_CONFIG       DEVELOPMENT/RELEASE    (default: DEVELOPMENT)
#   LIBDISPATCH_TAG     libdispatch release    (default: libdispatch-1324.60.3)
#   JOBS                make -j value          (default: hw.ncpu)
#
# The kernel ends up in $OBJROOT/${KERNEL_CONFIG}_ARM64_${MACHINE^^}/ and is
# copied to $BUILD_ROOT/mach_kernel.
#
# The iPhoneOS SDK does not ship the private kernel headers XNU expects, so a
# "kernel SDK root" is assembled in $BUILD_ROOT/sdk: libdispatch's os/*.h plus
# XNU's own installhdrs output.  patches/xnu/ci-kernel-sdkroot.patch makes the
# build look there through KERNEL_SDKROOT.
set -euo pipefail

ROOT=${BUILD_ROOT:-$PWD/build}
SRC=${XNU_SRC:-$PWD/.xnu-src}
MACHINE=${MACHINE:-bcm2837}
KERNEL_CONFIG=${KERNEL_CONFIG:-DEVELOPMENT}
LIBDISPATCH_TAG=${LIBDISPATCH_TAG:-libdispatch-1324.60.3}
JOBS=${JOBS:-$(sysctl -n hw.ncpu)}

MACHINE_UPPER=$(printf '%s' "$MACHINE" | tr '[:lower:]' '[:upper:]')
OUT="$ROOT/obj/${KERNEL_CONFIG}_ARM64_${MACHINE_UPPER}"
SDK=${KERNEL_SDKROOT:-$ROOT/sdk}

if [ ! -f "$SRC/Makefile" ]; then
	echo "build-xnu.sh: no XNU source at $SRC" >&2
	exit 1
fi

echo "== XNU ${XNU_TAG:-unknown} / $MACHINE / $KERNEL_CONFIG =="
# Note: no `| head -1` here; closing the pipe makes xcodebuild abort with
# SIGPIPE and fails the step.
clang --version
xcodebuild -version

mkdir -p "$ROOT/obj" "$ROOT/sym" "$ROOT/dst" "$SDK/usr/local/include/os"

# XNU's bsd/sys/make_symbol_aliasing.sh needs availability.pl, which the
# iPhoneOS SDK does not ship.  patches/xnu/ci-availability-pl.patch makes it
# honour this variable; the script is a version-list stand-in (see the file).
AVAILABILITY_PL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/availability.pl"
chmod +x "$AVAILABILITY_PL"
export XNU_AVAILABILITY_PL="$AVAILABILITY_PL"

# --- kernel SDK root: libdispatch os/ headers -------------------------------
if [ ! -f "$SDK/.libdispatch-$LIBDISPATCH_TAG" ]; then
	echo "== fetching $LIBDISPATCH_TAG headers =="
	rm -rf "$ROOT/libdispatch"
	git clone --depth 1 --branch "$LIBDISPATCH_TAG" \
		https://github.com/apple-oss-distributions/libdispatch.git "$ROOT/libdispatch"
	# os/internal/ and friends are included as <os/internal/...>, so take the
	# whole tree and then simplify the top-level headers for kernel use.
	cp -R "$ROOT"/libdispatch/os/. "$SDK/usr/local/include/os/"
	if command -v unifdef >/dev/null 2>&1; then
		for header in "$SDK"/usr/local/include/os/*.h; do
			rc=0
			unifdef -DKERNEL=1 -DOS_FIREHOSE_SPI=1 \
				-DOS_VOUCHER_ACTIVITY_SPI_TYPES=1 \
				-UOS_VOUCHER_ACTIVITY_SPI "$header" > "$header.tmp" || rc=$?
			if [ "$rc" -le 1 ]; then
				mv "$header.tmp" "$header"
			else
				rm -f "$header.tmp"
			fi
		done
	fi
	rm -rf "$ROOT/libdispatch"
	touch "$SDK/.libdispatch-$LIBDISPATCH_TAG"
fi

# --- kernel SDK root: XNU's exported headers --------------------------------
if [ ! -f "$SDK/.xnu-$MACHINE-$KERNEL_CONFIG" ]; then
	echo "== installhdrs =="
	make -C "$SRC" installhdrs \
		SDKROOT=iphoneos \
		ARCH_CONFIGS=ARM64 \
		MACHINE_CONFIGS="$MACHINE_UPPER" \
		KERNEL_CONFIGS="$KERNEL_CONFIG" \
		ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
		BUILD_LTO=0 \
		BUILD_WERROR=0 \
		MAKEJOBS=-j"$JOBS" \
		OBJROOT="$ROOT/obj" \
		SYMROOT="$ROOT/sym" \
		DSTROOT="$SDK"
	touch "$SDK/.xnu-$MACHINE-$KERNEL_CONFIG"
fi

# --- kernel -----------------------------------------------------------------
echo "== build =="
make -C "$SRC" build \
	SDKROOT=iphoneos \
	KERNEL_SDKROOT="$SDK" \
	ARCH_CONFIGS=ARM64 \
	MACHINE_CONFIGS="$MACHINE_UPPER" \
	KERNEL_CONFIGS="$KERNEL_CONFIG" \
	ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 \
	LARGE_MEMORY_DEFINE=-UARM_LARGE_MEMORY \
	BUILD_LTO=0 \
	BOUND_CHECKS=0 \
	BUILD_WERROR=0 \
	MAKEJOBS=-j"$JOBS" \
	OBJROOT="$ROOT/obj" \
	SYMROOT="$ROOT/sym" \
	DSTROOT="$ROOT/dst"

echo "== produced =="
ls -l "$OUT" || true

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
