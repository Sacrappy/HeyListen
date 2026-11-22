"""(Pillar 4B) RX_VK_WB_PIECE: per-piece write-back tracking and early write-back emission.

Compiles the source's WbPages table and piece-sequence lookup rule into a standalone C++ fixture and verifies:
- distinct pieces of the same submission value track independent rec_timeline sequences per page;
- later piece overwrites raise a page's sequence, while untouched pages keep the earlier piece's sequence;
- a pending page without a piece sequence (rec == 0) returns 0 so invalidate falls back to submission-level wait;
- pages already completed by submission value (pv <= done) are excluded from the pending rec sequence.

And, by the source text of gpu/vk/vk_memory.cpp and gpu/vk/vk_replay.cpp:
- RX_VK_WB_PIECE defaults to 1 (=0 off, =2 also flushes immediately after segmented command buffers);
- emit_writebacks_locked stamps both mirror write-backs and direct_writebacks with vk_next_rec_seq() and tags StagedWb;
- capture_make skips late pages whose piece sequence has already reached vk_rec_timeline_completed();
- note_rec_sent is recorded at both vkQueueSubmit2 sites that signal rec_timeline;
- wait_recorded_seq skips queue_flush_for(FR_WAIT) (and late_fix_wait_unlocked) when seq <= vk_rec_signal_sent();
- invalidate waits on wb_rec when known and skips the whole-submission strict wait once rec_timeline reaches wb_rec.

python -I -B tests/test_wb_piece.py [--cxx g++]
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()

    mem = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    rp = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")

    # Source wiring checks for vk_memory.cpp
    need("wb_pages, wb_rec_pages;" in mem, "vk_memory.cpp: wb_rec_pages table missing")
    sw = block(mem, "int wb_piece_mode() {")
    need('getenv("RX_VK_WB_PIECE")' in sw and "atoi(e) : 1" in sw, "wb_piece_mode: RX_VK_WB_PIECE does not default to 1")
    note = block(mem, "void wb_pages_note(uint64_t guest_lo, uint64_t guest_hi, uint64_t value, uint64_t rec")
    need("wb_pages.set(p, value);" in note and "wb_rec_pages.set(p, rec);" in note,
         "wb_pages_note: does not update both wb_pages and wb_rec_pages")
    wbrec = block(mem, "uint64_t memory_writeback_rec(uint64_t guest, uint64_t size, uint64_t done) {")
    need("wb_rec_pages.chunks" in wbrec and "if (!pr) return 0;" in wbrec,
         "memory_writeback_rec: does not require every pending page to carry a piece sequence")
    cap = block(mem, "std::unique_ptr<Capture> capture_make() {")
    need("vk_rec_timeline_completed()" in cap and "if (pr && pr <= rdone) continue;" in cap,
         "capture_make: does not skip pages whose piece write-back already completed on rec_timeline")
    emit = block(mem, "void emit_writebacks_locked(VkCommandBuffer cmd) {")
    need("vk_next_rec_seq()" in emit and "direct_writebacks.clear();" in emit and "staged.rec_seq = rec;" in emit,
         "emit_writebacks_locked: does not tag write-backs, direct_writebacks, or StagedWb with rec_seq")
    staged = block(mem, "void mkwvk::memory_staged_apply(uint64_t done) {")
    need("vk_rec_timeline_completed()" in staged and "s.rec_seq > rdone" in staged,
         "memory_staged_apply: does not release staged write-backs on rec_timeline piece completion")

    # Source wiring checks for vk_replay.cpp
    qf = block(rp, "void queue_flush_locked() {")
    need("if (res == VK_SUCCESS && pend.rec) note_rec_sent(pend.rec);" in qf,
         "queue_flush_locked: note_rec_sent missing")
    sr = block(rp, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0) {")
    need("if (r.submitted && seq) note_rec_sent(seq);" in sr,
         "submit_recording: note_rec_sent missing")
    wrs = block(rp, "bool wait_recorded_seq(uint64_t seq, uint64_t timeout_ns, int* ok) {")
    need("if (vk_rec_timeline_completed() >= seq)" in wrs and "if (seq > vk_rec_signal_sent()) queue_flush_for(FR_WAIT);" in wrs,
         "wait_recorded_seq: does not short-circuit completed sequences or avoid flushing already-sent recordings")
    rhe = block(rp, "const uint8_t* replay_handle_early(")
    rh = block(rp, "const uint8_t* replay_handle(")
    need("wb_flush_before_segments(r.cmd);" in rhe and "wb_flush_before_segments(r.cmd);" in rh,
         "replay_handle_early / replay_handle: missing pre-segment write-back flush")
    need("wb_flush_after_segments(cmd);" in rhe and "wb_flush_after_segments(cmd);" in rh,
         "replay_handle_early / replay_handle: missing mode>=2 post-segment write-back flush")
    inv = block(rp, "void invalidate(uint64_t gpu, uint64_t size) {")
    need("memory_writeback_rec(host->gpu_guest(gpu), size, c0)" in inv and
         "wb_rec ? wait_recorded_seq(wb_rec, UINT64_MAX, &ok) : wait_recorded(v, UINT64_MAX, &ok)" in inv and
         "!(wb_rec && vk_rec_timeline_completed() >= wb_rec)" in inv,
         "invalidate: does not wait via wait_recorded_seq(wb_rec, ...) or skip whole-submission strict wait after piece completion")

    # Standalone functional test of WbPages + memory_writeback_rec logic
    table = block(mem, "struct WbPages {")
    lookup = wbrec[wbrec.index("{") + 1:wbrec.rindex("}")]
    fixture = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>
constexpr uint64_t kChunkPages = 16, kPages = 64, kPageShift = 12;
static bool wb_piece_on() { return true; }
static std::mutex wb_pages_mu;
#define CHECK(ok) do { if (!(ok)) { std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #ok); std::exit(1); } } while (0)
''' + table + r''' wb_pages, wb_rec_pages;
uint64_t memory_writeback_rec(uint64_t guest, uint64_t size, uint64_t done) {
''' + lookup + r'''
}
static void note_page(uint64_t page, uint64_t val, uint64_t rec) {
    wb_pages.set(page, val);
    wb_rec_pages.set(page, rec);
}
int main() {
    // Submission 100 spans multiple pieces on rec_timeline:
    //   Piece 1 (rec=50): page 2
    //   Piece 2 (rec=51): page 15 & page 16 (chunk edge, e.g. exposure meter)
    //   Piece 4 (rec=53): page 40 (late event page)
    note_page(2, 100, 50);
    note_page(15, 100, 51);
    note_page(16, 100, 51);
    note_page(40, 100, 53);

    // Querying Piece 2's range does NOT inherit Piece 4's sequence (53):
    CHECK(memory_writeback_rec(15ull << 12, 2ull << 12, 99) == 51);
    // Querying Piece 1's page gives 50:
    CHECK(memory_writeback_rec(2ull << 12, 4096, 99) == 50);
    // Querying across Piece 1 and Piece 2 returns max(50, 51) = 51 (skipping unallocated chunk pages between them):
    CHECK(memory_writeback_rec(2ull << 12, 15ull << 12, 99) == 51);
    // Once submission 100 is completed (done >= 100), no piece sequence is pending:
    CHECK(memory_writeback_rec(15ull << 12, 2ull << 12, 100) == 0);

    // If Piece 3 overwrites page 2 with rec=52 while submission 101 writes page 5 without a rec tag (0):
    note_page(2, 100, 52);
    CHECK(memory_writeback_rec(2ull << 12, 4096, 99) == 52);
    note_page(5, 101, 0);
    // Range overlapping an untagged pending page falls back to 0:
    CHECK(memory_writeback_rec(2ull << 12, 4ull << 12, 99) == 0);
    // Range covering only tagged pages ignores untagged pages outside it:
    CHECK(memory_writeback_rec(15ull << 12, 2ull << 12, 99) == 51);

    // Already-done write-back (value 90 <= done 99) alongside pending page 15 (value 100, rec 51):
    note_page(14, 90, 0);
    CHECK(memory_writeback_rec(14ull << 12, 2ull << 12, 99) == 51);

    std::puts("PASS: per-piece write-back tracking and isolation across pieces and chunk edges");
}
'''
    with tempfile.TemporaryDirectory(prefix="wb-piece-") as tmp:
        cpp = Path(tmp) / "fixture.cpp"
        exe = Path(tmp) / "fixture.exe"
        cpp.write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        env = dict(os.environ)
        compiler = Path(args.cxx)
        if compiler.is_file():
            env["PATH"] = str(compiler.resolve().parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe)], check=True, env=env, timeout=60)


if __name__ == "__main__":
    main()
