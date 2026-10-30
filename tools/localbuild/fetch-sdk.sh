#!/usr/bin/env bash
# Fetch an iPhoneOS SDK without Xcode.  Only needed for the *full build*
# experiment (tools/localbuild/try-build-xnu.sh); the file-level check
# (tools/localbuild/local-kernel-check.sh) does not need an SDK at all.
#
# The SDK comes from https://github.com/theos/sdks, which publishes SDKs
# extracted from Xcode and trimmed for userland tweak builds:
#   - no System/Library/Frameworks/Kernel.framework
#   - no usr/include/mach_assert.h
# Both are needed to compile osfmk/arm64/sleh.c and the IOKit glue, so this SDK
# is *not* equivalent to the macos-14 runner's Xcode SDK.  See
# docs/LOCAL-BUILD.md.
#
# env: WORK (default $HOME/xnu-local), SDK_NAME (default iPhoneOS16.5.sdk)
# size: ~233 MB on disk, ~15 s on a 20 Mbit line (sparse git clone)
set -uo pipefail
WORK=${WORK:-$HOME/xnu-local}
SDK_NAME=${SDK_NAME:-iPhoneOS16.5.sdk}
mkdir -p "$WORK/sdk" || exit 2

if [ -d "$WORK/sdk/$SDK_NAME" ]; then
	echo "fetch-sdk: already have $WORK/sdk/$SDK_NAME"
	du -sh "$WORK/sdk/$SDK_NAME"
	exit 0
fi
if [ ! -d "$WORK/sdks-src/.git" ]; then
	rm -rf "$WORK/sdks-src"
	git clone --depth 1 --filter=blob:none --no-checkout \
		https://github.com/theos/sdks.git "$WORK/sdks-src" || exit 1
fi
cd "$WORK/sdks-src" || exit 2
git sparse-checkout init --cone
git sparse-checkout set "$SDK_NAME"
git checkout || exit 1
mv "$SDK_NAME" "$WORK/sdk/$SDK_NAME"
echo "fetch-sdk: $WORK/sdk/$SDK_NAME"
du -sh "$WORK/sdk/$SDK_NAME"
ls "$WORK/sdk/$SDK_NAME/usr/include/mach" | wc -l | sed 's/^/  mach headers: /'
