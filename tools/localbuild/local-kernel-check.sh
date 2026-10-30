#!/usr/bin/env bash
# Compile one XNU kernel source file on a Linux host, to catch the breakage
# that otherwise costs a CI round trip.
#
# This is a syntax/scope check, not a build: no config-generated headers, no
# MIG-generated headers, no assembled kernel SDK root.  Files whose include
# chain needs those are reported as "n/a" with the header that stopped it --
# see docs/LOCAL-BUILD.md (the short version: they need MIG, a macOS-only
# build tool, so no local build of those files is possible on Windows/WSL).
#
# usage:
#   tools/localbuild/local-kernel-check.sh                 # default file set
#   tools/localbuild/local-kernel-check.sh <file...>        # any kernel source
#   tools/localbuild/local-kernel-check.sh --emit-obj -o /tmp/objs <file>
#
# env:
#   XNU_TREE   XNU checkout   (default /mnt/d/aios/xnu)
#   CLANG      compiler       (default clang)
set -uo pipefail

TREE=${XNU_TREE:-/mnt/d/aios/xnu}
CLANG=${CLANG:-clang}
EMIT_OBJ=0
OUTDIR=

FILES=()
while [ $# -gt 0 ]; do
	case "$1" in
	-h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	--emit-obj) EMIT_OBJ=1; shift ;;
	-o) OUTDIR=$2; shift 2 ;;
	-*) echo "unknown option: $1" >&2; exit 2 ;;
	*) FILES+=("$1"); shift ;;
	esac
done
[ ${#FILES[@]} -eq 0 ] && FILES=(pexpert/arm/pe_fiq.c)
[ -d "$TREE" ] || { echo "local-kernel-check: no tree at $TREE (set XNU_TREE)" >&2; exit 2; }
command -v "$CLANG" >/dev/null 2>&1 || { echo "local-kernel-check: no '$CLANG' in PATH" >&2; exit 2; }
if [ "$EMIT_OBJ" = 1 ]; then
	OUTDIR=${OUTDIR:-$(mktemp -d)}
	mkdir -p "$OUTDIR" || exit 2
fi

# Four headers in the kernel include chain come from Apple's SDK, not from the
# tree.  These stubs only let the compiler get past them; nothing here is
# compiled into anything.  Anything that needs more reports itself below.
SHIM=$(mktemp -d)
trap 'rm -rf "$SHIM"' EXIT
mkdir -p "$SHIM/mach"
cat >"$SHIM/TargetConditionals.h" <<'EOF'
#pragma once
#define TARGET_OS_OSX 1
#define TARGET_OS_IPHONE 0
#define TARGET_OS_MAC 1
#define TARGET_OS_EMBEDDED 0
#define TARGET_OS_SIMULATOR 0
EOF
cat >"$SHIM/mach/clock.h" <<'EOF'
#pragma once
#include <mach/mach_types.h>
EOF
cat >"$SHIM/mach_assert.h" <<'EOF'
#pragma once
#define mach_assert(x)
#define __improbable(x, y) (x)
#define __probable(x, y) (x)
EOF
cat >"$SHIM/string.h" <<'EOF'
#pragma once
#include <stddef.h>
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
size_t strlcpy(char *, const char *, size_t);
size_t strlcat(char *, const char *, size_t);
EOF

# The real build compiles with -nostdinc and takes every header from -I paths,
# so the kernel's own headers win over the host's.  -fno-builtin plus a bare
# bare-metal triple keeps a Linux clang from assuming a hosted Darwin system.
COMMON=(
	--target=aarch64-none-elf
	-fno-builtin
	-Wno-everything
	-DAPPLE -D__APPLE__ -D__arm64__ -D__ARM64__ -DKERNEL
	-DDANDELION -DBSD_KERNEL_PRIVATE
	-I"$SHIM"
	-I. -IEXTERNAL_HEADERS -Iosfmk -Ibsd -Ilibkern -Ipexpert -Iiokit
	-Ibsd/sys -Iosfmk/arm64
	-include bsd/sys/types.h
)

fail=0
for rel in "${FILES[@]}"; do
	path="$TREE/$rel"
	if [ ! -f "$path" ]; then
		echo "  skip  $rel (not in the tree)"
		continue
	fi
	case "$rel" in
	*.cpp|*.cc) lang=(-x c++ -std=gnu++17) ;;
	*)          lang=(-x c -std=gnu99) ;;
	esac
	if [ "$EMIT_OBJ" = 1 ]; then
		obj="$OUTDIR/$(basename "${rel%.*}").o"
		out=$(cd "$TREE" && "$CLANG" "${COMMON[@]}" "${lang[@]}" -c -o "$obj" "$rel" 2>&1)
	else
		obj=
		out=$(cd "$TREE" && "$CLANG" "${COMMON[@]}" "${lang[@]}" -fsyntax-only "$rel" 2>&1)
	fi
	status=$?
	errors=$(printf '%s\n' "$out" | grep -E 'error:')
	if [ $status -eq 0 ]; then
		if [ -n "$obj" ]; then
			echo "  ok    $rel -> $obj"
			echo "        $(file -b "$obj")"
		else
			echo "  ok    $rel"
		fi
		continue
	fi
	# A missing generated header means this file cannot be checked here at
	# all; say which one instead of reporting the file as broken.
	missing=$(printf '%s\n' "$errors" | sed -nE "s/.*fatal error: '([^']+)' file not found.*/\1/p" | head -1)
	if [ -n "$missing" ] && [ -z "$(printf '%s\n' "$errors" | grep -v 'fatal error')" ]; then
		echo "  n/a   $rel (needs <$missing>, which only the macOS build generates)"
		continue
	fi
	echo "  FAIL  $rel"
	printf '%s\n' "$errors" | head -8 | sed 's/^/        /'
	fail=1
done

if [ $fail -eq 0 ]; then
	echo "local-kernel-check: done (${#FILES[@]} file(s) requested)"
fi
exit $fail
