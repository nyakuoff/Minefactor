#!/usr/bin/env bash
# Stop everything Minefactor runs: Minecraft (Prism instance or dev client), its Gradle daemon, and
# Abiotic Factor. (Patterns are bracketed so pkill never matches its own command line.)
pkill -f "[i]nstances/Minefactor" && echo "Minecraft (Prism) stopped"
pkill -f "[f]abric.dli.config" && echo "Minecraft (dev client) stopped"
pkill -f "[G]radleDaemon"
pkill -f "[A]bioticFactor-Win64-Shipping.exe" && echo "Abiotic Factor stopped"
exit 0
