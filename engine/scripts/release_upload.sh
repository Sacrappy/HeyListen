#!/bin/bash
# A tag's GitHub release with these files attached. The Linux AppImage and Windows builder workflows both call it. For
# a release from the Release workflow (RELEASE_FROM_WORKFLOW=true: it passed the tag), the release is there already, a
# draft made by release-please or Promote, and the files are only added; when it is not there, this stops and makes
# nothing. For a tag pushed by hand, the first to get there makes the release (from the tag, with GitHub's generated
# notes) and the other adds its files to it. A version with a prerelease part (v0.2.0-rc.1) is a prerelease and never
# "latest". The release is found by listing every release, drafts too, and the files go to it by its id
# (scripts/release_files.sh find and upload): GitHub's get-a-release-by-tag finds no draft, and a lookup that failed
# once made a published, "latest" v2.1.0 beside its draft while the draft's files were still being built.
# A file already on the release is replaced, so a run again on the same tag updates it. A release that is not a draft
# then gets its SHA256SUMS and the table of its files (scripts/release_files.sh finish), SHA256SUMS also written to
# $RELEASE_SUMS_OUT when that is set. Needs gh and jq, with GH_TOKEN (contents: write) and GH_REPO set. No game data
# is involved: a file whose name looks like the game's or a dump's, or like NVIDIA's runtime files, is refused.
#
# nightly is the latest development build: a prerelease, never "latest", whose tag is moved to the commit of each push
# to main (GITHUB_SHA). A run whose commit is no longer main's newest leaves it to the newer run. Its files are replaced,
# an AppImage and .zsync named by an older commit are removed once the new ones are there, and SHA256SUMS and the
# table are made again (the table is in its notes). rc, the latest release candidate, is refused: it gets its files from
# a published release (scripts/release_rc.sh).
#
#   scripts/release_upload.sh <tag> <file> ...
set -euo pipefail
shopt -s inherit_errexit
files=$(dirname "$0")/release_files.sh
tag=$1
shift
for f in "$@"; do
    name=$(basename "$f")
    shopt -s nocasematch
    case $name in
        *.nso|*.nca|*.nsp|*.xci|*.nsz|*.xcz|*.nup|*.keys|*.bin|*romfs*|*exefs*|*nvngx*|*dlss*|*streamline*|sl.*.dll)
            echo "release_upload.sh: $name is not a release file (game data or NVIDIA files never go on a release)" >&2
            exit 1 ;;
    esac
    shopt -u nocasematch
done
if [[ $tag == rc ]]; then
    echo "release_upload.sh: rc holds a release's own files and is made by scripts/release_rc.sh, not uploaded to" >&2
    exit 1
fi
if [[ $tag == nightly ]]; then
    sha=${GITHUB_SHA:?}
    head=$(gh api "repos/$GH_REPO/commits/main" --jq .sha)
    if [[ $head != "$sha" ]]; then
        echo "nightly: $sha is no longer main's newest commit ($head); its own run updates the nightly"
        exit 0
    fi
    if gh api "repos/$GH_REPO/git/ref/tags/nightly" >/dev/null 2>&1; then
        gh api -X PATCH "repos/$GH_REPO/git/refs/tags/nightly" -f sha="$sha" -F force=true >/dev/null
    else
        gh api -X POST "repos/$GH_REPO/git/refs" -f ref=refs/tags/nightly -f sha="$sha" >/dev/null
    fi
    date=$(gh api "repos/$GH_REPO/commits/$sha" --jq .commit.committer.date)
    notes="The latest development build: main at $sha ($date), made by CI on every push to main.

It is untested and may break your saves or settings. For playing, use the [latest release](https://github.com/$GH_REPO/releases/latest).

$("$files" table nightly)"
    title="Latest development build"
    if gh release view nightly >/dev/null 2>&1; then
        gh release edit nightly --prerelease --latest=false --title "$title" --notes "$notes" >/dev/null
    else
        gh release create nightly --verify-tag --prerelease --latest=false --title "$title" --notes "$notes" ||
            gh release edit nightly --prerelease --latest=false --title "$title" --notes "$notes" >/dev/null
    fi
    "$files" upload nightly "$@"
    new=()
    for f in "$@"; do new+=("$(basename "$f")"); done
    # (the AppImage and its .zsync, or the RecompLauncher zip, of an older commit, once this run has uploaded the new ones)
    gh release view nightly --json assets --jq '.assets[].name' | while read -r old; do
        case $old in
            Hookshot-*-x86_64.AppImage | Hookshot-*-x86_64.AppImage.zsync) want="Hookshot-.*-x86_64\\.AppImage${old##*.AppImage}" ;;
            Hookshot-*-Windows-x64.zip) want='Hookshot-.*-Windows-x64\.zip' ;;
            *) continue ;;
        esac
        printf '%s\n' "${new[@]}" | grep -qxF "$old" && continue
        printf '%s\n' "${new[@]}" | grep -qx "$want" || continue
        gh release delete-asset nightly "$old" --yes
    done
    "$files" finish nightly "${RELEASE_SUMS_OUT:-}"
    exit 0
fi
if [[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+- ]]; then
    kind=(--prerelease --latest=false)
else
    kind=(--latest)
fi
info=$("$files" find "$tag")
if [[ -z $info ]]; then
    if [[ ${RELEASE_FROM_WORKFLOW:-false} == true ]]; then
        echo "::error::$tag has no release. The Release workflow makes its draft before it builds the files: start it again (Promote, or re-run its failed jobs) rather than this job" >&2
        exit 1
    fi
    # (the other workflow can make it between the find and the create: its release is then the one used)
    gh release create "$tag" --verify-tag --title "$tag" --generate-notes "${kind[@]}" >/dev/null || true
    info=$("$files" find "$tag")
    [[ -n $info ]] || { echo "::error::$tag: its release could not be made" >&2; exit 1; }
fi
"$files" upload "$tag" "$@"
# (a draft is the Release workflow's: its publish job does this once both builds' files are there)
info=$("$files" find "$tag")
read -r _ draft _ <<<"$info"
if [[ $draft != true ]]; then
    "$files" finish "$tag" "${RELEASE_SUMS_OUT:-}"
fi
