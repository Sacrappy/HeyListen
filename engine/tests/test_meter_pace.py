# Copyright (C) 2026 Sacrappy

"""The exposure meter's reads before the frame's present (FLICK1; gpu/vk/vk_rbring.cpp rbring_meter_pace, called from vk_replay.cpp's
present with the order thread; no game/GPU). With the order thread the game's present is not held by the replay; when the game's
module job that reads the meter runs late, the present was handed over while it was still reading and the next frame showed the
whole 3D picture brighter (proven with HL_METER_DELAY_EVERY: 131 delays, 132 flashes; with the wait: 1). Checks the wiring (the
call before the frame clock's tick, with the order thread only; the reads counted by frame; the default bound) and, on a model of
the rule, that a frame whose reads are on time waits for nothing, a late one waits for them, a frame with fewer reads than usual
waits at most the bound, and nothing waits while the usual count is being learned.

python -I -B tests/test_meter_pace.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"


def check_wiring():
    ring = (VK / "vk_rbring.cpp").read_text(encoding="utf-8")
    for needle in ('getenv("RX_VK_ORDER_METER_PACE_MS") ? atoi(getenv("RX_VK_ORDER_METER_PACE_MS")) : 20;',
                   "if (size == 0x1040) meter_read_note(c);   // (FLICK1)",
                   "for (int i = 0; i < 16; i++) expect = std::min(expect, hist[i]);",
                   "if (hi < 16) expect = 0;   // (learning)",
                   "hist[hi++ % 16] = n;"):
        if needle not in ring:
            raise SystemExit("vk_rbring.cpp no longer has: " + needle)
    rep = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    call = "if (order_thread_on() && rb_fif_on()) rbring_meter_pace(rb_clock.load(std::memory_order_acquire));"
    tick = "if (rb_fif_on()) rb_clock.fetch_add(1, std::memory_order_release);   // (FIF1: the game's frame count)"
    if call not in rep or tick not in rep or rep.index(call) > rep.index(tick):
        raise SystemExit("vk_replay.cpp: the meter pace must run before the frame clock's tick, with the order thread only")
    print("wiring: before the tick, the order thread only, reads by frame, 20 ms")


class Pace:
    """the rule of rbring_meter_pace on a frame: reads[t] = the meter reads made by time t (ms) after the hand-over's arrival"""
    def __init__(self, bound=20):
        self.hist, self.bound = [], bound

    def present(self, reads_by_ms):
        expect = min(self.hist[-16:]) if len(self.hist) >= 16 else 0
        waited, n = 0, reads_by_ms(0)
        while expect and n < expect and waited < self.bound:
            waited += 1
            n = reads_by_ms(waited)
        self.hist.append(n)
        return waited


def model():
    p = Pace()
    for _ in range(3):   # (learning: nothing waits)
        assert p.present(lambda t: 0) == 0
    p.hist = []
    for _ in range(16):
        assert p.present(lambda t: 3) == 0   # (on time)
    assert p.present(lambda t: 3) == 0
    assert p.present(lambda t: 1 if t < 12 else 3) == 12   # (the job 12 ms late: waited for)
    assert p.present(lambda t: 2) == 20                     # (a read that never comes: the bound)
    print("model: on time no wait, late waited for, the bound holds, no wait while learning")


def main():
    check_wiring()
    model()


if __name__ == "__main__":
    main()
