# Copyright (C) 2026 Sacrappy

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
                                     HeyListen-<tag>-Windows-x64.zip (the whole builder with recomplauncher.json at its top: what
                                     RecompLauncher installs; tools/release/make_launcher_zip.py)
Nothing is uploaded or tagged here: the maintainer does that (see the printed commands). The notes (docs/release/RELEASE_NOTES.md)
get {TAG} and {SINCE} filled in: the tag, and the previous full release's tag (or --since). On GitHub's runners the same script
runs in .github/workflows/release.yml, which makes the draft release (docs/release.md).
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
ALLOWED = {"HookshotBuilder/tools/work/image/chunks.txt", "tools/work/image/chunks.txt"}   # (the second: in the RecompLauncher zip, which has no folder on top)


# (the CPM package of Vulkan Memory Allocator ships two sample shaders, compiled to SPIR-V: the RecompLauncher zip holds the cpm part)
ALLOWED_RE = re.compile(r"^tools/cpm/vulkanmemoryallocator/[^/]+/bin/Shader\.(frag|vert)\.spv$")


def forbidden(path):
    path = path.replace("\\", "/")
    return path not in ALLOWED and not ALLOWED_RE.match(path) and bool(FORBIDDEN.search(path))


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


def prompt_pins():
    """nx2/prompt_requirements.txt's pins as (name, version)"""
    with open(os.path.join(ENGINE, "nx2", "prompt_requirements.txt"), encoding="utf-8") as f:
        lines = [l.split("#")[0].strip() for l in f.read().splitlines()]
    return [tuple(l.split("==")) for l in lines if l]


CACHE = os.path.join(ROOT, "build", "release", "cache")   # (main() sets it to <out>/cache, where release_parts.py caches the parts)


def python_part_dist_infos(list_path):
    """The dist-info folder names inside the parts list's Python part, read from the parts cache (release_parts.py downloads it
    there). Empty when the list names no Python part or the file is not cached."""
    import zipfile
    if not list_path or not os.path.isfile(list_path):
        return set()
    with open(list_path, encoding="utf-8") as f:
        urls = [l.split()[-1] for l in f.read().splitlines() if l.startswith("part ") and "-python-" in l]
    for url in urls:
        cached = os.path.join(CACHE, "release-parts", url.rsplit("/", 1)[-1])
        if os.path.isfile(cached):
            with zipfile.ZipFile(cached) as z:
                return {m.split("/")[-2].lower() for m in z.namelist() if m.count("/") >= 1 and m.split("/")[-2].endswith(".dist-info")}
    return set()


def missing_prompt_packages(package, list_path=None):
    """The pins of nx2/prompt_requirements.txt that the package folder's Python (tools/python) does not have installed. When the
    package has no tools/python, release_parts.py found the list's Python part already carrying every pin and ships that part as
    it is: the part's own dist-infos are checked instead."""
    site = os.path.join(package, "tools", "python", "Lib", "site-packages")
    have = {n.lower() for n in os.listdir(site)} if os.path.isdir(site) else python_part_dist_infos(list_path)
    return ["%s==%s" % p for p in prompt_pins() if "%s-%s.dist-info" % (p[0].lower().replace("-", "_"), p[1]) not in have]


def check_prompt_packages(package, list_path=None):
    """Stops a release whose builder could not draw the button prompts: nx2/make_game.py skips them without fontTools (and the
    game then shows the Switch's icons whatever the Button prompts row says: issue #69)"""
    missing = missing_prompt_packages(package, list_path)
    if missing:
        sys.exit("the package's Python lacks %s: the game would keep the Switch's button icons (nx2/prompt_requirements.txt)"
                 % ", ".join(missing))


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


def load_launcher_zip():
    import importlib.util
    spec = importlib.util.spec_from_file_location("make_launcher_zip", os.path.join(ROOT, "tools", "release", "make_launcher_zip.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--tag", help="the release's tag, e.g. v0.1.0 (required unless --allow-dirty)")
    p.add_argument("--out", default=os.path.join(ROOT, "build", "release"))
    p.add_argument("--dotnet", help="dotnet.exe to use (default: the one on PATH)")
    p.add_argument("--since", help="the version the notes say the release is new since (default: the previous full release, "
                                   "the one heylisten-setup.txt's parts are reused from)")
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
    global CACHE
    CACHE = cache
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
    since = out_of(py + [parts_py, "previous", previous, "--repo", REPO, "--skip-tag", tag], env=env).splitlines()[-1]
    since = a.since or since

    # 2. llvm-mingw (official release, SHA-256 checked) and the setup exe
    llvm = out_of(py + [parts_py, "llvm-mingw", os.path.join(cache, "llvm")], env=env).splitlines()[-1]
    setup_dir = os.path.join(ENGINE, "builder", "setup")
    rc_o = os.path.join(work, "setup_rc.o")
    setup = os.path.join(work, "HeyListenBuilder-Setup.exe")
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
    #    (the pipeline state list in profile/ is shipped only when it holds nothing but hashes, formats and draw state:
    #    tools/release/check_states.py; HL_STATES_TITLE_IDS=<hex>[,<hex>] adds the search for those title ids)
    states = os.path.join(ROOT, "profile", "pipeline_states.bin")
    if os.path.isfile(states):
        ids = [t for t in os.environ.get("HL_STATES_TITLE_IDS", "").split(",") if t]
        run(py + [os.path.join(ROOT, "tools", "release", "check_states.py"), states] + ["--title-id=" + t for t in ids])
    package = os.path.join(work, "package")
    #    (TICK-045: the play profile from the repository's profile/, recorded on this lifter's default lift, not the previous release's)
    #    (--python-packages: the button prompts are drawn on the player's PC by nx2/prompt_assets.py, which needs fontTools, Pillow and
    #    the ASTC tools in the builder's Python; the previous release's Python part has none, and the game then keeps the Switch's icons)
    run(py + [parts_py, "package", package, "--publish", publish, "--setup", setup, "--list", previous,
              "--profile", os.path.join(ROOT, "profile"),
              "--python-packages", os.path.join(ENGINE, "nx2", "prompt_requirements.txt")], env=env)
    check_prompt_packages(package, previous)
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
        sys.exit("refusing: %s names parts hosted elsewhere: %s" % (LIST, ", ".join(foreign)))

    # 6. notes, guide, checksums
    with open(os.path.join(ROOT, "docs", "release", "RELEASE_NOTES.md"), encoding="utf-8") as f:
        notes = f.read().replace("{TAG}", tag).replace("{SINCE}", since)
    with open(os.path.join(dist, "RELEASE_NOTES.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(notes)
    shutil.copyfile(os.path.join(ROOT, "docs", "USER_GUIDE.md"), os.path.join(dist, "USER_GUIDE.md"))
    #    RecompLauncher: the whole builder in one zip, recomplauncher.json at its top (tools/release/make_launcher_zip.py)
    load_launcher_zip().make_zip(dist, tag, os.path.join(dist, load_launcher_zip().asset_name(tag)), forbidden=forbidden)
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
  gh release create {t} -R {r} --title "HeyListen {t}" --notes-file "{d}/RELEASE_NOTES.md" "{d}/HeyListenBuilder.exe" "{d}/{l}" "{d}/SHA256SUMS.txt" "{d}/USER_GUIDE.md" "{d}/{z}"
(the zip is what RecompLauncher installs: recomplauncher.json at its top; docs/recomplauncher.md)
""".format(r=REPO, d=dist, t=tag, l=LIST, z=load_launcher_zip().asset_name(tag)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
