"""(TICK-054) HOS_NVN_HOLD=2 (host/nvn_native.c): the submits of threads other than the presenting one are held and replayed by
the presenting thread before its own next submit, or by whoever needs them first. The source must keep: the mode read as a
number (2 is a mode of its own); the presenting thread learned at the present; its own submit flushing the held ones under
the same lock before it replays its own; every point that needs the replay current flushing (a queue flush, a finish, a present,
a fence wait, an event poll); a held submit keeping its queue and going to the replay with it; the recording sweep leaving a
held submit's recordings alone (dead copies freed only past the held count); the check mode's helpers marking the pages again.

python -I -B tests/test_nvn_hold.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "host"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class NvnHoldTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.nvn = (HOST / "nvn_native.c").read_text(encoding="utf-8")
        cls.rx = (HOST / "rx_backend.c").read_text(encoding="utf-8")

    def test_mode_is_a_number(self):
        init = block(self.nvn, "static void held_init(void)")
        self.assertIn('getenv("HOS_NVN_HOLD")', init)
        self.assertRegex(init, r"hold_on = e \? atoi\(e\) : 0;")
        self.assertIn('getenv("HOS_NVN_HOLD_CHECK")', init)

    def test_presenting_thread_learned_at_the_present(self):
        present = block(self.nvn, "NV(nvnQueuePresentTexture)")
        self.assertIn("present_thread = hos_self();", present)
        self.assertIn("held_flush_why(HF_PRESENT);", present)
        self.assertLess(present.index("present_thread = hos_self();"), present.index("held_flush_why(HF_PRESENT);"))
        self.assertLess(present.index("held_flush_why(HF_PRESENT);"), present.index("rx_nvn_present("))

    def test_presenting_threads_submit_flushes_first_under_the_flush_lock(self):
        submit = block(self.nvn, "NV(nvnQueueSubmitCommands)")
        self.assertIn("hold_on == 2 && (!present_thread || hos_self() == present_thread)", submit)
        own = submit[submit.index("if (!hold_on || mine)"):submit.index("return;", submit.index("if (!hold_on || mine)"))]
        self.assertIn("EnterCriticalSection(&flush_cs)", own)
        self.assertIn("held_flush_why(HF_SUBMIT)", own)
        self.assertLess(own.index("EnterCriticalSection(&flush_cs)"), own.index("held_flush_why(HF_SUBMIT)"))
        self.assertLess(own.index("held_flush_why(HF_SUBMIT)"), own.index("rx_nvn_submit_q(h, n,"))
        self.assertLess(own.index("rx_nvn_submit_q(h, n,"), own.index("LeaveCriticalSection(&flush_cs)"))
        # a held submit keeps its queue, the handles and when it was held
        held = submit[submit.index("held_submit *hs = &held[nheld];"):]
        for field in ("hs->queue = (uint64_t)(uintptr_t)q;", "memcpy(hs->h, h, 8 * (size_t)n);", "hs->t = hos_now_ns();",
                      "if (hold_check) hs->npages = hos_hold_snapshot(&hs->pages, &hs->copy);"):
            self.assertIn(field, held)
        self.assertIn("typedef struct { int n; uint64_t h[64]; uint64_t queue; uint32_t *pages; int64_t npages; uint8_t *copy; int64_t t; } held_submit;",
                      self.nvn)

    def test_flush_replays_in_order_with_the_queue(self):
        flush = block(self.nvn, "static void held_flush_why(int why) {")
        self.assertIn("EnterCriticalSection(&flush_cs);", flush)
        self.assertIn("LeaveCriticalSection(&held_cs);", flush)
        # the list lock is let go before the replay (the replay takes the recordings' lock)
        self.assertLess(flush.index("LeaveCriticalSection(&held_cs);"), flush.index("rx_nvn_submit_q(local[i].h, local[i].n, local[i].queue)"))
        self.assertIn("if (hold_on == 2) rx_nvn_submit_q(local[i].h, local[i].n, local[i].queue);", flush)
        self.assertIn("hos_hold_check(local[i].pages, local[i].npages, local[i].copy, &again, &differ);", flush)
        self.assertLess(flush.index("hos_hold_check("), flush.index("rx_nvn_submit_q(local[i].h"))
        self.assertIn("hos_vcore_away()", flush)

    def test_the_thread_is_woken_at_each_hold(self):
        submit = block(self.nvn, "NV(nvnQueueSubmitCommands)")
        self.assertIn("if (hold_event) SetEvent(hold_event);", submit)
        self.assertLess(submit.index("nheld++;"), submit.index("if (hold_event) SetEvent(hold_event);"))
        thread = block(self.nvn, "static DWORD WINAPI hold_flush_thread(void *p)")
        self.assertIn("WaitForSingleObject(hold_event, INFINITE);", thread)
        self.assertIn("held_flush_why(HF_THREAD);", thread)
        init = block(self.nvn, "static void held_init(void)")
        self.assertIn("if (hold_on == 2) {", init)
        self.assertIn("CreateThread(NULL, 0, hold_flush_thread, NULL, 0, NULL)", init)

    def test_every_consumer_flushes(self):
        self.assertIn("NV(nvnQueueFlush) { if (hold_on != 2) held_flush_why(HF_FLUSH); }", self.nvn)
        self.assertIn("held_flush_why(HF_FINISH);", block(self.nvn, "NV(nvnQueueFinish)"))
        wait = block(self.nvn, "NV(nvnSyncWait)")
        self.assertIn("if (s->fenced == 1 && s->fence > rx_nvn_fence()) held_flush_why(HF_SYNC);", wait)
        ev = block(self.nvn, "NV(nvnEventGetValue)")
        self.assertIn("if (hold_on == 2 && nheld) held_flush_why(HF_EVENT);", ev)
        self.assertLess(ev.index("held_flush_why(HF_EVENT)"), ev.index("nvn_gpu_read("))

    def test_fence_counts_the_held_submits(self):
        fence = block(self.nvn, "NV(nvnQueueFenceSync)")
        self.assertIn("held_count() ? rx_nvn_fence() + held_count() : rx_nvn_fence_q(", fence)

    def test_sweep_keeps_held_recordings(self):
        sweep = block(self.nvn, "static void sweep_overwritten_copies(void)")
        self.assertIn("const uint64_t after = rx_nvn_pushed() + held_count();", sweep)
        self.assertIn("dead[ndead].after = after;", sweep)

    def test_check_helpers_mark_the_pages_again(self):
        snap = block(self.rx, "int64_t hos_hold_snapshot(uint32_t **pages, uint8_t **copy)")
        self.assertIn("collect_in(ranges, hos_gpu_guest_ranges(ranges, 4096), pages)", snap)
        self.assertIn("n <= 16384", snap)
        check = block(self.rx, "void hos_hold_check(uint32_t *pages, int64_t n, uint8_t *copy, int64_t *again, int64_t *differ)")
        self.assertIn("__atomic_store_n(&a64_dirty[pages[i]], 1, __ATOMIC_RELEASE)", check)
        self.assertLess(check.index("(*differ)++"), check.index("__atomic_store_n(&a64_dirty[pages[i]], 1"))
        self.assertIn("free(pages);", check)
        self.assertIn("free(copy);", check)


if __name__ == "__main__":
    unittest.main()
