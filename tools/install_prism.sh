#!/usr/bin/env bash
# Builds the Fabric mod and installs/updates the Prism Launcher (Flatpak) instance "Minefactor":
# Minecraft 26.3 + Fabric Loader + Fabric API + our mod, starting hidden. Re-run after mod changes.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRISM="$HOME/.var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher"
INST="$PRISM/instances/Minefactor"
FABRIC_API="$(ls "$HOME"/.gradle/caches/modules-2/files-2.1/net.fabricmc.fabric-api/fabric-api/0.161.0+26.3/*/fabric-api-0.161.0+26.3.jar | head -1)"
E4MC="$(ls "$HOME"/.gradle/caches/modules-2/files-2.1/maven.modrinth/e4mc/AouleFRY/*/e4mc-AouleFRY.jar | head -1)"  # co-op links
export JAVA_HOME="${JAVA_HOME_25:-$PRISM/java/java-runtime-epsilon}"

[[ -d "$PRISM" ]] || { echo "Prism Launcher (Flatpak) not found at $PRISM" >&2; exit 1; }
(cd "$ROOT/fabric" && ./gradlew build -q && ./gradlew --stop -q)
JAR="$(ls "$ROOT"/fabric/build/libs/minefactor-*.jar | grep -v sources | head -1)"

mkdir -p "$INST/.minecraft/mods"
cp "$ROOT/tools/prism/instance.cfg" "$ROOT/tools/prism/mmc-pack.json" "$INST/"
rm -f "$INST/.minecraft/mods/"minefactor-*.jar "$INST/.minecraft/mods/"fabric-api-*.jar "$INST/.minecraft/mods/"e4mc-*.jar
cp "$JAR" "$FABRIC_API" "$E4MC" "$INST/.minecraft/mods/"
# Minecraft must see the host's /dev/shm (where Abiotic Factor's shared memory lives).
flatpak override --user --device=shm org.prismlauncher.PrismLauncher
echo "Prism instance ready: $INST (mods: $(basename "$JAR"), $(basename "$FABRIC_API"))"
