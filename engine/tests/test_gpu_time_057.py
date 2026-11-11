"""(HeyListen TICK-057) GPU time a frame on PC 3050: what the switches of this ticket do, read from the source.

- vk_work.h: every draw, pass edge, barrier and command buffer begin counts by its command buffer (all_work_slot), NGX's commands
  with none in foreign_work; vkCmdEndRendering's one-argument call still compiles (__VA_OPT__).
- vk_texture.cpp full_barrier: RX_VK_BARRIER_DEDUPE_PAR (off by default) leaves out a full barrier inside a parallel replay only when
  the command buffer's own count and the foreign count are what they were at this thread's last full barrier on it; the serial
  dedupe is as before.
- vk_memory.cpp mirrors_give_back: RX_VK_MIRROR_VRAM_FREE (off by default) judges the 80% VRAM rule by what the process holds less
  the free space in VMA's blocks (as the textures' budget does). RX_VK_MIRROR_STICKY (off by default) keeps a mirror made again soon after it went, against the
  VRAM rule only (not past kLongIdle, not when the mirrors' own budget is short); mirror_make counts such mirrors.
- vk_replay.cpp / vk_gprof.cpp: the GPU's busy tail before a late-page wait (diagnostic, always on; by label with
  RX_VK_PASS_TIMING).

python -I -B tests/test_gpu_time_057.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def read(name):
    return (VK / name).read_text(encoding="utf-8")


def body(src, start, end_marker="\n}\n"):
    a = src.index(start)
    return src[a:src.index(end_marker, a) + len(end_marker)]


def main():
    fails = []

    def need(cond, what):
        if not cond:
            fails.append(what)

    work = read("vk_work.h")
    need("inline std::atomic<uint64_t>& all_work_slot(VkCommandBuffer c)" in work, "vk_work.h: all_work_slot")
    for name in ("vkBeginCommandBuffer", "vkCmdPipelineBarrier2", "vkCmdBeginRendering", "vkCmdDraw", "vkCmdDrawIndexed",
                 "vkCmdDrawIndirect", "vkCmdDrawIndexedIndirect", "vkCmdDrawIndirectCount", "vkCmdDrawIndexedIndirectCount",
                 "vkCmdDrawMeshTasksEXT", "vkCmdDrawMeshTasksIndirectEXT", "vkCmdDrawMeshTasksIndirectCountEXT"):
        need(f"#define {name}(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::{name}(c_, __VA_ARGS__))" in work,
             f"vk_work.h: {name} counted by its command buffer")
    need("#define vkCmdEndRendering(c_, ...) (::mkwvk::note_work_cmd(c_), ::vkx::vkCmdEndRendering(c_ __VA_OPT__(,) __VA_ARGS__))"
         in work, "vk_work.h: vkCmdEndRendering(cmd) with one argument")
    nw = body(work, "inline void note_work() {")
    need("foreign_work.fetch_add(1" in nw and "work_epoch.fetch_add(1" in nw, "vk_work.h: note_work counts foreign work and the epoch")
    no = body(work, "inline void note_other_work(VkCommandBuffer c) {")
    need("all_work_slot(c).fetch_add(1" in no and "other_work_slot(c).fetch_add(1" in no, "vk_work.h: note_other_work counts all work")

    tex = read("vk_texture.cpp")
    on = body(tex, "bool barrier_dedupe_par_on() {")
    need('getenv("RX_VK_BARRIER_DEDUPE_PAR")' in on and "e && atoi(e) != 0" in on, "vk_texture.cpp: RX_VK_BARRIER_DEDUPE_PAR off by default")
    fb = body(tex, "void full_barrier(VkCommandBuffer cmd, const char* why) {")
    need("const bool dedupe = !off && !par;" in fb and "work_epoch.load(std::memory_order_relaxed) == last_barrier_epoch" in fb,
         "vk_texture.cpp: the serial dedupe as before")
    par = fb[fb.index("if (!off && par && barrier_dedupe_par_on()"):]
    par = par[:par.index("return;")]
    for needle in ("cmd == last_barrier_cmd", "all_work_slot(cmd).load(std::memory_order_relaxed) == last_barrier_cmd_work",
                   "foreign_work.load(std::memory_order_relaxed) == last_barrier_foreign"):
        need(needle in par, f"vk_texture.cpp: the parallel dedupe checks {needle}")
    after = fb[fb.index("vkCmdPipelineBarrier2(cmd, &d);"):]
    need(after.index("last_barrier_cmd_work = all_work_slot(cmd).load") > 0, "vk_texture.cpp: the count taken after the barrier is recorded")
    need("thread_local uint64_t last_barrier_cmd_work" in tex, "vk_texture.cpp: the count is the recording thread's")

    mem = read("vk_memory.cpp")
    st = body(mem, "bool mirror_sticky_on() {")
    need('getenv("RX_VK_MIRROR_STICKY")' in st and "e && atoi(e) != 0" in st, "vk_memory.cpp: RX_VK_MIRROR_STICKY off by default")
    gb = body(mem, "void mirrors_give_back(uint64_t want_bytes) {")
    need("const uint64_t want_own = want_bytes;" in gb and gb.index("const uint64_t want_own") < gb.index("if (vram_tight) want_bytes"),
         "vk_memory.cpp: the mirrors' own need taken before the VRAM rule")
    need("if (!long_idle && p->remade && mirror_sticky_on() && mirror_bytes + want_own <= mirror_budget())" in gb,
         "vk_memory.cpp: sticky only against the VRAM rule, never past kLongIdle")
    need("p->idle_freed_epoch = mirror_epoch;" in gb, "vk_memory.cpp: an idle mirror's going noted")
    need('getenv("RX_VK_MIRROR_VRAM_FREE")' in gb and "e && atoi(e) != 0" in gb and
         "if (count_free) used -= std::min(used, block_free);" in gb and "used > allowed / 100 * 80" in gb,
         "vk_memory.cpp: RX_VK_MIRROR_VRAM_FREE (off) counts VMA's free block space as free in the 80% rule")
    mk = body(mem, "bool mirror_make(Pool& pool) {")
    need("if (p->idle_freed_epoch && mirror_epoch - p->idle_freed_epoch <= kLongIdle)" in mk and "p->remade++" in mk,
         "vk_memory.cpp: a mirror made again soon after it went counted")
    need("gpu_tail_note_late(c->late_value)" in mem and
         mem.index("gpu_tail_note_late(c->late_value)") < mem.index("vk_timeline_wait(c->late_value);"),
         "vk_memory.cpp: the late value noted before the wait")

    rep = read("vk_replay.cpp")
    tc = body(rep, "void gpu_tail_compute(uint64_t v, double period) {")
    need("20000.0" in tc and "if (s[j].end + thr < lo) break;" in tc, "vk_replay.cpp: the tail ends at a gap of 20 us")
    need("if (gprof_on()) gprof_tail(lo, end);" in tc, "vk_replay.cpp: the tail's spans by label with the pass timing")
    need("ts_sig[r.slot] = v;" in rep, "vk_replay.cpp: each slot's signalled value")
    gp = read("vk_gprof.cpp")
    need("void gprof_tail(uint64_t lo, uint64_t hi) {" in gp and "if (r.counted || r.t0 < lo || r.t1 > hi) continue;" in gp,
         "vk_gprof.cpp: a span counted in one tail at most")

    if fails:
        for f in fails:
            print("FAIL", f)
        return 1
    print("PASS: TICK-057 switches (barrier dedupe by command buffer, sticky mirrors, GPU tail) as described")
    return 0


if __name__ == "__main__":
    sys.exit(main())
