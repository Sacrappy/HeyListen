"""(TICK-061, C1) RX_VK_CAPTURE_CLAIM: the capture's copy in claimed chunks writes the same slots and the same ok[] as the
equal parts.

gpu/vk/vk_memory.cpp's copy machinery (copy_part, the copy helpers, ClaimJob / claim_run / claim_all, copy_pages) is cut out
of the source and compiled with stubs: a fake guest memory whose "unreadable" pages make a guarded copy fail (as an unmapped
page does), so the per-page fallback runs too. The same random captures (page lists with runs, gaps, unreadable pages, sizes
under and over the helpers' threshold) are copied with the switch off, on (chunks by size) and on with 8-page chunks (chunk
edges inside runs and inside failing runs); each run prints a hash of every buffer and ok[] and the three must match. The
claimed runs also check that no token is left in the queue and the helpers' count is back to 0 after every copy.

python -I -B tests/test_capture_claim.py --cxx <compiler>
"""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    source = (ROOT / "gpu/vk/vk_memory.cpp").read_text(encoding="utf-8")
    start = source.index("struct ClaimJob;\nstruct CopyPart {")
    cp = source.index("void copy_pages(const uint32_t* pages, int64_t n, uint8_t* out, uint8_t* ok", start)
    end = source.index("\n}\n", cp) + 3
    block = source[start:end]
    # (TICK-060's capture hashes, RX_VK_CAPTURE_HASH, sit between: cut out, with stubs below; the claimed copy makes none)
    h0 = block.find("// (HeyListen TICK-060) RX_VK_CAPTURE_HASH")
    if h0 >= 0:
        h1 = block.index("std::atomic<int64_t> st_hash_made{0}", h0)
        block = block[:h0] + block[block.index(chr(10), h1) + 1:]
    for need in ("claim_all(job, helpers);", "if (w.claim) w.claim->users++;", "copy_helpers.done.wait(lk, [&job] { return job.users == 0; });"):
        if need not in block:
            raise SystemExit("the claimed copy is not as this check knows it: " + need)
    fixture = r'''
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#ifndef _WIN32   // (Linux: host/posix's windows.h declares these; the copy helpers only name and lift their threads)
extern "C" {
HANDLE GetCurrentThread(void) { return nullptr; }
BOOL SetThreadPriority(HANDLE, int) { return 1; }
HRESULT SetThreadDescription(HANDLE, PCWSTR) { return 0; }
}
#endif
constexpr uint64_t kPageShift = 12;
constexpr uint32_t kGuestPages = 4096;
static uint8_t* guest;
static uint8_t bad[kGuestPages];
struct Host {
    void* (*guest_base)();
    uint64_t (*guest_size)();
    int64_t (*collect_written_in)(const uint64_t (*)[2], int, uint32_t**);
};
static Host host_v{[]() -> void* { return guest; }, []() -> uint64_t { return (uint64_t)kGuestPages * 4096 - 4096; },
                   [](const uint64_t (*)[2], int, uint32_t**) -> int64_t { return 0; }};
static Host* host = &host_v;
static void logf(const char*, ...) {}
static bool readable(const void* src, size_t n) {
    const uint64_t a = (uint64_t)((const uint8_t*)src - guest) >> 12, b = (uint64_t)((const uint8_t*)src - guest + n - 1) >> 12;
    for (uint64_t p = a; p <= b; p++)
        if (bad[p]) return false;
    return true;
}
static bool guarded_copy(void* dst, const void* src, size_t n) {
    if (!readable(src, n)) return false;
    std::memcpy(dst, src, n);
    return true;
}
static bool guarded_copy_stream(void* dst, const void* src, size_t n) { return guarded_copy(dst, src, n); }
static bool stream_on() { return (std::getenv("STREAM") != nullptr); }
static void stream_fence() { std::atomic_thread_fence(std::memory_order_seq_cst); }
namespace msync_diff { constexpr int kChunks = 16; }
static bool guarded_copy_hash(uint8_t*, const uint8_t*, uint64_t*, bool) { std::abort(); }
static std::atomic<int64_t> st_hash_made{0};
''' + block + r'''
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t nx() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
int main() {
    guest = (uint8_t*)std::malloc((size_t)kGuestPages * 4096);
    for (size_t i = 0; i < (size_t)kGuestPages * 4096; i++) guest[i] = (uint8_t)(i * 131 + (i >> 12) * 7);
    uint64_t h = 1469598103934665603ull;
    std::vector<uint32_t> pages;
    std::vector<uint8_t> out, ok;
    for (int round = 0; round < 600; round++) {
        std::memset(bad, 0, sizeof bad);
        for (int k = 0; k < (int)(nx() % 6); k++) bad[nx() % kGuestPages] = 1;
        pages.clear();
        const int64_t want = round % 5 == 0 ? (int64_t)(nx() % 128) : (int64_t)(nx() % 2600);
        uint32_t p = (uint32_t)(nx() % 64);
        while ((int64_t)pages.size() < want && p < kGuestPages) {
            pages.push_back(p);
            p += nx() % 4 == 0 ? 1 + (uint32_t)(nx() % 5) : 1;   // (runs of adjacent pages, and gaps)
        }
        if (round % 11 == 0 && !pages.empty()) pages.back() = kGuestPages - 1;   // (past the guest size: skipped, ok 0)
        const int64_t n = (int64_t)pages.size();
        out.assign((size_t)n * 4096, 0xCD);
        ok.assign((size_t)n, 7);
        copy_pages(pages.data(), n, out.data(), ok.data());
        {
            std::lock_guard lk(copy_helpers.mu);
            for (const CopyPart& t : copy_helpers.parts)
                if (t.claim) { std::fprintf(stderr, "FAIL: a token left in the queue\n"); return 1; }
        }
        for (uint8_t b : out) h = (h ^ b) * 1099511628211ull;
        for (uint8_t b : ok) h = (h ^ b) * 1099511628211ull;
    }
    std::printf("%016llx\n", (unsigned long long)h);
#ifndef _WIN32
    // (as the game ends, host/posix ExitProcess: glibc's exit() destroys copy_helpers' condition variables under the detached
    // helpers still waiting on them, and pthread_cond_destroy waits for those waiters for ever; Windows ends the threads first)
    std::fflush(stdout);
    std::_Exit(0);
#endif
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="capture-claim-") as tmp:
        cpp = os.path.join(tmp, "t.cpp")
        exe = os.path.join(tmp, "t.exe")
        Path(cpp).write_text(fixture, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O2"] + ([] if os.name == "nt" else ["-I", str(ROOT / "host/posix"), "-pthread"]) + ["-o", exe, cpp], check=True)
        hashes = {}
        for name, extra in (("parts", {}), ("claimed", {"RX_VK_CAPTURE_CLAIM": "1"}), ("claimed 8", {"RX_VK_CAPTURE_CLAIM": "8"}),
                            ("claimed, streaming", {"RX_VK_CAPTURE_CLAIM": "1", "STREAM": "1"}), ("2 helpers", {"RX_VK_CAPTURE_CLAIM": "1", "RX_VK_CAPTURE_THREADS": "2"})):
            env = dict(os.environ)
            env.pop("RX_VK_CAPTURE_CLAIM", None)
            env.pop("RX_VK_CAPTURE_THREADS", None)
            env.update(extra)
            r = subprocess.run([exe], capture_output=True, text=True, env=env, timeout=120)
            if r.returncode:
                raise SystemExit("FAIL (%s): %s" % (name, r.stderr.strip()))
            hashes[name] = r.stdout.strip()
        if len(set(hashes.values())) != 1:
            raise SystemExit("FAIL: the copies differ: %s" % hashes)
        print("PASS: the claimed copy writes the same bytes and ok[] as the equal parts (%s)" % ", ".join(hashes))


if __name__ == "__main__":
    main()
