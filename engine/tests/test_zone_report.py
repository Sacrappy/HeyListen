"""The per-zone shader and pipeline usage (gpu/vk/zone_report.h, perf_report.cpp zones_queue, the sender in host/hos_crash_dialog.c), no game, no GPU.

- tests/test_zone_report.cpp: the recorder (once a key per map, the first-use order within a visit, the caps, the count
  buckets, the arrival flag), the sent file (hookshot_zones_sent.bin), the batching (as many maps a batch as fit, a map
  alone over the cap left out) and the gating (the performance statistics row, an address, HL_TELEMETRY_ZONES=0).
- The payload against the documented schema (docs/performance-statistics.md): its fields, the keys' hex form, the
  buckets, the arrival flags, the stamps, and nothing of the person (checked against this PC's own names and folders).
- The wiring in the source: recorded where a draw looks its program and pipeline up, queued at the window's close and at
  the exit, only with the row on and an address and without HL_TELEMETRY_ZONES=0, remembered once queued, deleted with
  the performance summaries when the row goes off, and sent by the host to /telemetry/zones.

    python -I -B tests/test_zone_report.py [--cxx g++]
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""

KEYS = ["v", "version", "release", "state_stamp", "shader_stamp", "vendor_id", "driver", "maps"]
MAP_KEYS = ["name", "visits", "arrival_s", "programs", "states"]


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)
    print("PASS:", what)


def source(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def function(text, head):
    at = text.index(head)
    end = text.index("\n}\n", at)
    return text[at:end]


def wiring():
    cpp = source("gpu/vk/perf_report.cpp")
    queue = function(cpp, "void zones_queue() {")
    check("zr::why_not(opts.send_perf == 1, telemetry_available(), getenv(HOS_TELEMETRY_ZONES_ENV))" in queue,
          "the batch needs the row, an address and the switch (the row decides first)")
    check(queue.index("write_summary(") < queue.index("std::ofstream f(dir / zr::kSentFile"), "the maps are marked as sent only once they are queued")
    check('"zones_pending"' in queue and "zr::payload_json(meta, parts)" in queue, "the batch goes to zones_pending as the payload")
    check(queue.count("meta.") == 6 and "meta.vendor_id = dev.props.vendorID" in queue and
          "meta.driver = machine.driver" in queue, "the payload's meta is the version, the release flag, the driver, the GPU's vendor and the stamps, nothing else")
    check("zones_queue();" in function(cpp, "void at_exit() {") and "zones_queue();" in function(cpp, "void perf_report_final(int event) {"),
          "the batch is queued at the exit and at the window's close")
    where = function(cpp, "bool where_now() {")
    check("if (name != zone_map) zone_enter(name);" in where, "the recorder follows the host's map name, on its own, once a second")
    enter = function(cpp, "void zone_enter(const std::string& name) {")
    check("->visits = 1" in enter and "->visits++" in enter,
          "a map's first entry counts as a visit (a map toured once is 1, as the payload's visits says)")
    off = function(cpp, "void switch_off() {")
    check('"zones_pending", "zones_sent"' in off and "zr::kSentFile" in off, "turning the row off deletes the zone records too, and the memory of what was sent")
    check("kMaxZonesPending = 4" in cpp and re.search(r"HOS_ZONES_MAX_PENDING = 4,", source("host/hos_perf_send.h")), "the renderer and the sender agree on how many batches wait")
    draw = source("gpu/vk/vk_draw.cpp")
    check("if (cp) zone_note_program(cp->key);" in draw and
          draw.index("if (cp) zone_note_program(cp->key);") > draw.index("cp = program_for("),
          "a draw's program key is recorded where the draw looked the program up")
    pipe = draw[draw.index("PipeEntry* graphics_pipeline("):draw.index("\n}\n", draw.index("PipeEntry* graphics_pipeline("))]
    check(pipe.count("zone_note_state(") >= 2 and "zone_note_state(f.key);" in pipe and "zone_note_state(pkey);" in pipe,
          "a draw's pipeline key is recorded on both the fast hit and the slow lookup, once a lookup")
    check("const uint64_t pkey = pipeline_key(p, s, at, topo);" in pipe, "the pipeline's key as the notes and the recorder know it, before its own object's key")
    dialog = source("host/hos_crash_dialog.c")
    run = function(dialog, "int hos_zones_send_run(const char *dir) {")
    check("hos_telemetry_zones_on(getenv(HOS_TELEMETRY_ZONES_ENV))" in run and "&zone_usage" in run, "the sender checks the switch")
    check('HOS_TELEMETRY_ZONES_PATH' in dialog and '"/telemetry/zones"' in source("host/hos_telemetry.h"), "to <address>/telemetry/zones")
    check("hos_zones_send_run(dir)" in dialog[dialog.index("perf_send_thread(void *p)"):], "the start's sender thread sends them after the pipeline states")
    docs = (ROOT.parent / "docs/performance-statistics.md").read_text(encoding="utf-8")
    check("/telemetry/zones" in docs and "HL_TELEMETRY_ZONES" in docs and "zones_pending" in docs, "documented: the route, the switch and the payload")
    guide = (ROOT.parent / "docs/USER_GUIDE.md").read_text(encoding="utf-8")
    check("per map" in guide and "zones_sent" in guide and "HL_TELEMETRY_ZONES=0" in guide, "the user guide's privacy section says what the per-map part is")
    check("HL_TELEMETRY_ZONES" in source("docs/development.md"), "the development guide lists the switch")


def unit(cxx):
    with tempfile.TemporaryDirectory(prefix="zone-report-") as t:
        tmp = Path(t)
        exe = tmp / ("unit" + EXE)
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/test_zone_report.cpp"), "-o", str(exe)], check=True, timeout=300)
        r = subprocess.run([str(exe)], capture_output=True, timeout=120)
        print(r.stdout.decode().strip())
        check(r.returncode == 0, "the recorder, the sent file, the batching and the gating" + (": " + r.stderr.decode("utf-8", "replace").strip() if r.returncode else ""))

        # the payload from a file of zones: the documented schema, field by field
        lines = [
            "map Kokiri-Forest-(child) 1",
            "program 0011223344556677 0 1 1",
            "program 8899aabbccddeeff 1 9 1",
            "state 0011223344556677 0 10 0",
            "map Hyrule-Field 2",
            "state 0102030405060708 0 1 0",
            "state 1112131415161718 1 1 0",
            "state 2122232425262728 2 1 0",
            "state deadbeefdeadbeef 3 2 0",
        ]
        (tmp / "zones.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        r = subprocess.run([str(exe), "payload", str(tmp / "zones.txt")], capture_output=True, timeout=120)
        check(r.returncode == 0, "the payload is built from the file")
        body = r.stdout.decode("utf-8")
        p = json.loads(body)
        check(list(p) == KEYS, "the payload's fields are exactly %s" % ", ".join(KEYS))
        check(p["v"] == 1 and p["version"] == "v0.4.0" and p["release"] is True and p["vendor_id"] == 0x10DE and
              p["driver"] == "NVIDIA 566.36" and p["state_stamp"] == "0123456789abcdef" and p["shader_stamp"] == "fedcba9876543210",
              "the release, the stamps, the vendor id and the driver")
        check(len(p["maps"]) == 2 and [m["name"] for m in p["maps"]] == ["Kokiri-Forest-(child)", "Hyrule-Field"],
              "the maps in first-visit order")
        first, second = p["maps"]
        check(list(first) == MAP_KEYS and list(second) == MAP_KEYS, "each map's fields are exactly %s" % ", ".join(MAP_KEYS))
        check(first["visits"] == 1 and second["visits"] == 2, "the visits")
        check(first["arrival_s"] == 10 and second["arrival_s"] == 10, "the arrival window the flag is against, seconds")
        check(first["programs"] == [["0011223344556677", 0, 0, 1], ["8899aabbccddeeff", 1, 1, 1]] and
              first["states"] == [["0011223344556677", 0, 2, 0]], "the keys as [hex key, first index, count bucket, arrival flag]")
        check(second["programs"] == [] and second["states"] == [["0102030405060708", 0, 0, 0], ["1112131415161718", 1, 0, 0],
              ["2122232425262728", 2, 0, 0], ["deadbeefdeadbeef", 3, 1, 0]],
              "a map with no program keys of its own, and a state's first index of its visit")
        for m in p["maps"]:
            for kind in ("programs", "states"):
                for e in m[kind]:
                    check(len(e) == 4 and re.fullmatch(r"[0-9a-f]{16}", e[0]) and isinstance(e[1], int) and e[1] >= 0 and
                          e[2] in (0, 1, 2) and e[3] in (0, 1), "an entry of %s is a 64-bit key, an index, a bucket and a flag" % kind)

        # nothing of the person: not this PC's user name, folders or host name
        mine = {os.environ.get(k, "") for k in ("USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME", "USERPROFILE", "HOME", "TEMP", "TMP")}
        mine |= {str(Path.home()), str(ROOT), os.getcwd(), str(tmp)}
        low = body.lower()
        for s in sorted(m for m in mine if len(m) >= 3):
            check(s.lower() not in low, "the payload does not hold this PC's name or folder (%d characters)" % len(s))
        check(not re.search(r"[A-Za-z]:[\\/]|\\\\", body), "no path in the body")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    wiring()
    unit(args.cxx)


if __name__ == "__main__":
    main()
