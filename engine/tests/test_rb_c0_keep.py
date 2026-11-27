# Copyright (C) 2026 Sacrappy

"""The meter's 0xc0 companion is the game's own block (FLICK1; gpu/vk/vk_rbring.cpp rbring_apply, "the meter's 0xc0 companion";
no game/GPU). The game's meter job invalidates the block and then fills all of it for the GPU; the ring answered the invalidate by
writing the GPU's staged bytes into guest memory, and when the job ran past the present's hand-over the next frame's capture took
those bytes as the game's parameters (the one-frame brightness flash with the order thread). Checks the wiring (the read of a 0xc0
range returns before anything is written, before any wait; on by default) and, on a model of a frame whose job is late, that the
capture then holds the game's last complete block instead of the GPU's bytes.

python -I -B tests/test_rb_c0_keep.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"


def check_wiring():
    src = (VK / "vk_rbring.cpp").read_text(encoding="utf-8")
    keep = "if (size == 0xc0 && rb_c0_keep()) return;"
    for needle in (keep, 'static const bool v = !getenv("RX_VK_RB_C0_KEEP") || atoi(getenv("RX_VK_RB_C0_KEEP")) != 0;'):
        if needle not in src:
            raise SystemExit("vk_rbring.cpp no longer has: " + needle)
    body = src[src.index("void rbring_apply(uint64_t gpu, uint64_t size) {"):]
    if body.index(keep) > body.index("rb_wait_staged(c, config().lag);") or body.index(keep) > body.index("host->gpu_write(gpu, buf, n);"):
        raise SystemExit("rbring_apply: the 0xc0 read must return before any wait and any write")
    print("wiring: a 0xc0 read returns before any wait or write; on by default")


def frame(late, keep):
    """guest memory's 0xc0 block as the next frame's capture takes it: the game's fill (zeros), the GPU's staged floats"""
    game_prev, gpu_staged = [0.0] * 4, [0.23, 1.02, 2.32, 0.11]
    guest = list(game_prev)
    if not keep:
        guest = list(gpu_staged)          # (the ring's write at the job's invalidate)
    if not late:
        guest = [0.0] * 4                 # (the job's own fill, before the hand-over)
    return guest                          # (late: the capture comes before the fill)


def model():
    assert frame(late=True, keep=False) != [0.0] * 4, "the late job's frame took the GPU's floats (the flash)"
    assert frame(late=True, keep=True) == [0.0] * 4, "kept: the game's last complete block"
    assert frame(late=False, keep=False) == frame(late=False, keep=True) == [0.0] * 4
    print("model: a late job's frame takes the GPU's bytes without the rule, the game's block with it")


def main():
    check_wiring()
    model()


if __name__ == "__main__":
    main()
