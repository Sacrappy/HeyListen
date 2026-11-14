#!/bin/bash
# The Linux files of a HeyListen release, checked. They are not made here (and never on a developer's PC): the Linux
# workflow (.github/workflows/linux.yml) builds the AppImage in an Ubuntu 22.04 container with scripts/linux/appimage.sh,
# which also writes the update parts and heylisten-linux.txt; this script is the workflow's gate before anything is
# published, and can be run by hand on the workflow's downloaded artifacts to look at them first.
#
#   tools/release/make_release_linux.sh <tag> <dir>
#
# <dir>: HeyListen-<tag>-x86_64.AppImage and parts/ (HeyListenLinux-<part>-<sha>.zip, heylisten-linux.txt), as the
# workflow's two artifacts hold them (download both: `gh run download <run> -n HeyListen-linux-x86_64 -D <dir>` and
# `-n HeyListen-linux-parts -D <dir>/parts`). It refuses (tools/release/check_release_linux.py) a list with a link that
# leads anywhere but https://github.com/Sacrappy/HeyListen/releases/download/linux-parts/, a part that is not as the list
# says, anything that looks like game data, keys or a cache, and an AppImage that is not this tag's or lacks the play
# profile, the pipeline state list (tools/release/check_states.py) or the licences. When all is well it writes
# <dir>/SHA256SUMS-linux.txt and prints how the files are published (the workflow does this on a published release):
#
#   engine/scripts/linux/publish_parts.sh <dir>/parts            (the parts to the linux-parts release; GH_TOKEN, GITHUB_REPOSITORY, GITHUB_SHA)
#   gh release upload <tag> <dir>/HeyListen-<tag>-x86_64.AppImage <dir>/parts/heylisten-linux.txt <dir>/SHA256SUMS-linux.txt
set -euo pipefail
[ $# -eq 2 ] || { sed -n '2,/^set -e/p' "$0" | sed '$d;s/^# \{0,1\}//'; exit 2; }
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
tag=$1
dist=$(cd "$2" && pwd)
python3 -I "$ROOT/tools/release/check_release_linux.py" "$tag" "$dist"
cat <<EOF

Published by the Linux workflow (nothing is uploaded here):
  engine/scripts/linux/publish_parts.sh $dist/parts
  gh release upload $tag $dist/HeyListen-$tag-x86_64.AppImage $dist/parts/heylisten-linux.txt $dist/SHA256SUMS-linux.txt
EOF
