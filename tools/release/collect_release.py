# Copyright (C) 2026 Sacrappy

"""The files of a HeyListen release, gathered from what CI made (.github/workflows/release.yml), checked, and checksummed.

    python3 -I tools/release/collect_release.py <tag> <windows dir> <linux dir> <out dir>

<windows dir>: what tools/release/make_release.py wrote (HeyListenBuilder.exe, heylisten-setup.txt, RELEASE_NOTES.md,
USER_GUIDE.md, HeyListen-<tag>-Windows-x64.zip, parts/). <linux dir>: the AppImage and
parts/ (heylisten-linux.txt, HeyListenLinux-*.zip), already accepted by tools/release/make_release_linux.sh. Writes:

    <out>/assets/          the tag's release: HeyListenBuilder.exe, heylisten-setup.txt, USER_GUIDE.md, HeyListen-<tag>-Windows-x64.zip,
                           HeyListen-<tag>-Linux-x64.AppImage, heylisten-linux.txt, SHA256SUMS.txt
    <out>/RELEASE_NOTES.md the release's body
    <out>/builder-parts/   the new Windows parts (the builder-parts release; a name already there is the same file)
    <out>/linux-parts/     the new Linux parts (the linux-parts release)

SHA256SUMS.txt lists every asset and every new part (builder-parts/<name>, linux-parts/<name>), computed here.
It refuses (exit 1) what make_release.py refuses (anything that looks like game data, keys or a cache, in a file or inside a
zip), a RecompLauncher zip whose recomplauncher.json is not the repository's, a heylisten-setup.txt that is not this tag's or whose new parts are not as it states, and a missing file. Nothing is
uploaded here.
"""
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_release  # noqa: E402  (its refusals: forbidden(), check_assets(); its checksum)

WINDOWS = ("HeyListenBuilder.exe", make_release.LIST, "USER_GUIDE.md", "RELEASE_NOTES.md")
MANIFEST = "recomplauncher.json"
BUILDER_PARTS = "https://github.com/%s/releases/download/builder-parts/" % make_release.REPO


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    tag, win, lin, out = sys.argv[1], *(os.path.abspath(x) for x in sys.argv[2:])
    problems = []
    image = "HeyListen-%s-Linux-x64.AppImage" % tag
    launcher = make_release.load_launcher_zip().asset_name(tag)
    need = [os.path.join(win, n) for n in WINDOWS + (launcher,)] + [os.path.join(lin, image), os.path.join(lin, "parts", "heylisten-linux.txt")]
    problems += ["missing: %s" % p for p in need if not os.path.isfile(p)]
    if problems:
        sys.exit("refusing:\n  " + "\n  ".join(problems))

    # the Windows list: this tag's, and each part it names that was made now, as it states (the reused ones are already online)
    with open(os.path.join(win, make_release.LIST), encoding="utf-8") as f:
        lines = [l.split() for l in f.read().splitlines() if l.strip()]
    if not lines or lines[0] != ["tag", tag]:
        problems.append("%s is not for %s" % (make_release.LIST, tag))
    win_parts = os.path.join(win, "parts")
    made = set(os.listdir(win_parts)) if os.path.isdir(win_parts) else set()
    named = set()
    for w in lines:
        if w[0] != "part" or not w[6].startswith(BUILDER_PARTS):
            continue
        name = w[6][len(BUILDER_PARTS):]
        named.add(name)
        if name in made:
            p = os.path.join(win_parts, name)
            if os.path.getsize(p) != int(w[2]) or make_release.sha256(p) != w[1]:
                problems.append("%s: size or SHA-256 differs from %s" % (name, make_release.LIST))
    problems += ["%s: in parts/ but not in %s" % (n, make_release.LIST) for n in sorted(made - named)]

    # the RecompLauncher zip carries the repository's recomplauncher.json at its top (as the AppImage does)
    if os.path.isfile(os.path.join(win, launcher)):
        with zipfile.ZipFile(os.path.join(win, launcher)) as z, open(os.path.join(make_release.ROOT, MANIFEST), "rb") as f:
            if MANIFEST not in z.namelist():
                problems.append("%s has no %s at its top" % (launcher, MANIFEST))
            elif z.read(MANIFEST) != f.read():
                problems.append("%s: its %s is not the repository's" % (launcher, MANIFEST))

    # no game data, keys or caches anywhere (the AppImage's own content: check_release_linux.py)
    for folder in (win, os.path.join(lin, "parts")):
        try:
            make_release.check_assets(folder)
        except SystemExit as e:
            problems.append(str(e))
    if problems:
        sys.exit("refusing:\n  " + "\n  ".join(problems))

    if os.path.exists(out):
        shutil.rmtree(out)
    assets = os.path.join(out, "assets")
    for d in (assets, os.path.join(out, "builder-parts"), os.path.join(out, "linux-parts")):
        os.makedirs(d)
    for n in ("HeyListenBuilder.exe", make_release.LIST, "USER_GUIDE.md", launcher):
        shutil.copyfile(os.path.join(win, n), os.path.join(assets, n))
    shutil.copyfile(os.path.join(win, "RELEASE_NOTES.md"), os.path.join(out, "RELEASE_NOTES.md"))
    shutil.copyfile(os.path.join(lin, image), os.path.join(assets, image))
    shutil.copyfile(os.path.join(lin, "parts", "heylisten-linux.txt"), os.path.join(assets, "heylisten-linux.txt"))
    for n in sorted(made):
        shutil.copyfile(os.path.join(win_parts, n), os.path.join(out, "builder-parts", n))
    for n in sorted(os.listdir(os.path.join(lin, "parts"))):
        if n.endswith(".zip"):
            shutil.copyfile(os.path.join(lin, "parts", n), os.path.join(out, "linux-parts", n))

    sums = []
    for sub in ("assets", "builder-parts", "linux-parts"):
        for n in sorted(os.listdir(os.path.join(out, sub))):
            sums.append((make_release.sha256(os.path.join(out, sub, n)), n if sub == "assets" else sub + "/" + n))
    with open(os.path.join(assets, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("".join("%s  %s\n" % s for s in sorted(sums, key=lambda s: s[1])))
    print("HeyListen release %s in %s:" % (tag, out))
    for sha, n in sorted(sums, key=lambda s: s[1]):
        print("  %s  %s" % (sha[:16], n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
