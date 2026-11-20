"""The switch reference is complete and current: every environment name the source reads is in docs/SWITCHES.md and
engine/docs/switches.json, and both files are what the generator gives today.

    python -I -B tests/test_switches_documented.py

Fails when a new getenv/os.environ (or env helper, table or wrapper) is read and neither file yet lists it, and when
either file was edited by hand (a switch removed, an explanation changed) instead of regenerated.
"""
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ENGINE = os.path.dirname(HERE)
REPO = os.path.dirname(ENGINE)
SCRIPT = os.path.join(ENGINE, "scripts", "list_switches.py")
JSON_PATH = os.path.join(ENGINE, "docs", "switches.json")
MD_PATH = os.path.join(REPO, "docs", "SWITCHES.md")

FIELDS = ("name", "area", "group", "audience", "files", "default", "values", "explanation", "comment",
          "documented_in", "source", "documented", "set_via", "use")
AREAS = {"renderer", "shaders", "memory", "audio", "input", "native", "host", "telemetry", "logging", "build"}
AUDIENCES = {"player", "diagnostic", "developer"}


def load_generator():
    spec = importlib.util.spec_from_file_location("list_switches", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_norm(path):
    """The file's text with LF endings: a Windows checkout may give CRLF, the generator writes LF."""
    with open(path, encoding="utf-8", newline="") as f:
        return f.read().replace("\r\n", "\n")


def heading_names(md):
    return re.findall(r"(?m)^#### `([^`]+)` - (?:player|diagnostic|developer)$", md)


def main():
    fails = []

    def check(cond, msg):
        if not cond:
            fails.append(msg)

    gen = load_generator()
    records = gen.build_records()
    names = [r["name"] for r in records]

    # (the source scan itself: one entry per name, all fields, the areas and audiences the page uses)
    check(len(names) == len(set(names)), "a switch appears twice in the scan")
    for r in records:
        for f in FIELDS:
            check(f in r, "%s: field %s is missing" % (r.get("name"), f))
        check(r["area"] in AREAS, "%s: unknown area %s" % (r["name"], r["area"]))
        check(r["audience"] in AUDIENCES, "%s: unknown audience %s" % (r["name"], r["audience"]))
        check(bool(r["files"]), "%s: no file:line recorded" % r["name"])
        check(r["documented"] == (not r["explanation"].startswith("undocumented:")),
              "%s: documented flag and explanation disagree" % r["name"])

    # (the committed files are exactly what the generator gives)
    if not os.path.exists(JSON_PATH):
        fails.append("engine/docs/switches.json is missing")
    else:
        want = gen.dump_json(records)
        got = read_norm(JSON_PATH)
        if got != want:
            try:
                old = {r["name"] for r in json.loads(got)["switches"]}
            except (ValueError, KeyError):
                old = set()
            new = set(names)
            extra = sorted(new - old)
            gone = sorted(old - new)
            fails.append("engine/docs/switches.json is out of date%s%s (run python -I engine/scripts/list_switches.py)" %
                         ((": new in the source: " + ", ".join(extra[:8])) if extra else "",
                          ("; no longer read: " + ", ".join(gone[:8])) if gone else ""))

    md = ""
    if not os.path.exists(MD_PATH):
        fails.append("docs/SWITCHES.md is missing")
    else:
        md = read_norm(MD_PATH)
        in_md = heading_names(md)
        missing = [n for n in names if n not in set(in_md)]
        check(not missing, "docs/SWITCHES.md has no entry for: " + ", ".join(missing[:10]))
        check(len(in_md) == len(names), "docs/SWITCHES.md has %d switch entries, the source has %d" %
              (len(in_md), len(names)))
        if md != gen.render_md(records):
            fails.append("docs/SWITCHES.md is out of date or was edited by hand "
                         "(run python -I engine/scripts/list_switches.py)")

    # (the page's promises: the top table entries exist, and every entry has its three lines)
    for n in gen.TOP_TABLE:
        check(("| `%s` |" % n) in md, "the top table has no row for %s" % n)
    check(md.count("**When a player would use it:**") == len(names),
          "each switch needs its 'When a player would use it' line")
    check(md.count("**Values:**") == len(names), "each switch needs its values and default")
    check("hookshot_env.txt" in md, "the page does not say how to set a switch")
    check("~/Games/HeyListen" in md, "the page does not say where hookshot_env.txt goes on Linux")

    print("%d switches read, %d in docs/SWITCHES.md, %d documented and %d undocumented" %
          (len(names), len(set(heading_names(md))), sum(1 for r in records if r["documented"]),
           sum(1 for r in records if not r["documented"])))
    if fails:
        print("\nFAILED:")
        for f in fails:
            print("  " + f)
        return 1
    print("both files are current")
    return 0


if __name__ == "__main__":
    sys.exit(main())
