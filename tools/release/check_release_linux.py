"""Check the Linux files of a HeyListen release before they are published (tools/release/make_release_linux.sh runs it in CI).

    python3 -I tools/release/check_release_linux.py <tag> <dir>

<dir> holds what the Linux workflow built: HeyListen-<tag>-x86_64.AppImage, and parts/ with the update parts
(HeyListenLinux-*.zip) and heylisten-linux.txt. The same refusals as tools/release/make_release.py makes for the Windows
files:

  - the list names this repository's linux-parts release and nothing else, its tag is the release's tag, and each part it
    names is in parts/ with the size and SHA-256 it states (and no part is there that it does not name);
  - nothing in the AppImage or in a part looks like game data, keys or a cache (make_release.forbidden's rules);
  - the AppImage carries the release's version, the play profile and the pipeline state list of the repository's profile/
    (the state list again through check_states.py), and a licence file for Hookshot's credit.

Prints what is wrong and exits 1; exits 0 and writes SHA256SUMS-linux.txt (the AppImage, the list and every part) when all
is well. Nothing is uploaded here.
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "engine", "builder", "linux"))
import make_release  # noqa: E402  (its refusals: FORBIDDEN, ALLOWED, forbidden())
import linux_update  # noqa: E402  (the parser the builder reads the list with)

REPO = make_release.REPO
PARTS_RELEASE = "https://github.com/%s/releases/download/linux-parts/" % REPO
LIST = linux_update.LIST_NAME
SUMS = "SHA256SUMS-linux.txt"
# Linux paths that match make_release.FORBIDDEN and hold no game data: the lifted-C chunk boundaries (guest addresses), as on Windows
ALLOWED = {"usr/share/hookshot/work/image/chunks.txt"}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def forbidden(path):
    return path not in ALLOWED and make_release.forbidden(path)


def appimage_files(image, folder):
    """Every path inside the AppImage, from its own extraction (the runtime, as a player's PC would run it)."""
    os.chmod(image, 0o755)
    subprocess.run([image, "--appimage-extract"], cwd=folder, check=True, stdout=subprocess.DEVNULL,
                   env=dict(os.environ, APPIMAGE_EXTRACT_AND_RUN="1"))
    root = os.path.join(folder, "squashfs-root")
    found = []
    for base, dirs, names in os.walk(root, followlinks=False):
        for name in dirs + names:
            found.append(os.path.relpath(os.path.join(base, name), root).replace(os.sep, "/"))
    return root, sorted(found)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    tag, dist = sys.argv[1], os.path.abspath(sys.argv[2])
    problems = []

    def bad(message):
        problems.append(message)

    if not re.fullmatch(r"v[0-9]+(\.[0-9]+)*(-[0-9A-Za-z.]+)?", tag):
        sys.exit("%s is not a release tag (v0.3.2)" % tag)
    image = os.path.join(dist, "HeyListen-%s-x86_64.AppImage" % tag)
    parts_dir = os.path.join(dist, "parts")
    listing = os.path.join(parts_dir, LIST)
    others = sorted(n for n in os.listdir(dist) if n.endswith(".AppImage") and n != os.path.basename(image))
    if others:
        bad("AppImages that are not this release's (named for another version): %s" % ", ".join(others))
    for need in (image, listing):
        if not os.path.isfile(need):
            bad("missing: %s" % os.path.relpath(need, dist))
    if problems:
        sys.exit("refusing:\n  " + "\n  ".join(problems))

    # 1. the list: this repository's linux-parts release only, this tag, each part present and as stated
    with open(listing, encoding="utf-8") as handle:
        text = handle.read()
    try:
        manifest = linux_update.parse_list(text, local=False)   # (https links only; the builder refuses the rest too)
    except ValueError as error:
        sys.exit("refusing: %s: %s" % (LIST, error))
    if manifest.tag != tag:
        bad("%s is for %s, not %s" % (LIST, manifest.tag, tag))
    named = set()
    for part in manifest.parts:
        if not part.url.startswith(PARTS_RELEASE) or "/" in part.url[len(PARTS_RELEASE):]:
            bad("%s names a part hosted elsewhere: %s" % (LIST, part.url))
            continue
        name = part.url[len(PARTS_RELEASE):]
        named.add(name)
        path = os.path.join(parts_dir, name)
        if not name.startswith("HeyListenLinux-") or not name.endswith("-%s.zip" % part.sha[:12]):
            bad("%s: not a part name with its checksum in it" % name)
        elif not os.path.isfile(path):
            bad("%s: named by the list, not built" % name)
        elif os.path.getsize(path) != part.size or sha256(path) != part.sha:
            bad("%s: size or SHA-256 differs from the list" % name)
    for name in sorted(os.listdir(parts_dir)):
        if name != LIST and name not in named:
            bad("%s: in parts/ but not in the list" % name)
    # (the builder installs the tools from the first part: the tag it reads there is the release's)
    for part in manifest.parts:
        path = os.path.join(parts_dir, part.url[len(PARTS_RELEASE):])
        if not os.path.isfile(path):
            continue
        with zipfile.ZipFile(path) as zipped:
            for entry in zipped.namelist():
                full = part.dest + "/" + entry
                if forbidden(full):
                    bad("%s: %s looks like game data, keys or a cache" % (os.path.basename(path), entry))
            if part.dest == "usr/share/hookshot":
                if "VERSION" not in zipped.namelist() or zipped.read("VERSION").decode("utf-8").strip() != tag:
                    bad("the tools part does not carry VERSION %s" % tag)

    # 2. the AppImage: what is in it, and the profile and the state list
    with tempfile.TemporaryDirectory(prefix="heylisten-linux-release-") as tmp:
        root, files = appimage_files(image, tmp)
        for path in files:
            if forbidden(path):
                bad("AppImage: %s looks like game data, keys or a cache" % path)
        tools = os.path.join(root, "usr", "share", "hookshot")
        version = os.path.join(tools, "VERSION")
        if not os.path.isfile(version) or open(version, encoding="utf-8").read().strip() != tag:
            bad("the AppImage does not carry VERSION %s" % tag)
        for name in ("hookshot_pieces.prof", "hot_funcs.txt", "hot_order.txt", "pipeline_states.bin"):
            shipped, repo = os.path.join(tools, "work", name), os.path.join(ROOT, "profile", name)
            if not os.path.isfile(shipped):
                bad("the AppImage has no play profile file %s" % name)
            elif os.path.isfile(repo) and sha256(shipped) != sha256(repo):
                bad("the AppImage's %s is not the repository's profile/%s" % (name, name))
        shipped = os.path.join(tools, "work", "pipeline_states.bin")
        if os.path.isfile(shipped):
            check = subprocess.run([sys.executable, "-I", os.path.join(HERE, "check_states.py"), shipped],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            if check.returncode:
                bad("the AppImage's pipeline state list is refused by check_states.py: " + check.stdout.strip()[-400:])
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            if not os.path.isfile(os.path.join(tools, name)):
                bad("the AppImage has no %s (the credit to Hookshot and the licences)" % name)
        for name in ("heylisten-linux.txt",):
            beside = os.path.join(root, name)
            if not os.path.isfile(beside) or open(beside, encoding="utf-8").read() != text:
                bad("the AppImage's %s is not the release's list" % name)
        if not os.path.isfile(os.path.join(root, "heylisten-builder.desktop")) or not os.path.isfile(os.path.join(root, "heylisten.png")):
            bad("the AppImage has no heylisten-builder.desktop or heylisten.png")

    if problems:
        sys.exit("refusing:\n  " + "\n  ".join(problems))

    sums = [(sha256(image), os.path.basename(image)), (sha256(listing), LIST)]
    sums += [(part.sha, part.url[len(PARTS_RELEASE):]) for part in manifest.parts]
    with open(os.path.join(dist, SUMS), "w", encoding="utf-8", newline="\n") as handle:
        handle.write("".join("%s  %s\n" % pair for pair in sorted(sums, key=lambda pair: pair[1])))
    print("Linux release %s: %d parts, all links lead to %s; no game data; %s written" % (tag, len(manifest.parts), PARTS_RELEASE, SUMS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
