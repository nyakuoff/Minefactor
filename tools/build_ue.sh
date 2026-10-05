#!/usr/bin/env bash
# Cross-compile the Minefactor UE4SS mod and UE4SS.dll itself (xwin + clang-cl), on Linux.
#   tools/build_ue.sh [--debug] [--clean]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/ue4ss"
BUILD="$SRC/build"
TYPE=Game__Shipping__Win64
for arg in "$@"; do
    case "$arg" in
        --debug) TYPE=Game__Debug__Win64 ;;
        --clean) rm -rf "$BUILD" ;;
        *) echo "unknown arg $arg" >&2; exit 1 ;;
    esac
done
export XWIN_DIR="${XWIN_DIR:-$HOME/.xwin}"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$TYPE" \
    -DCMAKE_TOOLCHAIN_FILE="$SRC/RE-UE4SS/cmake/toolchains/xwin-clang-cl-toolchain.cmake" >/dev/null
cmake --build "$BUILD"
find "$BUILD" -iname main.dll -o -iname UE4SS.dll | grep -v CMakeFiles
