#!/usr/bin/env bash
# Pack a release: a zip with everything needed to play, and nothing derived from a game.
#
#   scripts/package.sh                  # every host that is built: dist/host (this OS) and dist/host-windows
#   scripts/package.sh native windows   # only these; a host that is missing is built first
#
# Writes dist/sc2hd-<version>-<os>-<arch>.zip per target, holding sc2hd/ with the launcher, the prebuilt host, the SDK
# the launcher compiles the game against, and the licenses. The native host is the one of the OS the script runs on, so
# a Linux zip is made by running this on Linux (a container will do), next to a build_host.sh run there.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(cat "$ROOT/VERSION")"
cd "$ROOT"

NATIVE_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"; [[ "$NATIVE_OS" == darwin ]] && NATIVE_OS=macos
NATIVE_ARCH="$(uname -m)"

host_exe() {  # host_exe <target>: the host executable a target is built to
  case "$1" in
    native) echo dist/host/sc2hd_host ;;
    windows) echo dist/host-windows/sc2hd_host.exe ;;
  esac
}

pack() {  # pack <target>
  local target="$1" host os arch
  case "$target" in
    native) host=dist/host; os="$NATIVE_OS"; arch="$NATIVE_ARCH"; ;;
    windows) host=dist/host-windows; os=windows; arch=x86_64; ;;
  esac
  local name="sc2hd-$VERSION-$os-$arch" stage="$ROOT/dist/stage"
  [[ -f "$(host_exe "$target")" ]] || SC2HD_TARGET="$target" "$ROOT/scripts/build_host.sh"

  rm -rf "${stage:?}"
  mkdir -p "$stage/sc2hd/licenses"
  cp README.md LICENSE LICENSE-GPL-2.0.txt VERSION sc2hd.py "$stage/sc2hd/"
  cp -R launcher "$stage/sc2hd/launcher"
  cp -R docs "$stage/sc2hd/docs"
  cp -R "$host" "$stage/sc2hd/host"
  mv "$stage/sc2hd/host/licenses/"* "$stage/sc2hd/licenses/"
  rmdir "$stage/sc2hd/host/licenses"
  find "$stage" \( -name __pycache__ -o -name .DS_Store \) -prune -exec rm -rf {} +

  rm -f "$ROOT/dist/$name.zip"
  (cd "$stage" && zip -qr -X "$ROOT/dist/$name.zip" sc2hd)
  rm -rf "${stage:?}"
  echo "packed: dist/$name.zip ($(du -h "$ROOT/dist/$name.zip" | cut -f1))"
}

targets=("$@")
if [[ ${#targets[@]} -eq 0 ]]; then
  for t in native windows; do
    [[ -f "$(host_exe "$t")" ]] && targets+=("$t")
  done
  [[ ${#targets[@]} -gt 0 ]] || targets=(native)   # nothing built yet: the default is this machine's host
fi
for t in "${targets[@]}"; do
  case "$t" in native|windows) ;; *) echo "unknown target $t (native or windows)" >&2; exit 1 ;; esac
done
for t in "${targets[@]}"; do
  pack "$t"
done
