"""The crash reports, grouped by how the game ended (the renderer rework plan's M0: every report in a named group).

    python -I scripts/crash_groups.py [--state open|closed|all] [--limit 1000] [--no-attachments] [--markdown] [--json]
    python -I scripts/crash_groups.py --files report.txt crash-20261116T134514Z.txt hookshot_log.txt ...

Reads Sacrappy/HeyListen's issues on GitHub (the public API), keeps the ones the crash window titled
("HeyListen closed unexpectedly ...", "HeyListen couldn't start ...") and reads each one's text: the
crash and unclean-end records (host/hos_crash_record.c), the crash window's report (host/hos_crash_dialog.c) and the end of
hookshot_log.txt. Of a log over 2 MiB only the first 64 KiB and the last 1 MiB are fetched. --files reads local files
as one report each, with no network.

A group is named by three things, none of which changes from one build to the next:

- how the run ended: the record's reason and exception code, or what the log's end says (device lost, the GPU hung),
  or that nothing recorded the end;
- where: the kind of module the faulting address is in (the graphics driver, the renderer, the game's lifted code, a
  system library);
- memory at the end, when the log shows it short: VRAM at its allowance, Windows at its commit limit.

Inside a group, the reports that share a build and a faulting module+offset are listed together: an offset means the
same code only within one build. A fault in the game's lifted code is listed by the game's own address of its caller
(the record's first guest_backtrace), which is the same in every build; newer records also name each frame
(f_<guest address>, or the host function), and the crash report's "signature" field is the top frame's resolved
name, the key the telemetry relay groups on. A report with several records counts once in each group one of them
falls in. It counts reports, not players, and nothing is sent anywhere. Standard library only.
"""
import argparse
import collections
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request



def section(body: str, name: str) -> str:
    m = re.search(r"### " + re.escape(name) + r"[^\n]*\n+(.*?)(\n### |\Z)", body or "", re.S)
    return m.group(1).strip() if m else ""


def classify(gpu: str):
    g = gpu.lower()
    if re.search(r"nvidia|rtx|gtx|geforce", g):
        cls = ("RTX 50" if re.search(r"rtx ?50\d0", g) else "RTX 40" if re.search(r"rtx ?40\d0", g) else
               "RTX 30" if re.search(r"rtx ?30\d0", g) else "RTX 20" if re.search(r"rtx ?20\d0", g) else
               "GTX 10/16 (no mesh shaders)" if re.search(r"gtx ?1[06]\d0", g) else "NVIDIA other")
        return "NVIDIA", cls
    # (whole words: "steamdeck" holds "amd", "finally" "ally"; a Deck's Vulkan name is "AMD Custom GPU 0932 (RADV VANGOGH)")
    handheld = r"\b(780m|680m|890m|860m|8060s|z1|legion|ally)\b|steam ?deck|vangogh|custom gpu 0(405|932)"
    if re.search(r"\bamd\b|radeon|\brx ?\d|rdna|" + handheld, g):
        cls = ("AMD APU / handheld" if re.search(handheld + r"|\bapu\b|vega", g) else
               "RX 9000" if re.search(r"9070|9060", g) else "RX 7000" if re.search(r"7900|7800|7700|7600", g) else
               "RX 6000" if re.search(r"6[5-9]\d0|6600|6650|6700|6800|6900", g) else
               "RX 5000 or older (no mesh shaders)" if re.search(r"5[5-7]00|rx ?5[78]0|rx ?4[78]0", g) else "AMD other")
        return "AMD", cls
    if re.search(r"intel|arc\b|iris|uhd", g):
        return "Intel", "Intel"
    return "unknown", "unknown"


class hardware_report:   # (the names the rest of the file uses)
    section = staticmethod(section)
    classify = staticmethod(classify)

TITLES = ("closed unexpectedly", "couldn't start", "HeyListen stopped")
# (a crash in the graphics driver's own code: hos_crash_driver_module's list in host/hos_crash_dialog.h, kept the same
#  by tests/test_crash_groups.py)
DRIVER_MODULES = ("nvoglv64", "nvgpucomp", "nvvk", "amdvlk", "amdxc", "igvk", "igc", "igd", "libnvidia-", "libvulkan_",
                  "libGLX_nvidia", "libamdvlk")
RENDERER_MODULES = ("hookshot_vk", "libhookshot_vk")
GAME_MODULES = ("heylisten.exe", "heylisten", "hookshot.exe", "hookshot")
EXCEPTIONS = {
    0xC0000005: "access violation",
    0x20474343: "uncaught C++ exception",   # (llvm-mingw's code for one nothing caught)
    0xC0000017: "out of memory (STATUS_NO_MEMORY)",
    0xC00000FD: "stack overflow",
    0xC000001D: "illegal instruction",
    0xC0000094: "integer divide by zero",
    0xC0000409: "fail-fast (stack buffer overrun or abort)",
    0x80000003: "breakpoint (an assert or abort)",
}
SIGNALS = {4: "illegal instruction (SIGILL)", 6: "abort (SIGABRT)", 7: "bus error (SIGBUS)",
           8: "arithmetic error (SIGFPE)", 11: "access violation (SIGSEGV)"}
TAIL_BYTES, HEAD_BYTES, WHOLE_BYTES = 1 << 20, 64 << 10, 2 << 20
VRAM_FULL_MB = 64   # (within this of its allowance, VRAM counts as full)


# ---- reading one report's text ----

def records(text: str):
    """Every crash or unclean-end record in the text, as {field: value} with "kind" ("crash", "unclean") and
    "host_backtrace"/"guest_backtrace" as lists; the same run's record given twice (a report and its attachment) once"""
    out, seen = [], set()
    for m in re.finditer(r"^HeyListen (crash|unclean) record 1[ \t]*\r?\n((?:[a-z_]+: [^\n]*\n?)+)", text or "", re.M):
        r = {"kind": m.group(1), "host_backtrace": [], "guest_backtrace": []}
        for line in m.group(2).splitlines():
            key, _, value = line.partition(": ")
            value = value.strip()
            if key in ("host_backtrace", "guest_backtrace"):
                r[key].append(value)
            elif key != "guest_register":
                r.setdefault(key, value)
        ident = (r["kind"], r.get("run_id", ""), r.get("when", ""))
        if ident not in seen:
            seen.add(ident)
            out.append(r)
    return out


def log_facts(text: str):
    """What the end of a log says about how the run ended and the memory it had: {"device_lost", "hung", "commit_limit",
    "vram_used_mb", "vram_allowed_mb", "working_set_mb", "private_mb", "crash_line"}; the last of each memory line"""
    t = text or ""
    f = {"device_lost": "the GPU device was lost" in t or "VK_ERROR_DEVICE_LOST" in t,
         "hung": "FATAL: the GPU hung" in t,
         "commit_limit": bool(re.search(r"at the commit limit|commit limit near|Windows is out of memory|error 1455", t)),
         "vram_used_mb": None, "vram_allowed_mb": None, "working_set_mb": None, "private_mb": None, "crash_line": None}
    vram = re.findall(r"vk: VRAM: (\d+) MB used of (\d+) MB allowed", t)
    if vram:
        f["vram_used_mb"], f["vram_allowed_mb"] = int(vram[-1][0]), int(vram[-1][1])
    mem = re.findall(r"vk: memory: working set (\d+) MB \(peak \d+\), private (\d+) MB", t)
    if mem:
        f["working_set_mb"], f["private_mb"] = int(mem[-1][0]), int(mem[-1][1])
    crash = re.findall(r"CRASH: exception (0x[0-9a-fA-F]+) \((?:read|write|exec)? ?(0x[0-9a-fA-F]+)?[^)]*\) at (\S+)", t)
    if crash:
        f["crash_line"] = {"exception": crash[-1][0], "module_offset": crash[-1][2]}
    return f


def stage_of(title: str) -> str:
    """When, from the crash window's headline: "start", "shaders", "play", or "" when it does not say"""
    t = (title or "").lower()
    return ("start" if "during the start" in t else "shaders" if "prepared the shaders" in t else
            "play" if "while you were playing" in t else "")


def module_of(module_offset: str) -> str:
    """The module's name from "name+0xoffset", of a record line that may now end with the resolved function
    ("HeyListen.exe+0x12f44593 f_80580b38+0x1c4") ("" for "unknown")"""
    token = (module_offset or "").split()[0] if (module_offset or "").strip() else ""
    name = token.rsplit("+0x", 1)[0].strip()
    return "" if name in ("", "unknown") else name


def module_kind(name: str) -> str:
    """Whose code a module is: "graphics driver", "renderer", "game code", "system library", or "" for none"""
    n = (name or "").lower()
    if not n:
        return ""
    if any(n.startswith(d.lower()) for d in DRIVER_MODULES):
        return "graphics driver"
    if any(n.startswith(r) for r in RENDERER_MODULES):
        return "renderer"
    if n in GAME_MODULES:
        return "game code"
    return "system library"


def faulting(record) -> str:
    """The module the group is named by: the record's own, or for a fault raised inside a system library (a thrown
    exception passes through KERNELBASE; an abort through the C runtime) the first frame of ours or the driver's under it"""
    first = module_of(record.get("module_offset", "")) or (module_of(record["host_backtrace"][0]) if record["host_backtrace"] else "")
    if module_kind(first) != "system library":
        return first
    for frame in record["host_backtrace"][1:]:
        if module_kind(module_of(frame)) not in ("", "system library"):
            return module_of(frame)
    return first


def ending(record) -> str:
    """How the record says the run ended, in words that hold across builds"""
    if record["kind"] == "unclean":
        return "ended with no crash record"
    reason = record.get("reason", "unknown")
    if reason in ("device lost", "Vulkan fatal", "hos_fatal"):
        return {"device lost": "GPU device lost", "Vulkan fatal": "Vulkan fatal error", "hos_fatal": "fatal error (hos_fatal)"}[reason]
    try:
        code = int(record.get("exception", "0x0"), 16)
    except ValueError:
        code = 0
    try:
        signal = int(record.get("signal", "0"))
    except ValueError:
        signal = 0
    if code:
        return EXCEPTIONS.get(code, "exception 0x%x" % code)
    if signal:
        return SIGNALS.get(signal, "signal %d" % signal)
    return "crash, cause not recorded" if reason == "unknown" else reason


OOM_AT_LIMIT = "out of memory (uncaught C++ exception at the commit limit)"


def memory_state(facts) -> str:
    """"commit limit", "VRAM full", both, or "" """
    out = []
    if facts["commit_limit"]:
        out.append("commit limit")
    if facts["vram_allowed_mb"] and facts["vram_used_mb"] is not None and facts["vram_used_mb"] + VRAM_FULL_MB >= facts["vram_allowed_mb"]:
        out.append("VRAM full")
    return ", ".join(out)


def events(title: str, text: str):
    """One report's events, one a record (or one from the log's end or the form when it holds no record):
    [{"end", "where", "module", "memory", "build", "frame", "stage"}]. A device lost counts as that however the process
    then ended, and an uncaught C++ exception at the commit limit as out of memory (#1056)."""
    facts = log_facts(text)
    memory = memory_state(facts)
    found = records(text)
    version = hardware_report.section(text, "HeyListen version").splitlines()[0].strip() if hardware_report.section(text, "HeyListen version") else ""
    out = []
    for r in found:
        module = faulting(r)
        end = ending(r)
        if r["kind"] == "unclean" and facts["hung"]:
            end = "GPU hung (no submission finished)"
        elif r["kind"] == "unclean" and facts["device_lost"]:
            end = "GPU device lost"
        elif end == "uncaught C++ exception" and facts["commit_limit"]:
            end = OOM_AT_LIMIT
        frame = r.get("module_offset", "unknown")
        if frame == "unknown" and r["host_backtrace"]:
            frame = r["host_backtrace"][0]
        where = "" if end.startswith("GPU ") or r["kind"] == "unclean" else module_kind(module)
        if end.startswith("GPU "):
            frame = "unknown"   # (the address is the renderer's own report of the loss, not where it was lost)
        elif where == "game code" and r["guest_backtrace"]:
            frame = "the game's " + r["guest_backtrace"][0].split()[0]   # (the lifted code's caller: the same in every build)
        out.append({"end": end, "where": where, "module": module, "memory": memory,
                    "build": "" if frame.startswith("the game's ") else r.get("version", "") or version, "frame": frame})
    if not out:
        cause = re.search(r"What went wrong: ([^\n]*)", text or "")
        line = facts["crash_line"]
        if facts["hung"]:
            end = "GPU hung (no submission finished)"
        elif facts["device_lost"] or (cause and "graphics driver failed" in cause.group(1)):
            end = "GPU device lost"
        elif line:
            try:
                end = EXCEPTIONS.get(int(line["exception"], 16), "exception " + line["exception"].lower())
            except ValueError:
                end = "exception " + line["exception"]
            if end == "uncaught C++ exception" and facts["commit_limit"]:
                end = OOM_AT_LIMIT
        elif cause and "ran out of memory" in cause.group(1):
            end = "out of memory (the crash window's verdict)"
        elif cause and "without a crash report" in cause.group(1):
            end = "ended with no crash record"
        elif "ended without a crash report" in (text or ""):
            end = "ended with no crash record"
        else:
            end = "nothing in the report says how it ended"
        module = module_of(line["module_offset"]) if line and not end.startswith("GPU ") else ""
        out.append({"end": end, "where": module_kind(module), "module": module, "memory": memory, "build": version,
                    "frame": line["module_offset"] if line else "unknown"})
    stage = stage_of(title)
    for e in out:
        e["stage"] = stage
        if e["end"] == OOM_AT_LIMIT:   # (the name says it already)
            e["memory"] = ", ".join(m for m in e["memory"].split(", ") if m != "commit limit")
    return out


def group_name(e) -> str:
    name = e["end"]
    if e["where"]:
        name += " in the " + e["where"] + (" (%s)" % e["module"] if e["where"] in ("graphics driver", "system library") else "")
    if e["memory"]:
        name += ", " + e["memory"]
    return name


# ---- reading GitHub's issues ----

REPO = "Sacrappy/HeyListen"


def api(path):
    """GitHub's public REST API (no sign-in: the repository is public; at most 60 requests an hour without one)"""
    req = urllib.request.Request("https://api.github.com" + path, headers={"User-Agent": "heylisten-crash-groups",
                                                                           "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def crash_issues(state, limit):
    """The issues the crash window titled, newest first: [{number, title, body, assets, url}]. The text of a pasted report is
    in the issue's body; a file attached to an issue is not in the API, so assets is empty (use --files for those)"""
    out, page = [], 1
    while len(out) < limit:
        items = api("/repos/%s/issues?state=%s&per_page=100&page=%d" % (REPO, urllib.parse.quote(state), page)) or []
        out += [{"number": i["number"], "title": i.get("title") or "", "body": i.get("body") or "", "assets": [],
                 "url": i.get("html_url") or ""}
                for i in items if "pull_request" not in i and any(t in (i.get("title") or "") for t in TITLES)]
        if len(items) < 100:
            break
        page += 1
    return out[:limit]


def attachments(issue, failed):
    """Nothing: GitHub's API does not list an issue's attached files"""
    return []


# ---- grouping ----

def group(reports):
    """reports: [{"id", "title", "text"}]. Returns the groups, the one with the most reports first:
    [{"name", "reports": [id], "events", "vendors", "stages", "frames": {(build, frame): [id]}}]"""
    groups = {}
    for rep in reports:
        gpu = hardware_report.section(rep["text"], "Graphics card")
        vendor = hardware_report.classify(gpu + " " + rep["title"])[0]
        for e in events(rep["title"], rep["text"]):
            g = groups.setdefault(group_name(e), {"name": group_name(e), "reports": [], "events": 0,
                                                 "vendors": collections.Counter(), "stages": collections.Counter(),
                                                 "frames": collections.OrderedDict()})
            g["events"] += 1
            if rep["id"] not in g["reports"]:
                g["reports"].append(rep["id"])
                g["vendors"][vendor] += 1
                g["stages"][e["stage"] or "not said"] += 1
            ids = g["frames"].setdefault((e["build"] if e["frame"].startswith("the game's ") else e["build"] or "build not known",
                                          e["frame"]), [])
            if rep["id"] not in ids:
                ids.append(rep["id"])
    return sorted(groups.values(), key=lambda g: (-len(g["reports"]), g["name"]))


def counts(counter) -> str:
    return ", ".join("%s %d" % kv for kv in counter.most_common())


def show(groups, total, link, markdown):
    ref = (lambda i: "[#%s](%s)" % (i, link(i))) if markdown and link else (lambda i: "#%s" % i if link else str(i))
    lines = ["%d crash reports in %d groups" % (total, len(groups)), ""]
    for n, g in enumerate(groups, 1):
        head = "%d. %s: %d report%s" % (n, g["name"], len(g["reports"]), "" if len(g["reports"]) == 1 else "s")
        lines.append(("**%s**" % head) if markdown else head)
        indent = "   "
        lines.append("%s- by card: %s; when: %s; records: %d" % (indent, counts(g["vendors"]), counts(g["stages"]), g["events"]))
        lines.append("%s- reports: %s" % (indent, ", ".join(ref(i) for i in g["reports"])))
        shared = [(k, ids) for k, ids in g["frames"].items() if k[1] != "unknown"]
        for (build, frame), ids in sorted(shared, key=lambda kv: -len(kv[1]))[:8]:
            lines.append("%s- %s%s: %s" % (indent, build + " at " if build else "called from ",
                                           ("`%s`" % frame) if markdown else frame, ", ".join(ref(i) for i in ids)))
        if len(shared) > 8:
            lines.append("%s- and %d more build and offset pairs" % (indent, len(shared) - 8))
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--state", default="open", choices=("all", "open", "closed"))
    ap.add_argument("--limit", type=int, default=1000, help="at most this many crash reports (default 1000)")
    ap.add_argument("--no-attachments", action="store_true", help="read the issues' own text only")
    ap.add_argument("--files", nargs="+", metavar="FILE", help="group these local files, one report each, with no network")
    ap.add_argument("--markdown", action="store_true", help="a list to paste into an issue or the wiki, the numbers as links")
    ap.add_argument("--json", action="store_true", help="the groups as JSON")
    a = ap.parse_args()
    failed, link = [], None
    if a.files:
        reports = []
        for path in a.files:
            with open(path, encoding="utf-8", errors="replace") as f:
                reports.append({"id": os.path.basename(path), "title": "", "text": f.read()})
    else:
        try:
            found = crash_issues(a.state, a.limit)
        except Exception as e:  # (no network, a rate limit: the message says)
            print(e, file=sys.stderr)
            return 1
        urls = {i["number"]: i["url"] for i in found}
        link = urls.get
        reports = [{"id": i["number"], "title": i["title"],
                    "text": "\n".join([i["body"]] + ([] if a.no_attachments else attachments(i, failed)))} for i in found]
    groups = group(reports)
    if a.json:
        print(json.dumps([{"name": g["name"], "reports": g["reports"], "records": g["events"], "vendors": dict(g["vendors"]),
                           "stages": dict(g["stages"]),
                           "frames": [{"build": k[0], "frame": k[1], "reports": v} for k, v in g["frames"].items()]}
                          for g in groups], indent=1))
    else:
        print(show(groups, len(reports), link, a.markdown))
    for f in failed:
        print("not read: " + f, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
