#!/usr/bin/env bash
# Start the hidden Minecraft side (dev client), then Abiotic Factor through Steam.
# Minecraft waits for the game's shared memory and links on its own.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Minecraft 26.3 needs Java 25; Prism's bundled runtime has a full JDK.
export JAVA_HOME="${JAVA_HOME_25:-$HOME/.var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher/java/java-runtime-epsilon}"
mkdir -p "$ROOT/fabric/run"
if ! pgrep -f "minefactor.quitWithSkyrim" >/dev/null; then
    (cd "$ROOT/fabric" && nohup ./gradlew runClient --console=plain >"$ROOT/fabric/run/minefactor-client.log" 2>&1 &)
    echo "Minecraft starting (log: fabric/run/minefactor-client.log)"
fi
steam -applaunch 427410 >/dev/null 2>&1 &
echo "Abiotic Factor starting; game log: .../Binaries/Win64/ue4ss/UE4SS.log"
