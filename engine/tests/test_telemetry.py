"""The opt-in reports' rules (host/hos_telemetry.h, host/hos_perf_send.h, the sender in host/hos_crash_dialog.c), with no game.

- The address: empty by default (nx2/addresses.ini), so nothing can be sent; https only; HL_TELEMETRY_URL for a test; the one
  exception, a plain http:// address on this PC itself, needs HL_TELEMETRY_TEST_HTTP=1 as well; a bad address is no address.
- The opt-in rows are off by default: no line, 0, or anything but 1 is off.
- The sender (hos_perf_send_run) against a stand-in for curl: nothing is sent with the row off, with no address, or with
  nothing waiting; with both, the summaries go oldest first to <address>/telemetry/perf, one a connection, the ones the server
  took or refused are deleted (a copy of those taken kept in perf_sent), a busy server is left alone until its Retry-After (and
  doubled when it is busy again), a server not reached keeps them for the next start, 12 a start at most, the newest 48 kept.
- The pipeline state records' sender (hos_pipes_send_run): the same rules from pipelines_pending to /telemetry/pipelines, off by itself\n  with HL_TELEMETRY_PIPELINES=0, 2 a start, the newest 4 kept, a batch up to 1.5 MiB, the last 3 sent kept in pipelines_sent.
- The per-zone shader and pipeline usage's sender (hos_zones_send_run): the same rules from zones_pending to /telemetry/zones, off by\n  itself with HL_TELEMETRY_ZONES=0, 2 a start, the newest 4 kept, a batch up to 512 KiB, the last 3 sent kept in zones_sent.
- nx2/addresses.py: the build stamps an https telemetry address only, and nothing when it is empty (the default).

    python -I -B tests/test_telemetry.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

RUN_ALL_ARGS = ["--cc", "{cc}"]
ROOT = Path(__file__).resolve().parents[1]
EXE = ".exe" if os.name == "nt" else ""

PURE = r"""
#include "hos_telemetry.h"
#include "hos_perf_send.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    (void)argc;
    const char *cmd = argv[1];
    if (!strcmp(cmd, "urlok")) printf("%d\n", hos_telemetry_url_ok(argv[2], atoi(argv[3])));
    else if (!strcmp(cmd, "base")) {   /* env url, test http, stamp url ("-": unset) */
        char out[400];
        const int ok = hos_telemetry_base(strcmp(argv[2], "-") ? argv[2] : NULL, strcmp(argv[3], "-") ? argv[3] : NULL,
                                          strcmp(argv[4], "-") ? argv[4] : NULL, out, sizeof out);
        printf("%d|%s\n", ok, out);
    } else if (!strcmp(cmd, "on")) printf("%d\n", hos_telemetry_opt_on(argv[2], argv[3]));
    else if (!strcmp(cmd, "zoneson")) printf("%d\n", hos_telemetry_zones_on(strcmp(argv[2], "-") ? argv[2] : NULL));
    else if (!strcmp(cmd, "stamp")) {
        char out[400];
        const int ok = hos_telemetry_stamp_value(argv[2], argv[3], out, sizeof out);
        printf("%d|%s\n", ok, out);
    } else if (!strcmp(cmd, "name")) {
        int64_t a = 0, n = 0;
        const int ok = hos_perf_name(argv[2], &a, &n);   /* (the call first: the arguments' order is the compiler's) */
        printf("%d|%lld|%lld\n", ok, (long long)a, (long long)n);
    } else if (!strcmp(cmd, "older")) printf("%d\n", hos_perf_older(argv[2], argv[3]));
    else if (!strcmp(cmd, "answer")) printf("%d\n", hos_perf_answer(atoi(argv[2])));
    else if (!strcmp(cmd, "retry")) printf("%ld\n", hos_perf_header_retry(argv[2]));
    else if (!strcmp(cmd, "wait")) printf("%ld\n", hos_perf_busy_wait(atol(argv[2]), atol(argv[3])));
    else if (!strcmp(cmd, "next")) {
        int64_t at = 0;
        long wait = 0;
        const int ok = hos_perf_next_parse(argv[2], &at, &wait);
        printf("%d|%lld|%ld\n", ok, (long long)at, wait);
    } else if (!strcmp(cmd, "number")) printf("%ld\n", hos_perf_env_number(strcmp(argv[2], "-") ? argv[2] : NULL, 5, 1, 10));
    return 0;
}
"""

SENDER = r"""
#include <stdio.h>
#include <stdlib.h>
int hos_perf_send_run(const char *dir);
int hos_pipes_send_run(const char *dir);
int hos_zones_send_run(const char *dir);
int hos_profile_send_run(const char *dir);
void hos_log(const char *fmt, ...) { (void)fmt; }
void hos_log_flush(void) {}
const char *hos_version(void) { return "vtest-1"; }
const char *hos_lift_key(void) { return "testlift01"; }
int hos_crash_test_box(const char *t, const char *x, const char *const *b, int n) { (void)t, (void)x, (void)b; return n - 1; }
int hos_crash_test_clipboard(const char *t) { (void)t; return 1; }
int hos_crash_test_open(const char *w) { (void)w; return 1; }
int main(int argc, char **argv) {
    printf("%d\n", argc > 2 && argv[2][0] == 'p' ? hos_pipes_send_run(argv[1])
                              : argc > 2 && argv[2][0] == 'z' ? hos_zones_send_run(argv[1])
                              : argc > 2 && argv[2][0] == 'f' ? hos_profile_send_run(argv[1]) : hos_perf_send_run(argv[1]));
    return 0;
}
"""

# A stand-in for curl: records the address and the payload's file, and answers by CURL_ANSWER (sent, busy, refused, down)
CURL = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *out = NULL, *head = NULL, *data = NULL, *url = NULL, *proto = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--output")) out = argv[++i];
        else if (!strcmp(argv[i], "--dump-header")) head = argv[++i];
        else if (!strcmp(argv[i], "--data-binary")) data = argv[++i] + 1;
        else if (!strcmp(argv[i], "--proto")) proto = argv[++i];
        else if (!strcmp(argv[i], "--max-time") || !strcmp(argv[i], "--connect-timeout") || !strcmp(argv[i], "--header") ||
                 !strcmp(argv[i], "--user-agent")) i++;
        else if (argv[i][0] != '-') url = argv[i];
    }
    const char *calls = getenv("CURL_CALLS"), *answer = getenv("CURL_ANSWER");
    FILE *c = calls ? fopen(calls, "ab") : NULL;
    if (c) {
        char body[200] = "";
        FILE *d = data ? fopen(data, "rb") : NULL;
        if (d) body[fread(body, 1, 199, d)] = 0, fclose(d);
        fprintf(c, "%s %s %.60s\n", url ? url : "?", proto ? proto : "?", body);
        fclose(c);
    }
    if (answer && !strcmp(answer, "down")) return 7;
    FILE *h = head ? fopen(head, "wb") : NULL;
    if (!h) return 2;
    if (answer && !strcmp(answer, "busy")) fputs("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7200\r\n\r\n", h);
    else if (answer && !strcmp(answer, "busy-plain")) fputs("HTTP/1.1 503 Service Unavailable\r\n\r\n", h);
    else if (answer && !strcmp(answer, "refused")) fputs("HTTP/1.1 400 Bad Request\r\n\r\n", h);
    else fputs("HTTP/1.1 202 Accepted\r\n\r\n", h);
    fclose(h);
    FILE *o = out ? fopen(out, "wb") : NULL;
    if (o) fputs("{}", o), fclose(o);
    return 0;
}
"""


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)
    print("PASS:", what)


def build(cc, tmp):
    (tmp / "pure.c").write_text(PURE, encoding="utf-8")
    (tmp / "sender.c").write_text(SENDER, encoding="utf-8")
    (tmp / "curl.c").write_text(CURL, encoding="utf-8")
    flags = ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(ROOT / "host")]
    subprocess.run([cc, *flags, str(tmp / "pure.c"), "-o", str(tmp / ("pure" + EXE))], check=True, timeout=120)
    subprocess.run([cc, "-std=gnu11", str(tmp / "curl.c"), "-o", str(tmp / ("curl" + EXE))], check=True, timeout=120)
    libs = ["-lole32", "-lshell32", "-luser32", "-ladvapi32", "-ldbghelp"] if os.name == "nt" else ["-pthread", "-ldl"]
    subprocess.run([cc, "-std=gnu11", "-O1", "-DHOS_CRASH_DIALOG_TEST", "-I", str(ROOT / "host"),
                    str(tmp / "sender.c"), str(ROOT / "host/hos_crash_dialog.c"), str(ROOT / "host/hos_crash_record.c"),
                    str(ROOT / "host/hos_crash_names.c"), str(ROOT / "host/hos_report_run.c"), "-o", str(tmp / ("sender" + EXE)),
                    *libs], check=True, timeout=300)


def pure(tmp, *args):
    r = subprocess.run([str(tmp / ("pure" + EXE)), *map(str, args)], capture_output=True, text=True, encoding="utf-8", timeout=30)
    assert r.returncode == 0, (args, r.stderr)
    return r.stdout.rstrip("\n")


def rules(tmp):
    # the address
    for url, loop, want in (("https://t.example", 0, 1), ("https://t.example/", 0, 1), ("https://t.example:8443/x", 0, 1),
                            ("http://t.example", 0, 0), ("ftp://t.example", 0, 0), ("https://", 0, 0), ("https:///x", 0, 0),
                            ("https://a b", 0, 0), ("https://user:pw@t.example", 0, 0), ("https://t.example\\x", 0, 0),
                            ("", 0, 0), ("http://127.0.0.1:8080", 0, 0), ("http://127.0.0.1:8080", 1, 1), ("http://localhost", 1, 1),
                            ("http://localhost:99/x", 1, 1), ("http://[::1]:5000", 1, 1), ("http://127.0.0.1.evil.example", 1, 0),
                            ("http://localhostx", 1, 0), ("http://example.com", 1, 0), ("http://127.0.0.1:80a", 1, 0),
                            ("http://127.0.0.1@evil.example", 1, 0)):
        check(int(pure(tmp, "urlok", url, loop)) == want, "address %r (loopback http allowed %d): %d" % (url, loop, want))
    check(pure(tmp, "base", "-", "-", "-") == "0|", "no address anywhere: none (the default: nothing can be sent)")
    check(pure(tmp, "base", "-", "-", "") == "0|", "an empty stamp: none")
    check(pure(tmp, "base", "-", "-", "https://t.example/") == "1|https://t.example", "the stamp's address, the slash taken off")
    check(pure(tmp, "base", "https://other.example", "-", "https://t.example") == "1|https://other.example", "HL_TELEMETRY_URL over the stamp")
    check(pure(tmp, "base", "http://127.0.0.1:8080", "-", "https://t.example") == "0|",
          "a plain http override without the test switch is no address, and the stamp's does not stand in")
    check(pure(tmp, "base", "http://127.0.0.1:8080", "0", "-") == "0|", "the test switch must be 1")
    check(pure(tmp, "base", "http://127.0.0.1:8080", "1", "-") == "1|http://127.0.0.1:8080", "http to this PC: only with HL_TELEMETRY_TEST_HTTP=1")
    check(pure(tmp, "base", "-", "1", "http://127.0.0.1:8080") == "0|", "the test switch never lets a stamped http address through")
    check(pure(tmp, "base", "http://example.com", "1", "-") == "0|", "the test switch never lets another host through")
    check(pure(tmp, "stamp", "version=v1\ntelemetry=https://t.example\n", "telemetry") == "1|https://t.example", "the stamp's key")
    check(pure(tmp, "stamp", "version=v1\n", "telemetry") == "0|", "no key: none")
    # the opt-in rows: off unless a line says 1
    for text, want in (("", 0), ("send_performance_stats=0\n", 0), ("send_performance_stats=1\n", 1), ("send_performance_stats = 1\r\n", 1),
                       ("send_performance_stats=2\n", 0), ("send_performance_stats=10\n", 0), ("send_performance_stats=on\n", 0),
                       ("#send_performance_stats=1\n", 0), ("x_send_performance_stats=1\n", 0), ("send_crash_reports=1\n", 0),
                       ("send_performance_stats=1\nsend_performance_stats=0\n", 0)):
        check(int(pure(tmp, "on", text, "send_performance_stats")) == want, "send_performance_stats in %r: %d" % (text, want))
    check(int(pure(tmp, "on", "send_crash_reports=1\n", "send_crash_reports")) == 1 and
          int(pure(tmp, "on", "send_performance_stats=1\n", "send_crash_reports")) == 0, "each row has its own key")
    # the separate switches: off only when they are exactly "0"
    check([int(pure(tmp, "zoneson", v)) for v in ("-", "", "0", "1", "00", "off")] == [1, 1, 0, 1, 1, 1],
          "the zone usage is on unless HL_TELEMETRY_ZONES is exactly 0")
    # the sender's decisions
    check(pure(tmp, "name", "1763366400-0.json") == "1|1763366400|0" and pure(tmp, "name", "x.json").startswith("0|"), "a summary's name")
    check(pure(tmp, "name", "1763366400-0.json.tmp").startswith("0|") and pure(tmp, "name", "-1-0.json").startswith("0|"), "only whole names")
    check(pure(tmp, "older", "100-2.json", "100-10.json") == "1" and pure(tmp, "older", "101-0.json", "100-9.json") == "0", "oldest first")
    check([int(pure(tmp, "answer", s)) for s in (200, 202, 400, 404, 429, 500, 503)] == [0, 0, 1, 1, 2, 2, 2], "the answers: sent, refused, busy")
    check(int(pure(tmp, "retry", "HTTP/1.1 429 X\r\nRetry-After: 120\r\n\r\n")) == 120, "Retry-After in seconds")
    check(int(pure(tmp, "retry", "HTTP/1.1 429 X\r\nRetry-After: Wed, 21 Oct 2026 07:28:00 GMT\r\n\r\n")) == -1, "an HTTP date is not read")
    check(int(pure(tmp, "retry", "HTTP/1.1 301 X\r\nRetry-After: 5\r\n\r\nHTTP/1.1 200 OK\r\n\r\n")) == -1, "the last answer's headers only")
    check([int(pure(tmp, "wait", r, b)) for r, b in ((60, 0), (999999, 0), (-1, 0), (-1, 3600), (-1, 50000), (-1, 90000))] ==
          [60, 86400, 3600, 7200, 86400, 86400], "the busy wait: Retry-After, else an hour doubled, a day at most")
    check(pure(tmp, "next", "1763366400 3600\n") == "1|1763366400|3600" and pure(tmp, "next", "garbage").startswith("0|"), ".next's text")
    check([int(pure(tmp, "number", v)) for v in ("-", "", "x", "3", "0", "99", "7x")] == [5, 5, 5, 3, 1, 10, 5], "an environment number, clamped")


def sender(tmp):
    curl = tmp / ("curl" + EXE)
    exe = tmp / ("sender" + EXE)
    case = [0]

    def folder(on=True, stamp="telemetry=https://t.example\n", waiting=3, ini_extra=""):
        case[0] += 1
        f = tmp / ("game%d" % case[0])
        (f / "perf_pending").mkdir(parents=True)
        if on is not None:
            (f / "hookshot_options.ini").write_text(("send_performance_stats=1\n" if on else "send_performance_stats=0\n") + ini_extra, encoding="utf-8")
        if stamp is not None:
            (f / "hookshot_version.txt").write_text("version=v1\n" + stamp, encoding="utf-8")
        for i in range(waiting):
            (f / "perf_pending" / ("%d-%d.json" % (1763366400 + i, i))).write_text('{"v":1,"i":%d}' % i, encoding="utf-8")
        return f

    def run(f, answer="sent", **env):
        calls = f / "calls.txt"
        calls.unlink(missing_ok=True)   # (this run's calls only)
        e = {k: v for k, v in os.environ.items() if not k.startswith(("HL_", "CURL_"))}
        e.update(CRASH_TEST_CURL=str(curl), CURL_CALLS=str(calls), CURL_ANSWER=answer, **env)
        r = subprocess.run([str(exe), str(f)], capture_output=True, text=True, env=e, timeout=120)
        assert r.returncode == 0, r.stderr
        lines = calls.read_text(encoding="utf-8").splitlines() if calls.exists() else []
        return int(r.stdout.strip()), lines

    def waiting(f):
        return sorted(p.name for p in (f / "perf_pending").glob("*.json"))

    # nothing is sent unless the row is on AND the build has an address AND something waits
    f = folder(on=None)
    check(run(f) == (-1, []) and len(waiting(f)) == 3, "no options file (the default): nothing sent, the summaries stay")
    f = folder(on=False)
    check(run(f) == (-1, []) and len(waiting(f)) == 3, "the row off: nothing sent")
    f = folder(stamp="")
    check(run(f) == (-1, []) and len(waiting(f)) == 3, "the row on but the build's address empty: nothing sent")
    f = folder(stamp=None)
    check(run(f) == (-1, []), "no stamp at all: nothing sent")
    f = folder(stamp="telemetry=http://t.example\n")
    check(run(f) == (-1, []), "an http stamp is no address")
    f = folder(waiting=0)
    check(run(f) == (0, []), "nothing waiting: nothing posted")
    f = folder(on=True)
    check(run(f, HL_TELEMETRY_URL="http://127.0.0.1:1") == (-1, []), "an http override without the test switch: nothing sent")
    # a send
    f = folder()
    n, calls = run(f)
    check(n == 3 and len(calls) == 3 and all(c.startswith("https://t.example/telemetry/perf =https ") for c in calls),
          "the row on and an address: each summary posted to <address>/telemetry/perf over https only, one a connection")
    check([c.split(" ", 2)[2] for c in calls] == ['{"v":1,"i":0}', '{"v":1,"i":1}', '{"v":1,"i":2}'], "oldest first, the summary as the game wrote it")
    check(waiting(f) == [] and len(list((f / "perf_sent").glob("*.json"))) == 3, "the ones taken are deleted, a copy kept in perf_sent")
    check(not (f / "perf_pending" / ".next").exists(), "no backoff note after a good send")
    f = folder()
    n, calls = run(f, HL_TELEMETRY_URL="http://127.0.0.1:8123/", HL_TELEMETRY_TEST_HTTP="1")
    check(n == 3 and calls[0].startswith("http://127.0.0.1:8123/telemetry/perf =http "), "the test exception: http to this PC with HL_TELEMETRY_TEST_HTTP=1")
    f = folder()
    n, calls = run(f, HL_TELEMETRY_URL="https://override.example")
    check(calls[0].startswith("https://override.example/telemetry/perf "), "HL_TELEMETRY_URL over the stamp")
    # answers
    f = folder()
    n, calls = run(f, "refused")
    check(n == 3 and waiting(f) == [] and not (f / "perf_sent").exists(), "refused: deleted, never kept as sent")
    f = folder()
    n, calls = run(f, "down")
    check(n == 0 and len(calls) == 1 and len(waiting(f)) == 3, "not reached: the first tried, all kept for the next start")
    f = folder()
    n, calls = run(f, "busy")
    check(n == 0 and len(calls) == 1 and len(waiting(f)) == 3, "busy: stops at the first, all kept")
    at, wait = (f / "perf_pending" / ".next").read_text(encoding="utf-8").split()
    check(int(wait) == 7200 and abs(int(at) - (time.time() + 7200)) < 120, "the server's Retry-After sets when to ask again")
    check(run(f) == (-1, []), "left alone until then: nothing posted")
    check(run(f, HL_PERF_BACKOFF="0")[0] == 3 and not (f / "perf_pending" / ".next").exists(), "HL_PERF_BACKOFF=0 asks anyway, and a good send clears the note")
    f = folder()
    run(f, "busy-plain")
    check((f / "perf_pending" / ".next").read_text(encoding="utf-8").split()[1] == "3600", "busy without Retry-After: an hour")
    (f / "perf_pending" / ".next").write_text("0 3600\n", encoding="utf-8")   # (due now, the wait it set 3600)
    run(f, "busy-plain")
    check((f / "perf_pending" / ".next").read_text(encoding="utf-8").split()[1] == "7200", "busy again: doubled")
    # limits
    f = folder(waiting=20)
    n, calls = run(f)
    check(n == 12 and len(calls) == 12 and len(waiting(f)) == 8, "at most 12 a start (HL_PERF_MAX_SENDS)")
    check(run(f, HL_PERF_MAX_SENDS="3")[0] == 3 and len(waiting(f)) == 5, "HL_PERF_MAX_SENDS lowers it")
    f = folder(waiting=60)
    n, calls = run(f, HL_PERF_MAX_SENDS="1")
    check(len(waiting(f)) == 47 and calls[0].endswith('"i":12}'), "more than 48 waiting: the oldest go unsent, the newest 48 stay")
    f = folder(waiting=1)
    (f / "perf_pending" / "1763366400-0.json").write_bytes(b"x" * (61 * 1024))
    n, calls = run(f)
    check(n == 1 and calls == [] and waiting(f) == [], "a summary over 60 KiB is dropped, never posted")
    f = folder(waiting=2)
    (f / "perf_pending" / "notes.txt").write_text("x", encoding="utf-8")
    (f / "perf_pending" / "1763366400-5.json.tmp").write_text("x", encoding="utf-8")
    n, calls = run(f)
    check(n == 2 and (f / "perf_pending" / "notes.txt").exists() and (f / "perf_pending" / "1763366400-5.json.tmp").exists(), "other files are left alone")


def pipelines(tmp):
    """The pipeline state records' sender (hos_pipes_send_run): the performance summaries' rules, from pipelines_pending, to /telemetry/pipelines"""
    curl = tmp / ("curl" + EXE)
    exe = tmp / ("sender" + EXE)
    case = [0]

    def folder(on=True, stamp="telemetry=https://t.example\n", waiting=1):
        case[0] += 100
        f = tmp / ("game%d" % case[0])
        (f / "pipelines_pending").mkdir(parents=True)
        (f / "perf_pending").mkdir()
        (f / "hookshot_options.ini").write_text("send_performance_stats=%d\n" % (1 if on else 0), encoding="utf-8")
        if stamp is not None:
            (f / "hookshot_version.txt").write_text("version=v1\n" + stamp, encoding="utf-8")
        for i in range(waiting):
            (f / "pipelines_pending" / ("%d-0.json" % (1763366400 + i))).write_text('{"v":1,"i":%d}' % i, encoding="utf-8")
        return f

    def run(f, answer="sent", **env):
        calls = f / "calls.txt"
        calls.unlink(missing_ok=True)
        e = {k: v for k, v in os.environ.items() if not k.startswith(("HL_", "CURL_"))}
        e.update(CRASH_TEST_CURL=str(curl), CURL_CALLS=str(calls), CURL_ANSWER=answer, **env)
        r = subprocess.run([str(exe), str(f), "p"], capture_output=True, text=True, env=e, timeout=120)
        assert r.returncode == 0, r.stderr
        return int(r.stdout.strip()), (calls.read_text(encoding="utf-8").splitlines() if calls.exists() else [])

    def waiting(f):
        return sorted(p.name for p in (f / "pipelines_pending").glob("*.json"))

    f = folder()
    n, calls = run(f)
    check(n == 1 and len(calls) == 1 and calls[0].startswith('https://t.example/telemetry/pipelines =https {"v":1,"i":0}'),
          "pipeline records: the row on and an address: posted to <address>/telemetry/pipelines over https only")
    check(waiting(f) == [] and len(list((f / "pipelines_sent").glob("*.json"))) == 1, "the batch taken is deleted, a copy kept in pipelines_sent for the player to read")
    f = folder(on=False)
    check(run(f) == (-1, []) and len(waiting(f)) == 1, "the performance statistics row off: nothing sent, the batch stays")
    f = folder(stamp="")
    check(run(f) == (-1, []) and len(waiting(f)) == 1, "no address: nothing sent")
    f = folder()
    check(run(f, HL_TELEMETRY_PIPELINES="0") == (-1, []) and len(waiting(f)) == 1, "HL_TELEMETRY_PIPELINES=0: nothing sent, by itself")
    n, calls = run(f, HL_TELEMETRY_PIPELINES="1")
    check(n == 1 and len(calls) == 1, "HL_TELEMETRY_PIPELINES=1 is not off")
    f = folder(waiting=1)
    (f / "perf_pending" / "1763366400-0.json").write_text('{"v":1}', encoding="utf-8")
    e = {k: v for k, v in os.environ.items() if not k.startswith(("HL_", "CURL_"))}
    e.update(CRASH_TEST_CURL=str(curl), CURL_ANSWER="sent", CURL_CALLS=str(f / "perf-calls.txt"), HL_TELEMETRY_PIPELINES="0")
    r = subprocess.run([str(exe), str(f)], capture_output=True, text=True, env=e, timeout=120)
    check(r.stdout.strip() == "1" and "/telemetry/perf " in (f / "perf-calls.txt").read_text(encoding="utf-8") and len(waiting(f)) == 1,
          "a summary still goes with HL_TELEMETRY_PIPELINES=0, and the batch is left where it is")
    f = folder(waiting=3)
    n, calls = run(f)
    check(n == 2 and len(calls) == 2 and len(waiting(f)) == 1, "two batches a start at most")
    f = folder(waiting=6)
    n, calls = run(f, HL_PERF_MAX_SENDS="1")
    check(n == 1 and len(waiting(f)) == 3 and calls[0].endswith('"i":2}'), "more than 4 waiting: the oldest go unsent, the newest 4 stay")
    f = folder(waiting=3)
    for i in range(3):
        run(f)
    check(len(list((f / "pipelines_sent").glob("*.json"))) == 3, "the last 3 sent are kept")
    f = folder(waiting=1)
    (f / "pipelines_pending" / "1763366400-0.json").write_bytes(b"x" * (1500 * 1024))
    n, calls = run(f)
    check(n == 1 and len(calls) == 1 and waiting(f) == [], "a batch of 1.5 MB goes (a summary's 60 KiB limit does not apply to it)")
    f = folder(waiting=1)
    (f / "pipelines_pending" / "1763366400-0.json").write_bytes(b"x" * (1536 * 1024 + 1))
    n, calls = run(f)
    check(n == 1 and calls == [] and waiting(f) == [], "a batch over 1.5 MiB is dropped, never posted")
    f = folder()
    n, calls = run(f, "busy")
    check(n == 0 and len(waiting(f)) == 1 and (f / "pipelines_pending" / ".next").exists() and not (f / "perf_pending" / ".next").exists(),
          "a busy server: the batch is kept, the wait noted in its own folder")
    check(run(f) == (-1, []), "and left alone until then")


def zones(tmp):
    """The per-zone shader and pipeline usage's sender (hos_zones_send_run): the performance summaries' rules, from zones_pending, to /telemetry/zones"""
    curl = tmp / ("curl" + EXE)
    exe = tmp / ("sender" + EXE)
    case = [2000]   # (this test's own folders: the summaries' and the pipeline records' tests made theirs below)

    def folder(on=True, stamp="telemetry=https://t.example\n", waiting=1):
        case[0] += 100
        f = tmp / ("game%d" % case[0])
        (f / "zones_pending").mkdir(parents=True)
        (f / "perf_pending").mkdir()
        (f / "hookshot_options.ini").write_text("send_performance_stats=%d\n" % (1 if on else 0), encoding="utf-8")
        if stamp is not None:
            (f / "hookshot_version.txt").write_text("version=v1\n" + stamp, encoding="utf-8")
        for i in range(waiting):
            (f / "zones_pending" / ("%d-0.json" % (1763366400 + i))).write_text('{"v":1,"i":%d}' % i, encoding="utf-8")
        return f

    def run(f, answer="sent", **env):
        calls = f / "calls.txt"
        calls.unlink(missing_ok=True)
        e = {k: v for k, v in os.environ.items() if not k.startswith(("HL_", "CURL_"))}
        e.update(CRASH_TEST_CURL=str(curl), CURL_CALLS=str(calls), CURL_ANSWER=answer, **env)
        r = subprocess.run([str(exe), str(f), "z"], capture_output=True, text=True, env=e, timeout=120)
        assert r.returncode == 0, r.stderr
        return int(r.stdout.strip()), (calls.read_text(encoding="utf-8").splitlines() if calls.exists() else [])

    def waiting(f):
        return sorted(p.name for p in (f / "zones_pending").glob("*.json"))

    f = folder()
    n, calls = run(f)
    check(n == 1 and len(calls) == 1 and calls[0].startswith('https://t.example/telemetry/zones =https {"v":1,"i":0}'),
          "zone usage: the row on and an address: posted to <address>/telemetry/zones over https only")
    check(waiting(f) == [] and len(list((f / "zones_sent").glob("*.json"))) == 1, "the batch taken is deleted, a copy kept in zones_sent for the player to read")
    f = folder(on=False)
    check(run(f) == (-1, []) and len(waiting(f)) == 1, "the performance statistics row off: nothing sent, the batch stays")
    f = folder(stamp="")
    check(run(f) == (-1, []) and len(waiting(f)) == 1, "no address: nothing sent")
    f = folder()
    check(run(f, HL_TELEMETRY_ZONES="0") == (-1, []) and len(waiting(f)) == 1, "HL_TELEMETRY_ZONES=0: nothing sent, by itself")
    n, calls = run(f, HL_TELEMETRY_ZONES="1")
    check(n == 1 and len(calls) == 1, "HL_TELEMETRY_ZONES=1 is not off")
    f = folder(waiting=1)
    (f / "perf_pending" / "1763366400-0.json").write_text('{"v":1}', encoding="utf-8")
    e = {k: v for k, v in os.environ.items() if not k.startswith(("HL_", "CURL_"))}
    e.update(CRASH_TEST_CURL=str(curl), CURL_ANSWER="sent", CURL_CALLS=str(f / "perf-calls.txt"), HL_TELEMETRY_ZONES="0")
    r = subprocess.run([str(exe), str(f)], capture_output=True, text=True, env=e, timeout=120)
    check(r.stdout.strip() == "1" and "/telemetry/perf " in (f / "perf-calls.txt").read_text(encoding="utf-8") and len(waiting(f)) == 1,
          "a summary still goes with HL_TELEMETRY_ZONES=0, and the zone batch is left where it is")
    f = folder(waiting=3)
    n, calls = run(f)
    check(n == 2 and len(calls) == 2 and len(waiting(f)) == 1, "two batches a start at most")
    f = folder(waiting=6)
    n, calls = run(f, HL_PERF_MAX_SENDS="1")
    check(n == 1 and len(waiting(f)) == 3 and calls[0].endswith('"i":2}'), "more than 4 waiting: the oldest go unsent, the newest 4 stay")
    f = folder(waiting=3)
    for i in range(3):
        run(f)
    check(len(list((f / "zones_sent").glob("*.json"))) == 3, "the last 3 sent are kept")
    f = folder(waiting=1)
    (f / "zones_pending" / "1763366400-0.json").write_bytes(b"x" * (500 * 1024))
    n, calls = run(f)
    check(n == 1 and len(calls) == 1 and waiting(f) == [], "a batch of 500 KB goes (a summary's 60 KiB limit does not apply to it)")
    f = folder(waiting=1)
    (f / "zones_pending" / "1763366400-0.json").write_bytes(b"x" * (512 * 1024 + 1))
    n, calls = run(f)
    check(n == 1 and calls == [] and waiting(f) == [], "a batch over 512 KiB is dropped, never posted")
    f = folder()
    n, calls = run(f, "busy")
    check(n == 0 and len(waiting(f)) == 1 and (f / "zones_pending" / ".next").exists() and not (f / "perf_pending" / ".next").exists(),
          "a busy server: the zone batch is kept, the wait noted in its own folder")
    check(run(f) == (-1, []), "and left alone until then")


def profile(tmp):
    """The detailed profile's sender (hos_profile_send_run): the performance summaries' rules, from profile_pending, to /telemetry/profile"""
    curl = tmp / ("curl" + EXE)
    exe = tmp / ("sender" + EXE)
    case = [7000]

    def folder(on=True, stamp="telemetry=https://t.example\n", waiting=1):
        case[0] += 100
        f = tmp / ("game%d" % case[0])
        (f / "profile_pending").mkdir(parents=True)
        (f / "perf_pending").mkdir()
        (f / "hookshot_options.ini").write_text("send_performance_stats=%d\n" % (1 if on else 0), encoding="utf-8")
        if stamp is not None:
            (f / "hookshot_version.txt").write_text("version=v1\n" + stamp, encoding="utf-8")
        for i in range(waiting):
            (f / "profile_pending" / ("%d-0.json" % (1763366400 + i))).write_text('{"v":1,"i":%d}' % i, encoding="utf-8")
        return f

    def run(f, answer="sent", **env):
        calls = f / "calls.txt"
        calls.unlink(missing_ok=True)
        e = {k: v for k, v in os.environ.items() if not k.startswith(("HL_", "CURL_"))}
        e.update(CRASH_TEST_CURL=str(curl), CURL_CALLS=str(calls), CURL_ANSWER=answer, **env)
        r = subprocess.run([str(exe), str(f), "f"], capture_output=True, text=True, env=e, timeout=120)
        assert r.returncode == 0, r.stderr
        return int(r.stdout.strip()), (calls.read_text(encoding="utf-8").splitlines() if calls.exists() else [])

    def waiting(f):
        return sorted(p.name for p in (f / "profile_pending").glob("*.json"))

    f = folder()
    n, calls = run(f)
    check(n == 1 and len(calls) == 1 and calls[0].startswith('https://t.example/telemetry/profile =https {"v":1,"i":0}'),
          "profile: the row on and an address: posted to <address>/telemetry/profile over https only")
    check(waiting(f) == [] and len(list((f / "profile_sent").glob("*.json"))) == 1, "the batch taken is deleted, a copy kept in profile_sent")
    f = folder(on=False)
    check(run(f) == (-1, []) and len(waiting(f)) == 1, "the row off: nothing sent, the batch stays")
    f = folder(stamp="")
    check(run(f) == (-1, []) and len(waiting(f)) == 1, "no address: nothing sent")
    f = folder()
    check(run(f, HL_TELEMETRY_PROFILE="0") == (-1, []) and len(waiting(f)) == 1, "HL_TELEMETRY_PROFILE=0: nothing sent, by itself")
    n, calls = run(f, HL_TELEMETRY_PROFILE="1")
    check(n == 1 and len(calls) == 1, "HL_TELEMETRY_PROFILE=1 is not off")
    f = folder(waiting=5)
    n, calls = run(f)
    check(n == 2 and len(calls) == 2 and len(waiting(f)) == 3, "two batches a start at most")
    f = folder(waiting=11)
    n, calls = run(f, HL_PERF_MAX_SENDS="1")
    check(n == 1 and len(waiting(f)) == 7 and calls[0].endswith('"i":3}'), "more than 8 waiting: the oldest go unsent, the newest 8 stay")
    f = folder(waiting=1)
    (f / "profile_pending" / "1763366400-0.json").write_bytes(b"x" * (256 * 1024 + 1))
    n, calls = run(f)
    check(n == 1 and calls == [] and waiting(f) == [], "a batch over 256 KiB is dropped, never posted")
    f = folder(waiting=1)
    (f / "profile_pending" / "1763366400-0.json").write_bytes(b"x" * (200 * 1024))
    n, calls = run(f)
    check(n == 1 and len(calls) == 1, "a batch of 200 KB goes")
    f = folder()
    n, calls = run(f, "busy")
    check(n == 0 and len(waiting(f)) == 1 and (f / "profile_pending" / ".next").exists() and not (f / "perf_pending" / ".next").exists(),
          "a busy server: the batch is kept, the wait noted in its own folder")
    check(run(f) == (-1, []), "and left alone until then")


def stamp_lines():
    sys.path.insert(0, str(ROOT / "nx2"))
    import addresses
    ini = Path(tempfile.mkdtemp(prefix="addresses-")) / "a.ini"
    try:
        check(addresses.address("telemetry") == "https://141-253-120-55.sslip.io" and
              addresses.stamp_lines() == ["telemetry=https://141-253-120-55.sslip.io\n"],
              "nx2/addresses.ini's telemetry= is the release's https address, which the build stamps")
        ini.write_text("[addresses]\ntelemetry = https://t.example/\ncrash_issue = https://g.example/o/r/issues/1\n", encoding="utf-8")
        check(addresses.stamp_lines(str(ini)) == ["telemetry=https://t.example\n", "crash_issue=https://g.example/o/r/issues/1\n"],
              "an https address is stamped, its trailing slash taken off")
        ini.write_text("[addresses]\ntelemetry = http://t.example\ncrash_issue = ftp://x\n", encoding="utf-8")
        check(addresses.stamp_lines(str(ini)) == [], "an address that is not https is not stamped")
        ini.write_text("[addresses]\ntelemetry = https://a b\n", encoding="utf-8")
        check(addresses.stamp_lines(str(ini)) == [], "an address with a space is not stamped")
    finally:
        shutil.rmtree(ini.parent, ignore_errors=True)
    source = (ROOT / "nx2/make_game.py").read_text(encoding="utf-8")
    check("f.writelines(_addresses.stamp_lines())" in source, "make_game.py stamps them into hookshot_version.txt")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    stamp_lines()
    with tempfile.TemporaryDirectory(prefix="telemetry-") as t:
        tmp = Path(t)
        build(args.cc, tmp)
        rules(tmp)
        sender(tmp)
        pipelines(tmp)
        zones(tmp)
        profile(tmp)


if __name__ == "__main__":
    main()
