#!/bin/bash
# Upload the Linux builder's parts (scripts/linux/make_parts.py) to the linux-parts release, which is not "latest".
# A zip whose name is already there is left (its checksum is in the name). hookshot-linux.txt itself goes on the
# version release, with the AppImage. Run from CI on a tag, with GH_TOKEN and GITHUB_REPOSITORY.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PARTS=${1:-$ROOT/build-linux/parts}
REPO=${GITHUB_REPOSITORY:?}
TOKEN=${GH_TOKEN:?}
API="https://api.github.com/repos/$REPO"
AUTH=(-H "Authorization: Bearer $TOKEN" -H "Accept: application/vnd.github+json" -H "User-Agent: Hookshot")
J=$(mktemp)   # (the release's JSON: a private file, not a fixed name in /tmp)
trap 'rm -f "$J"' EXIT

code=$(curl -s -o "$J" -w '%{http_code}' "${AUTH[@]}" "$API/releases/tags/linux-parts")
if [ "$code" = "404" ]; then
    curl -fsSL "${AUTH[@]}" -X POST "$API/releases" -d "$(python3 - <<'PY'
import json, os
print(json.dumps({
    "tag_name": "linux-parts",
    "target_commitish": os.environ["GITHUB_SHA"],
    "name": "Linux builder parts",
    "body": "Parts Hookshot's Linux builder downloads. A file's name is its checksum; a part that did not change is not uploaded again. This release is not a version of the game.",
    "prerelease": True,
    "make_latest": "false",
}))
PY
)" > "$J"
fi
python3 - "$J" <<'PY'
import json, sys
json.load(open(sys.argv[1]))["upload_url"]
PY
upload=$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["upload_url"].split("{")[0])' "$J")
have=$(python3 -c 'import json, sys; print("\n".join(a["name"] for a in json.load(open(sys.argv[1])).get("assets", [])))' "$J")
for zip in "$PARTS"/HookshotLinux-*.zip; do
    [ -f "$zip" ] || continue
    name=$(basename "$zip")
    if printf '%s\n' "$have" | grep -qx "$name"; then
        echo "already on linux-parts: $name"
        continue
    fi
    echo "uploading $name"
    # (a release and the nightly of the same commit can upload the same part at once: then one of them wins)
    if ! curl -fsSL --retry 5 --retry-delay 10 -X POST "${AUTH[@]}" -H "Content-Type: application/zip" \
        --data-binary @"$zip" "$upload?name=$name" >/dev/null; then
        curl -fsSL "${AUTH[@]}" "$API/releases/tags/linux-parts" |
            python3 -c 'import json, sys; sys.exit(sys.argv[1] not in [a["name"] for a in json.load(sys.stdin)["assets"]])' "$name"
        echo "already on linux-parts: $name (uploaded by another run)"
    fi
done
echo "parts published"
