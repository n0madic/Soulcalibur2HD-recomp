#!/usr/bin/env bash
# Build the host of the recompiled Soulcalibur II HD: the ps3recomp runtime and the port, with every dependency linked
# in statically, and nothing derived from the game. The result is what a release ships and what runs the game module.
#
#   scripts/build_host.sh                         # for this machine (macOS, Linux)
#   SC2HD_TARGET=windows scripts/build_host.sh    # cross-compile for 64-bit Windows with MinGW-w64
#   scripts/build_host.sh                         # in Git Bash / MSYS on Windows: the same Windows build, natively
#
# Needs git, cmake, ninja, python3 (3.10+) and a C/C++ compiler (macOS: Xcode command line tools; Windows target: a
# MinGW-w64 clang -- llvm-mingw, or on Windows WinLibs' LLVM build -- or the x86_64-w64-mingw32 g++ with the posix
# thread model, see scripts/mingw-w64.cmake; Linux: the Vulkan and X11 headers, libvulkan-dev, libx11-dev and libxext-dev on
# Debian, for the window, and the PulseAudio or ALSA ones, libpulse-dev / libasound2-dev, for sound). Layout (generated):
#   build/ps3recomp/   pinned clone of github.com/sp00nznet/ps3recomp, with patches/ applied
#   build/deps/        pinned sources and the static install prefix of SDL2, glslang, SPIRV-Tools, SPIRV-Cross
#                      (prefix/), and for Windows of SDL2, glslang and the Vulkan headers (prefix-windows/): the
#                      game is drawn through the Vulkan backend there, in an SDL window
#   build/host/        cmake build tree of the host (build/host-windows/ for Windows)
#   dist/host/         sc2hd_host and sdk/ (the headers and tools a game module is built against); for Windows
#                      dist/host-windows/ with sc2hd_host.exe and, in sdk/, the import library the game DLL links against
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
TOOLKIT="$BUILD/ps3recomp"
DEPS="$BUILD/deps"
PREFIX="$DEPS/prefix"
TOOLKIT_REV="4e8f3ca87a3e1d87bb83b8b7e9348fe2685cd514"   # v0.18.0

# Pinned dependency sources (all permissive: zlib, BSD-3-Clause / Apache-2.0).
SDL_TAG="release-2.32.10"
GLSLANG_TAG="16.6.0"
SPIRV_TOOLS_REV="ef96ed763b43b59b33b31b362f09a02b729fa1c9"    # glslang 16.6.0 known_good.json
SPIRV_HEADERS_REV="496543121ce6419f23d6fa5d7194ba66c36212d2"  # likewise
SPIRV_CROSS_TAG="vulkan-sdk-1.4.363.0"
VULKAN_HEADERS_TAG="v1.4.328"                                  # headers only: vulkan-1.dll is loaded at run time

JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

TARGET="${SC2HD_TARGET:-native}"
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) TARGET=windows ;;   # Git Bash / MSYS on Windows: the only host this machine can build
esac
case "$TARGET" in
  native|windows) ;;
  *) echo "SC2HD_TARGET must be native or windows, not $TARGET" >&2; exit 1 ;;
esac

# --- toolkit ---------------------------------------------------------------------------------------------------------
if [[ ! -d "$TOOLKIT/.git" ]]; then
  mkdir -p "$BUILD"
  git -c core.autocrlf=false clone https://github.com/sp00nznet/ps3recomp.git "$TOOLKIT"
fi
# A clone from before the last move of the pin does not have the new revision yet.
if ! git -C "$TOOLKIT" cat-file -e "$TOOLKIT_REV^{commit}" 2>/dev/null; then
  git -C "$TOOLKIT" fetch -q --tags origin
fi
# The patches are made against the sources as committed: no line-ending conversion (Git for Windows defaults to it).
git -C "$TOOLKIT" config core.autocrlf false
# The patches are applied to a clean checkout, once per change of the revision or of the patches: they overlap, so a
# patch cannot be tested for "already applied" one by one. A stamp inside .git records what the tree was built from.
PATCH_STAMP="$TOOLKIT/.git/sc2hd-patches"
# patches/ is this port's own changes to the toolkit. patches/upstream/ (if present) holds open pull requests of the
# toolkit, imported as they are (each file names its PR and head commit); they go on first and are dropped once the
# pinned revision contains them.
shopt -s nullglob
PATCHES=("$ROOT"/patches/upstream/*.patch "$ROOT"/patches/*.patch)
shopt -u nullglob
PATCH_ID="$TOOLKIT_REV $(cat "${PATCHES[@]}" | git hash-object --stdin)"
if [[ "$(cat "$PATCH_STAMP" 2>/dev/null)" != "$PATCH_ID" ]]; then
  git -C "$TOOLKIT" checkout -q -f "$TOOLKIT_REV"
  git -C "$TOOLKIT" clean -fdq
  rm -f "$PATCH_STAMP"
  # The toolkit generates its NID -> handler table from the sources of libs/ at build time, but its build rule only
  # depends on the generator: a table made for the previous revision would still name functions that are gone.
  rm -f "$BUILD"/host*/gen/ppu_hle_nids.cpp
  for p in "${PATCHES[@]}"; do
    git -C "$TOOLKIT" apply "$p" || { echo "patch does not apply: $p" >&2; exit 1; }
  done
  echo "$PATCH_ID" > "$PATCH_STAMP"
fi

# --- static dependencies ---------------------------------------------------------------------------------------------
fetch() {  # fetch <dir> <url> <tag-or-rev>: a shallow checkout of exactly that ref
  local dir="$DEPS/src/$1" url="$2" ref="$3"
  if [[ ! -d "$dir/.git" ]]; then
    mkdir -p "$dir"
    git -C "$dir" init -q
    git -C "$dir" remote add origin "$url"
  fi
  git -C "$dir" fetch -q --depth 1 origin "$ref"
  git -C "$dir" checkout -q FETCH_HEAD
}
build_dep() {  # build_dep <name> <src dir> <cmake args...>
  local name="$1" src="$2"; shift 2
  [[ -f "$DEPS/$name.done" ]] && return
  cmake -S "$src" -B "$DEPS/build/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
  cmake --build "$DEPS/build/$name" -j "$JOBS"
  cmake --install "$DEPS/build/$name"
  touch "$DEPS/$name.done"
}
build_glslang() {  # build_glslang <name> <cmake args...>: glslang with SPIRV-Tools built in (HLSL legalization)
  local name="$1"; shift
  build_dep "$name" "$DEPS/src/glslang" -DBUILD_SHARED_LIBS=OFF -DENABLE_OPT=ON -DENABLE_GLSLANG_BINARIES=OFF \
            -DGLSLANG_TESTS=OFF -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF \
            -DALLOW_EXTERNAL_SPIRV_TOOLS=OFF "$@"
}
fetch_glslang() {
  fetch glslang https://github.com/KhronosGroup/glslang.git "$GLSLANG_TAG"
  fetch glslang/External/spirv-tools https://github.com/KhronosGroup/SPIRV-Tools.git "$SPIRV_TOOLS_REV"
  fetch glslang/External/spirv-tools/external/spirv-headers https://github.com/KhronosGroup/SPIRV-Headers.git "$SPIRV_HEADERS_REV"
}

if [[ "$TARGET" == native && "$(uname -s)" == Linux ]]; then
  # The window is a Vulkan swapchain on an SDL2 window: SDL builds its X11 video driver only if it finds the headers,
  # and silently ends up with no driver (a headless host) otherwise.
  have_header() { echo "#include <$1>" | ${CC:-cc} -E -x c - >/dev/null 2>&1; }
  for h in vulkan/vulkan.h X11/Xlib.h X11/extensions/Xext.h; do
    have_header "$h" || { echo "missing header $h (Debian/Ubuntu: apt install libvulkan-dev libx11-dev libxext-dev)" >&2; exit 1; }
  done
  # Likewise its audio drivers: without these headers SDL has only the dummy one and the game is silent.
  if [[ ! -f "$DEPS/sdl2.done" ]] && ! have_header pulse/pulseaudio.h && ! have_header alsa/asoundlib.h; then
    echo "warning: no PulseAudio or ALSA headers, the host will have no sound (Debian/Ubuntu: apt install libpulse-dev libasound2-dev)" >&2
  fi
fi

if [[ "$TARGET" == native ]]; then
  needs() { [[ ! -f "$DEPS/$1.done" ]]; }   # a dependency that is already built needs neither its sources nor the network
  if needs sdl2; then fetch sdl2 https://github.com/libsdl-org/SDL.git "$SDL_TAG"; fi
  if needs spirv-cross; then fetch spirv-cross https://github.com/KhronosGroup/SPIRV-Cross.git "$SPIRV_CROSS_TAG"; fi
  if needs glslang; then fetch_glslang; fi

  build_dep sdl2 "$DEPS/src/sdl2" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
  build_dep spirv-cross "$DEPS/src/spirv-cross" -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF \
            -DSPIRV_CROSS_ENABLE_TESTS=OFF
  # The HLSL legalization passes glslang runs need the optimizer, so SPIRV-Tools is built in.
  build_glslang glslang
fi

if [[ "$TARGET" == windows ]]; then
  # The Vulkan backend's window (SDL2), guest shaders (glslang) and headers, built with the Windows toolchain into
  # their own prefix. XInput and WASAPI stay the pad and audio backends.
  PREFIX="$DEPS/prefix-windows"
  WIN_TC=(-DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/mingw-w64.cmake")
  needs() { [[ ! -f "$DEPS/windows-$1.done" ]]; }
  if needs sdl2; then fetch sdl2 https://github.com/libsdl-org/SDL.git "$SDL_TAG"; fi
  if needs glslang; then fetch_glslang; fi
  if needs vulkan-headers; then fetch vulkan-headers https://github.com/KhronosGroup/Vulkan-Headers.git "$VULKAN_HEADERS_TAG"; fi
  build_dep windows-sdl2 "$DEPS/src/sdl2" "${WIN_TC[@]}" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF
  build_dep windows-vulkan-headers "$DEPS/src/vulkan-headers" "${WIN_TC[@]}"
  build_glslang windows-glslang "${WIN_TC[@]}"
fi

# --- host ------------------------------------------------------------------------------------------------------------
if [[ "$TARGET" == windows ]]; then
  HOST_BUILD="$BUILD/host-windows"
  DIST="$ROOT/dist/host-windows"
  EXE="sc2hd_host.exe"
  cmake -S "$ROOT/src" -B "$HOST_BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/mingw-w64.cmake" \
        -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_DIR="$TOOLKIT" -DPS3RECOMP_RSX_VULKAN=ON \
        -DCMAKE_PREFIX_PATH="$PREFIX" -DPS3RECOMP_VULKAN_INCLUDE_DIR="$PREFIX/include"
else
  HOST_BUILD="$BUILD/host"
  DIST="$ROOT/dist/host"
  EXE="sc2hd_host"
  # Keep Homebrew's shared libraries out of the link: the host takes the static ones from $PREFIX.
  # No pkg-config: CMake splits the -I path it returns at a space, and the checkout may have one in its path.
  IGNORE=()
  [[ -d /opt/homebrew ]] && IGNORE=(-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew)
  VULKAN=()
  [[ "$(uname -s)" == Linux ]] && VULKAN=(-DPS3RECOMP_RSX_VULKAN=ON)
  cmake -S "$ROOT/src" -B "$HOST_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS3RECOMP_DIR="$TOOLKIT" \
        -DCMAKE_PREFIX_PATH="$PREFIX" "${IGNORE[@]}" ${VULKAN[@]+"${VULKAN[@]}"} -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON
fi
cmake --build "$HOST_BUILD" -j "$JOBS"

rm -rf "${DIST:?}"
mkdir -p "$DIST/licenses"
cp "$HOST_BUILD/$EXE" "$DIST/"
cp -R "$HOST_BUILD/sdk" "$DIST/sdk"

# The licenses of everything linked into the host, for a release.
lic() {  # lic <name> <file>
  [[ -f "$2" ]] && cp "$2" "$DIST/licenses/$1.txt"
}
lic ps3recomp "$TOOLKIT/LICENSE"
lic SDL2 "$DEPS/src/sdl2/LICENSE.txt"
lic glslang "$DEPS/src/glslang/LICENSE.txt"
lic SPIRV-Tools "$DEPS/src/glslang/External/spirv-tools/LICENSE"
lic SPIRV-Headers "$DEPS/src/glslang/External/spirv-tools/external/spirv-headers/LICENSE"
if [[ "$TARGET" == native ]]; then
  lic SPIRV-Cross "$DEPS/src/spirv-cross/LICENSE"
else
  lic Vulkan-Headers "$DEPS/src/vulkan-headers/LICENSE.md"
fi
echo "built: $DIST/$EXE (ABI $(cat "$DIST/sdk/ABI"))"
