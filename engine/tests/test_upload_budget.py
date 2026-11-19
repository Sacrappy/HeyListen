"""(HeyListen) The upload budget (RX_VK_UPLOAD_BUDGET_MS, vk_texture.cpp's upload_defer): a new ASTC texture a draw samples is
held back for a later present once the looking-up thread's own upload time this present reached the budget, not every
thread's summed (the parallel replay's segment workers upload side by side: their sum reached 40 ms when the busiest had spent
11-31 ms, at the Market's arrival). RX_VK_UPLOAD_BUDGET_SUM=1 is the summed rule, as before. And the deferral's measurement:
how long each held texture waited (defer_note_done, defer_frame_end), its percentile, and its log lines.

upload_defer and defer_pct are cut out of vk_texture.cpp and compiled with the stubs below (a C++20 compiler: g++ or clang++;
skipped without one).

python -I -B tests/test_upload_budget.py
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"
DOC = ROOT / "docs" / "development.md"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


FIXTURE = r"""
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
struct Texture {
    bool uploaded = false, astc = true, gpu_written = false;
    uint32_t id = 1, width = 64, height = 64, layers = 1;
    int nvn_format = 0x7b;
    uint64_t defer_frame = 0, defer_seen = 0;
    int64_t defer_ns = 0;
    uint32_t defer_asks = 0;
};
uint64_t frame_no = 7;
std::atomic<int64_t> frame_upload_ns{0}, st_deferred{0}, st_deferred_frame{0}, st_deferred_tex_frame{0};
thread_local uint64_t tl_upload_frame = 0;
thread_local int64_t tl_upload_ns = 0;
thread_local bool upload_defer_ok = true;
struct Held { uint32_t w, h, layers; int nvn; };
struct { int64_t deferred = 0; std::unordered_map<uint32_t, Held> held; } burst;
std::mutex burst_mu;
int64_t steady_ns() { return 123; }
CUT_DEFER
CUT_PCT
// (this thread spent `ms` in upload() this present, every thread `sum_ms` in all: is a new ASTC texture held back?)
bool held(double ms, double sum_ms, bool astc = true, bool uploaded = false) {
    Texture t;
    t.astc = astc, t.uploaded = uploaded;
    tl_upload_frame = frame_no, tl_upload_ns = (int64_t)(ms * 1e6);
    frame_upload_ns = (int64_t)(sum_ms * 1e6);
    return upload_defer(t);
}
int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "thread";
    int bad = 0;
    auto expect = [&](bool got, bool want, const char* what) {
        if (got != want) { std::printf("FAIL (%s): %s: %d, wanted %d\n", mode, what, got, want); bad++; }
    };
    if (std::string(mode) == "thread") {
        expect(held(31, 82), false, "the busiest thread at 31 ms of a 40 ms budget, 82 ms summed");
        expect(held(45, 45), true, "this thread at 45 ms");
        tl_upload_frame = frame_no - 1, tl_upload_ns = 90000000, frame_upload_ns = 0;   // (an earlier present's time)
        Texture t;
        expect(upload_defer(t), false, "this thread's time from an earlier present");
        bool other = true;   // (another thread's own count starts at nothing)
        std::thread([&] { Texture u; frame_upload_ns = 200000000; other = upload_defer(u); }).join();
        expect(other, false, "a thread that uploaded nothing this present, the sum at 200 ms");
        expect(held(45, 45, false), false, "an uncompressed texture");
        expect(held(45, 45, true, true), false, "an ASTC texture uploaded before");
        Texture w;
        tl_upload_frame = frame_no, tl_upload_ns = 50000000;
        upload_defer(w), upload_defer(w);   // (two lookups in one present: one texture, held back since this present)
        expect(w.defer_frame == frame_no && w.defer_asks == 2 && w.defer_ns == 123, true, "the held texture's age noted once");
        expect(st_deferred_tex_frame.load() >= 1 && burst.held.count(w.id) == 1, true, "the distinct textures counted");
        std::vector<uint32_t> v{9, 1, 5, 3, 7, 2, 8, 4, 6, 10};
        expect(defer_pct(v, 1.0) == 10 && defer_pct(v, 0.95) == 10 && defer_pct(v, 0.5) == 6, true, "the percentiles");
        std::vector<float> none;
        expect(defer_pct(none, 0.95) == 0.0f, true, "no waits: 0");
    } else if (std::string(mode) == "sum") {
        expect(held(31, 82), true, "RX_VK_UPLOAD_BUDGET_SUM=1: 82 ms summed");
        expect(held(31, 39), false, "RX_VK_UPLOAD_BUDGET_SUM=1: 39 ms summed");
    } else {
        expect(held(500, 500), false, "RX_VK_UPLOAD_BUDGET_MS=0");
    }
    if (!bad) std::printf("ok (%s)\n", mode);
    return bad != 0;
}
"""


class UploadBudget(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = TEX.read_text(encoding="utf-8")

    def test_the_budget_is_the_threads_own_unless_summed_is_asked(self):
        defer = block(self.tex, "bool upload_defer(Texture& tex)")
        self.assertIn('getenv("RX_VK_UPLOAD_BUDGET_SUM")', defer)
        self.assertIn("tl_upload_frame == frame_no ? tl_upload_ns : 0", defer)
        self.assertIn("frame_upload_ns.load(", defer)   # (the summed rule kept behind the switch)
        up = block(self.tex, "bool upload(Texture& tex, const Ranges* dirty)")
        self.assertIn("if (tl_upload_frame != frame_no) tl_upload_frame = frame_no, tl_upload_ns = 0;", up)
        self.assertIn("tl_upload_ns += ns", up)
        self.assertIn("frame_kind_ns[kind].fetch_add(ns", up)

    def test_a_held_texture_is_measured_until_it_comes_in_or_goes(self):
        touch = block(self.tex, "Texture* texture_touch(Texture* tex, bool for_sampling)")
        self.assertLess(touch.index("if (upload_defer(*tex)) return nullptr;"),
                        touch.index("if (tex->defer_frame) defer_note_done(*tex, true);"))
        self.assertIn("if (tex.defer_frame) defer_note_done(tex, false);", block(self.tex, "void destroy_later(Texture& tex)"))
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        self.assertLess(frame.index("defer_frame_end("), frame.index("st_deferred_frame.exchange(0"))
        end = block(self.tex, "void defer_frame_end(int64_t asks_now)")
        self.assertIn("vk: deferral done:", end)
        self.assertIn("frame_no - burst.last_frame < 600", end)   # (a run ends once all are in, or after 600 quiet presents)
        self.assertIn("if (burst.told++ < 40)", end)              # (quiet: a run's first 40 deferring presents only)

    def test_the_switches_are_documented(self):
        doc = DOC.read_text(encoding="utf-8")
        self.assertIn("RX_VK_UPLOAD_BUDGET_MS", doc)
        self.assertIn("RX_VK_UPLOAD_BUDGET_SUM=1", doc)
        self.assertIn("RX_VK_UPLOAD_BUDGET_FRAME_MS", doc)

    def test_the_frame_budget_is_off_by_default(self):
        # (STUT1) every thread's summed uploads cap a present too, but only with RX_VK_UPLOAD_BUDGET_FRAME_MS set
        body = block(self.tex, "bool upload_defer(Texture& tex)")
        self.assertIn('getenv("RX_VK_UPLOAD_BUDGET_FRAME_MS") ? atof(getenv("RX_VK_UPLOAD_BUDGET_FRAME_MS")) : 0.0', body)
        self.assertIn("if (spent < budget_ns && (!frame_ns || frame_upload_ns.load(std::memory_order_relaxed) < frame_ns)) return false;", body)

    def test_the_rule_runs(self):
        cxx = shutil.which("g++") or shutil.which("clang++")
        if not cxx:
            self.skipTest("no C++ compiler")
        code = FIXTURE.replace("CUT_DEFER", block(self.tex, "bool upload_defer(Texture& tex)"))
        code = code.replace("CUT_PCT", block(self.tex, "template <class T> T defer_pct(std::vector<T>& v, double q)"))
        code = code.replace("#include <vector>", "#include <vector>\n#include <string>")
        with tempfile.TemporaryDirectory(prefix="upload-budget-") as tmp:
            src, exe = Path(tmp) / "f.cpp", Path(tmp) / ("f.exe" if os.name == "nt" else "f")
            src.write_text(code, encoding="utf-8")
            built = subprocess.run([cxx, "-std=c++20", "-D_CRT_SECURE_NO_WARNINGS", "-O0", "-Wall", "-Wextra", "-Werror",
                                    "-pthread", str(src), "-o", str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, "the code cut out of vk_texture.cpp does not build in the fixture: " +
                             built.stderr[-3000:])
            env = {k: v for k, v in os.environ.items() if not k.startswith("RX_VK_UPLOAD_BUDGET")}
            env["PATH"] = str(Path(cxx).parent) + os.pathsep + env.get("PATH", "")
            for mode, extra in (("thread", {}), ("sum", {"RX_VK_UPLOAD_BUDGET_SUM": "1"}), ("off", {"RX_VK_UPLOAD_BUDGET_MS": "0"})):
                r = subprocess.run([str(exe), mode], env={**env, **extra}, capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
