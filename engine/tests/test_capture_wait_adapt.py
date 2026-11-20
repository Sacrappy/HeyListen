"""(ported from Hookshot 54530a0d, pixl27) The captures' wait for late pages follows the GPU: no white flashes on slower cards.

Read from the source (no GPU or game):

- vk_replay.cpp: vk_timeline_wait_for(v, ms) is the one wait, true when the timeline reached v; vk_timeline_wait keeps its
  250 ms bound by calling it, and the lost-device and timeout lines are still in it.
- vk_memory.cpp take_capture: the late-page wait's cap is RX_VK_CAPTURE_WAIT_ADAPT (on by default) as
  clamp(RX_VK_CAPTURE_WAIT_K * p90 / 1000, RX_VK_CAPTURE_WAIT_MAX_MS, max(that, 250)) ms; =0 the fixed 250 ms this port
  found (RX_VK_CAPTURE_WAIT_MAX_MS, when set). p90 is the running estimate of the long waits: +2 ms when a wait is cut,
  +0.9 ms when one takes longer than it, -0.1 ms otherwise, never below 0, started at half the floor. A slow card is said
  once; a 1,200-wait line carries the cap, the cuts and the estimate.
- A cut wait leaves the submit's copy of its late pages (and their hashes) in place: re-copying guest memory there is what
  puts the CPU's reset sums over the GPU's light (the white flash). The switch off restores the fixed cap and the same
  copy-on-cut, so an A/B changes the bound only.
- With RX_VK_LATE_DEFER on (the default) the takes it keeps still wait here; the deferred ones already follow the GPU on
  the fix thread (up to 2 s, readers waiting), so this port neither replaces nor disables that path.

python -I -B tests/test_capture_wait_adapt.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def read(name):
    return (VK / name).read_text(encoding="utf-8")


def braces(src, start):
    a = src.index(start)
    end = src.index("{", a) + 1
    depth = 1
    while depth:
        depth += (src[end] == "{") - (src[end] == "}")
        end += 1
    return src[a:end]


def main():
    fails = []

    def need(cond, what):
        if not cond:
            fails.append(what)

    rep = read("vk_replay.cpp")
    wait = braces(rep, "bool vk_timeline_wait_for(uint64_t v, uint32_t timeout_ms) {")
    need("if (!timeline || completed() >= v) return true;" in wait, "vk_timeline_wait_for: an already reached value needs no wait")
    need("vkWaitSemaphores(dev.device, &wi, (uint64_t)timeout_ms * 1000000ull)" in wait,
         "vk_timeline_wait_for: the caller's bound is the wait's")
    need("return wres == VK_SUCCESS;" in wait, "vk_timeline_wait_for: whether the value was reached")
    need(re.search(r"void vk_timeline_wait\(uint64_t v\) \{.*?\(void\)vk_timeline_wait_for\(v, 250\);", rep, re.S) is not None,
         "vk_timeline_wait: still at most 250 ms, through vk_timeline_wait_for")
    need("bool vk_timeline_wait_for(uint64_t v, uint32_t timeout_ms);" in read("vk_internal.h"),
         "vk_internal.h: the timed wait declared")

    mem = read("vk_memory.cpp")
    take = braces(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    a = take.index("if (!late_merge && !late_defer) {")
    late = braces(take, "if (!late_merge && !late_defer) {")
    need(a >= 0, "take_capture: the wait branch")
    need("static const bool adapt = !getenv(\"RX_VK_CAPTURE_WAIT_ADAPT\") || atoi(getenv(\"RX_VK_CAPTURE_WAIT_ADAPT\")) != 0;" in late,
         "take_capture: RX_VK_CAPTURE_WAIT_ADAPT on by default, =0 the fixed cap")
    need("static const int wait_max_ms = getenv(\"RX_VK_CAPTURE_WAIT_MAX_MS\") ? std::max(0, atoi(getenv(\"RX_VK_CAPTURE_WAIT_MAX_MS\"))) : 20;" in late,
         "take_capture: the floor defaults to 20 ms (RX_VK_CAPTURE_WAIT_MAX_MS)")
    need("static const int wait_fixed_ms = getenv(\"RX_VK_CAPTURE_WAIT_MAX_MS\") ? wait_max_ms : 250;" in late,
         "take_capture: the switch off keeps this port's fixed 250 ms unless MAX_MS names another")
    need("static const int64_t k = getenv(\"RX_VK_CAPTURE_WAIT_K\") ? std::clamp<int64_t>(atoi(getenv(\"RX_VK_CAPTURE_WAIT_K\")), 1, 16) : 4;" in late,
         "take_capture: RX_VK_CAPTURE_WAIT_K, 4 by default")
    need("static std::atomic<int64_t> p90_us{(int64_t)wait_max_ms * 500};" in late,
         "take_capture: the estimate starts at half the floor")
    need("const int cap_ms = adapt ? (int)std::clamp<int64_t>(k * p90 / 1000, wait_max_ms, std::max(wait_max_ms, 250)) : wait_fixed_ms;" in late,
         "take_capture: the cap follows the GPU, at most 250 ms, and is the fixed one when off")
    need("const bool must_wait = adapt && done0 < c->late_value;" in late,
         "take_capture: only a real wait under the adaptive cap moves the estimate")
    need("if (!done) p90_us.fetch_add(2000, std::memory_order_relaxed);" in late and
         "else if (took > p90) p90_us.fetch_add(900, std::memory_order_relaxed);" in late and
         "else if (p90_us.fetch_sub(100, std::memory_order_relaxed) < 100) p90_us.store(0, std::memory_order_relaxed);" in late,
         "take_capture: +2 ms cut, +0.9 ms over the estimate, -0.1 ms otherwise, never below 0")
    need("if (cap_ms >= 2 * wait_max_ms && !told.exchange(true))" in late and "the cap follows a slower GPU, now %d ms" in late,
         "take_capture: a card slower than the floor is said once")
    need("st_cut += !done;" in late and "the waits' 90th percentile %.1f ms" in late and
         re.search(r"if \(\+\+st_waits % 1200 == 0\)", late) is not None,
         "take_capture: the 1,200-wait line names the cap, the cuts and the estimate")
    copy = late[late.index("for (uint32_t i : c->late) {"):]
    need(copy.index("if (!done) continue;") < copy.index("guarded_copy((uint8_t*)c->map + (uint64_t)i * 4096"),
         "take_capture: a cut wait leaves the submit's copy of a late page (no reset words over the GPU's light)")
    need("if (done) flush_mapped(c->alloc, c->nbytes);" in late, "take_capture: nothing to flush when the copies were left")
    need("if (cap_ms > 0) done = vk_timeline_wait_for(c->late_value, (uint32_t)cap_ms);" in late and
         "else vk_timeline_wait(c->late_value);" in late,
         "take_capture: the timed wait, and RX_VK_CAPTURE_WAIT_MAX_MS=0 as before")

    # our wait path already there stays: LATE_DEFER's fix thread waits up to 8 x 250 ms, off the replay thread
    need("bool late_defer_on() {" in mem and
         "for (int i = 0; i < 8 && vkWaitSemaphores(dev.device, &wi, 250000000ull) == VK_TIMEOUT; i++)" in rep,
         "the deferred fix's wait (up to 8 x 250 ms) is untouched")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("test_capture_wait_adapt: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
