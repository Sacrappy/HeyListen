"""HeyListen's icon: an original glowing blue fairy orb with four translucent wings (no Nintendo artwork).

    python -I tools/release/make_icon.py

Draws the same shapes as engine/assets/heylisten.svg (the editable source; keep the two in step) with the standard library only
(no PIL/cairo needed) and writes:
    engine/assets/heylisten.png                      256x256
    engine/assets/heylisten.ico                      16, 32, 48 (32-bit DIB) and 256 (PNG)
    engine/builder/HookshotBuilder/Images/heylisten.png   (the builder window's splash/header image)
    engine/builder/setup/heylisten.ico               (the setup exe's icon, builder/setup/setup.rc)
    engine/assets/heylisten.res                      (the same icon as a resource the game's HeyListen.exe links in,
                                                      nx2/build.py; group icon 1, as nx2/make_icon.py writes it)
"""
import math
import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SS = 3                 # supersampling
N = 256 * SS

# (shapes in 256-unit space, as in the SVG)
WINGS = [  # cx, cy, rx, ry, rotation degrees
    (66, 88, 60, 28, 32), (190, 88, 60, 28, -32),
    (78, 168, 44, 20, -28), (178, 168, 44, 20, 28),
]
ORB = (128, 128, 44)


def over(dst, src):
    sr, sg, sb, sa = src
    dr, dg, db, da = dst
    oa = sa + da * (1 - sa)
    if oa <= 0:
        return (0.0, 0.0, 0.0, 0.0)
    return ((sr * sa + dr * da * (1 - sa)) / oa, (sg * sa + dg * da * (1 - sa)) / oa, (sb * sa + db * da * (1 - sa)) / oa, oa)


def wing_rgba(x, y):
    best = None
    for cx, cy, rx, ry, rot in WINGS:
        t = math.radians(rot)
        dx, dy = x - cx, y - cy
        u = dx * math.cos(t) + dy * math.sin(t)
        v = -dx * math.sin(t) + dy * math.cos(t)
        d = math.sqrt((u / rx) ** 2 + (v / ry) ** 2)
        if d <= 1.0:
            edge = max(0.0, (d - 0.86) / 0.14)          # brighter rim
            vein = max(0.0, 1 - abs(v) / 2.0) * 0.25 * (1 - d)
            a = 0.38 + 0.45 * edge + vein
            c = (0.55 + 0.35 * edge, 0.82 + 0.15 * edge, 1.0, min(a, 0.9))
            best = c if best is None else over(best, c)
    return best


def pixel(x, y):
    c = (0.0, 0.0, 0.0, 0.0)
    ox, oy, r = ORB
    d = math.hypot(x - ox, y - oy)
    if d < 112:                                          # glow
        g = (1 - d / 112) ** 2 * 0.75
        c = over(c, (0.35, 0.68, 1.0, g))
    w = wing_rgba(x, y)
    if w:
        c = over(c, w)
    if d <= r:                                           # orb, lit from the top left
        hx, hy = ox - 14, oy - 16
        t = min(1.0, math.hypot(x - hx, y - hy) / (r + 18))
        stops = [(0.0, (1.0, 1.0, 1.0)), (0.35, (0.62, 0.86, 1.0)), (0.75, (0.24, 0.56, 0.98)), (1.0, (0.10, 0.30, 0.82))]
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t <= t1:
                k = (t - t0) / (t1 - t0)
                rgb = tuple(a + (b - a) * k for a, b in zip(c0, c1))
                break
        aa = min(1.0, (r - d) + 0.5)
        c = over(c, rgb + (aa,))
        sd = math.hypot((x - (ox - 15)) / 11, (y - (oy - 18)) / 7)
        if sd < 1:
            c = over(c, (1.0, 1.0, 1.0, 0.75 * (1 - sd)))
    return c


def render():
    """256x256 RGBA bytes (straight alpha)"""
    big = [[None] * N for _ in range(N)]
    for j in range(N):
        y = (j + 0.5) / SS
        row = big[j]
        for i in range(N):
            row[i] = pixel((i + 0.5) / SS, y)
    return downsample(big, N, 256)


def downsample(img, n, m):
    """Area average of an n x n RGBA float image (lists) to m x m, premultiplied; bytes"""
    f = n // m
    out = bytearray()
    for j in range(m):
        for i in range(m):
            r = g = b = a = 0.0
            for y in range(j * f, j * f + f):
                row = img[y]
                for x in range(i * f, i * f + f):
                    pr, pg, pb, pa = row[x]
                    r += pr * pa; g += pg * pa; b += pb * pa; a += pa
            k = f * f
            if a > 0:
                out += bytes((round(255 * r / a), round(255 * g / a), round(255 * b / a), round(255 * a / k)))
            else:
                out += b"\0\0\0\0"
    return bytes(out)


def shrink(rgba, n, m):
    img = [[tuple(v / 255 for v in rgba[(y * n + x) * 4:(y * n + x) * 4 + 4]) for x in range(n)] for y in range(n)]
    if n % m:
        raise ValueError(m)
    return downsample(img, n, m)


def png(rgba, n):
    raw = b"".join(b"\0" + rgba[y * n * 4:(y + 1) * n * 4] for y in range(n))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib(rgba, n):
    head = struct.pack("<IiiHHIIiiII", 40, n, n * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    px = bytearray()
    for y in range(n - 1, -1, -1):
        for x in range(n):
            r, g, b, a = rgba[(y * n + x) * 4:(y * n + x) * 4 + 4]
            px += bytes((b, g, r, a))
    stride = ((n + 31) // 32) * 4
    return head + bytes(px) + b"\0" * (stride * n)       # (an all-zero AND mask: alpha decides)


def ico(images):
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for n, data in images:
        entries += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(data), offset + len(blobs))
        blobs += data
    return head + entries + blobs


def res_entry(kind, name, data, flags=0x1010):
    header = struct.pack("<II", len(data), 32) + struct.pack("<HHHH", 0xFFFF, kind, 0xFFFF, name)
    header += struct.pack("<IHHII", 0, flags, 0x0409, 0, 0)
    return header + data + b"\0" * (-len(data) & 3)


def res(images):
    """A .res with RT_ICON 1..n and RT_GROUP_ICON 1 (what the game's window class loads)"""
    group = struct.pack("<HHH", 0, 1, len(images))
    out = res_entry(0, 0, b"", 0)
    for i, (n, data) in enumerate(images):
        group += struct.pack("<BBBBHHIH", n % 256, n % 256, 0, 0, 1, 32, len(data), i + 1)
        out += res_entry(3, i + 1, data)
    return out + res_entry(14, 1, group, 0x1030)


def main():
    big = render()
    # (16 and 32 divide 256: area averages; 48 does not: 256 -> 192 bilinear, then an area average)
    sizes = {256: big, 32: shrink(big, 256, 32), 16: shrink(big, 256, 16), 48: shrink(bilinear(big, 256, 192), 192, 48)}
    out = [os.path.join(ROOT, "engine", "assets", "heylisten.png"),
           os.path.join(ROOT, "engine", "builder", "HookshotBuilder", "Images", "heylisten.png")]
    for p in out:
        with open(p, "wb") as f:
            f.write(png(big, 256))
    images = [(16, dib(sizes[16], 16)), (32, dib(sizes[32], 32)), (48, dib(sizes[48], 48)), (256, png(big, 256))]
    data = ico(images)
    with open(os.path.join(ROOT, "engine", "assets", "heylisten.res"), "wb") as f:
        f.write(res(images))
    for p in (os.path.join(ROOT, "engine", "assets", "heylisten.ico"), os.path.join(ROOT, "engine", "builder", "setup", "heylisten.ico")):
        with open(p, "wb") as f:
            f.write(data)
    for p in out + ["engine/assets/heylisten.ico", "engine/builder/setup/heylisten.ico", "engine/assets/heylisten.res"]:
        print(os.path.relpath(os.path.join(ROOT, p), ROOT), os.path.getsize(os.path.join(ROOT, p)))


def bilinear(rgba, n, m):
    out = bytearray()
    s = n / m
    for j in range(m):
        fy = (j + 0.5) * s - 0.5
        y0 = max(0, min(n - 1, int(math.floor(fy)))); y1 = min(n - 1, y0 + 1); ty = fy - y0
        for i in range(m):
            fx = (i + 0.5) * s - 0.5
            x0 = max(0, min(n - 1, int(math.floor(fx)))); x1 = min(n - 1, x0 + 1); tx = fx - x0
            acc = [0.0] * 4
            for (xx, yy, w) in ((x0, y0, (1 - tx) * (1 - ty)), (x1, y0, tx * (1 - ty)), (x0, y1, (1 - tx) * ty), (x1, y1, tx * ty)):
                k = (yy * n + xx) * 4
                a = rgba[k + 3]
                acc[0] += rgba[k] * a * w; acc[1] += rgba[k + 1] * a * w; acc[2] += rgba[k + 2] * a * w; acc[3] += a * w
            a = acc[3]
            out += bytes((round(acc[0] / a), round(acc[1] / a), round(acc[2] / a), round(a))) if a > 0 else b"\0\0\0\0"
    return bytes(out)


if __name__ == "__main__":
    main()
