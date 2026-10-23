#!/bin/sh
# Cross-compile the Dandelion platform expert for aarch64.
#
# The full XNU build is not wired up yet, so this is a quick syntax/ABI check
# for the platform wiring in iokit/Kernel/arm/DandelionPlatformExpert.cpp.
# It works with clang's built-in aarch64 backend (no WSL required) or with a
# native aarch64 cross compiler:
#
#   ./tools/check-platform-expert.sh
#   CC=aarch64-linux-gnu-g++ ./tools/check-platform-expert.sh

set -eu
cd "$(dirname "$0")/.."

CC=${CC:-clang}
OUT=${OUT:-${TMPDIR:-/tmp}/DandelionPlatformExpert.o}

case "$CC" in
*clang*)
	TARGET_FLAGS="--target=aarch64-none-elf"
	;;
*)
	TARGET_FLAGS=""
	;;
esac

"$CC" \
	$TARGET_FLAGS \
	-std=c++17 \
	-O2 \
	-ffreestanding \
	-nostdlib \
	-fno-pic \
	-fno-exceptions \
	-fno-rtti \
	-mgeneral-regs-only \
	-mstrict-align \
	-fno-stack-protector \
	-fno-builtin \
	-mcpu=cortex-a53 \
	-Wall -Wextra -Werror \
	-Ipexpert -Ibootshim \
	-c iokit/Kernel/arm/DandelionPlatformExpert.cpp \
	-o "$OUT"

echo "ok: $OUT"
