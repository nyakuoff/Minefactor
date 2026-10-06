#!/usr/bin/env bash
# Start the hidden Minecraft side (dev client), then Abiotic Factor through Steam.
# Minecraft waits for the game's shared memory and links on its own.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Minecraft 26.3 needs Java 25; Prism's bundled runtime has a full JDK.
export JAVA_HOME="${JAVA_HOME_25:-$HOME/.var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher/java/java-runtime-epsilon}"
mkdir -p "$ROOT/fabric/run"
PRISM_INSTANCE="$HOME/.var/app/org.prismlauncher.PrismLauncher/data/PrismLauncher/instances/Minefactor"
if pgrep -f "[f]abric.dli.config|[i]nstances/Minefactor" >/dev/null; then
    echo "Minecraft already running"
elif [[ -d "$PRISM_INSTANCE" && "${MINEFACTOR_DEV:-}" != 1 ]]; then
    # The installed instance (tools/install_prism.sh): your Microsoft account, starts hidden.
    nohup flatpak run org.prismlauncher.PrismLauncher -l Minefactor >/dev/null 2>&1 &
    echo "Minecraft starting from Prism (instance Minefactor; MINEFACTOR_DEV=1 for the dev client)"
else
    (cd "$ROOT/fabric" && nohup ./gradlew runClient --console=plain >"$ROOT/fabric/run/minefactor-client.log" 2>&1 &)
    echo "Minecraft starting (dev client; log: fabric/run/minefactor-client.log)"
fi
steam -applaunch 427410 >/dev/null 2>&1 &
echo "Abiotic Factor starting; game log: .../Binaries/Win64/ue4ss/UE4SS.log"
