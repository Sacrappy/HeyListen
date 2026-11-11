"""(TICK-060m) RX_VK_LATE_DEFER's pending late fixes when their values do not rise in take order.

A capture's late value is the newest write-back of its own late pages, so the next capture's can be lower (its late pages
written back by an older submission still on the GPU). The fixes' queue in gpu/vk/vk_memory.cpp is cut out (LateFix, its
globals, late_fix_slot_pending, memory_late_fix_apply, late_fix_push, late_fix_range_value, memory_late_fix_wait_range)
and compiled with stubs:
- late_fix_top is the highest pending value, whatever order the fixes came in (a fix behind a lower one kept a top below its
  own value: a flush waited for the lower value and sent the recordings reading the unfixed slots);
- a flush as vk_replay.cpp's late_fix_flush_locked makes it (wait for the top, then apply) leaves nothing pending;
- an apply copies every fix its value reached, in any position, each slot from its own guest page, and no other;
- late_fix_slot_pending and late_fix_range_value see exactly the pending ones; memory_late_fix_wait_range waits for the
  range's value and applies;
- random pushes and applies keep these over 200000 steps.
And, by the text: vk_replay.cpp's fix thread idles only when nothing is pending; submit_recording and the fix thread apply
whatever is reached; the CPU readers (a replay read as submitted, a texture's storage read for an upload) wait and apply.

python -I -B tests/test_late_fix_order.py --cxx <compiler>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    rp = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    tex = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")

    lm = function(rp, "void late_fix_main() {")
    need("if (!memory_late_fix_pending()) lf_want = 0;" in lm and "lf_want <= v &&" not in lm,
         "late_fix_main: idle only when nothing is pending")
    need("if (memory_late_fix_pending()) {" in lm and "memory_late_fix_apply(completed());" in lm,
         "late_fix_main: applies whatever is reached")
    sr = function(rp, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0) {")
    need("if (memory_late_fix_pending()) {\n        memory_late_fix_apply(completed());" in sr,
         "submit_recording: applies whatever is reached")
    rd = function(mem, "bool memory_read_as_submitted(uint64_t gpu, void* dst, uint64_t n) {")
    a = rd.index("late_fix_slot_pending(e.gen, e.slot, &lv)")
    need(a < rd.index("vk_timeline_wait_sent(lv);") < rd.index("memory_late_fix_apply(vk_timeline_completed());")
         < rd.index("if (late_fix_slot_pending(e.gen, e.slot, nullptr)) {"),
         "memory_read_as_submitted: a pending slot's value waited for, the fix made, the slot read")
    gb = function(tex, "const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, std::vector<uint8_t>& scratch) {")
    need(gb.index("memory_late_fix_wait_range(gpu, n);") < gb.index("memory_as_submitted("),
         "guest_bytes: a pending late page's value waited for before the read")
    take = function(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    need("late_fix_push(std::move(f));" in take and "late_fixes.push_back" not in take, "take_capture: the fix registered by late_fix_push")

    first = mem.index("struct LateFix {")
    last = mem.index("void memory_late_fix_wait_range(uint64_t guest, uint64_t n) {")
    parts = mem[first:last] + function(mem, "void memory_late_fix_wait_range(uint64_t guest, uint64_t n) {")
    need("late_fixes.front()" not in parts, "the fixes are applied by value, not from the front only")

    fixture = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)

using VmaAllocation = void*;
constexpr unsigned kPageShift = 12;
constexpr uint64_t kGuestPages = 64;
static uint8_t guest_mem[kGuestPages * 4096];
struct Host { uint8_t* guest_base() { return guest_mem; } } host_obj, *host = &host_obj;
bool guarded_copy(void* dst, const void* src, size_t n) { std::memcpy(dst, src, n); return true; }
int flushes = 0;
void flush_mapped(VmaAllocation, size_t) { flushes++; }
uint64_t woken = 0;
void late_fix_wake(uint64_t v) { woken = std::max(woken, v); }
uint64_t gpu_done = 0, waited_for = 0;
void vk_timeline_wait_sent(uint64_t v) { waited_for = v; gpu_done = std::max(gpu_done, v); }
uint64_t vk_timeline_completed() { return gpu_done; }

PARTS

// (each capture's buffer: 8 slots)
struct Buf { uint8_t b[8 * 4096]; };
std::map<uint32_t, Buf*> bufs;
LateFix make(uint64_t value, uint32_t gen, std::vector<std::pair<uint32_t, uint32_t>> slots) {
    Buf*& b = bufs[gen];
    if (!b) b = new Buf, std::memset(b->b, 0xEE, sizeof b->b);
    LateFix f;
    f.value = value, f.gen = gen, f.map = b->b, f.alloc = nullptr, f.nbytes = sizeof b->b;
    f.slots = std::move(slots);
    return f;
}
bool slot_is(uint32_t gen, uint32_t slot, uint32_t page) {
    return std::memcmp(bufs[gen]->b + (uint64_t)slot * 4096, guest_mem + (uint64_t)page * 4096, 4096) == 0;
}
uint64_t pending_max() {
    std::lock_guard lk(late_fix_mu);
    uint64_t v = 0;
    for (const LateFix& f : late_fixes) v = std::max(v, f.value);
    return v;
}
void reset() {
    std::lock_guard lk(late_fix_mu);
    late_fixes.clear();
    late_fix_top.store(0);
    for (auto& g : late_fix_gens) g.store(0);
}

int main() {
    for (uint64_t i = 0; i < sizeof guest_mem; i++) guest_mem[i] = (uint8_t)(i * 7 + (i >> 12));
    // (1) a fix with a lower value behind a higher one: the top stays the higher one
    late_fix_push(make(10, 1, {{0, 3}, {2, 5}}));
    late_fix_push(make(8, 2, {{1, 7}}));
    CHECK(late_fix_top.load() == 10);
    CHECK(memory_late_fix_pending() == 10 && woken == 10);
    CHECK(st_late_def_lower.load() == 1);
    CHECK(late_fix_slot_pending(1, 0, nullptr) && late_fix_slot_pending(1, 2, nullptr) && !late_fix_slot_pending(1, 1, nullptr));
    uint64_t v = 0;
    CHECK(late_fix_slot_pending(2, 1, &v) && v == 8);
    CHECK(late_fix_range_value(3 * 4096 + 100, 8) == 10 && late_fix_range_value(7 * 4096, 4096) == 8);
    CHECK(late_fix_range_value(0, 3 * 4096) == 0 && late_fix_range_value(3 * 4096, 5 * 4096) == 10);
    // (2) the GPU at 9: the second fix (8) applied though behind the first, the first still pending, the top still 10
    memory_late_fix_apply(9);
    CHECK(slot_is(2, 1, 7));
    CHECK(!late_fix_slot_pending(2, 1, nullptr) && late_fix_slot_pending(1, 0, nullptr));
    CHECK(!slot_is(1, 0, 3) && !slot_is(1, 2, 5));
    CHECK(late_fix_top.load() == 10 && late_fix_count() == 1);
    // (3) a flush (late_fix_flush_locked): waits for the top, applies: nothing pending, every slot the guest page's
    { const uint64_t t = memory_late_fix_pending(); gpu_done = t; memory_late_fix_apply(std::max(gpu_done, t)); }
    CHECK(memory_late_fix_pending() == 0 && late_fix_count() == 0);
    CHECK(slot_is(1, 0, 3) && slot_is(1, 2, 5));
    for (auto& g : late_fix_gens) CHECK(g.load() == 0);
    // (4) the texture storage read: waits for the range's value, applies
    reset();
    gpu_done = 20;
    late_fix_push(make(25, 3, {{4, 9}}));
    late_fix_push(make(22, 4, {{0, 11}}));
    memory_late_fix_wait_range(11 * 4096 + 64, 128);
    CHECK(waited_for == 22 && slot_is(4, 0, 11) && !late_fix_slot_pending(4, 0, nullptr));
    CHECK(late_fix_slot_pending(3, 4, nullptr) && memory_late_fix_pending() == 25);
    waited_for = 0;
    memory_late_fix_wait_range(12 * 4096, 4096);   // (no pending page there: no wait)
    CHECK(waited_for == 0);
    memory_late_fix_wait_range(9 * 4096, 1);
    CHECK(waited_for == 25 && memory_late_fix_pending() == 0 && slot_is(3, 4, 9));
    // (5) random pushes (values up and down, at most kLateFixMax pending) and applies
    reset();
    std::mt19937_64 rng(60);
    gpu_done = 0;
    uint32_t gen = 10;
    int64_t lower = 0, applied = 0;
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> live;   // (gen -> its slots, pending)
    std::map<uint32_t, uint64_t> live_v;
    for (int step = 0; step < 200000; step++) {
        const int op = (int)(rng() % 3);
        if (op == 0 && late_fix_count() < (size_t)kLateFixMax) {
            const uint64_t val = gpu_done + 1 + rng() % 12;
            std::vector<std::pair<uint32_t, uint32_t>> s;
            for (uint32_t k = 0; k < 8; k++)
                if (rng() % 3 == 0) s.push_back({k, (uint32_t)(rng() % kGuestPages)});
            if (s.empty()) s.push_back({0, (uint32_t)(rng() % kGuestPages)});
            if (pending_max() > val) lower++;
            gen++;
            live[gen] = s, live_v[gen] = val;
            late_fix_push(make(val, gen, s));
            // (guest memory changes once the fix is registered: the fix copies what is there when it is applied)
            for (int k = 0; k < 4; k++) guest_mem[rng() % sizeof guest_mem] ^= (uint8_t)(1 + rng() % 255);
        } else if (op == 1) {
            gpu_done += rng() % 4;
            memory_late_fix_apply(gpu_done);
            for (auto it = live.begin(); it != live.end();) {
                if (live_v[it->first] <= gpu_done) {
                    for (const auto& [slot, page] : it->second) CHECK(slot_is(it->first, slot, page));
                    for (const auto& [slot, page] : it->second) CHECK(!late_fix_slot_pending(it->first, slot, nullptr));
                    applied++;
                    it = live.erase(it);
                } else {
                    for (const auto& [slot, page] : it->second) {
                        uint64_t pv = 0;
                        CHECK(late_fix_slot_pending(it->first, slot, &pv) && pv == live_v[it->first]);
                    }
                    ++it;
                }
            }
        } else {   // (a flush: the top waited for, then applied: nothing left)
            const uint64_t t = memory_late_fix_pending();
            if (t) {
                CHECK(t == pending_max());
                gpu_done = std::max(gpu_done, t);
                memory_late_fix_apply(gpu_done);
                CHECK(memory_late_fix_pending() == 0 && late_fix_count() == 0);
                for (auto& [g, s] : live)
                    for (const auto& [slot, page] : s) CHECK(slot_is(g, slot, page));
                applied += (int64_t)live.size();
                live.clear();
            }
        }
        CHECK(late_fix_top.load() == pending_max());
        CHECK(late_fix_count() == live.size());
        size_t gens = 0;
        for (auto& g : late_fix_gens) gens += g.load() != 0;
        CHECK(gens == live.size());
    }
    CHECK(lower > 1000 && applied > 10000);
    std::printf("PASS the pending late fixes: top = highest pending value (%lld of the fixes below a pending one), every reached fix "
                "applied in any position (%lld), a flush leaves none, the readers' lookups exact\n", (long long)lower, (long long)applied);
    return 0;
}
'''.replace("PARTS", parts)
    work = ROOT / "work"
    work.mkdir(exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix="late-fix-order-", dir=work))
    cpp, exe = out / "fixture.cpp", out / "fixture.exe"
    cpp.write_text(fixture, encoding="utf-8")
    subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Wno-unused-function", "-Wno-unused-variable", "-Werror",
                    "-pthread", str(cpp), "-o", str(exe)], check=True)
    env = dict(os.environ)
    compiler = Path(args.cxx)
    if compiler.is_file():
        env["PATH"] = str(compiler.resolve().parent) + os.pathsep + env.get("PATH", "")
    subprocess.run([str(exe)], check=True, env=env, timeout=300)
    print("Retained:", out)


if __name__ == "__main__":
    main()
