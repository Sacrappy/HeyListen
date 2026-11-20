"""Game-free: the Button prompts setting has something to show (HeyListen issue #69). The icons are not shipped: nx2/prompt_assets.py
draws them at the player's build (the game's own font and textures, with Kenney's CC0 faces), and only when the builder's Python has
fontTools and the other pins of nx2/prompt_requirements.txt. A release whose Python part lacked them (v0.4.2's) built games with no
prompts/ folder, and host/prompts.c then left the game's Switch icons whatever the row said. Checks:
- tools/release/make_release.py packages the Python part with prompt_requirements.txt's pins and stops when the package lacks one;
- the package check itself, on made-up site-packages folders (missing, complete, wrong version);
- the files host/prompts.c opens under prompts/ are the ones prompt_assets.py writes, for every face prompts.c knows;
- the game says in its log why nothing changed when there is no prompts/ folder;
- the Kenney fonts the faces are drawn from are in third_party, with their CC0 license, and the README credits Kenney.

python -I -B tests/test_release_prompt_assets.py
"""
import importlib.util
import re
import sys
import tempfile
from pathlib import Path

ENGINE = Path(__file__).resolve().parents[1]
ROOT = ENGINE.parent
sys.path.insert(0, str(ENGINE / "nx2"))


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def site(root, dist_infos):
    folder = Path(root) / "tools" / "python" / "Lib" / "site-packages"
    folder.mkdir(parents=True)
    for d in dist_infos:
        (folder / d).mkdir()
    return root


def main():
    mr = load("make_release", ROOT / "tools" / "release" / "make_release.py")
    text = (ROOT / "tools" / "release" / "make_release.py").read_text(encoding="utf-8")

    # 1. the release packages the Python part with the pins, and checks the result
    m = re.search(r'run\(py \+ \[parts_py, "package".*?\], env=env\)\n(\s*check_prompt_packages\(package\))?', text, re.S)
    check(m is not None, "make_release.py has no 'release_parts.py package' call")
    check('"--python-packages"' in m.group(0) and "prompt_requirements.txt" in m.group(0),
          "make_release.py's package call has no --python-packages nx2/prompt_requirements.txt")
    check(m.group(1) is not None, "make_release.py does not check the package's Python after packaging it")

    # 2. the package check
    pins = mr.prompt_pins()
    names = [p[0] for p in pins]
    for need in ("fonttools", "pillow", "astc-encoder-py", "texture2ddecoder"):
        check(need in names, "nx2/prompt_requirements.txt has no %s" % need)
    with tempfile.TemporaryDirectory() as t:
        none = site(Path(t) / "none", ["numpy-2.5.3.dist-info", "lz4-4.4.5.dist-info"])
        miss = mr.missing_prompt_packages(str(none))
        check(len(miss) == len(pins), "a Python without the pins is missing all of them, not %s" % miss)
        try:
            mr.check_prompt_packages(str(none))
            check(False, "check_prompt_packages passed a package whose Python has no fontTools")
        except SystemExit as e:
            check("fonttools" in str(e).lower() and "Switch" in str(e), "the refusal does not say what is missing: %s" % e)
        full = site(Path(t) / "full", ["%s-%s.dist-info" % (n.replace("-", "_"), v) for n, v in pins])
        check(mr.missing_prompt_packages(str(full)) == [], "a Python with every pin is complete")
        mr.check_prompt_packages(str(full))
        wrong = site(Path(t) / "wrong", ["%s-0.0.1.dist-info" % n.replace("-", "_") for n, _ in pins])
        check(len(mr.missing_prompt_packages(str(wrong))) == len(pins), "pins of another version do not count")
        check(len(mr.missing_prompt_packages(str(Path(t) / "no-python"))) == len(pins), "no Python at all misses every pin")

    # 3. the lookup: what host/prompts.c opens is what the build writes
    src = (ENGINE / "host" / "prompts.c").read_text(encoding="utf-8")
    assets = (ENGINE / "nx2" / "prompt_assets.py").read_text(encoding="utf-8")
    import prompt_faces
    faces = re.search(r"faces\[HOS_PROMPT_N\] = \{([^}]*)\}", src)
    check(faces is not None, "host/prompts.c has no faces[]")
    face_names = re.findall(r'"([a-z0-9]+)"', faces.group(1))
    check(face_names[1:] == list(prompt_faces.FACES) + ["keyboard"], "faces[] is not the Switch, prompt_faces.FACES, then the keyboard")
    per_face = set(re.findall(r'"%sprompts/%s/([A-Za-z.]+)"', src))
    per_face |= set(re.findall(r'prompts/%s/([A-Za-z.]+)"', src))
    check({"External.bfotf", "textures.bin", "hint.bin"} <= per_face, "prompts.c reads %s under prompts/<face>/" % sorted(per_face))
    for f in per_face:
        check('"%s"' % f in assets, "prompts.c opens prompts/<face>/%s, which prompt_assets.py never writes" % f)
    top = set(re.findall(r'prompts/([A-Za-z.]+)"', src)) - per_face - {"%s"}
    for f in ("Common.bntx", "Common.blarc", "ring.bin", "rsizetable.zs"):
        check(f in top or f in src, "prompts.c no longer uses prompts/%s" % f)
        check('"%s"' % f in assets, "prompts.c uses prompts/%s, which prompt_assets.py never writes" % f)
    # the build puts the folder in the game folder, where prompts.c looks (the folder with image.bin, not bin\)
    mg = (ENGINE / "nx2" / "make_game.py").read_text(encoding="utf-8")
    check('prompts = os.path.join(out, "prompts")' in mg, "make_game.py does not make <game folder>/prompts")
    check('"%sprompts"' in src and "game_dir(d, sizeof d)" in src, "prompts.c does not look for <game folder>/prompts")
    # the ini the row writes (gpu/vk/vk_menu.cpp ini_path: the game folder's) is the one choose_face reads
    check('"%shookshot_options.ini"' in src and "button_prompts=" in src, "prompts.c does not read button_prompts=")
    menu = (ENGINE / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    check('"Button prompts", &opts.prompts' in menu, "the menu has no Button prompts row on opts.prompts")

    # 4. no prompts/ folder: the log says so
    check("prompts: no prompts folder" in src, "prompts.c is silent when the game folder has no prompts/")

    # 5. the faces' fonts: free to ship, credited
    kenney = ENGINE / "third_party" / "kenney_input"
    check("Creative Commons Zero" in (kenney / "License.txt").read_text(encoding="utf-8"), "Kenney's license is not CC0")
    for f in ("kenney_input_xbox_series.otf", "kenney_input_playstation_series.otf", "kenney_input_steam_deck.otf"):
        check((kenney / f).is_file(), "third_party/kenney_input/%s is missing" % f)
    # (the keyboard face's: nx2/prompt_keys.py FONT, in its own folder with the same release's license)
    import prompt_keys
    keys = ENGINE / "third_party" / "kenney_input_keyboard"
    check("Creative Commons Zero" in (keys / "License.txt").read_text(encoding="utf-8"),
          "the keyboard face's Kenney license is not CC0")
    check((keys / prompt_keys.FONT).is_file(), "third_party/kenney_input_keyboard/%s is missing" % prompt_keys.FONT)
    check((keys / "PROVENANCE.md").is_file(), "third_party/kenney_input_keyboard/PROVENANCE.md is missing")
    check("Kenney" in (ROOT / "README.md").read_text(encoding="utf-8"), "the README does not credit Kenney")
    check("Kenney" in (ROOT / "THIRD_PARTY_NOTICES.md").read_text(encoding="utf-8"), "THIRD_PARTY_NOTICES.md has no Kenney")
    print("ok: the release packages the prompt packages, prompts.c and prompt_assets.py agree on the files, Kenney is credited")
    return 0


if __name__ == "__main__":
    sys.exit(main())
