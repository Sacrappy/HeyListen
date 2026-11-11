"""(HeyListen TICK-060) RX_VK_CAPTURE_HASH: the capture's copy makes the mirror syncs' chunk hashes, read from the source.
(msync_diff::copy_hash_page itself is checked against memcpy + page_hash by tests/test_msync_diff.cpp, with and without AVX2.)

- on by default since the PC 3050 confirm (=0: off), and only with the chunk diff on and an AVX2 build; RX_VK_MSYNC_FAST
  (TICK-056) on by default where the diff is on;
- copy_part copies and hashes page by page in one pass, a slot's hashes valid (hok) only when its copy succeeded;
- the slots copied again after the copy (a late page after the take's wait, a deferred late page at its fix) lose their hashes;
- the kept capture carries them (CapBuf::hash), retired ones give their arrays back;
- the mirror sync takes a slot's hashes only when valid, and hashes the slot as before otherwise.

python -I -B tests/test_capture_hash.py
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def body(src, start, end_marker="\n}\n"):
    a = src.index(start)
    return src[a:src.index(end_marker, a) + len(end_marker)]


def main():
    fails = []

    def need(cond, what):
        if not cond:
            fails.append(what)

    mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    on = body(mem, "bool capture_hash_on() {")
    need('(!getenv("RX_VK_CAPTURE_HASH") || atoi(getenv("RX_VK_CAPTURE_HASH")) != 0) && diff_mode() != 0' in on and
         "#if defined(__AVX2__)" in on and "return false;" in on, "capture_hash_on: on by default with the diff and AVX2 only, =0 off")
    fast = body(mem, "bool msync_fast_on() {")
    need('getenv("RX_VK_MSYNC_FAST") ? atoi(getenv("RX_VK_MSYNC_FAST")) != 0 : diff_mode() != 0' in fast,
         "msync_fast_on: on by default where the diff is on, the switch either way")
    cp = body(mem, "void copy_part(const CopyPart& w) {")
    hp = cp[cp.index("if (w.hash) {"):cp.index("for (int64_t i = w.i0; i < w.i1;) {")]
    need("guarded_copy_hash(w.out + (uint64_t)i * 4096, base + ga, w.hash + (uint64_t)i * msync_diff::kChunks, stream)" in hp and
         "w.ok[i] = 1, w.hok[i] = 1" in hp and "else\n                w.ok[i] = guarded_copy(" in hp and "stream_fence();" in hp,
         "copy_part: copy and hash in one pass, hok only on success, fenced")
    need("msync_diff::copy_hash_page(dst, src, h, nt);" in mem, "guarded_copy_hash: msync_diff::copy_hash_page")
    mk = mem[mem.index("std::unique_ptr<Capture> capture_make() {"):]
    need("c->hok.clear();" in mk and "copy_pages(pages, n, (uint8_t*)c->map, ok.data(), c->hashes.data(), c->hok.data());" in mk,
         "capture_make: hashes only from the plain copy path, none left from a reused capture")
    take = body(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    need("if (i < c->hok.size()) c->hok[i] = 0;" in take, "take_capture: a late page copied again loses its hashes")
    need("for (uint32_t i : c->late) c->hok[i] = 0;" in take, "take_capture: a deferred late page loses its hashes")
    need("ch->h.swap(c->hashes);" in take and "b.hash = std::move(ch);" in take, "take_capture: the kept capture carries them")
    need("cap_hash_free.push_back(std::move(it->second.hash));" in mem, "cap_buf_retire: arrays given back")
    need("if (ch && e.slot < ch->ok.size() && ch->ok[e.slot]) {" in mem and
         "msync_diff::page_hash((const uint8_t*)b->second.map + (uint64_t)e.slot * 4096, h);" in mem,
         "mirror sync: valid slot hashes taken, else hashed as before")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("test_capture_hash: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
