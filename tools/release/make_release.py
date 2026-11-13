"""Build a HeyListen release locally: the HeyListen Builder setup and its parts, checksums, release notes and the user guide.
No game data is involved: the builder is built from engine/ and the tools come from their official releases (as upstream
Hookshot's .github/workflows/windows-builder.yml does it, which this follows step by step).

    python -I tools/release/make_release.py --tag v0.1.0 [--out build/release] [--allow-dirty]
    (or powershell -File tools/release/make_release.ps1 -Tag v0.1.0)

Needs: git, Python 3.10+, the .NET 8 SDK (`dotnet --list-sdks`), and internet (GitHub: the previous release's setup list,
llvm-mingw, the reused parts). Nothing is installed system-wide; downloads are cached in <out>/cache.

Writes <out>/<tag>/:
    upload to the tag's release:     HeyListenBuilder.exe (the setup), heylisten-setup.txt, SHA256SUMS.txt,
                                     RELEASE_NOTES.md (the release body), USER_GUIDE.md
    upload to the builder-parts release (once each; named by checksum): parts/HeyListenBuilder-<part>-<sha>.zip
The parts that did not change (Python, emsdk, CPM, wgpu-native, maybe third_party) are not written: heylisten-setup.txt points
at this repository's builder-parts release for them (it mirrors the ones first made by upstream Hookshot, whose repository
is gone), and at llvm-mingw's official release. No link of the list leads anywhere else: the build fails otherwise.
Nothing is uploaded or tagged here: the maintainer does that (see the printed commands).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ENGINE = os.path.join(ROOT, "engine")
REPO = "Sacrappy/HeyListen"
LIST = "heylisten-setup.txt"

# Never in a release asset: the game, keys, what is made from the game, caches.
FORBIDDEN = re.compile(r"(\.(nup|nsp|xci|nsz|xcz|nca|nso|keys|spv)$|(^|/)(romfs|exefs|objcache|generated|gen|image|user)/|"
                       r"(^|/)(image\.bin|main|prod\.keys|title\.keys)$|_(pipelines|shaders|programs|prepared)\.bin$)", re.I)
# Exact paths that match FORBIDDEN but hold no game data: upstream's public builder profile ships the lifted-C chunk
# boundaries (57 guest addresses, nx2/chunks.py) under tools/work/image/
ALLOWED = {"HookshotBuilder/tools/work/image/chunks.txt"}


def forbidden(path):
    return path.replace("\\", "/") not in ALLOWED and bool(FORBIDDEN.search(path))


def run(cmd, **kw):
    print("> " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, **kw)


def out_of(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True, **kw).stdout.strip()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def need_dotnet():
    exe = shutil.which("dotnet")
    sdks = ""
    if exe:
        try:
            sdks = out_of([exe, "--list-sdks"])
        except subprocess.CalledProcessError:
            sdks = ""
    if not re.search(r"^(8|9|1\d)\.", sdks, re.M):
        sys.exit("The .NET SDK (8.0 or newer) is needed to build HeyListenBuilder.exe and was not found"
                 + (" (dotnet is there, but with no SDK)" if exe else "") + ".\n"
                 "Install it from https://dotnet.microsoft.com/download/dotnet/8.0 (SDK x64), or without admin rights:\n"
                 "  powershell -c \"& ([scriptblock]::Create((iwr https://dot.net/v1/dotnet-install.ps1))) -Channel 8.0 "
                 "-InstallDir $env:LOCALAPPDATA\\dotnet8\"\n"
                 "then put that folder first in PATH (or pass --dotnet <path to dotnet.exe>).")
    return exe


def check_assets(folder):
    """Fails when anything that looks like game data, keys or a cache is in a release asset"""
    bad = []
    for d, _, fs in os.walk(folder):
        for f in fs:
            p = os.path.join(d, f)
            rel = os.path.relpath(p, folder).replace(os.sep, "/")
            if forbidden(rel):
                bad.append(rel)
            if f.lower().endswith(".zip"):
                with zipfile.ZipFile(p) as z:
                    bad += ["%s: %s" % (rel, n) for n in z.namelist() if forbidden(n)]
    if bad:
        sys.exit("refusing: these look like game data, keys or caches:\n  " + "\n  ".join(bad[:50]))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--tag", help="the release's tag, e.g. v0.1.0 (required unless --allow-dirty)")
    p.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    p.add_argument("--dotnet", help="dotnet.exe to use (default: the one on PATH)")
    p.add_argument("--allow-dirty", action="store_true", help="build with uncommitted changes (a test, not a release)")
    a = p.parse_args()
    if os.name != "nt":
        sys.exit("make_release.py builds the Windows builder: run it on Windows")

    dotnet = a.dotnet or need_dotnet()
    if not a.allow_dirty and out_of(["git", "status", "--porcelain", "--", "engine", "docs", "tools/release"], cwd=ROOT):
        sys.exit("uncommitted changes in engine/, docs/ or tools/release/: commit them (the package holds the tracked files) "
                 "or pass --allow-dirty")
    if a.tag:
        tag = a.tag
    elif a.allow_dirty:
        tag = "v0.0.0-test-" + out_of(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT)
    else:
        sys.exit("pass --tag vX.Y.Z (the release's tag; the builder compares it with the latest release's tag_name)")
    version = tag[1:] if tag.startswith("v") else tag
    out = os.path.abspath(a.out)
    work = os.path.join(out, "work-" + tag)
    dist = os.path.join(out, tag)
    cache = os.path.join(out, "cache")
    for d in (work, dist):
        if os.path.exists(d):
            shutil.rmtree(d)
    os.makedirs(work)
    os.makedirs(os.path.join(dist, "parts"))
    os.makedirs(cache, exist_ok=True)
    env = dict(os.environ, RUNNER_TEMP=cache)   # (release_parts.py's download cache)
    py = [sys.executable, "-I"]
    parts_py = os.path.join(ENGINE, "scripts", "release_parts.py")
    print("HeyListen release %s -> %s" % (tag, dist), flush=True)

    # 1. the previous release's setup list: the Python, emsdk, CPM, wgpu-native and
    #    third_party parts and the play profile come from it
    previous = os.path.join(work, "previous-setup.txt")
    run(py + [parts_py, "previous", previous, "--repo", REPO, "--skip-tag", tag], env=env)

    # 2. llvm-mingw (official release, SHA-256 checked) and the setup exe
    llvm = out_of(py + [parts_py, "llvm-mingw", os.path.join(cache, "llvm")], env=env).splitlines()[-1]
    setup_dir = os.path.join(ENGINE, "builder", "setup")
    rc_o = os.path.join(work, "setup_rc.o")
    setup = os.path.join(work, "HookshotBuilder-Setup.exe")
    run([os.path.join(llvm, "bin", "x86_64-w64-mingw32-windres.exe"), "setup.rc", "-O", "coff", "-o", rc_o], cwd=setup_dir)
    run([os.path.join(llvm, "bin", "x86_64-w64-mingw32-clang.exe"), "-O2", "-mwindows", "-municode", "setup.c", rc_o,
         "-lwinhttp", "-lbcrypt", "-lcomctl32", "-lshell32", "-lole32", "-o", setup], cwd=setup_dir)

    # 3. the builder (self-contained .NET 8 folder)
    publish = os.path.join(work, "publish")
    run([dotnet, "publish", os.path.join(ENGINE, "builder", "HookshotBuilder", "HookshotBuilder.csproj"), "--configuration",
         "Release", "-p:Version=" + re.sub(r"[^0-9A-Za-z.\-]", "", version), "-p:NuGetAudit=false", "--output", publish])
    if not os.path.isfile(os.path.join(publish, "HeyListenBuilder.exe")):
        sys.exit("dotnet publish made no HeyListenBuilder.exe")

    # 4. the package folder (builder, setup, LICENSE, tracked engine sources under tools/, profile, prebuilt third_party)
    package = os.path.join(work, "package")
    #    (TICK-045: the play profile from the repository's profile/, recorded on this lifter's default lift, not the previous release's)
    run(py + [parts_py, "package", package, "--publish", publish, "--setup", setup, "--list", previous,
              "--profile", os.path.join(ROOT, "profile")], env=env)
    for f in ("THIRD_PARTY_NOTICES.md",):
        shutil.copyfile(os.path.join(ROOT, f), os.path.join(package, f))

    # 5. the parts and heylisten-setup.txt
    staging = os.path.join(work, "parts")
    run(py + [os.path.join(ENGINE, "scripts", "make_setup_parts.py"), package, tag, staging,
              "--base", "https://github.com/%s/releases/download/builder-parts" % REPO, "--reuse", previous])
    for f in os.listdir(staging):
        shutil.copyfile(os.path.join(staging, f), os.path.join(dist, "parts" if f.endswith(".zip") else "", f))
    shutil.copyfile(setup, os.path.join(dist, "HeyListenBuilder.exe"))
    #    (every part from this repository's builder-parts release or llvm-mingw's own: an install must not depend on another
    #    repository staying online, as v0.3.0's did on upstream's)
    ours = "https://github.com/%s/releases/download/builder-parts/" % REPO
    with open(os.path.join(dist, LIST), encoding="utf-8") as f:
        links = [l.split()[6] for l in f.read().splitlines() if l.startswith("part ")]
    foreign = [u for u in links if not u.startswith((ours, "https://github.com/mstorsjo/llvm-mingw/releases/download/"))]
    if foreign:
        sys.exit("refusing: %s names parts hosted elsewhere:
  %s" % (LIST, "
  ".join(foreign)))

    # 6. notes, guide, checksums
    with open(os.path.join(ROOT, "docs", "release", "RELEASE_NOTES.md"), encoding="utf-8") as f:
        notes = f.read().replace("{TAG}", tag)
    with open(os.path.join(dist, "RELEASE_NOTES.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(notes)
    shutil.copyfile(os.path.join(ROOT, "docs", "USER_GUIDE.md"), os.path.join(dist, "USER_GUIDE.md"))
    check_assets(dist)
    sums = []
    for d, _, fs in os.walk(dist):
        for f in sorted(fs):
            rel = os.path.relpath(os.path.join(d, f), dist).replace(os.sep, "/")
            sums.append("%s  %s" % (sha256(os.path.join(d, f)), rel))
    with open(os.path.join(dist, "SHA256SUMS.txt"), "w", newline="\n") as f:
        f.write("\n".join(sorted(sums, key=lambda l: l.split("  ", 1)[1])) + "\n")

    print("\nRelease assets in %s:" % dist)
    for d, _, fs in os.walk(dist):
        for f in sorted(fs):
            q = os.path.join(d, f)
            print("  %-60s %10d" % (os.path.relpath(q, dist), os.path.getsize(q)))
    print("""
Publish (the maintainer, after review):
  gh release view builder-parts -R {r} || gh release create builder-parts -R {r} --prerelease --latest=false --title builder-parts --notes "Parts HeyListenBuilder.exe downloads"
  gh release upload builder-parts -R {r} "{d}/parts/"*.zip      (parts already there have the same name: skip them)
  git tag {t} && git push origin {t}
  gh release create {t} -R {r} --title "HeyListen {t}" --notes-file "{d}/RELEASE_NOTES.md" "{d}/HeyListenBuilder.exe" "{d}/{l}" "{d}/SHA256SUMS.txt" "{d}/USER_GUIDE.md"
""".format(r=REPO, d=dist, t=tag, l=LIST))
    return 0


if __name__ == "__main__":
    sys.exit(main())
