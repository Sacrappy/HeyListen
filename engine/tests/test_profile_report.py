"""The detailed performance profile (gpu/vk/profile_report.h, gpu/vk/profile_run.cpp, perf_report.cpp profile_queue, the sender in host/hos_crash_dialog.c), no game, no GPU.

- tests/test_profile_report.cpp: the module classifier on fake ranges, the thread roles, the hitch tagging of samples, the tables'
  caps, the JSON, the batching and the gating.
- The payload against the documented schema (docs/performance-statistics.md): its fields, their types, the caps (maps, threads,
  top_guest, top_engine, passes), and nothing of the person (checked against this PC's own names and folders).
- The wiring in the source: sampled without a lock or an allocation between the suspend and the resume, queued every five minutes
  and at the end, only with the row on and an address and without HL_TELEMETRY_PROFILE=0 (or HL_PROFILE_LOCAL), sent by the host to
  /telemetry/profile, deleted with the performance summaries when the row goes off, and documented.

    python -I -B tests/test_profile_report.py [--cxx g++]
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

TOP = ["v", "version", "release", "build_id", "vendor_id", "driver", "gpu", "cpu", "os", "maps"]
MODULES = ["game_lifted", "native_engine", "renderer", "host", "audio", "driver", "os", "runtime", "other"]
ROLES = ["game", "render", "present", "compile", "upload", "audio", "other"]
KINDS = ["shadow", "main", "post", "ui", "upscale", "framegen", "hdr", "copy", "compute", "other"]


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


def count(v):
    return isinstance(v, int) and not isinstance(v, bool) and v >= 0


def ms(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and v >= 0


def validate(p, what):
    check(list(p)[:6] == TOP[:6] and set(p) <= set(TOP) and list(p)[-1] == "maps", what + ": the top level's fields are %s" % ", ".join(TOP))
    check("first" not in p, what + ": no 'first'")
    check(p["v"] == 1 and isinstance(p["release"], bool) and re.fullmatch(r"[0-9a-f]{16}", p["build_id"]) and count(p["vendor_id"]),
          what + ": v, release, build_id (16 lowercase hex), vendor_id")
    for k in ("version", "driver", "gpu", "cpu", "os"):
        if k in p:
            check(isinstance(p[k], str) and len(p[k]) <= 120, what + ": %s is a plain name of at most 120 characters" % k)
    maps = p["maps"]
    check(isinstance(maps, list) and len(maps) <= 32, what + ": at most 32 maps")
    for m in maps:
        check(set(m) <= {"map", "seconds", "frames", "hitch_frames", "cpu", "gpu"} and list(m)[:4] == ["map", "seconds", "frames", "hitch_frames"],
              what + ": a map's fields")
        check(isinstance(m["map"], str) and m["map"] and ms(m["seconds"]) and count(m["frames"]) and count(m["hitch_frames"]), what + ": a map's totals")
        if "cpu" in m:
            c = m["cpu"]
            check(list(c) == ["hz", "threads", "top_guest", "top_engine"] and 50 <= c["hz"] <= 1000, what + ": the cpu block's fields and its rate")
            check(1 <= len(c["threads"]) <= 16, what + ": at most 16 thread entries")
            roles = [t["role"] for t in c["threads"]]
            check(len(set(roles)) == len(roles) and set(roles) <= set(ROLES), what + ": one entry a role, from the fixed list")
            for t in c["threads"]:
                check(list(t) == ["role", "count", "samples", "hitch_samples", "modules", "hitch_modules"], what + ": a thread entry's fields")
                check(t["count"] >= 1 and count(t["samples"]) and count(t["hitch_samples"]) and t["hitch_samples"] <= t["samples"], what + ": its counts")
                for key in ("modules", "hitch_modules"):
                    check(list(t[key]) == MODULES and all(count(v) for v in t[key].values()), what + ": %s has the nine module keys" % key)
                check(sum(t["modules"].values()) == t["samples"] and sum(t["hitch_modules"].values()) == t["hitch_samples"],
                      what + ": the modules add up to the samples")
            check(len(c["top_guest"]) <= 32 and len(c["top_engine"]) <= 32, what + ": top_guest and top_engine are capped at 32")
            for e in c["top_guest"]:
                check(list(e) == ["addr", "samples", "hitch_samples"] and re.fullmatch(r"0x[0-9a-f]+", e["addr"]) and count(e["samples"]) and
                      count(e["hitch_samples"]) and e["hitch_samples"] <= e["samples"], what + ": a top_guest entry")
            for e in c["top_engine"]:
                check(list(e)[0] == "rva" and set(e) <= {"rva", "module", "samples", "hitch_samples"} and re.fullmatch(r"0x[0-9a-f]+", e["rva"]) and
                      e.get("module", "vk") == "vk" and count(e["samples"]) and count(e["hitch_samples"]), what + ": a top_engine entry")
            sam = [e["samples"] for e in c["top_guest"]]
            check(sam == sorted(sam, reverse=True), what + ": the most samples first")
        if "gpu" in m:
            g = m["gpu"]
            check(list(g) == ["frame_ms_sum", "passes"] and ms(g["frame_ms_sum"]) and len(g["passes"]) <= 24, what + ": the gpu block, at most 24 passes")
            for e in g["passes"]:
                check(list(e) == ["kind", "key", "count", "ms_sum", "ms_max", "hitch_ms_sum"] and e["kind"] in KINDS and
                      re.fullmatch(r"[0-9a-f]{16}", e["key"]) and count(e["count"]) and ms(e["ms_sum"]) and ms(e["ms_max"]) and ms(e["hitch_ms_sum"]),
                      what + ": a pass entry")


def wiring():
    cpp = source("gpu/vk/perf_report.cpp")
    run = source("gpu/vk/profile_run.cpp")
    check("pf::why_not(opts.send_perf == 1, telemetry_available(), getenv(HOS_TELEMETRY_PROFILE_ENV))" in function(cpp, "bool profile_gate_telemetry() {"),
          "the telemetry copy needs the row, an address and the switch (the row decides first)")
    check("pf::local_on(getenv(\"HL_PROFILE_LOCAL\"))" in cpp, "HL_PROFILE_LOCAL=1 turns the local copy on")
    queue = function(cpp, "void profile_queue(bool final) {")
    check('"profile_pending"' in queue and '"profile_local"' in queue and "pf::payload_json(meta, sub)" in queue,
          "a batch goes to profile_pending and profile_local as the payload")
    check("meta.build_id" in queue and "meta.vendor_id = dev.props.vendorID" in queue and "meta.driver = machine.driver" in queue,
          "the payload's meta is the version, the release flag, the build id, the vendor, the driver and the plain names")
    check("profile_queue(true)" in function(cpp, "void at_exit() {") and "profile_queue(true)" in function(cpp, "void perf_report_final(int event) {"),
          "the last batch is queued at the exit and at the window's close")
    check('pf::batch_seconds(getenv("HL_PROFILE_BATCH_S"))' in cpp and "kBatchSeconds = 300" in source("gpu/vk/profile_report.h"), "a batch every five minutes (HL_PROFILE_BATCH_S)")
    off = function(cpp, "void switch_off() {")
    check('"profile_pending"' in off, "turning the row off deletes the profile batches too")
    check("kMaxProfilePending = 8" in cpp and re.search(r"HOS_PROFILE_MAX_PENDING = 8,", source("host/hos_perf_send.h")),
          "the renderer and the sender agree on how many batches wait")
    # the sampler: between the suspend and the resume nothing but the context's read
    sample = function(run, "void sample_thread(ThreadRec& t, SampledPc& out) {")
    between = sample[sample.index("SuspendThread("):sample.index("ResumeThread(")]
    check(not re.search(r"\b(new|malloc|logf|printf|lock|push_back|emplace|std::)\b", between) and "GetThreadContext(" in between,
          "between the suspend and the resume the sampler only reads the context (no allocation, no log, no lock)")
    check("THREAD_PRIORITY_ABOVE_NORMAL" in run and "QueryThreadCycleTime(" in run, "the sampler samples a thread for each share of its CPU time, above normal priority")
    check("pf::sample_hz(getenv(\"HL_PROFILE_HZ\"))" in cpp, "the rate is HL_PROFILE_HZ")
    api = source("gpu/vk/vk_api.h")
    check(re.search(r"#define MKWVK_API_VERSION 1[89]\b", api) and "profile_pc" in api and "build_id" in api and "profile_threads" in api, "the host's profile_pc and build_id are in the API")
    check("hos_profile_pc" in source("host/rx_backend.c"), "rx_backend.c fills profile_pc")
    dialog = source("host/hos_crash_dialog.c")
    sender = function(dialog, "int hos_profile_send_run(const char *dir) {")
    check("hos_telemetry_profile_on(getenv(HOS_TELEMETRY_PROFILE_ENV))" in sender and "&profile_usage" in sender, "the sender checks the switch")
    check('"/telemetry/profile"' in source("host/hos_telemetry.h"), "to <address>/telemetry/profile")
    check("hos_profile_send_run(dir)" in dialog[dialog.index("perf_send_thread(void *p)"):], "the start's sender thread sends them after the zone usage")
    docs = (ROOT.parent / "docs/performance-statistics.md").read_text(encoding="utf-8")
    check("/telemetry/profile" in docs and "HL_TELEMETRY_PROFILE" in docs and "profile_pending" in docs and "HL_PROFILE_LOCAL" in docs and "HL_PROFILE_HZ" in docs,
          "documented: the route, the switches and the payload")
    guide = (ROOT.parent / "docs/USER_GUIDE.md").read_text(encoding="utf-8")
    check("profile_pending" in guide and "HL_TELEMETRY_PROFILE=0" in guide, "the user guide's privacy section says what the profile is")
    check("HL_TELEMETRY_PROFILE" in source("docs/development.md"), "the development guide lists the switch")


def unit(cxx):
    with tempfile.TemporaryDirectory(prefix="profile-report-") as t:
        tmp = Path(t)
        exe = tmp / ("unit" + EXE)
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "gpu/vk"),
                        str(ROOT / "tests/test_profile_report.cpp"), "-o", str(exe)], check=True, timeout=300)
        r = subprocess.run([str(exe)], capture_output=True, timeout=120)
        print(r.stdout.decode().strip())
        check(r.returncode == 0, "the classifier, the roles, the hitch tagging, the caps, the JSON, the batches and the gating" +
              (": " + r.stderr.decode("utf-8", "replace").strip() if r.returncode else ""))

        r = subprocess.run([str(exe), "payload"], capture_output=True, timeout=120)
        body = r.stdout.decode("utf-8")
        p = json.loads(body)
        validate(p, "payload")
        check(p["version"] == "v0.4.3" and p["release"] is True and p["build_id"] == "0123456789abcdef" and p["vendor_id"] == 0x10DE and
              p["gpu"] == "NVIDIA GeForce RTX 5070", "the head's values")
        first, second = p["maps"]
        check(first["map"] == "Kokiri Forest (child)" and first["frames"] == 18000 and first["hitch_frames"] == 12 and first["seconds"] == 312.5, "a map's totals")
        check("cpu" in first and "gpu" in first and "cpu" not in second and "gpu" not in second, "a map with nothing but frames has neither block")
        check([e["addr"] for e in first["cpu"]["top_guest"]][:1] != [] and len(first["cpu"]["top_guest"]) == 10, "the guest functions")
        check(any(e.get("module") == "vk" for e in first["cpu"]["top_engine"]), "the renderer's locations carry their module")

        r = subprocess.run([str(exe), "caps"], capture_output=True, timeout=120)
        body_caps = r.stdout.decode("utf-8")
        pc = json.loads(body_caps)
        validate(pc, "caps")
        check(all(len(m["cpu"]["top_guest"]) == 32 and len(m["cpu"]["top_engine"]) == 32 and len(m["gpu"]["passes"]) == 24 for m in pc["maps"]),
              "over their caps the tables are cut to 32, 32 and 24")
        check(len(body_caps) < 256 * 1024, "a payload of full maps is under the 256 KiB the sender takes")

        # nothing of the person: not this PC's user name, folders or host name
        mine = {os.environ.get(k, "") for k in ("USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME", "USERPROFILE", "HOME", "TEMP", "TMP")}
        mine |= {str(Path.home()), str(ROOT), os.getcwd(), str(tmp)}
        low = body.lower() + body_caps.lower()
        for s in sorted(m for m in mine if len(m) >= 3):
            check(s.lower() not in low, "the payload does not hold this PC's name or folder (%d characters)" % len(s))
        check(not re.search(r"[A-Za-z]:[\\/]|\\\\", body + body_caps), "no path in the body")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    unit(args.cxx)
    wiring()


if __name__ == "__main__":
    main()
