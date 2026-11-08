#!/bin/bash
# Walks Link in Kokiri for a fixed time at a frame rate limit and saves the game's own screenshots every 2.5 s, to check that the
# game's speed does not follow the frame rate. Usage: scripts/walk-test.sh <limit Hz> <out dir> [game folder]
# The game starts from its title (a scripted A reaches the save and loads it); the walk is 5 s of the left stick up, at 75 s.
# WALK="" (set, empty): no walk, Link stands at the spawn point, a steady scene to compare builds with.
# One game at a time, and never longer than 2 minutes.
LIMIT=${1:-60}
OUT=${2:-/e/OOT/hookshot-shots/walk$LIMIT}
GAME=${3:-/e/OOT/hookshot-fork-game}
mkdir -p "$OUT"
cd "$GAME" || exit 1
rm -f "$OUT"/vk_*.png
EVERY=$((150 * LIMIT / 60))   # (a shot every 2.5 s)
RX_VK_SHOTS="$(cygpath -w "$OUT")" RX_VK_SHOT_EVERY=${RX_VK_SHOT_EVERY:-$EVERY} HOS_FPS_LIMIT=$LIMIT HOS_AUDIO=0 HOS_WINDOW_NOACTIVATE=1   HOS_INPUT_SCRIPT="30:A,34:A,38:A,42:A,46:A,50:A,54:A,58:A,62:A,66:A,70:A${WALK-,76:LUP/6000,83:LRIGHT/4000,89:LDOWN/5000}" ./hookshot.exe > "$OUT/log.txt" 2>&1 &
sleep 95
taskkill //F //IM hookshot.exe > /dev/null 2>&1
echo finished
