#!/bin/bash
# The replay thread's cost in Kokiri: a walk-test run at the 120 Hz limit, then the median frame rate and the median "submit" time (the replay's
# whole time a frame) over the gameplay windows of its log (the title and the menus draw far fewer than 1500 draws a frame).
# Usage: scripts/bench-replay.sh [out dir]
OUT=${1:-/e/OOT/hookshot-shots/bench}
WALK="" bash "$(dirname "$0")/walk-test.sh" 120 "$OUT" > /dev/null
python "$(dirname "$0")/bench-replay.py" "$OUT/log.txt"
