"""A texture's first upload, and its re-upload, take only the submit-time copies their own submission's capture took
(gpu/vk/vk_memory.cpp: memory_as_submitted, first_upload_own_on, reupload_own_on, take_capture's shadow_seq_own;
gpu/vk/vk_texture.cpp: upload, guest_bytes; issue 66).

OoT fills a new texture's storage, which another texture had, after submitting the draws that sample it. The copies the
capture kept of those pages were the old texture's, taken many captures earlier, and the first upload read them: one frame of
the Market's lightmaps blown out, or a material in another's colours. With that fixed, OoT's in-place rewrites of the same
lightmaps and materials, a page written more than once, gave a re-upload an earlier capture's copy taken between two writes:
bursts of the same flashes. Source checks that take_capture marks its copies as the recorded submission's, that upload()
flags a first upload and a re-upload around upload_inner and guest_bytes passes both on; then memory_as_submitted,
first_upload_own_on and reupload_own_on compiled alone with a two-page shadow: a first upload and a re-upload take the page
their own capture copied and read guest memory for the one an earlier capture copied, a read outside an upload takes both,
and RX_VK_FIRST_UPLOAD_OWN=0 / RX_VK_REUPLOAD_OWN=0 take both as before. No game or GPU required.

python -I -B tests/test_first_upload_own.py [--cxx g++]
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MEM = ROOT / "gpu" / "vk" / "vk_memory.cpp"
TEX = ROOT / "gpu" / "vk" / "vk_texture.cpp"


def function(text, marker):
    """From marker to the brace closing its first one"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, what):
    if not cond:
        raise SystemExit(f"FAIL: {what}")


FIXTURE = r"""
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
static void logf(const char*, ...) {}
static bool overlay_off() { return false; }
struct ShadowPage {
    std::unique_ptr<uint8_t[]> b;
    uint64_t seq = 0;
};
std::mutex shadow_mu;
std::unordered_map<uint32_t, ShadowPage> shadow;
std::unordered_map<uint32_t, uint32_t> shadow_regions;
std::atomic<size_t> shadow_n{0};
std::atomic<uint64_t> shadow_seq_own{0};
std::map<uint64_t, std::pair<uint64_t, uint64_t>> tex_spans;
uint64_t tex_merged_bytes = 0;
@@FUNCS@@
static void keep(uint32_t page, uint8_t fill, uint64_t seq) {
    ShadowPage& s = shadow[page];
    s.b.reset(new uint8_t[4096]);
    std::memset(s.b.get(), fill, 4096);
    s.seq = seq;
    shadow_regions[page >> 9]++;
    shadow_n++;
}
int main(int argc, char** argv) {
    const bool first = argc > 1 && argv[1][0] == '1', again = argc > 1 && argv[1][0] == '0';   // ('n': not an upload)
    // two pages of a texture's storage: guest memory holds 0x11 in both; the capture of the submission being recorded (seq 40)
    // copied page 0x1000 as 0x22, an earlier one (seq 13) page 0x1001 as 0x33
    const uint64_t guest = 0x1000ull << 12;
    std::vector<uint8_t> live(8192, 0x11), scratch;
    keep(0x1000, 0x22, 40);
    keep(0x1001, 0x33, 13);
    shadow_seq_own = 40;
    const uint8_t* got = memory_as_submitted(guest, live.data(), live.size(), scratch, first, again);
    std::printf("%02x %02x\n", got[0], got[4096]);
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    mem = MEM.read_text(encoding="utf-8")
    tex = TEX.read_text(encoding="utf-8")

    take = function(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    i_clear = take.index("shadow_seq_own.store(0, std::memory_order_relaxed);")
    i_block = take.index("if (!c->pages.empty() && tex_capture_on()) {   // (the texture storage it took")
    i_inc = take.index("shadow_seq++;", i_block)
    i_set = take.index("shadow_seq_own.store(shadow_seq, std::memory_order_relaxed);")
    need(i_clear < i_block < i_inc < i_set, "take_capture: the recorded submission's copies are the ones its capture's block takes")
    up = function(tex, "bool upload(Texture& tex, const Ranges* dirty) {")
    i_flag, i_up, i_off = up.index("tl_first_upload = !tex.uploaded;"), up.index("upload_inner(tex, dirty)"), up.index("tl_first_upload = tl_reupload = false;")
    i_again = up.index("tl_reupload = tex.uploaded;")
    need(i_flag < i_up < i_off and i_again < i_up, "upload: a first upload and a re-upload flagged around upload_inner and cleared after it")
    gb = function(tex, "const uint8_t* guest_bytes(uint64_t gpu, uint64_t n, std::vector<uint8_t>& scratch) {")
    need("memory_as_submitted(gpu, p, n, scratch, tl_first_upload, tl_reupload)" in gb, "guest_bytes: the uploads' flags passed on")
    print("source: take_capture marks the recorded submission's copies; a first upload and a re-upload are flagged and passed on")

    cxx = shutil.which(a.cxx)
    if not cxx:
        print(f"SKIP: {a.cxx} not found (compiled check)")
        return
    funcs = (mem[mem.index("std::atomic<int64_t> st_overlaid{0}, st_differed{0};"):mem.index("bool first_upload_own_on() {")]
             + function(mem, "bool first_upload_own_on() {") + "\n"
             + function(mem, "bool reupload_own_on() {") + "\n"
             + function(mem, "const uint8_t* memory_as_submitted(uint64_t guest, const uint8_t* live, uint64_t n, std::vector<uint8_t>& scratch, bool first,"))
    with tempfile.TemporaryDirectory(prefix="first-upload-own-") as tmp:
        cpp, exe = Path(tmp) / "t.cpp", Path(tmp) / "t.exe"
        cpp.write_text(FIXTURE.replace("@@FUNCS@@", funcs), encoding="utf-8")
        subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(cpp), "-o", str(exe)],
                       check=True)
        F, A = "RX_VK_FIRST_UPLOAD_OWN", "RX_VK_REUPLOAD_OWN"
        cases = ((None, "1", "22 11", "a first upload: its own capture's copy, guest memory over an earlier capture's"),
                 (None, "0", "22 11", "a re-upload: the same"),
                 (None, "n", "22 33", "a read outside an upload: every copy"),
                 ({F: "1", A: "1"}, "1", "22 11", f"{F}=1: as the default"),
                 ({F: "0"}, "1", "22 33", f"{F}=0: a first upload takes every copy, as before"),
                 ({F: "0"}, "0", "22 11", f"{F}=0: a re-upload still its own capture's"),
                 ({A: "0"}, "0", "22 33", f"{A}=0: a re-upload takes every copy, as before"),
                 ({A: "0"}, "1", "22 11", f"{A}=0: a first upload still its own capture's"))
        for value, first, want, what in cases:
            env = {k: v for k, v in os.environ.items() if k not in (F, A)}
            env.update(value or {})
            got = subprocess.run([str(exe), first], env=env, capture_output=True, text=True, timeout=30).stdout.strip()
            need(got == want, f"{what}: read {got}, expected {want}")
            print(f"{what}: {got}")
    print("PASS")


if __name__ == "__main__":
    main()
