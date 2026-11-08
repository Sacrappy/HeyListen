"""The flashes in a frame log (RX_VK_FRAMELOG, gpu/vk/vk_replay.cpp): every frame shown, 320x180.

    python -I scripts/framelog.py <file> [out.png]

A frame is a flash where a part of the picture goes brighter (or darker) than BOTH its neighbours by more than they differ
from each other, with the brightness of that part back within reach the next frame. Blocks of 20x20, so that an edge moving
past counts little; a block must jump in brightness, not only in detail. Prints the events by size, with whether the frame was
the game's or a made one, and writes the worst as rows of (before, frame, after).
"""
import struct
import sys

import numpy as np

W, H = 320, 180
SIZE = 16 + W * H * 4


def blocks(a):
    """luma means of 20x20 blocks"""
    l = a[..., 0] * 0.3 + a[..., 1] * 0.6 + a[..., 2] * 0.1
    return l[:H // 20 * 20, :].reshape(H // 20, 20, W // 20, 20).mean(axis=(1, 3))


def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    raw = np.memmap(path, dtype=np.uint8, mode="r")
    n = len(raw) // SIZE
    print(n, "frames")
    ts, seqs, kinds = [], [], []
    for i in range(n):
        t, s, k = struct.unpack_from("<QII", raw[i * SIZE:i * SIZE + 16].tobytes())
        ts.append(t), seqs.append(s), kinds.append(k)
    ts = np.array(ts)
    kinds = np.array(kinds)
    d = np.diff(ts) / 1e6
    print("shown apart: median %.1f ms, 99%% %.1f, worst %.1f; made %d of %d; missing %d" % (
        np.median(d), np.percentile(d, 99), d.max(), kinds.sum(), n, seqs[-1] - seqs[0] + 1 - n))
    frame = lambda i: np.asarray(raw[i * SIZE + 16:(i + 1) * SIZE]).reshape(H, W, 4)[..., :3].astype(np.float32)
    prev = blocks(frame(0))
    cur = blocks(frame(1))
    events = []
    for i in range(1, n - 1):
        nxt = blocks(frame(i + 1))
        up = np.minimum(cur - prev, cur - nxt)
        down = np.minimum(prev - cur, nxt - cur)
        span = np.abs(prev - nxt)
        for sign, jump in ((1, up), (-1, down)):
            hit = (jump > 12) & (jump > span * 1.5)
            c = int(hit.sum())
            if c:
                events.append((float(jump[hit].sum()), i, sign, c, float(jump.max())))
        prev, cur = cur, nxt
    events.sort(reverse=True)
    tot = {0: 0, 1: 0}
    for e in events:
        tot[int(kinds[e[1]])] += 1
    print("events: %d on the game's frames, %d on made frames" % (tot[0], tot[1]))
    for size, i, sign, c, peak in events[:24]:
        print("frame %d (%s) %s %d blocks, peak %.0f, at %.2f s" % (i, "made" if kinds[i] else "game", "brighter" if sign > 0 else "darker",
                                                                     c, peak, (ts[i] - ts[0]) / 1e9))
    if out and events:
        from PIL import Image
        rows = events[:8]
        im = Image.new("RGB", (W * 3, H * len(rows)))
        for r, e in enumerate(rows):
            for k in range(3):
                im.paste(Image.fromarray(frame(e[1] - 1 + k).astype(np.uint8)), (k * W, r * H))
        im.save(out)


main()
