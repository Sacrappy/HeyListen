"""(TICK-058) RX_VK_FENCE_POLL_FAST: a fence poll (a wait of timeout 0) on a submission finished whole on its queue answers
without sync_order_mu, with the locked path's answer.

By the text of gpu/vk/vk_replay.cpp:
- off by default (fence_poll_fast_on reads RX_VK_FENCE_POLL_FAST);
- a value is queued for the mark only where run_submit finishes a submission whole (beside rec_open_values, not where it
  stops at a wait), only for a submission on a queue (value_queue has those: submit_q), and only with the recordings' timeline;
- the mark is stored (release) under rec_map_mu after value_rec has the recording's values, then the list is cleared;
- in wait_inner the fast path is after the completed() check and before sync_order_mu, only for timeout 0, only under the
  conditions of the locked path's "mine" (not async, sync waits on, per-queue fences on), only when the slot holds exactly f
  (acquire) and f is within value_queue's 8192 newest, and it calls wait_recorded(f, 0, ...) as the locked path does; when
  wait_recorded does not know f, the locked path runs;
- the locked path is unchanged: sync_order_mu, value_queue, qruns' blocked()/oldest(), then wait_recorded;
- submit_q still holds sync_order_mu for the replay (the reason for the fast path) and assigns value_queue only for a queue;
- with the switch on, a readback of GPU results that have write-backs (invalidate's write-back branch) waits for the replay in
  progress (sync_order_mu) before it reads the range's write-back value: the poll's lock did that before (without it OoT's
  exposure readback read the state of before that replay: the scene blown out to white); with TICK-059's
  RX_VK_READBACK_TICKET the ticket's wait replaces it (tests/test_readback_ticket.py).

python -I -B tests/test_fence_poll_fast.py
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

    on = function(rp, "bool fence_poll_fast_on()")
    need('getenv("RX_VK_FENCE_POLL_FAST") && atoi(getenv("RX_VK_FENCE_POLL_FAST")) != 0' in on,
         "RX_VK_FENCE_POLL_FAST is not off by default")
    need("std::atomic<uint64_t> whole_val[kWholeSlots];" in rp and "constexpr uint64_t kWholeSlots = 16384;" in rp,
         "the marks are not an atomic slot array")

    rs = function(rp, "bool run_submit(")
    i = rs.index("rec_open_values.push_back(value);")
    need("if (queue && rec_timeline) rec_open_qvalues.push_back(value);" in rs[i:i + 200],
         "a whole submission on a queue is not queued for the mark beside rec_open_values")
    need(rs.count("rec_open_qvalues") == 1, "a value is queued for the mark elsewhere in run_submit")
    stop = rs.index("if (stop) {")
    els = rs.index("} else {", stop)
    need("rec_open_qvalues" not in rs[stop:els], "a stopped submission is queued for the mark")
    need(rp.count("rec_open_qvalues.push_back") == 1, "another place queues values for the mark")

    sr = function(rp, "void submit_recording(")
    lock = sr.index("std::lock_guard mk(rec_map_mu);")
    emp = sr.index("for (uint64_t v : rec_open_values) value_rec.emplace(v, seq);")
    mark = sr.index("whole_val[v & (kWholeSlots - 1)].store(v, std::memory_order_release);")
    clr = sr.index("rec_open_qvalues.clear();")
    need(lock < emp < mark < clr, "the mark is not set under rec_map_mu after value_rec has the values, then cleared")
    need(sr[lock:clr].count("}") == 0, "the mark is set outside the rec_map_mu scope")
    need(rp.count("whole_val[") == 3, "whole_val is used outside the mark and wait_inner")

    wi = function(rp, "int wait_inner(uint64_t f, uint64_t timeout_ns) {")
    c = wi.index("if (completed() >= f) return 1;")
    fast = wi.index("if (!timeout_ns && fence_poll_fast_on() && !async_on() && sync_waits_on() && !per_queue_fences_off() &&")
    locked = wi.index("std::lock_guard lk(sync_order_mu);")
    need(c < fast < locked, "the fast path is not between the completed() check and the locked path")
    body = wi[fast:locked]
    need("whole_val[f & (kWholeSlots - 1)].load(std::memory_order_acquire) == f" in body, "the slot is not checked for exactly f")
    need("f + 8000 > q_values.load()" in body, "f is not bounded to value_queue's newest entries")
    need("if (wait_recorded(f, 0, &ok)) return ok;" in body, "the fast path does not answer through wait_recorded(f, 0)")
    need("return" not in body.replace("if (wait_recorded(f, 0, &ok)) return ok;", ""), "the fast path returns otherwise")
    lk = wi[locked:locked + 600]
    need("auto it = value_queue.find(f);" in lk and "mine = q == qruns.end() || !q->second.blocked() || q->second.oldest() > f;" in lk,
         "the locked path's mine changed")
    need("if (mine && wait_recorded(f, timeout_ns, &ok)) return ok;" in wi, "the locked path's wait_recorded changed")

    wr = function(rp, "bool wait_recorded(uint64_t f, uint64_t timeout_ns, int* ok) {")
    need("std::lock_guard mk(rec_map_mu);" in wr and "value_rec.find(f)" in wr, "wait_recorded does not look f up under rec_map_mu")

    sq = function(rp, "void submit_q(const uint64_t* handles, int n, uint64_t queue) {")
    need("std::lock_guard order(sync_order_mu);" in sq, "submit_q no longer holds sync_order_mu")
    need("if (queue) {\n                q_last[queue] = value;\n                value_queue[value] = queue;" in sq,
         "value_queue is not assigned only for a queue")
    need("while (value_queue.size() > 8192)" in sq, "value_queue keeps another count")
    inv = function(rp, "void invalidate(uint64_t gpu, uint64_t size) {")
    br = inv.index("if (!off && (memory_writeback_value(host->gpu_guest(gpu), size, &ever), ever)) {")
    bar = inv.index("{ std::lock_guard lk(sync_order_mu); }")
    val = inv.index("const uint64_t v = memory_writeback_value(host->gpu_guest(gpu), size, nullptr);")
    need(br < bar < val, "the readback does not wait for the replay in progress before its write-back value")
    need("else if (!ticket && fence_poll_fast_on() && !async_on() && sync_waits_on() && !per_queue_fences_off()) {" in inv[br:bar],
         "the readback's wait is not under the fast poll's conditions (without TICK-059's ticket: test_readback_ticket.py)")
    print("ok")


if __name__ == "__main__":
    main()
