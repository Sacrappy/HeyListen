"""Rewrite an exefs's ZBIC-compressed NSOs (flags bit 7, Switch firmware 22.0.0+) as plain uncompressed NSOs, in place,
so a64-kit's nso.py (LZ4 or none) and everything after it reads them unchanged.

    python -I nx2/unpack.py <exefs dir>

The segment hashes at 0xa0 are of the decompressed data, so they stay valid; they are checked here.
NSOs with LZ4 or no compression are left as they are.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import zbic  # noqa: E402

ZBIC_FLAG = 1 << 7
NAMES = ["rtld", "main"] + ["subsdk%d" % i for i in range(10)] + ["sdk"]


def unpack_nso(path):
    raw = open(path, "rb").read()
    if raw[:4] != b"NSO0":
        return None
    flags = struct.unpack_from("<I", raw, 0xC)[0]
    if not flags & ZBIC_FLAG:
        return False
    segs = [list(struct.unpack_from("<4I", raw, 0x10 + 0x10 * i)) for i in range(3)]
    csize = struct.unpack_from("<3I", raw, 0x60)
    datas = []
    for i, ((foff, moff, size, _), cs) in enumerate(zip(segs, csize)):
        d = raw[foff:foff + cs]
        if flags & (1 << i):
            d = zbic.decompress(d, size)
        if len(d) != size:
            raise SystemExit("%s segment %d: %d bytes, %d expected" % (path, i, len(d), size))
        if flags & (8 << i):
            want = raw[0xA0 + 0x20 * i:0xC0 + 0x20 * i]
            if hashlib.sha256(d).digest() != want:
                raise SystemExit("%s segment %d: hash mismatch after ZBIC" % (path, i))
        datas.append(d)
    hdr = bytearray(raw[:0x100])
    body = bytearray(raw[0x100:min(s[0] for s in segs)])   # (the module name, which the header points at)
    for i, d in enumerate(datas):
        struct.pack_into("<I", hdr, 0x10 + 0x10 * i, 0x100 + len(body))
        struct.pack_into("<I", hdr, 0x60 + 4 * i, len(d))
        body += d
    struct.pack_into("<I", hdr, 0xC, flags & ~(ZBIC_FLAG | 7))   # (uncompressed; the hash bits stay)
    tmp = path + ".plain"
    with open(tmp, "wb") as g:
        g.write(hdr)
        g.write(body)
    os.replace(tmp, path)
    return True


def main():
    exefs = sys.argv[1] if len(sys.argv) > 1 else None
    if not exefs:
        raise SystemExit(__doc__)
    for n in NAMES:
        p = os.path.join(exefs, n)
        if os.path.exists(p):
            r = unpack_nso(p)
            print("%-8s %s" % (n, {None: "not an NSO", False: "not ZBIC, kept", True: "ZBIC -> plain"}[r]))


if __name__ == "__main__":
    main()
