"""The pipeline state telemetry (gpu/vk/state_report.h, perf_report.cpp states_queue, the sender in host/hos_crash_dialog.c), with no game and no GPU.

- tests/test_state_report.cpp: which records are new (not in the shipped list, not sent before, once each, fit to send), what is
  remembered, the cap (1800 records, under 1 MB), the gating (the performance statistics row, an address, HL_TELEMETRY_PIPELINES=0),
  and the payload's fields.
- Every record the batch takes passes the rules tools/release/check_states.py holds a shipped list to: the same verdict on a few
  hundred mutated and random records, and the batch decoded from a payload is a list check_states.py takes.
- The payload holds nothing of the person: only the fields of the format, no path, no user name (checked against this PC's own).
- The wiring in the source: the batch is queued at the window's close and at the exit, only with the row on and an address and
  without HL_TELEMETRY_PIPELINES=0, remembered once it is queued, deleted with the performance summaries when the row goes off,
  and sent by the host to /telemetry/pipelines.

    python -I -B tests/test_state_report.py [--cxx g++]
"""
import argparse
import base64
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / "tools" / "release"))
import check_states  # noqa: E402

EXE = ".exe" if os.name == "nt" else ""
SIZE = 528
KEYS = ["v", "version", "release", "state_stamp", "shader_stamp", "vendor_id", "driver", "record_size", "count", "states"]


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)
    print("PASS:", what)


def good(n):
    r = bytearray(SIZE)
    struct.pack_into("<QQ", r, 0, 0x1000 + n, 0x77)
    struct.pack_into("<Q", r, 32, n)
    struct.pack_into("<6I", r, 40, 0, 3, 1, 1, 126, 1)
    struct.pack_into("<I", r, 64, 37)
    return bytes(r)


def as_list(recs):
    return b"HKSTATE1" + struct.pack("<I", SIZE) + b"".join(recs)


def source(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


def function(text, head):
    at = text.index(head)
    end = text.index("\n}\n", at)
    return text[at:end]


def wiring():
    cpp = source("gpu/vk/perf_report.cpp")
    queue = function(cpp, "void states_queue() {")
    check("sr::why_not(opts.send_perf == 1, telemetry_available(), getenv(HOS_TELEMETRY_PIPELINES_ENV))" in queue,
          "the batch needs the row, an address and the switch (the row decides first)")
    check(queue.index("write_summary(") < queue.index("std::ofstream f(dir / sr::kSentFile"), "the records are marked as sent only once they are queued")
    check('"pipelines_pending"' in queue and "sr::payload_json(meta, batch)" in queue, "the batch goes to pipelines_pending as the payload")
    check(queue.count("meta.") == 6 and "meta.vendor_id = dev.props.vendorID" in queue and
          "meta.driver = machine.driver" in queue, "the payload's meta is the version, the release flag, the driver, the GPU's vendor and the stamps, nothing else")
    check("states_queue();" in function(cpp, "void at_exit() {") and "states_queue();" in function(cpp, "void perf_report_final(int event) {"),
          "the batch is queued at the exit and at the window's close")
    off = function(cpp, "void switch_off() {")
    check('"pipelines_pending", "pipelines_sent"' in off and "kSentFile" in off, "turning the row off deletes the pipeline records too, and the memory of what was sent")
    check("kMaxPipelinesPending = 4" in cpp and re.search(r"HOS_PIPES_MAX_PENDING = 4,", source("host/hos_perf_send.h")), "the renderer and the sender agree on how many batches wait")
    check(re.search(r"state_stamp = pipeline_state_stamp\(\), shader_stamp = program_cache_stamp\(\)", cpp) and
          "uint64_t pipeline_state_stamp() { return kStateStamp; }" in source("gpu/vk/vk_draw.cpp"), "the stamps are the renderer's own")
    check("static_assert(sizeof(StateRec) == 528" in source("gpu/vk/vk_draw.cpp") and "kRecordSize = 528" in source("gpu/vk/state_report.h"),
          "the record is the state list's: 528 bytes")
    dialog = source("host/hos_crash_dialog.c")
    run = function(dialog, "int hos_pipes_send_run(const char *dir) {")
    check("hos_telemetry_pipelines_on(getenv(HOS_TELEMETRY_PIPELINES_ENV))" in run and "&pipeline_states" in run, "the sender checks the switch")
    feed = function(dialog, "static int perf_feed_run(const char *dir, const PerfFeed *feed) {")
    check("hos_telemetry_opt_on(ini, HOS_TELEMETRY_KEY_PERF)" in feed and "feed->path" in feed, "and the same row, and the feed's own path")
    check('HOS_TELEMETRY_PIPELINES_PATH' in dialog and '"/telemetry/pipelines"' in source("host/hos_telemetry.h"), "to <address>/telemetry/pipelines")
    check("hos_pipes_send_run(dir)" in function(dialog, "static void *perf_send_thread(void *p) {") or "hos_pipes_send_run(dir)" in dialog[dialog.index("perf_send_thread(void *p)"):],
          "the start's sender thread sends them after the summaries")
    check("HL_TELEMETRY_PIPELINES" in source("docs/development.md") and "/telemetry/pipelines" in (ROOT.parent / "docs/performance-statistics.md").read_text(encoding="utf-8"),
          "documented: the switch and the payload")


def run(exe, *args):
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, timeout=120)
    return r.returncode, r.stdout, r.stderr.decode("utf-8", "replace")


def unit(cxx):
    with tempfile.TemporaryDirectory(prefix="state-report-") as t:
        tmp = Path(t)
        exe = tmp / ("unit" + EXE)
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"), str(ROOT / "tests/test_state_report.cpp"), "-o", str(exe)],
                       check=True, timeout=300)
        code, out, err = run(exe)
        print(out.decode().strip())
        check(code == 0, "the new records, the dedupe and memory, the cap, the gating and the payload" + (": " + err.strip() if code else ""))

        # the same verdict as check_states.py, on records mutated from a good one, with names and paths put in, and on random bytes
        rnd = random.Random(20261118)
        recs = []
        for i in range(500):
            r = bytearray(good(i + 1))
            kind = i % 5
            if kind == 0:
                for _ in range(rnd.randint(1, 3)):
                    r[rnd.randrange(SIZE)] = rnd.randrange(256)
            elif kind == 1:
                at = rnd.randrange(8, SIZE - 12)
                word = rnd.choice([b"zelda", b"NAVI", b"n\0a\0v\0i\0", b"C:\\Users\\a", b"d:/abc", b"\\\\srv", b"\\\\a b", b"c:\\ab", b"Users", b"AppData", b"hookshot", b"nintend"])
                r[at:at + len(word)] = word
            elif kind == 2:
                struct.pack_into("<I", r, rnd.choice([40, 44, 48, 52, 56, 60, 64, 232, 236, 300, 364, 372, 376, 380, 384, 388, 392, 396]), rnd.choice([0, 1, 2, 3, 8, 9, 16, 17, 24, 25, 33, 185, 0xFFFF, 0x10000, 0xFFFFF, 0x100000, 0xFFFFFFFF, 6 << 28]))
            elif kind == 3:
                struct.pack_into("<Q", r, 104 + 8 * rnd.randrange(8), rnd.choice([0, 1 << 37, 0x0C << 3, 6 << 31, 0x61 << 10, 0x65 << 17, 0x13 << 24, 7 << 34]))
            else:
                r = bytearray(rnd.randrange(256) for _ in range(SIZE))
            recs.append(bytes(r))
        (tmp / "recs.bin").write_bytes(b"".join(recs))
        code, out, err = run(exe, "verdict", tmp / "recs.bin")
        mine = [int(x) for x in out.decode().split()]
        theirs = [0 if check_states.check(as_list([r]), []) else 1 for r in recs]
        check(code == 0 and len(mine) == 500 and mine == theirs, "every record is judged as check_states.py judges it (%d of 500 are good)" % sum(theirs))
        check(0 < sum(theirs) < 500, "the mutations leave both good and bad records")

        # a payload from files: what the person's game made against the shipped list and what was sent
        player = [good(i) for i in range(1, 9)] + [good(3)]
        player[5] = player[5][:384] + struct.pack("<I", 7) + player[5][388:]   # (a bad one)
        (tmp / "player.bin").write_bytes(as_list(player))
        (tmp / "shipped.bin").write_bytes(as_list([good(1), good(2)]))
        code, out, err = run(exe, "payload", tmp / "player.bin", tmp / "shipped.bin", "-", 1800)
        body = out.decode()
        p = json.loads(body)
        check(list(p) == KEYS, "the payload's fields are exactly %s" % ", ".join(KEYS))
        states = base64.b64decode(p["states"])
        check(p["count"] == 5 and p["record_size"] == SIZE and len(states) == 12 + 5 * SIZE and
              [states[12 + i * SIZE:12 + (i + 1) * SIZE] for i in range(5)] == [good(3), good(4), good(5), good(7), good(8)],
              "the new records only, as they are stored, each once")
        check(check_states.check(states, []) == [], "the batch is a list check_states.py takes as it is")
        check(p["v"] == 1 and p["version"] == "v0.4.0" and p["release"] is True and p["vendor_id"] == 0x10DE and p["driver"] == "NVIDIA 566.36" and
              p["state_stamp"] == "0123456789abcdef" and p["shader_stamp"] == "fedcba9876543210", "the release, the stamps, the vendor id and the driver")
        # what was sent before is not sent again
        first = [states[12 + i * SIZE:12 + (i + 1) * SIZE] for i in range(2)]
        def fnv(b):  # (the game's own hash: FNV-1a over the record)
            h = 0xCBF29CE484222325
            for x in b:
                h = ((h ^ x) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
            return h
        (tmp / "sent.bin").write_bytes(b"HKSENT1\0" + b"".join(struct.pack("<Q", fnv(r)) for r in first))
        code, out, err = run(exe, "payload", tmp / "player.bin", tmp / "shipped.bin", tmp / "sent.bin", 1800)
        again = base64.b64decode(json.loads(out.decode())["states"])
        check([again[12 + i * SIZE:12 + (i + 1) * SIZE] for i in range((len(again) - 12) // SIZE)] == [good(5), good(7), good(8)], "records sent before are not in the next batch")
        code, out, err = run(exe, "payload", tmp / "player.bin", tmp / "shipped.bin", "-", 2)
        check(json.loads(out.decode())["count"] == 2, "the cap holds the batch to its size")

        # nothing of the person: not this PC's user name, folders or host name, in the body or its decoded records
        mine = {os.environ.get(k, "") for k in ("USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME", "USERPROFILE", "HOME", "TEMP", "TMP")}
        mine |= {str(Path.home()), str(ROOT), os.getcwd(), str(tmp)}
        fields = json.dumps({k: v for k, v in p.items() if k != "states"}).lower().encode("utf-8")
        for s in sorted(m for m in mine if len(m) >= 3):
            low = s.lower().encode("utf-8")
            check(low not in fields and low not in states.lower(), "the payload does not hold this PC's name or folder (%d characters)" % len(s))
        check(not re.search(rb"[A-Za-z]:[\\/]|\\\\", fields), "no path in the fields")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    wiring()
    unit(args.cxx)


if __name__ == "__main__":
    main()
