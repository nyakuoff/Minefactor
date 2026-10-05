#!/usr/bin/env bash
# Install the built mod (and its matching UE4SS.dll) into the Abiotic Factor game folder.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME="${ABF_DIR:-$HOME/.local/share/Steam/steamapps/common/AbioticFactor}"
U="$GAME/AbioticFactor/Binaries/Win64/ue4ss"
BIN="$ROOT/ue4ss/build"
[[ -d "$U/Mods" ]] || { echo "no UE4SS install at $U" >&2; exit 1; }

mkdir -p "$U/Mods/Minefactor/dlls"
cp "$BIN/Minefactor/main.dll" "$U/Mods/Minefactor/dlls/main.dll"
# The mod must run under a UE4SS core built by the same toolchain/commit.
[[ -f "$U/UE4SS.dll.pre-minefactor" ]] || cp "$U/UE4SS.dll" "$U/UE4SS.dll.pre-minefactor"
cp "$BIN/Game__Shipping__Win64/bin/UE4SS.dll" "$U/UE4SS.dll"
if ! grep -q '^Minefactor : ' "$U/Mods/mods.txt"; then
    # Above the built-in Keybinds entry, which must stay last.
    sed -i 's/^; Built-in keybinds/Minefactor : 1\n\n; Built-in keybinds/' "$U/Mods/mods.txt"
fi
echo "deployed to $U"
