"""One log line a talk, saying how long its text box took (host/oot_talk_log.h, host/hl_talklog.c, nx2/hl_gates.py HL_TALK_HOOK).

The rule (the header, compiled alone): a talk request starts the measure, the first text box drawn after it ends it with one line:
the time, the stages passed (the dialog's open request, the text thread's first run: their times from the request) and the
walk-back ("none", or its time and updates, or "not ended" while it goes on); over 1500 ms also the frames in between and the
map; a dialog open request with no talk request waiting starts a measure of its own (a chest's item text); no box in 10 s: a line
that says so; a second request before the box does not move the start; nothing waiting: no line.

The hooks: made-up lifted functions get a call at their entry, once, hash unchanged, idempotent, taken out when the tree stops
asking; the host asks for all six addresses; HOS_TALK_LOG is documented (docs/switches.json, development.md).

python -I -B tests/test_talk_log.py
"""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import hl_gates  # noqa: E402

FIXTURE = r"""
#include "oot_talk_log.h"
#include <string.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define MS 1000000LL
static int same(const char *a, const char *b) { if (strcmp(a, b)) { printf("got  %s\nwant %s\n", a, b); return 0; } return 1; }

int main(void) {
    char line[400];
    {   /* an ordinary talk: no walk-back */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        CHECK(!oot_talk_box(&t, 5 * MS, 1, "", line, sizeof line, 0, 40));   /* (nothing waits: no line) */
        CHECK(oot_talk_event(&t, OOT_TALK_REQUEST, 1000 * MS, 100));
        oot_talk_event(&t, OOT_TALK_DIALOG, 1310 * MS, 120);
        oot_talk_event(&t, OOT_TALK_THREAD, 1640 * MS, 130);
        oot_talk_event(&t, OOT_TALK_THREAD, 1650 * MS, 131);   /* (the first run counts) */
        CHECK(oot_talk_box(&t, 1812 * MS, 150, "Map", line, sizeof line, 0, 40));
        CHECK(same(line, "talk: text box 812 ms after the talk request (dialog asked at 310 ms, text thread ran at 640 ms; walk-back: none)"));
        CHECK(!oot_talk_box(&t, 1900 * MS, 160, "", line, sizeof line, 0, 40));   /* (once) */
    }
    {   /* a walk-back, slow: the frames and the map */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        oot_talk_event(&t, OOT_TALK_REQUEST, 0, 10);
        oot_talk_event(&t, OOT_TALK_WALK_BEGIN, 100 * MS, 16);
        for (int k = 0; k < 159; k++) oot_talk_event(&t, OOT_TALK_WALK_UPDATE, (100 + k) * MS, 16);
        oot_talk_event(&t, OOT_TALK_WALK_END, 2750 * MS, 180);
        oot_talk_event(&t, OOT_TALK_WALK_UPDATE, 2800 * MS, 181);   /* (after its end: not counted) */
        oot_talk_event(&t, OOT_TALK_DIALOG, 2900 * MS, 190);
        CHECK(oot_talk_box(&t, 3410 * MS, 205, "Hyrule Field", line, sizeof line, 0, 40));
        CHECK(same(line, "talk: text box 3410 ms after the talk request (dialog asked at 2900 ms, text thread not run; walk-back: 2650 ms, 159 updates), "
                         "195 frames in between, map Hyrule Field"));
    }
    {   /* a walk-back that has not ended; exactly 1500 ms is not over; no map at hand */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        oot_talk_event(&t, OOT_TALK_REQUEST, 0, 0);
        oot_talk_event(&t, OOT_TALK_WALK_BEGIN, 400 * MS, 0);
        oot_talk_event(&t, OOT_TALK_WALK_UPDATE, 410 * MS, 0);
        CHECK(oot_talk_box(&t, 1500 * MS, 90, NULL, line, sizeof line, 0, 40));
        CHECK(same(line, "talk: text box 1500 ms after the talk request (dialog not asked, text thread not run; walk-back: 1100 ms so far, 1 updates, not ended)"));
        oot_talk_event(&t, OOT_TALK_REQUEST, 2000 * MS, 100);
        CHECK(oot_talk_box(&t, 3501 * MS, 190, NULL, line, sizeof line, 0, 40));
        CHECK(strstr(line, "1501 ms") && strstr(line, "90 frames in between") && !strstr(line, "map"));
    }
    {   /* a dialog with no talk request (a chest's text) measures from its open request */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        CHECK(oot_talk_event(&t, OOT_TALK_DIALOG, 50 * MS, 5));
        oot_talk_event(&t, OOT_TALK_DIALOG, 60 * MS, 6);   /* (a second one: the start stays) */
        oot_talk_event(&t, OOT_TALK_THREAD, 90 * MS, 7);
        {   OotTalkLog q = t;   /* (a copy: 200 ms is quiet by default, told with all) */
            CHECK(!oot_talk_box(&q, 250 * MS, 12, "Map", line, sizeof line, 0, 40));
        }
        CHECK(oot_talk_box(&t, 250 * MS, 12, "Map", line, sizeof line, 1, 40));
        CHECK(same(line, "talk: text box 200 ms after the dialog open request, no talk request (text thread ran at 40 ms; walk-back: none)"));
    }
    {   /* a short label's box is not the message: the measure goes on to the box with a text; the last dialog request counts */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        oot_talk_event(&t, OOT_TALK_REQUEST, 0, 0);
        oot_talk_event(&t, OOT_TALK_DIALOG, 0, 0);
        CHECK(!oot_talk_box(&t, 99 * MS, 5, "", line, sizeof line, 0, 8));
        CHECK(t.armed);
        oot_talk_event(&t, OOT_TALK_DIALOG, 100 * MS, 6);
        CHECK(!oot_talk_box(&t, 300 * MS, 10, "", line, sizeof line, 0, OOT_TALK_MIN_LEN - 1));
        CHECK(oot_talk_box(&t, 700 * MS, 20, "", line, sizeof line, 0, OOT_TALK_MIN_LEN));
        CHECK(same(line, "talk: text box 700 ms after the talk request (dialog asked at 100 ms, text thread not run; walk-back: none)"));
    }
    {   /* a second talk request before the box does not move the start; events with nothing waiting change nothing */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        oot_talk_event(&t, OOT_TALK_WALK_BEGIN, 10 * MS, 0);
        oot_talk_event(&t, OOT_TALK_THREAD, 10 * MS, 0);
        CHECK(!t.armed && !t.walk_begin_ns && !t.thread_ns);
        CHECK(oot_talk_event(&t, OOT_TALK_REQUEST, 100 * MS, 0));
        CHECK(!oot_talk_event(&t, OOT_TALK_REQUEST, 500 * MS, 0));
        CHECK(oot_talk_box(&t, 700 * MS, 3, "", line, sizeof line, 0, 40));
        CHECK(strstr(line, "text box 600 ms"));
    }
    {   /* a slow dialog with no talk is told by default; a talk request takes over a measure begun at a dialog */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        oot_talk_event(&t, OOT_TALK_DIALOG, 0, 0);
        CHECK(oot_talk_box(&t, 2000 * MS, 40, "Map", line, sizeof line, 0, 40));
        CHECK(strstr(line, "2000 ms after the dialog open request") && strstr(line, "40 frames in between, map Map"));
        oot_talk_event(&t, OOT_TALK_DIALOG, 5000 * MS, 50);
        CHECK(oot_talk_event(&t, OOT_TALK_REQUEST, 5100 * MS, 52));   /* (the talk starts its own measure) */
        oot_talk_event(&t, OOT_TALK_DIALOG, 5300 * MS, 55);
        CHECK(oot_talk_box(&t, 5900 * MS, 60, "Map", line, sizeof line, 0, 40));
        CHECK(same(line, "talk: text box 800 ms after the talk request (dialog asked at 200 ms, text thread not run; walk-back: none)"));
    }
    {   /* a dialog that never shows a box ends silently at 10 s, and a new measure may begin */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        oot_talk_event(&t, OOT_TALK_DIALOG, 0, 0);
        CHECK(!oot_talk_poll(&t, 10000 * MS, 600, "Map", line, sizeof line));
        CHECK(!t.armed);
        CHECK(oot_talk_event(&t, OOT_TALK_REQUEST, 11000 * MS, 0));
    }
    {   /* no box: nothing before 10 s, one line at 10 s, then nothing */
        OotTalkLog t = OOT_TALK_LOG_INIT;
        oot_talk_event(&t, OOT_TALK_REQUEST, 0, 0);
        oot_talk_event(&t, OOT_TALK_DIALOG, 300 * MS, 0);
        CHECK(!oot_talk_poll(&t, 9999 * MS, 600, "Map", line, sizeof line));
        CHECK(oot_talk_poll(&t, 10000 * MS, 600, "Map", line, sizeof line));
        CHECK(same(line, "talk: no text box within 10 s of the talk request (dialog asked at 300 ms, text thread not run; walk-back: none), "
                         "600 frames in between, map Map"));
        CHECK(!oot_talk_poll(&t, 11000 * MS, 660, "Map", line, sizeof line));
        CHECK(!oot_talk_box(&t, 12000 * MS, 700, "Map", line, sizeof line, 0, 40));
    }
    if (fails) return 1;
    printf("all passed\n");
    return 0;
}
"""

LIFTED = """
void f_81000000(Ctx *__restrict A64_NOESC c) {
    c->x[0] = 1;
}

void f_81000100(Ctx *__restrict A64_NOESC c) {
    c->x[0] = 2;
}
"""
HOOKS = {"81000000": 0, "81000100": 4}


class TalkLogTests(unittest.TestCase):
    def test_rule(self):
        compiler = shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            self.skipTest("no C compiler")
        with tempfile.TemporaryDirectory(prefix="talk-log-") as tmp:
            tmp = Path(tmp)
            (tmp / "rule.c").write_text(FIXTURE, encoding="utf-8")
            binary = tmp / "rule.exe"
            build = subprocess.run([compiler, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-O1", "-I", str(ROOT / "host"),
                                    str(tmp / "rule.c"), "-o", str(binary)], text=True, capture_output=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary)], text=True, capture_output=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("all passed", run.stdout)

    def test_hook(self):
        out = LIFTED
        for addr, event in HOOKS.items():
            out = hl_gates.apply_lift(out, addr, "talk", (event,))
            self.assertIsNotNone(out)
        for addr, event in HOOKS.items():
            call = "    { extern void hos_oot_talk_event(int); hos_oot_talk_event(%d); }   /* hl gate %08x */\n" % (event, int(addr, 16) + 1)
            self.assertIn("void f_%s(Ctx *__restrict A64_NOESC c) {\n%s" % (addr, call), out)   # (the entry, nothing else changed)
            self.assertEqual(hl_gates.text_hash(out, addr), hl_gates.text_hash(LIFTED, addr))     # (the hash leaves the hook out)
        self.assertEqual(out.count("hos_oot_talk_event(int)"), 2)
        self.assertEqual(hl_gates.apply_lift(out, "81000000", "talk", (0,)), out)   # (hooked before: as it is)
        self.assertIsNone(hl_gates.apply_lift(LIFTED, "91000000", "talk", (0,)))      # (no such function)
        self.assertIsNone(hl_gates.apply_lift(LIFTED, "81000000", "talk", ()))        # (no event given)
        out, _ = hl_gates.apply_text(out, {}, None, None, None)                        # (the tree stops asking: taken out)
        self.assertEqual(out, LIFTED)

    def test_hook_is_asked_for_by_the_host(self):
        host = (ROOT / "host" / "hl_talklog.c").read_text(encoding="utf-8")
        got = hl_gates.wanted_lifts()
        for addr, event in (("826df3c0", 0), ("826cd180", 1), ("826cdb90", 2), ("826cd9c0", 3), ("83adc560", 4), ("80e6dfd0", 5)):
            self.assertEqual(got[addr][1:], ("talk", (event,)))
            self.assertIn('HL_TALK_HOOK(%s, "%s", %d)' % (addr, got[addr][0], event), host)
            self.assertEqual(len(got[addr][0].split(",")), 2)   # (both lifts: without and with the native cold helpers)
        self.assertTrue(host.startswith("// Copyright (C) 2026 Sacrappy"))

    def test_host_wiring(self):
        host = (ROOT / "host" / "hl_talklog.c").read_text(encoding="utf-8")
        for needle in ('getenv("HOS_TALK_LOG")', "void hos_oot_talk_event(int event) {", "void hos_talk_box_drawn(uint64_t box) {",
                       "void hos_talk_poll(void) {", "oot_talk_event(&talk, event,"):
            self.assertIn(needle, host)
        macro = (ROOT / "host" / "hl_host.h").read_text(encoding="utf-8")
        self.assertIn("#define HL_TALK_HOOK(addr, hash, event) " + chr(92) + chr(10), macro.replace(chr(13) + chr(10), chr(10)))   # (a real line continuation)
        prompts = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
        self.assertIn("if (hos_talk_armed) hos_talk_box_drawn(box);", prompts)
        self.assertIn("if (hos_talk_armed) hos_talk_poll();", (ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8"))

    def test_switch_documented(self):
        sw = json.loads((ROOT / "docs" / "switches.json").read_text(encoding="utf-8"))
        rows = [r for r in (sw["switches"] if isinstance(sw, dict) else sw) if r["name"] == "HOS_TALK_LOG"]
        self.assertEqual(len(rows), 1)
        self.assertIn("host/hl_talklog.c", [f["path"] for f in rows[0]["files"]])
        self.assertIn("HOS_TALK_LOG", (ROOT / "docs" / "development.md").read_text(encoding="utf-8"))
        self.assertIn("HOS_TALK_LOG", (ROOT.parent / "docs" / "SWITCHES.md").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
