#!/usr/bin/env bash
# Stop the hidden Minecraft (dev client) and its Gradle daemon.
pkill -f "[f]abric.dli.config"; pkill -f "[G]radleDaemon"; echo "Minecraft stopped"
