"""Game-free checks of recomplauncher.json (repository root), the file RecompLauncher (recomplauncher.com) reads: it has what the
launcher requires, the build it checks for is the one make_game.py supports, every setting writes a key the in-game menu reads, the
prepare step's arguments are ones make_game.py takes, and the Windows zip and the AppImage carry it.

The launcher's JSON schema (https://recomplauncher.com/schema/recomplauncher.json) is not copied here: a test that needs the network is
not a game-free check. With RECOMPLAUNCHER_SCHEMA=<a downloaded copy of it> the file is also validated against it (the subset of
JSON Schema that file uses).

Run with: python -I -B tests/test_recomplauncher_json.py
"""
import importlib.util
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest
import zipfile

ENGINE = Path(__file__).resolve().parents[1]
REPO = ENGINE.parent
MANIFEST = REPO / "recomplauncher.json"
GAME = "{install}/HeyListen"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


make_game = load("make_game_for_recomplauncher_json", ENGINE / "nx2" / "make_game.py")


def menu_rows():
    """The in-game menu's rows as the options file keys them (gpu/vk/vk_menu.cpp ini_key): key -> (lo, hi) or None"""
    menu = (ENGINE / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    ini_key = menu.split("const char* ini_key(const Row& r) {", 1)[1].split("\n}", 1)[0]
    renamed = {  # (rows whose key is not their name)
        "Output resolution": "output_res", "Render resolution": "resolution", "Window scaling filter": "upscaling", "Upscaler": "dlss",
        "Send performance statistics": "send_performance_stats", "Send crash reports automatically": "send_crash_reports",
        "Check for updates": "update_check"}
    for key in re.findall(r'return "([a-z_0-9]+)";', ini_key):
        assert key in renamed.values(), key   # (a new special case: add it above)
    rows = {}
    for name, lo, hi in re.findall(r'\{TAB_[A-Z]+, "([^"]+)", &opts\.\w+, (-?\d+|\d+), ([^,]+),', menu):
        key = renamed.get(name, re.sub(" ", "_", name.lower()))
        rows[key] = (int(lo), int(hi) if hi.strip().lstrip("-").isdigit() else None)
    return rows


def validate(value, schema, root, where="$"):
    """The subset of JSON Schema recomplauncher.json's schema uses: type, enum, required, properties, additionalProperties, items,
    minItems, uniqueItems, minimum, pattern, $ref. Returns the problems"""
    if "$ref" in schema:
        node = root
        for part in schema["$ref"].lstrip("#/").split("/"):
            node = node[part]
        return validate(value, node, root, where)
    problems = []
    kinds = {"object": dict, "array": list, "string": str, "integer": int, "boolean": bool, "number": (int, float)}
    if "type" in schema:
        ok = isinstance(value, kinds[schema["type"]]) and not (schema["type"] in ("integer", "number") and isinstance(value, bool))
        if not ok:
            return ["%s: not a %s" % (where, schema["type"])]
    if "enum" in schema and value not in schema["enum"]:
        problems.append("%s: %r is not one of %s" % (where, value, schema["enum"]))
    if isinstance(value, str) and "pattern" in schema and not re.search(schema["pattern"], value):
        problems.append("%s: %r does not match %s" % (where, value, schema["pattern"]))
    if isinstance(value, (int, float)) and "minimum" in schema and value < schema["minimum"]:
        problems.append("%s: below %s" % (where, schema["minimum"]))
    if isinstance(value, dict):
        for name in schema.get("required", []):
            if name not in value:
                problems.append("%s: no %s" % (where, name))
        props = schema.get("properties", {})
        for name, item in value.items():
            if name in props:
                problems += validate(item, props[name], root, where + "." + name)
            elif schema.get("additionalProperties") is False:
                problems.append("%s: %s is not a known field" % (where, name))
            elif isinstance(schema.get("additionalProperties"), dict):
                problems += validate(item, schema["additionalProperties"], root, where + "." + name)
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            problems.append("%s: fewer than %d items" % (where, schema["minItems"]))
        if schema.get("uniqueItems") and len({json.dumps(v, sort_keys=True) for v in value}) != len(value):
            problems.append("%s: repeated items" % where)
        if "items" in schema:
            for i, item in enumerate(value):
                problems += validate(item, schema["items"], root, "%s[%d]" % (where, i))
    return problems


class RecompLauncherJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def test_the_launcher_reads_it(self):
        d = self.doc
        self.assertEqual(d["format"], 1)
        self.assertEqual(d["$schema"], "https://recomplauncher.com/schema/recomplauncher.json")
        self.assertEqual(d["name"], "HeyListen")
        self.assertEqual(d["repo"], "Sacrappy/HeyListen")
        # (a UTF-8 file with no byte order mark: the launcher shows the line and column of a JSON mistake)
        self.assertFalse(MANIFEST.read_bytes().startswith(b"\xef\xbb\xbf"))
        for level2 in ("config", "settings"):
            self.assertTrue(d[level2], level2)
        for level3 in ("prepare", "saves", "folders", "actions"):
            self.assertTrue(d[level3], level3)

    def test_validates_against_the_schema_when_it_is_at_hand(self):
        path = os.environ.get("RECOMPLAUNCHER_SCHEMA")
        if not path or not os.path.isfile(path):
            self.skipTest("RECOMPLAUNCHER_SCHEMA is not set (a copy of recomplauncher.com/schema/recomplauncher.json)")
        schema = json.loads(Path(path).read_text(encoding="utf-8"))
        self.assertEqual(validate(self.doc, schema, schema), [])

    def test_the_game_is_the_one_make_game_supports(self):
        game = self.doc["game"]
        self.assertEqual(game["check"], {"nup_build_id": make_game.BUILD_ID})
        self.assertIn(make_game.BUILD_ID, game["label"])
        self.assertEqual(game["kind"], "iso_or_folder")   # (the .nup, or its romfs and exefs: make_game.py --game takes both)

    def test_start_plays_through_the_builder(self):
        # (like Longshot's: the program that starts the game is the builder's own, so updates are looked for before each launch.
        # Windows: the setup program in the zip, which looks for a newer release and starts the game; Linux: the AppImage's AppRun)
        start = self.doc["start"]
        self.assertEqual(start["program"], ["HeyListenBuilder-Setup.exe", "HeyListen.AppImage"])
        out = self.doc["prepare"]["args"][self.doc["prepare"]["args"].index("--out") + 1]
        self.assertEqual(start["args"], ["--play", out])   # (the folder prepare builds into is the one it plays)
        # (no env: the setup tells the game not to ask again once it asked itself, and on Linux the game's own offer is the question)
        self.assertNotIn("env", start)
        # (the launcher features list was not the cause of the developer page's 0 of 6 features: it is not declared)
        self.assertNotIn("requires", self.doc)
        # (make_game.py puts the game where the setup and AppRun look: the Windows launcher layout, bin/ beside image.bin, and the Linux one)
        self.assertTrue("HeyListen.exe" in make_game.BESIDE or "hookshot" in make_game.BESIDE)   # (BESIDE is the host platform's)

    def test_the_setup_program_takes_play(self):
        setup = (ENGINE / "builder" / "setup" / "setup.c").read_text(encoding="utf-8")
        self.assertIn('L"--play"', setup)
        self.assertIn('#include "../../host/hos_update_ver.h"', setup)   # (the same version order and update_check line as the game's offer)
        play = setup[setup.index("static int play_folder("):setup.index("int WINAPI wWinMain(")]
        self.assertIn("ver_newer(look.tag, have)", play)                 # (a newer release: asked about, never installed here)
        self.assertIn("ini_update_off(ini)", play)                       # (the player's Never ask again, or the Settings row)
        self.assertIn('_wputenv_s(L"HOOKSHOT_NO_UPDATE_CHECK", L"1")', play)   # (asked here: the game does not ask again)
        self.assertIn("WaitForSingleObject(t, 4500)", play)              # (never held back by the network)
        self.assertLess(play.index("WaitForSingleObject(t, 4500)"), play.index("CreateProcessW"))
        # (no network: no question, the game starts; a folder that is not there or not built: an error and exit 1, no start)
        self.assertGreaterEqual(play.count("play_error("), 4)

    def test_prepare_takes_arguments_make_game_has(self):
        prepare = self.doc["prepare"]
        self.assertEqual(prepare["version"], "1")
        self.assertIn("HeyListenBuilder.exe", prepare["program"])   # (the window-less builder: HeadlessCli.cs)
        self.assertIn("HeyListen.AppImage", prepare["program"])      # (AppRun --cli: the same arguments, make_game.py)
        args = prepare["args"]
        self.assertEqual(args[0], "--cli")
        self.assertIn(GAME + "/image.bin", prepare["done"])           # (a folder the builder made counts as prepared)
        asked = [a.replace("{game}", "X").replace("{install}", "Y") for a in args[1:]]
        for option in prepare["options"]:
            asked += option["args"]
        old = sys.argv
        try:
            sys.argv = ["make_game.py"] + asked
            parsed = make_game.parse_args()
        finally:
            sys.argv = old
        self.assertTrue(parsed.prepare)
        self.assertEqual(parsed.game, "X")
        self.assertEqual(parsed.out, "Y/HeyListen")
        self.assertTrue(parsed.fetch_ngx_sdk and parsed.accept_nvidia_license)
        self.assertTrue(parsed.fsr and parsed.fetch_fsr_runtime and parsed.accept_amd_license)
        # (a vendor's SDK is fetched only by ticking its license)
        for option in prepare["options"]:
            self.assertRegex(option["id"], r"^[a-z0-9_]+$")
            if option["id"] in ("dlss", "fsr4"):
                self.assertTrue(option["license"]["url"].startswith("https://"), option["id"])
            else:   # (the two opt-in reports: no license, and they start OFF, which the launcher takes as ticked when "default" is left out)
                self.assertIs(option["default"], False, option["id"])
                self.assertNotIn("license", option)
        self.assertTrue(parsed.send_perf_stats and parsed.send_crash_reports)
        self.assertEqual(sorted(o["id"] for o in prepare["options"]), ["dlss", "fsr4", "send_crash_reports", "send_perf_stats"])

    def test_privacy_settings_write_the_two_opt_in_keys_off_by_default(self):
        privacy = [x for x in self.doc["settings"] if x.get("group") == "Privacy"]
        self.assertEqual([x["id"] for x in privacy], ["send_perf", "send_crash"])
        for x in privacy:
            self.assertEqual([c["label"] for c in x["choices"]], ["Off", "On"])
            (key,) = x["defaults"]
            self.assertIn(key, ("send_performance_stats", "send_crash_reports"))
            self.assertEqual(x["defaults"][key], "0")
            self.assertEqual(x.get("in", "main"), "main")
        self.assertIn("Privacy", [g["id"] for g in self.doc["groups"]])

    def test_dlss_runtime_is_the_launchers_optional_shared_input(self):
        (inp,) = self.doc["inputs"]
        self.assertEqual((inp["role"], inp["shared"], inp["optional"]), ("runtime", "nvidia.dlss", True))
        self.assertEqual(inp["destination"], "HeyListen/ngx/nvngx_dlss.dll")   # (gpu/vk/vk_dlss.cpp: the game folder's ngx folder)
        src = (ENGINE / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
        self.assertIn('game_dir() / L"ngx"', src)

    def test_set_option_keys_touches_only_its_keys(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(make_game.set_option_keys(d, {"send_performance_stats": "1"}), ["send_performance_stats"])
            ini = Path(d) / "hookshot_options.ini"
            self.assertEqual(ini.read_bytes(), b"send_performance_stats=1\n")
            ini.write_bytes(b"fullscreen=1\r\nsend_crash_reports=0\r\n# note\r\nsend_crash_reports=0\r\ndlss=3")
            make_game.set_option_keys(d, {"send_crash_reports": "1", "send_performance_stats": "1"})
            self.assertEqual(ini.read_bytes(), b"fullscreen=1\r\nsend_crash_reports=1\r\n# note\r\ndlss=3\r\nsend_performance_stats=1\r\n")

    def test_license_links_are_the_ones_the_build_shows(self):
        ngx = load("ngx_sdk_for_recomplauncher_json", ENGINE / "scripts" / "ngx_sdk.py")
        ffx = load("ffx_sdk_for_recomplauncher_json", ENGINE / "scripts" / "ffx_sdk.py")
        urls = {o["id"]: o["license"]["url"] for o in self.doc["prepare"]["options"] if "license" in o}
        self.assertEqual(urls["dlss"], ngx.LICENSE_URL)
        self.assertEqual(urls["fsr4"], ffx.LICENSE_URL)

    def test_settings_are_menu_rows_with_values_the_row_takes(self):
        rows = menu_rows()
        config_ids = {c["id"] for c in self.doc["config"]}
        groups = {g["id"] for g in self.doc.get("groups", [])}
        seen_ids, seen_keys = set(), set()
        for setting in self.doc["settings"]:
            self.assertNotIn(setting["id"], seen_ids)
            seen_ids.add(setting["id"])
            self.assertIn(setting.get("in", "main"), config_ids)
            if "group" in setting:
                self.assertIn(setting["group"], groups)
            self.assertGreaterEqual(len(setting["choices"]), 2, setting["id"])
            keys = {k for c in setting["choices"] for k in c["set"]}
            self.assertEqual(len(keys), 1, setting["id"])
            (key,) = keys
            self.assertIn(key, rows, "%s: no menu row writes %s" % (setting["id"], key))
            seen_keys.add(key)
            lo, hi = rows[key]
            values = [c["set"][key] for c in setting["choices"]]
            self.assertEqual(len(set(values)), len(values), setting["id"])
            for v in values + list(setting["defaults"].values()):
                self.assertRegex(v, r"^\d+$")
                self.assertGreaterEqual(int(v), lo, setting["id"])
                if hi is not None:
                    self.assertLessEqual(int(v), hi, setting["id"])
            self.assertEqual(set(setting["defaults"]), {key})
            self.assertIn(setting["defaults"][key], values)
        # (the main video and gameplay rows are there)
        for key in ("fullscreen", "output_res", "dlss", "resolution", "frame_generation", "frame_rate_limit", "sharpness", "ultrawide",
                    "text_speed", "upscaling", "game_speed"):
            self.assertIn(key, seen_keys)

    def test_the_upscaler_and_frame_rate_rows_have_the_menu_values(self):
        by_key = {k: s for s in self.doc["settings"] for k in s["choices"][0]["set"]}
        up = {c["set"]["dlss"]: c["label"] for c in by_key["dlss"]["choices"]}
        self.assertEqual(up, {"0": "The game's own", "1": "NVIDIA DLSS", "2": "NVIDIA DLAA", "3": "AMD FSR 3.1", "4": "AMD FSR 4",
                              "5": "AMD FSR 4 Native AA", "6": "Off"})   # (gpu/vk/upscaler_rows.h: GAME DLSS DLAA FSR3 FSR4 FSR4_AA OFF)
        fps = [c["label"].split()[0] for c in by_key["frame_rate_limit"]["choices"]]
        self.assertEqual(fps, ["60", "90", "120", "144", "165", "240"])   # (vk_menu.cpp tx_fps_limit)
        gen = [c["set"]["frame_generation"] for c in by_key["frame_generation"]["choices"]]
        self.assertEqual(gen, [str(i) for i in range(9)])

    def test_every_file_is_in_the_game_folder_prepare_makes(self):
        main = next(c for c in self.doc["config"] if c["id"] == "main")
        self.assertEqual(main["format"], "ini")
        self.assertEqual([f["path"] for f in main["file"]], [GAME + "/hookshot_options.ini"])
        for path in [f["path"] for f in self.doc["folders"]["game"]]:
            self.assertEqual(path, GAME)
        self.assertEqual([f["path"] for f in self.doc["folders"]["logs"]], [GAME + "/logs"])   # (host/hos_run_logs.h)
        self.assertIn(GAME + "/user/save", self.doc["saves"])
        for action in self.doc["actions"]:
            self.assertIn(action.get("open", action.get("clear", "").split("/")[0]), self.doc["folders"])

    def test_the_options_file_name_is_the_one_the_game_reads(self):
        self.assertIn('"hookshot_options.ini"', (ENGINE / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8"))

    def test_the_appimage_carries_it(self):
        script = (ENGINE / "scripts" / "linux" / "appimage.sh").read_text(encoding="utf-8")
        self.assertIn('cp ../recomplauncher.json "$APPDIR/recomplauncher.json"', script)
        self.assertIn("OUT=$B/HeyListen-$VERSION-Linux-x64.AppImage", script)

    def test_the_windows_zip_carries_it_at_the_top(self):
        zipper = load("make_launcher_zip_under_test", REPO / "tools" / "release" / "make_launcher_zip.py")
        self.assertEqual(zipper.asset_name("v0.4.1"), "HeyListen-v0.4.1-Windows-x64.zip")
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            release = tmp / "release"
            (release / "parts").mkdir(parents=True)

            def part(name, members):
                with zipfile.ZipFile(release / "parts" / name, "w") as z:
                    for member, data in members.items():
                        z.writestr(member, data)
                return name, (release / "parts" / name).stat().st_size

            app, app_size = part("HeyListenBuilder-app-aaaaaaaaaaaa.zip", {
                "HookshotBuilder/HeyListenBuilder.exe": b"MZ", "HookshotBuilder/HeyListenBuilder-Setup.exe": b"MZ",
                "HookshotBuilder/tools/nx2/make_game.py": b"#",
                "HookshotBuilder/tools/version.txt": b"v0.4.1\n"})
            py, py_size = part("HookshotBuilder-python-bbbbbbbbbbbb.zip", {"HookshotBuilder/tools/python/python.exe": b"MZ"})
            llvm, llvm_size = part("llvm-mingw.zip", {"llvm-mingw-20261104-ucrt-x86_64/bin/clang.exe": b"MZ"})
            base = "https://github.com/Sacrappy/HeyListen/releases/download/builder-parts/"
            (release / zipper.LIST).write_text(
                "tag v0.4.1\npart %s %d . - - %s%s\npart %s %d . - - %s%s\npart %s %d HookshotBuilder/tools "
                "llvm-mingw-20261104-ucrt-x86_64 llvm-mingw %s%s\nrun HookshotBuilder/HeyListenBuilder.exe\n"
                % ("a" * 64, app_size, base, app, "b" * 64, py_size, base, py, "c" * 64, llvm_size, base, llvm), encoding="utf-8")
            out = tmp / zipper.asset_name("v0.4.1")
            names = zipper.make_zip(str(release), "v0.4.1", str(out), fetch=lambda *a: self.fail("a part was downloaded"))
            with zipfile.ZipFile(out) as z:
                self.assertEqual(sorted(z.namelist()), sorted([
                    "recomplauncher.json", "HeyListenBuilder.exe", "HeyListenBuilder-Setup.exe", "tools/nx2/make_game.py", "tools/version.txt",
                    "tools/python/python.exe", "tools/llvm-mingw/bin/clang.exe"]))
                self.assertEqual(z.read("recomplauncher.json"), MANIFEST.read_bytes())
            self.assertEqual(sorted(names), sorted(zipfile.ZipFile(out).namelist()))
            # (the program the launcher starts, and the one it prepares with, are at the top, where the file says)
            self.assertIn(self.doc["prepare"]["program"][0], names)
            self.assertIn(self.doc["start"]["program"][0], names)


if __name__ == "__main__":
    unittest.main()
