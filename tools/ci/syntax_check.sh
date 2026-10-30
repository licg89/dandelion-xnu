#!/usr/bin/env bash
# Syntax-check the handful of XNU files this port keeps breaking, without macOS.
#
# The kernel is built in CI on a macOS runner, so a typo costs a round trip.
# This uses the clang that is in WSL and the headers that are already in the
# checked-out tree: no SDK, no generated headers, -fsyntax-only.  It is not the
# real build -- it cannot catch anything that depends on config-generated
# headers -- but it does catch the failures that actually happened:
#
#   * a stray U+FEFF  (error: use of undeclared identifier '\ufeff')
#   * an identifier that is not in scope, e.g. ISB_SY in pe_fiq.c after the
#     patch dropped #include <machine/atomic.h>   (five pushes' worth of lesson)
#
# usage, from Windows:
#   wsl -d Ubuntu -e bash /mnt/d/aios/dandelion-xnu/tools/ci/syntax_check.sh
#   wsl -d Ubuntu -e bash .../syntax_check.sh pexpert/arm/pe_fiq.c
#
# env: XNU_TREE (default /mnt/d/aios/xnu), CLANG (default clang)
set -uo pipefail

TREE=${XNU_TREE:-/mnt/d/aios/xnu}
CLANG=${CLANG:-clang}

# Files that the Dandelion patches touch and whose include chain resolves here.
# bsd/kern/bsd_init.c and iokit/bsddev/IOKitBSDInit.cpp cannot be checked this
# way: their include chain needs real SDK headers (mach/clock_priv.h and C11
# atomics from the SDK), and stubbing those would test the stubs, not the code.
# Those two are still verified by CI; keep the edits small and obvious there.
DEFAULT_FILES=(
	pexpert/arm/pe_fiq.c
)

FILES=("$@")
if [ ${#FILES[@]} -eq 0 ]; then
	FILES=("${DEFAULT_FILES[@]}")
fi

if [ ! -d "$TREE" ]; then
	echo "syntax_check.sh: no tree at $TREE (set XNU_TREE)" >&2
	exit 2
fi
if ! command -v "$CLANG" >/dev/null 2>&1; then
	echo "syntax_check.sh: no $CLANG in PATH" >&2
	exit 2
fi

# A bare-metal triple has no libc, and XNU's kernel headers legitimately include
# <string.h>.  These declarations are only there so the compiler can get past
# them; nothing here is compiled into anything.
SHIM=$(mktemp -d)
trap 'rm -rf "$SHIM"' EXIT
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

# Three headers in XNU's include chain come from the macOS SDK, not the tree, so
# without stubs only pexpert/arm/pe_fiq.c could be checked at all.  These are the
# minimum that keeps the compiler going; a file that needs more SDK declarations
# still reports itself as n/a below.
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

COMMON=(
	--target=aarch64-none-elf
	-fsyntax-only
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
	out=$(cd "$TREE" && "$CLANG" "${COMMON[@]}" "${lang[@]}" "$rel" 2>&1)
	status=$?
	if [ $status -eq 0 ]; then
		echo "  ok    $rel"
		continue
	fi
	errors=$(echo "$out" | grep -E "error:")
	# A missing SDK header (TargetConditionals.h, mach/clock.h, mach_assert.h,
	# ...) means this file cannot be checked here at all: those come from the
	# macOS SDK, not from the source tree.  Say so instead of reporting a
	# failure that is not the patch's fault.
	if [ -n "$errors" ] && ! echo "$errors" | grep -qvE "fatal error: '[^']+' file not found"; then
		first=$(echo "$errors" | head -1 | sed -E "s/.*'([^']+)'.*/\1/")
		echo "  n/a   $rel (needs the macOS SDK for <$first>)"
		continue
	fi
	echo "  FAIL  $rel"
	echo "$errors" | head -8 | sed 's/^/        /'
	fail=1
done

if [ $fail -eq 0 ]; then
	echo "syntax_check: OK (${#FILES[@]} file(s))"
fi
exit $fail
