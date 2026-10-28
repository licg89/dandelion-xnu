#!/bin/bash
#
# Local build driver for the dandelion UEFI (line B) firmware.
#
#   wsl -d Ubuntu -e bash /mnt/d/aios/dandelion-xnu/tools/build-local.sh [extra build_uefi.py args...]
#
# It performs exactly the steps .github/workflows/build-uefi-dandelion.yml
# performs, in the same order, so a local FD and a CI FD are the same firmware:
#
#   1. overlay this repository's Silicon/MT6765Pkg + Platforms/dandelionPkg
#      (+ Resources/Configs/dandelion.toml, Resources/DTBs/dandelion.dtb)
#   2. restore every file the patch series touches to its pristine upstream
#      content, then apply Resources/MuPatches/*.patch in name order
#   3. tools/prepare_uefi_tree.py (SimpleFbDxe scanout pitch 720 -> 736)
#   4. build_uefi.py -d dandelion -m 0 -r DEBUG
#   5. copy the resulting FD to $OUT
#
# Environment / gotchas learned the hard way:
#
#   * `wsl -d Ubuntu` is required.  The default WSL distro on this machine is
#     docker-desktop, which has no bash, so a bare `wsl -e bash ...` dies with
#     "execvpe(bash) failed: No such file or directory".
#
#   * The build tree must live on the WSL ext4 filesystem, not on /mnt/d.
#     On the Windows 9p mount the build periodically wedges with four python3
#     processes parked in p9_client_rpc and never finishes; on ext4 the same
#     build is several times faster.  Move a tree with
#     `_work/migrate_tree.sh`, or clone it directly inside WSL.
#
#   * A zip-extracted tree has populated-but-not-checked-out "submodules"
#     (Binaries, Common/Mu, Mu_Basecore, ...), so stuart --setup dies on
#     `git submodule update --init --recursive Binaries`.  build_uefi.py only
#     runs that step when Build/ does not exist, so creating an empty Build/
#     first skips it -- correct, because everything those submodules provide is
#     already on disk.  A real git clone (CI) is unaffected.
#
#   * Never hand-edit files under the upstream tree: put the change in
#     Resources/MuPatches instead.  Step 2 resets those files, and a hand-edited
#     tree is why "git apply" kept failing locally while CI was fine.
#
set -o pipefail
export HOME=${HOME:-/root}

SRC=$(cd "$(dirname "$0")/.." && pwd)
WORK=${DANDELION_WORK:-$HOME/uefi-a04br3}
OUT=${DANDELION_OUT:-/mnt/d/aios/artifacts}
LOG=${DANDELION_LOG:-/mnt/d/aios/_work/build_dandelion_last.log}

cd "$WORK" || exit 1
mkdir -p "$(dirname "$LOG")"
echo "== $(date -Is) local build in $WORK, extra args: $* ==" | tee "$LOG"
mkdir -p "$WORK/Build"

echo "-- overlay our packages" | tee -a "$LOG"
mkdir -p "$WORK/Silicon/MediaTek" "$WORK/Platforms/Xiaomi" "$WORK/Resources/Configs" "$WORK/Resources/DTBs"
rm -rf "$WORK/Silicon/MediaTek/MT6765Pkg" "$WORK/Platforms/Xiaomi/dandelionPkg"
cp -r "$SRC/Silicon/MediaTek/MT6765Pkg" "$WORK/Silicon/MediaTek/" || exit 1
cp -r "$SRC/Platforms/Xiaomi/dandelionPkg" "$WORK/Platforms/Xiaomi/" || exit 1
cp -f "$SRC/Resources/Configs/dandelion.toml" "$WORK/Resources/Configs/" || exit 1
cp -f "$SRC/Resources/DTBs/dandelion.dtb" "$WORK/Resources/DTBs/" || exit 1
echo "   MT6765Pkg:   $(find "$WORK/Silicon/MediaTek/MT6765Pkg" -type f | wc -l) files" | tee -a "$LOG"
echo "   dandelionPkg: $(find "$WORK/Platforms/Xiaomi/dandelionPkg" -type f | wc -l) files" | tee -a "$LOG"

shopt -s nullglob
PATCHES=( "$SRC"/Resources/MuPatches/*.patch )

if [ ${#PATCHES[@]} -gt 0 ]; then
  echo "-- restore patched upstream files to pristine, then apply ${#PATCHES[@]} patch(es)" | tee -a "$LOG"
  TARGETS=$(grep -h '^+++ b/' "${PATCHES[@]}" | sed 's|^+++ b/||' | sort -u)
  for t in $TARGETS; do
    if git -C "$WORK" ls-files --error-unmatch "$t" >/dev/null 2>&1; then
      git -C "$WORK" checkout -- "$t"
    else
      echo "   ?? $t is not tracked upstream -- patch cannot be applied" | tee -a "$LOG"
    fi
  done
  for p in "${PATCHES[@]}"; do
    if git -C "$WORK" apply -p1 --ignore-whitespace "$p" 2>/dev/null; then
      echo "   applied $(basename "$p")" | tee -a "$LOG"
    else
      echo "   !! FAILED $(basename "$p")" | tee -a "$LOG"
      echo "      (refusing to continue: a half-patched tree is worse than no build)" | tee -a "$LOG"
      exit 1
    fi
  done
fi

echo "-- SimpleFbDxe pitch fix" | tee -a "$LOG"
python3 "$SRC/tools/prepare_uefi_tree.py" "$WORK" 2>&1 | tee -a "$LOG"

echo "== build_uefi.py -d dandelion -m 0 -r DEBUG $* ==" | tee -a "$LOG"
python3 build_uefi.py -d dandelion -m 0 -r DEBUG "$@" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}

if [ $rc -eq 0 ]; then
  FD=$(ls -t "$WORK"/Build/dandelionPkg/DEBUG_CLANGPDB/FV/*.fd 2>/dev/null | head -1)
  if [ -n "$FD" ]; then
    mkdir -p "$OUT"
    cp -f "$FD" "$OUT/SILICIUM_UEFI.fd"
    echo "== fd: $(stat -c %s "$OUT/SILICIUM_UEFI.fd") bytes -> $OUT/SILICIUM_UEFI.fd ==" | tee -a "$LOG"
  else
    echo "== fd: NOT FOUND under Build/dandelionPkg/DEBUG_CLANGPDB/FV ==" | tee -a "$LOG"
  fi
fi

echo "== exit=$rc ==" | tee -a "$LOG"
exit $rc
