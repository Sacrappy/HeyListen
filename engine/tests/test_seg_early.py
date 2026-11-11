"""(TICK-056) RX_VK_SEG_EARLY: a big command buffer's segments handed out while its dry pass still runs keep what they read stable.

By the text of gpu/vk/vk_replay.cpp and vk_draw.cpp:
- off by default (seg_early_on reads RX_VK_SEG_EARLY);
- the dry pass's appends to ub_updates and par_conds hold ub_grow_mu (UbGrowGuard), the workers' reads of them hold it shared
  (UbReadGuard in ub_overlay, ub_read and a worker's render condition), and only a worker's read takes it, only while growing;
- the dry pass calls the hook right after it keeps a point, and the hook copies the point's state into the next segment (the
  points' vector moves as it grows) and hands a segment out only once its end is passed;
- before the dry pass, in rec_cur: the pre-sync and the part's readback refresh; par_active on before the dry pass, the growth
  flag and the hook cleared after it; rec_cur submitted before the replay thread records the last segment, and the segments
  submitted in order after it; every handed-out segment waited for (the pool's pending count) before replay_handle_early returns;
- the dry pass's written-image marks take ParLock (segments read them meanwhile).

python -I -B tests/test_seg_early.py
"""
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
    rp = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    dr = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")

    on = function(rp, "bool seg_early_on()")
    need('getenv("RX_VK_SEG_EARLY") && atoi(getenv("RX_VK_SEG_EARLY")) != 0' in on, "RX_VK_SEG_EARLY is not off by default")

    rg = function(rp, "struct UbReadGuard")
    need("rmode == Mode::Worker && ub_growing.load(std::memory_order_acquire)" in rg, "a read guard locks outside a worker or growth")
    need("ub_read_depth++ == 0" in rg, "a nested read takes the shared lock again")
    need("std::unique_lock<std::shared_mutex>(ub_grow_mu)" in function(rp, "struct UbGrowGuard"), "the growth guard is not exclusive")
    need("UbReadGuard ug;" in function(rp, "uint64_t ub_overlay("), "ub_overlay reads the list unguarded")
    need("UbReadGuard ug;" in function(rp, "bool ub_read(uint64_t addr, void* dst, uint64_t n)"), "ub_read reads the list unguarded")

    rep = function(rp, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr)")
    i = rep.index("ub_updates.push_back(std::move(up));")
    need("UbGrowGuard ug;" in rep[i - 200:i], "the dry pass appends a uniform update unguarded")
    i = rep.index("par_conds.push_back(s.render_enabled ? 1 : 0);")
    need("UbGrowGuard ug;" in rep[i - 200:i], "the dry pass appends a render condition unguarded")
    i = rep.index("s.render_enabled = cond_at < par_conds.size() ? par_conds[cond_at] != 0 : true;")
    need("UbReadGuard ug;" in rep[i - 200:i], "a worker reads a render condition unguarded")
    i = rep.index("par_points.push_back({p + h.size, s, ub_updates.size(), par_conds.size(), dry_work});")
    need("if (seg_early_hook) seg_early_hook->on_point(par_points.back());" in rep[i:i + 300], "the hook is not called after a point")

    e = function(rp, "const uint8_t* replay_handle_early(")
    i_dry = e.index("jt_mark(13, 0);")
    for before in ("memory_sync_mirrors_before_segments();", "texture_rb_refresh_now();", "texture_segments_begin(inits.data(), inits.size());",
                   "par_active = true;", "ub_growing.store(true", "seg_early_hook = &hook;", "memory_rb_segments_begin(base, (uint64_t)nplan);"):
        need(e.index(before) < i_dry, "not before the dry pass: " + before)
    after = e[e.index("jt_mark(14, 0);"):]
    need(after.index("seg_early_hook = nullptr;") < after.index("const size_t used = open + 1;"), "the hook outlives the dry pass")
    need("ub_growing.store(false, std::memory_order_release);" in after, "the growth flag is not cleared after the dry pass")
    hook = e[e.index("hook.on_point = [&](const ParPoint& q) {"):e.index("if (early) {")]
    need("(int)open + 1 >= nplan || q.work < plan[open]" in hook, "a segment is handed out before its planned end is passed")
    need("n->s = q.s;" in hook and "&n->s" in hook, "the next segment points into the moving points' vector")
    need(hook.index("j->g.end = q.p;") < hook.index("par_pool.jobs.push_back"), "a segment is handed out before its end is set")
    need(e.index("submit_recording(r, false);") < e.index("record_job(last);"), "rec_cur is not submitted before the segments")
    need(e.index("record_job(last);") < e.index("vkQueueSubmit2(dev.queue, 1, &si, j->r->fence)"), "the segments' submits are out of place")
    need(e.index("par_pool.done.wait(lk, [] { return par_pool.pending == 0; });") < e.index("par_active = false;\n    texture_segments_begin(nullptr, 0);\n    seg_ms"),
         "par_active goes off before every segment is done")
    whole = e[e.index("if (used < 2 || !early) {"):e.index("*cut = true;")]
    need("par_active = false;" in whole and "record_segment(cmd, whole);" in whole, "no boundary reached: not recorded whole, serially")

    rh = function(rp, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop)")
    need("seg_early_count(cut ? 0 : 1);" in rh and "seg_early_count(2);" in rh, "the plans used and rejected are not counted")
    need(rh.index("if (nplan >= 2) {") < rh.index("replay_handle_early("), "a plan of fewer than two segments is not rejected")
    need(rh.index("seg_early_count(2);") < rh.index("rmode = Mode::Dry;"), "a rejected plan does not fall back to the old path")

    dm = function(dr, "void dry_mark_written(NvnState& s, bool compute, bool first)")
    need(dm.count("ParLock lk;") == 2, "the dry pass's marks are not taken under ParLock")
    print("PASS early segments: guarded list growth, marks under ParLock, pre-sync and refresh first, submits in order, off by default")


if __name__ == "__main__":
    main()
