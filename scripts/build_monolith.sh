#!/usr/bin/env bash
# Personal use only: link the lifted game code into ONE executable instead of loading it as a module. The result contains
# code derived from the game; do not share it.
#
#   scripts/build_monolith.sh <work dir>
#
# <work dir> is a games/<title>-<version> directory of the launcher's data directory after `sc2hd build` (it holds recompiled/ and
# spu/). Writes <work dir>/sc2hd_monolith. Needs the source checkout and what build_host.sh needs; it runs build_host.sh
# first if the static dependencies are not built yet. The executable's icon is the game's own ICON0.PNG from the launcher's
# data directory (hdd0/game/<title>/), the neutral icon of the host if it is not there. Run it with `sc2hd run --monolith`.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="$ROOT/build/deps/prefix"
[[ $# -eq 1 && -d "$1/recompiled" && -d "$1/spu" ]] || { sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
WORK="$(cd "$1" && pwd)"

[[ -d "$PREFIX" ]] || "$ROOT/scripts/build_host.sh"

# The launcher's data directory is two levels above a work dir (<data>/games/<title>-<version>).
ICON=()
for png in "$WORK"/../../hdd0/game/*/ICON0.PNG; do
  [[ -f "$png" ]] && { ICON=("-DSC2HD_ICON_PNG=$png"); break; }
done

IGNORE=()
[[ -d /opt/homebrew ]] && IGNORE=(-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew)
VULKAN=()
[[ "$(uname -s)" == Linux ]] && VULKAN=(-DPS3RECOMP_RSX_VULKAN=ON)
cmake -S "$ROOT/src" -B "$WORK/monolith-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPS3RECOMP_DIR="$ROOT/build/ps3recomp" -DCMAKE_PREFIX_PATH="$PREFIX" "${IGNORE[@]}" ${VULKAN[@]+"${VULKAN[@]}"} \
      -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON -DSC2HD_MONOLITH_DIR="$WORK" ${ICON[@]+"${ICON[@]}"}
cmake --build "$WORK/monolith-build"
cp "$WORK/monolith-build/sc2hd_monolith" "$WORK/sc2hd_monolith"
echo "built: $WORK/sc2hd_monolith"
