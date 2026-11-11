"""(HeyListen TICK-053) The late pages merged: the capture's wait for the GPU ended (gpu/vk/vk_memory.cpp, "the late pages merged").

Compiles late_diff_runs (cut out of vk_memory.cpp) and checks its runs: 4-byte granular, adjacent words joined, nothing for
equal pages, a whole page of changes one run, alternating words 512 runs. Then reads the source: RX_VK_LATE_MERGE with the
wait as the off path, the shadow checked before the wait is skipped and wanted after it, the late bit in the capture table's
slot, the merge at the sync with the chunk hashes voided, the shadow refreshed by a whole or chunked sync, and void where the
mirror takes the page another way (live from guest memory, a readback refresh, a kept bare page, a segment's sync).

python -I -B tests/test_late_merge.py [--cxx clang++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
%s
static int fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %%s:%%d %%s\n", __FILE__, __LINE__, #c); fail++; } } while (0)
int main() {
    alignas(16) uint8_t a[4096], b[4096];
    uint32_t off[512], len[512];
    std::memset(a, 0x11, sizeof a), std::memset(b, 0x11, sizeof b);
    CHECK(late_diff_runs(a, b, off, len, 512) == 0);                  // (equal: nothing)
    a[100] ^= 1;                                                       // (one byte: its word)
    CHECK(late_diff_runs(a, b, off, len, 512) == 1 && off[0] == 100 && len[0] == 4);
    a[104] ^= 1;                                                       // (the next word: one run of 8)
    CHECK(late_diff_runs(a, b, off, len, 512) == 1 && off[0] == 100 && len[0] == 8);
    a[4095] ^= 1;                                                      // (the page's last word: a second run)
    CHECK(late_diff_runs(a, b, off, len, 512) == 2 && off[1] == 4092 && len[1] == 4);
    std::memset(a, 0x22, sizeof a);                                    // (every word: one run of the page)
    CHECK(late_diff_runs(a, b, off, len, 512) == 1 && off[0] == 0 && len[0] == 4096);
    std::memcpy(a, b, sizeof a);
    for (int i = 0; i < 4096; i += 8) a[i] ^= 0x80;                    // (every other word: 512 runs of 4)
    CHECK(late_diff_runs(a, b, off, len, 512) == 512 && off[511] == 4088 && len[511] == 4);
    CHECK(late_diff_runs(a, b, off, len, 511) == -1);                  // (one short: too many)
    std::memcpy(a, b, sizeof a);
    a[3] ^= 0x40, a[8] ^= 1;                                           // (words 0 and 2: two runs, word 1 left out)
    CHECK(late_diff_runs(a, b, off, len, 512) == 2 && off[0] == 0 && len[0] == 4 && off[1] == 8 && len[1] == 4);
    std::printf(fail ? "late_diff_runs: %%d FAILED\n" : "late_diff_runs: ok\n", fail);
    return fail ? 1 : 0;
}
"""


def cut(src, start, end_marker="\n}\n"):
    a = src.index(start)
    b = src.index(end_marker, a) + len(end_marker)
    return src[a:b]


def check_source(src):
    for needle in (
            # (the switch, and the wait as the off path)
            'static const bool v = !getenv("RX_VK_LATE_MERGE") || atoi(getenv("RX_VK_LATE_MERGE")) != 0;',
            "if (!late_merge) {\n            st_late_waited.fetch_add(1, std::memory_order_relaxed);",
            "std::lock_guard sl(pools_mu);\n            late_merge = true;",
            "vk_timeline_wait(c->late_value);",
            # (no wait only when every late page has a shadow of its mirror; else one wanted for each)
            "if (!late_shadow_valid_locked(c->pages[i])) {\n                    late_merge = false;",
            "for (uint32_t i : c->late) late_shadow_want(c->pages[i]);",
            "p.mstate == Pool::MIRROR_ON && p.mirror && p.mirror_id == it->second.mid) return true;",
            # (the late bit rides in the capture table's slot, stripped by cap_entry)
            "constexpr uint32_t kCapLateBit = 0x80000000u;",
            "e.slot &= ~kCapLateBit;",
            "CapEntry{g, c->slots[i] | (late_mark[c->slots[i]] ? kCapLateBit : 0u)}",
            # (the merge at the sync: only outside segments, for a whole page, with the shadow of this mirror)
            "if (lsh != late_shadows.end() && cap_entry_late(page) && b->second.map) {",
            "if (!seg && gb - ga == 4096 && s.mid == p.mirror_id && s.bytes) {",
            "const int nr = late_diff_runs(now, s.bytes.get(), off, len, 512);",
            "late_shadow_set(page, p.mirror_id, now);\n                            d_void(page);",
            # (the shadow refreshed by a whole or chunked sync of the page, void by a segment's)
            "late_shadow_set(page, p.mirror_id, (const uint8_t*)b->second.map + (uint64_t)e.slot * 4096);",
            # (void where the mirror takes the page another way)
            "late_shadow_void(page);   // (TICK-053: the mirror takes guest memory as it is at the replay, not a capture's page)",
            "late_shadow_void_range(pp->lo + r.dstOffset, pp->lo + r.dstOffset + r.size);",
            "st_prot_kept++;\n            d_void(page);\n            late_shadow_void(page);",
            # (the copies between two full barriers, as the scatter's)
            'full_barrier(upload_command_buffer(), "barrier (write-back or sync)");\n        for (const auto& [buf, rs] : late_runs) {'):
        if needle not in src:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)
    sync = cut(src, "int64_t mirror_sync(Pool& p) {")
    if sync.count("lsh->second.mid = 0;") < 1 or "s.mid = 0;   // (copied whole below" not in sync:
        raise SystemExit("mirror_sync no longer voids the shadow of a page it copies whole in a segment or cut by the pool's end")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    src = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    check_source(src)
    fn = cut(src, "int late_diff_runs(const uint8_t* now, const uint8_t* shadow, uint32_t* off, uint32_t* len, int max_runs) {")
    with tempfile.TemporaryDirectory(prefix="late-merge-") as tmp:
        cpp = Path(tmp) / "late_merge.cpp"
        cpp.write_text(HARNESS % fn, encoding="utf-8")
        exe = Path(tmp) / "late_merge.exe"
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
