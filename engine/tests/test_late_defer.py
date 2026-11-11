"""(HeyListen TICK-060) RX_VK_LATE_DEFER: the late pages' wait moved from the capture's take to the queue, read from the source.

- vk_memory.cpp take_capture: on by default since the PC 3050 confirm (=0: off); a take does not wait only when the switch is on, LATE_MERGE is off, the late value is
  already sent to the queue (vk_timeline_signal_sent), no late page is a live texture's storage and fewer than kLateFixMax fixes are
  pending; otherwise the wait and the copies as before. A deferred take registers a LateFix (value, generation, buffer, slots) only
  for a kept buffer, and gives no texture shadow copy of a late slot.
- memory_late_fix_apply copies each late slot from guest memory (as the take did after its wait), flushes the buffer, frees the
  generation, and leaves late_fix_top at the highest pending value (0: none).
- The CPU's readers of a capture buffer ask late_fix_slot_pending: memory_read_as_submitted waits for the value and reads guest
  memory (not kept in its one-page cache); the mirror sync's chunk diff copies such a page whole and records no hash.
- vk_replay.cpp: queue_flush_locked applies the pending fixes before anything goes to the queue; submit_recording holds a
  recording (the batched path) while a fix is pending and flushes the held ones before a direct submit; every other submit site
  waits for a pending fix outside dev.present_mu and flushes first; a poll neither waits nor flushes; the fix thread applies and
  sends the held recordings once the value is reached; the timeline's sent signal is noted at both vkQueueSubmit2 calls that
  signal it.

python -I -B tests/test_late_defer.py
"""
import re
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

    mem = read("vk_memory.cpp")
    on = body(mem, "bool late_defer_on() {")
    need('!getenv("RX_VK_LATE_DEFER") || atoi(getenv("RX_VK_LATE_DEFER")) != 0' in on, "vk_memory.cpp: RX_VK_LATE_DEFER on by default, =0 off")

    take = body(mem, "bool take_capture(std::unique_ptr<Capture> c, uint64_t value, uint64_t completed) {")
    a = take.index("// (TICK-060) RX_VK_LATE_DEFER: no wait here")
    gate = take[a:take.index("if (!late_merge && !late_defer) {", a)]
    need("!late_merge_on() && late_defer_on() && c->late_value > vk_timeline_completed()" in gate,
         "take_capture: deferred only with the switch on, LATE_MERGE off, the value not yet reached")
    need("replay_tex_storage_in((uint64_t)c->pages[i] << kPageShift, 4096)" in gate, "take_capture: a live texture's storage waits")
    need("c->late_value > vk_timeline_signal_sent()" in gate, "take_capture: a value not yet sent waits")
    need("late_fix_count() >= kLateFixMax" in gate, "take_capture: at most kLateFixMax pending")
    need(gate.rstrip().endswith("}") and "else late_defer = true" in gate, "take_capture: late_defer set only in the last branch")
    old = take[take.index("if (!late_merge && !late_defer) {"):]
    need("vk_timeline_wait(c->late_value);" in old and "guarded_copy((uint8_t*)c->map + (uint64_t)i * 4096" in old,
         "take_capture: the wait and the copies as before otherwise")
    kept = take[take.index("if (!c->copied.empty() && g && c->map) {"):]
    reg = kept[kept.index("if (late_defer) {"):]
    need("f.value = c->late_value, f.gen = g, f.map = (uint8_t*)b.map" in reg and "late_fix_push(std::move(f));" in reg,
         "take_capture: a kept buffer's fix registered")
    push = body(mem, "void late_fix_push(LateFix&& f) {")
    need("late_fixes.push_back(std::move(f));" in push and "late_fix_top.store(std::max(top, v)" in push and
         push.index("late_fix_wake(v);") > push.index("late_fixes.push_back"), "late_fix_push: registered, the top raised only, then woken")
    need("if (late_defer && std::find(c->late.begin(), c->late.end(), c->slots[i]) != c->late.end()) continue;" in take,
         "take_capture: no texture shadow copy of a deferred late slot")

    ap = body(mem, "void memory_late_fix_apply(uint64_t done) {")
    need("if (f.value > done) {" in ap and "top = std::max(top, f.value);" in ap and "late_fixes.front()" not in ap and
         "guarded_copy(f.map + (uint64_t)slot * 4096, base + ((uint64_t)page << kPageShift), 4096);" in ap and
         "flush_mapped(f.alloc, f.nbytes);" in ap and "late_fix_gens[k].store(0" in ap and
         "late_fix_top.store(top, std::memory_order_release);" in ap,
         "memory_late_fix_apply: every reached fix (any position) copied and flushed, top the highest pending (TICK-060m)")
    pend = body(mem, "bool late_fix_slot_pending(uint32_t gen, uint32_t slot, uint64_t* value) {")
    need("if (!gen || !late_fix_top.load(std::memory_order_acquire)) return false;" in pend and "std::lower_bound" in pend,
         "late_fix_slot_pending: quick test, then the slots")
    rd = body(mem, "bool memory_read_as_submitted(uint64_t gpu, void* dst, uint64_t n) {")
    need("late_fix_slot_pending(e.gen, e.slot, &lv)" in rd and "vk_timeline_wait_sent(lv);" in rd and
         "memory_late_fix_apply(vk_timeline_completed());" in rd and
         "if (page0 == page1 && !late_read) {" in rd, "memory_read_as_submitted: a pending late slot read after its value and fix")
    need("const bool late_pending = late_fix_slot_pending(e.gen, e.slot, nullptr);" in mem and
         "if (!late_pending && d_ok && gb - ga == 4096 && b->second.map) {" in mem, "mirror sync: no hash of a pending late slot")

    rep = read("vk_replay.cpp")
    qf = body(rep, "void queue_flush_locked() {")
    need(qf.index("late_fix_flush_locked();") < qf.index("if (pend.cbs.empty()) return;"), "queue_flush_locked: fixes first")
    need("if (res == VK_SUCCESS && pend.any_signal) note_signal_sent(pend.signal);" in qf, "queue_flush_locked: sent signal noted")
    sr = body(rep, "void submit_recording(Recording& r, bool signal_value = true, uint64_t signal_at = 0) {")
    need("if (batch_on() || late_hold) {" in sr, "submit_recording: held while a fix is pending")
    direct = sr[sr.index("PROF(PROF_QSUBMIT);") - 400:]
    need("queue_flush_locked();" in direct[:direct.index("vkQueueSubmit2")], "submit_recording: held ones flushed before a direct submit")
    need("if (r.submitted && signal_value) note_signal_sent(signal.value);" in sr, "submit_recording: sent signal noted")
    poll = body(rep, "void queue_flush_poll() {")
    need("lv && completed() < lv) return;" in poll, "queue_flush_poll: no flush while a fix waits")
    fl = body(rep, "void queue_flush_for(int why) {")
    need(fl.index("late_fix_wait_unlocked()") < fl.index("std::lock_guard lk(dev.present_mu);"), "queue_flush_for: waits outside the lock")
    wu = body(rep, "bool late_fix_wait_unlocked() {")
    need("if (tl_poll) return false;" in wu, "late_fix_wait_unlocked: a poll does not wait")
    wi = body(rep, "int wait_inner(uint64_t f, uint64_t timeout_ns) {")
    need("poll_flag(timeout_ns == 0);" in wi and wi.index("poll_flag(") < wi.index("queue_flush_for(FR_WAIT);"), "wait_inner: polls flagged")
    lm = body(rep, "void late_fix_main() {")
    need("memory_late_fix_apply(completed());" in lm and "if (!memory_late_fix_pending() && !pend.cbs.empty())" in lm,
         "late_fix_main: applies and sends the held recordings")

    # every vkQueueSubmit2 on dev.queue in the replay and the textures: a flush (which applies the fixes) under dev.present_mu first,
    # with the fix waited for outside the lock where the submit is not the batched flush itself
    for name in ("vk_replay.cpp", "vk_texture.cpp", "vk_memory.cpp"):
        src = read(name)
        for m in re.finditer(r"vkQueue(?:Submit2|BindSparse)\(dev\.queue", src):
            before = src[src.rfind("\n}\n", 0, m.start()):m.start()]   # (from the enclosing function's start)
            if "void queue_flush_locked() {" in before:
                need("late_fix_flush_locked();" in before, f"{name}: the batched flush applies the fixes")
                continue
            line = src.count("\n", 0, m.start()) + 1
            need("queue_flush_locked();" in before, f"{name}:{line}: a queue submit without a flush before it")
            need("late_fix_wait_unlocked();" in before, f"{name}:{line}: a queue submit without the fix waited for outside the lock")

    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print("test_late_defer: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
