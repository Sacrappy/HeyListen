"""The Windows release as RecompLauncher installs it: HeyListen-<tag>-Windows-x64.zip.

The zip holds the whole HeyListen Builder, ready to run where it is unpacked: HeyListenBuilder.exe and its .NET files, tools\\
with its own Python, the compiler (llvm-mingw), emsdk, CPM and the engine's sources, and recomplauncher.json at the top, next
to the program. RecompLauncher unpacks it, asks for the player's copy of the game, and runs the builder without a window
(recomplauncher.json "prepare": HeyListenBuilder.exe --cli --prepare ...) to make the game in <install>\\HeyListen. Nothing of
the game is in it.

It is made from the files the release already has: HeyListenBuilder.exe's list (heylisten-setup.txt) names the parts the setup
downloads, and the zip is those parts unpacked together, so it is the same builder the setup installs, byte for byte.

    python -I tools/release/make_launcher_zip.py <release folder> <tag> [--out <zip>] [--manifest <recomplauncher.json>]

<release folder> holds heylisten-setup.txt and parts\\ (what make_release.py writes in build\\release\\<tag>). A part that is not
in parts\\ (the ones an earlier release made: Python, emsdk, CPM, wgpu-native) is downloaded from the list's address, size and
SHA-256 checked, into $RUNNER_TEMP (or the system's temporary folder) / release-parts.
"""
import argparse
import importlib.util
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LIST = "heylisten-setup.txt"
PREFIX = "HookshotBuilder/"   # (every part's folder: the setup's install folder)
MANIFEST = "recomplauncher.json"
DATE = (2026, 1, 1, 0, 0, 0)


def asset_name(tag):
    return "HeyListen-%s-Windows-x64.zip" % tag


def parts_module():
    spec = importlib.util.spec_from_file_location("release_parts", os.path.join(ROOT, "engine", "scripts", "release_parts.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def part_files(release, lines, fetch):
    """(sha, size, dest, from, to, zip path) for each part of the list"""
    for w in lines:
        sha, size, dest, frm, to, url = w[1], int(w[2]), w[3], w[4], w[5], w[6]
        local = os.path.join(release, "parts", url.rsplit("/", 1)[-1])
        if os.path.isfile(local) and os.path.getsize(local) == size:
            path = local
        else:
            path = fetch(sha, size, url)
        yield sha, size, dest, frm, to, path


def entries(release, lines, fetch):
    """Every file of the unpacked builder, as (name in the zip, the part zip, the member's name there), in the order the parts are
    listed. A name two parts both hold is an error (the setup would let the later one win: a package must not rely on that)"""
    seen = {}
    for sha, size, dest, frm, to, path in part_files(release, lines, fetch):
        with zipfile.ZipFile(path) as z:
            for m in z.infolist():
                if m.is_dir():
                    continue
                name = m.filename
                if frm != "-" and to != "-":   # (llvm-mingw-<version>-ucrt-x86_64 -> llvm-mingw, under tools\)
                    if not name.startswith(frm + "/"):
                        continue
                    name = to + name[len(frm):]
                    base = dest if dest.startswith(PREFIX) else PREFIX + dest
                    name = base.rstrip("/") + "/" + name
                elif dest not in (".", ""):
                    name = dest.rstrip("/") + "/" + name
                if not name.startswith(PREFIX) or ".." in name.split("/"):
                    raise SystemExit("%s: member %s is outside %s" % (os.path.basename(path), m.filename, PREFIX))
                name = name[len(PREFIX):]
                if name in seen:
                    raise SystemExit("%s is in two parts (%s and %s)" % (name, seen[name], os.path.basename(path)))
                seen[name] = os.path.basename(path)
                yield name, path, m.filename


def make_zip(release, tag, out, manifest=None, fetch=None, forbidden=None):
    """The zip at `out`. forbidden: a function of a name, true for one that must not be in a release (make_release.FORBIDDEN)"""
    manifest = manifest or os.path.join(ROOT, MANIFEST)
    list_path = os.path.join(release, LIST)
    if not os.path.isfile(list_path):
        raise SystemExit("%s has no %s" % (release, LIST))
    fetch = fetch or parts_module().fetch_part
    with open(list_path, encoding="utf-8") as f:
        lines = [l.split() for l in f.read().splitlines() if l.startswith("part ")]
    if not lines:
        raise SystemExit("%s names no part" % list_path)
    with open(manifest, "rb") as f:
        manifest_bytes = f.read()
    names = []
    open_zips = {}
    tmp = out + ".new"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        info = zipfile.ZipInfo(MANIFEST, date_time=DATE)
        info.compress_type, info.external_attr = zipfile.ZIP_DEFLATED, 0o644 << 16
        z.writestr(info, manifest_bytes)
        names.append(MANIFEST)
        for name, path, member in entries(release, lines, fetch):
            if name == MANIFEST:
                raise SystemExit("a part holds its own %s" % MANIFEST)
            if forbidden and forbidden(name):
                raise SystemExit("refusing: %s looks like game data, keys or a cache" % name)
            src = open_zips.get(path) or open_zips.setdefault(path, zipfile.ZipFile(path))
            info = zipfile.ZipInfo(name, date_time=DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o755 << 16 if name.lower().endswith((".exe", ".dll")) else 0o644 << 16
            with src.open(member) as s, z.open(info, "w", force_zip64=True) as d:
                while chunk := s.read(1 << 20):
                    d.write(chunk)
            names.append(name)
    for z in open_zips.values():
        z.close()
    for need in ("HeyListenBuilder.exe", "tools/nx2/make_game.py", "tools/python/python.exe"):
        if need not in names:
            os.remove(tmp)
            raise SystemExit("the zip has no %s: the list's parts do not make a builder that can build the game" % need)
    os.replace(tmp, out)
    print("%s: %d files, %d MB" % (out, len(names), os.path.getsize(out) >> 20))
    return names


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("release")
    p.add_argument("tag")
    p.add_argument("--out")
    p.add_argument("--manifest")
    a = p.parse_args()
    if not re.match(r"^v?\d+\.\d+\.\d+", a.tag):
        print("note: %r does not look like a release tag" % a.tag, file=sys.stderr)
    out = a.out or os.path.join(a.release, asset_name(a.tag))
    make_zip(os.path.abspath(a.release), a.tag, os.path.abspath(out), a.manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
