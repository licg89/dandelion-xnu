#!/usr/bin/env bash
# Install a fake xcrun(1) plus compiler wrappers so that XNU's macOS build
# system can be driven on a Linux host.
#
# XNU's makedefs/MakeInc.cmd resolves *every* tool through
#   $(XCRUN) -sdk $(SDKROOT) -find <tool>
# and passes XCRUN on the make command line, so overriding it is enough to
# take over tool discovery.  This script writes into $WORK/bin:
#
#   xcrun          answers -sdk/-show-sdk-path/-show-sdk-version/-find
#   clang, clang++ wrappers that add -target arm64-apple-ios16.5 -isysroot SDK
#   <tool>         symlinks to the matching Linux tool, when one exists
#
# env: WORK (default $HOME/xnu-local), XC_SDK_DIR, XC_SDK_VERSION
set -uo pipefail
WORK=${WORK:-$HOME/xnu-local}
SDK=${XC_SDK_DIR:-$WORK/sdk/iPhoneOS16.5.sdk}
VERSION=${XC_SDK_VERSION:-16.5}
BIN=$WORK/bin
mkdir -p "$BIN" || exit 2

cat >"$BIN/xcrun" <<XC
#!/usr/bin/env bash
# Fake xcrun(1): answers the queries XNU's makedefs makes.
set -u
SDK_DIR=\${XC_SDK_DIR:-$SDK}
SDK_VERSION=\${XC_SDK_VERSION:-$VERSION}
BIN_DIR=\${XC_BIN_DIR:-$BIN}
sdk=""; query=""; findtool=""
while [ $# -gt 0 ]; do
	case "$1" in
	-verbose) shift ;;
	-sdk) sdk=$2; shift 2 ;;
	-show-sdk-path) query=path; shift ;;
	-show-sdk-version) query=version; shift ;;
	-show-sdk-platform-path) query=platpath; shift ;;
	-show-sdk-platform-version) query=platversion; shift ;;
	-find) query=find; findtool=$2; shift 2 ;;
	*) shift ;;
	esac
done
case "$query" in
path) echo "$SDK_DIR" ;;
version|platversion) echo "$SDK_VERSION" ;;
platpath) echo "$(dirname "$SDK_DIR")/Platforms/iPhoneOS.platform" ;;
find)
	if [ -x "$BIN_DIR/$findtool" ]; then echo "$BIN_DIR/$findtool"
	elif command -v "$findtool" >/dev/null 2>&1; then command -v "$findtool"
	else echo "xcrun: error: unable to find utility \"$findtool\"" >&2; exit 72; fi ;;
*) exit 1 ;;
esac
XC
chmod +x "$BIN/xcrun"

for pair in "clang:clang" "clang++:clang++"; do
	name=${pair%%:*}
	real=$(command -v "${pair#*:}") || { echo "no ${pair#*:} in PATH" >&2; exit 2; }
	cat >"$BIN/$name" <<WRAP
#!/usr/bin/env bash
exec "$real" -target arm64-apple-ios$VERSION -isysroot "$SDK" "\$@"
WRAP
	chmod +x "$BIN/$name"
done

# Tools the build may ask for; link the ones this host has, leave the rest
# missing so that the build fails where it really is missing.
for t in nm strip ar ranlib lipo otool libtool dsymutil nmedit git perl python3 \
	make bison flex gm4 mig migcom iig ctf_insert ctfconvert ctfmerge ctfdump \
	codesign_allocate segedit unifdef; do
	if [ ! -e "$BIN/$t" ] && command -v "$t" >/dev/null 2>&1; then
		ln -sf "$(command -v "$t")" "$BIN/$t"
	fi
done

echo "xcrun-shim: $BIN"
echo "  -find clang   -> $("$BIN/xcrun" -sdk iphoneos -find clang)"
echo "  -show-sdk-path-> $("$BIN/xcrun" -sdk iphoneos -show-sdk-path)"
echo "  missing tools: $(for t in mig migcom iig libtool otool lipo unifdef flex bison gm4 ctf_insert; do [ -x "$BIN/$t" ] || printf '%s ' "$t"; done)"
