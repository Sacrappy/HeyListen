"""(TICK-056) RX_VK_PRESYNC_EARLY: the mirrors' pre-sync on a replay worker during the dry pass keeps its place in the queue.

By the text of gpu/vk/vk_replay.cpp and vk_texture.cpp:
- off by default (presync_early_at reads RX_VK_PRESYNC_EARLY, 0 when unset);
- the job records into a command buffer of its own (the thread's upload command buffer swapped for the slot's cmd and back), with
  a full barrier after its copies, and runs the same memory_sync_mirrors_before_segments;
- a job is taken once, under its mutex, by its number (a job the replay thread took back or dropped is a no-op later);
- replay_handle finishes it after the dry pass and before every path that records or submits (the one-segment path, the init
  fallback, the parallel path's submit of rec_cur), and the finish submits it before returning; the pre-sync on the replay thread
  stays before rec_cur's submit (the safety net);
- it is kicked in the dry pass (at a draw count) or before it (the last frame's big command buffer of the same order);
- texture_upload_swap forgets the thread's last full barrier (a command buffer handle comes round again).

python -I -B tests/test_presync_early.py
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
    tx = (ROOT / "gpu/vk/vk_texture.cpp").read_text(encoding="utf-8")

    at = function(rp, "int64_t presync_early_at()")
    need('getenv("RX_VK_PRESYNC_EARLY") ? ' in at and ": 0;" in at, "RX_VK_PRESYNC_EARLY is not off by default")

    run = function(rp, "void presync_early_run(PresyncEarly& e)")
    need(run.count("texture_upload_swap(") == 2, "the job does not swap the upload command buffer in and back")
    need("texture_upload_swap({e.r->cmd, false})" in run, "the job does not record into its slot's command buffer")
    need("memory_sync_mirrors_before_segments();" in run, "the job does not run the pre-sync")
    need("if (mine.used) full_barrier(e.r->cmd);" in run, "no barrier after the job's copies")

    start = function(rp, "void presync_early_start() {")
    need("begin_segment_recording(rec_cur_value)" in start, "the job's slot is not a recording slot")
    need("if (e.gen != gen || e.taken) return;" in start, "a job is not taken once by its number")
    need("par_pool.pending++" in start, "the job is not counted in the pool's pending work")

    fin = function(rp, "void presync_early_finish(bool segments)")
    need("if (!e.taken) e.taken = here = true;" in fin, "the finish does not take a job no worker started")
    need("e.cv.wait(l, [&e] { return e.done; });" in fin, "the finish does not wait for a worker's job")
    need("if (here && segments) presync_early_run(e);" in fin, "a job taken back is not run when segments follow")
    need("vkEndCommandBuffer(r.cmd);" in fin and "vkQueueSubmit2(dev.queue, 1, &si, r.fence)" in fin,
         "the finish does not end and submit the job's command buffer")
    need(fin.index("queue_flush_locked();") < fin.index("vkQueueSubmit2("), "the batched submits are not flushed first")

    rh = function(rp, "const uint8_t* replay_handle(VkCommandBuffer& cmd, const uint8_t* h, bool may_stop)")
    i_dry = rh.index("jt_mark(14, 0);")
    i_fin = rh.index("if (presync_early.r) presync_early_finish(seg.size() > 1);")
    need(i_dry < i_fin, "the job is finished before the dry pass ends")
    for later in ("if (seg.size() == 1) {", "Recording& r = *rec_cur;", "submit_recording(r, false);",
                  "texture_set_segment(++seq);", "par_active = true;"):
        need(i_fin < rh.index(later), "the job is finished after: " + later)
    need(rh.index("memory_sync_mirrors_before_segments();") < rh.index("submit_recording(r, false);"),
         "the replay thread's pre-sync no longer precedes rec_cur's submit")
    need("presync_kick_at = par_pool.started ? presync_early_at() : 0;" in rh, "the kick is not armed before the dry pass")
    need("if (presync_kick_at && !seg_early_on() && (pk_big_last >> pk_this & 1)) presync_early_start(), presync_kick_at = 0;" in rh,
         "the kick at the dry pass's start (last frame's big command buffer) is missing")
    need(rh.index("presync_kick_at = 0;\n") < i_fin, "the kick is not disarmed when the dry pass ends")

    rep = function(rp, "void replay(VkCommandBuffer& cmd, NvnState& s, const uint8_t* p, const uint8_t* end = nullptr)")
    need("if (dry_work == presync_kick_at) presync_early_start();" in rep, "the dry pass does not kick the job at its draw count")

    swap = function(tx, "UploadTarget texture_upload_swap(UploadTarget to)")
    need("last_barrier_cmd = VK_NULL_HANDLE;" in swap, "texture_upload_swap keeps the last full barrier's command buffer")
    print("PASS the early pre-sync records into its own command buffer, is finished and submitted ahead of rec_cur, off by default")


if __name__ == "__main__":
    main()
