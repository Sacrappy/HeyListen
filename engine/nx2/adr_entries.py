"""Code addresses the code itself takes (ADR, or ADRP followed by ADD on the same register): extra function entries.

Discovery starts functions at .eh_frame starts, exports and call targets. A function the game only reaches through
a pointer it builds with ADR/ADRP+ADD (a callback handed to a formatter, say) and that has no unwind entry is not
found, and the lifted code then has nothing to call: "indirect call to an address with no function" (OoT: the
character-output callback at 0x824c5560 of a printf-style formatter, on the thread Porter). MKW had none.

Most such addresses are labels inside functions (jump-table bases, landing pads). As entries those are costly: each
one lifts the rest of its function again, and in OoT the 1,512 new ones added 1.68 M instructions (+9.5%), mostly
inside a few 30k-instruction functions near 0x81b8c000, which made one chunk 122 MB and took clang hours. So a
target strictly inside a function the unwind info covers (its FDE's [pc_begin, pc_begin + pc_range)) is dropped:
such functions are found from .eh_frame anyway. lift.py adds the rest unless --no-adr-entries.
"""
import struct

import numpy as np


def fde_spans(modules):
    """(start, end) of every function .eh_frame_hdr's table points to an FDE for, absolute addresses, sorted.

    The table is (initial_loc, fde) pairs, datarel sdata4 from the header. The FDE: u32 length, u32 CIE pointer,
    pc_begin (pcrel sdata4, as the SDK's toolchain emits), pc_range (u32). An FDE whose pc_begin disagrees with
    the table is skipped rather than guessed."""
    spans = []
    for m in modules:
        s, e = m.eh_frame_hdr
        if e <= s:
            continue
        img = m.image
        ver, ptr_enc, cnt_enc, tbl_enc = img[s:s + 4]
        if ver != 1 or tbl_enc != 0x3B or cnt_enc != 0x03:
            continue
        count = struct.unpack_from("<I", img, s + 8)[0]
        for i in range(count):
            loc, fde = struct.unpack_from("<ii", img, s + 12 + 8 * i)
            loc, fde = s + loc, s + fde
            if fde + 16 > len(img):
                continue
            length, cie, begin, rng = struct.unpack_from("<IIiI", img, fde)
            if length < 12 or cie == 0 or fde + 8 + begin != loc:
                continue
            spans.append((m.base + loc, m.base + loc + rng))
    spans.sort()
    return spans


def code_address_refs(image_bin, meta, max_ahead=4, spans=None):
    """The 4-aligned code addresses formed by ADR / ADRP+ADD in the image's .text segments (with spans: only the
    ones not strictly inside one of those functions)"""
    base = meta["base"]
    texts = [(s["addr"], s["addr"] + s["size"]) for m in meta["modules"] for s in m["segments"] if s["name"] == ".text"]
    found = []
    for lo, hi in texts:
        w = np.frombuffer(image_bin[lo - base:hi - base], dtype="<u4").astype(np.int64)
        pcs = lo + 4 * np.arange(len(w), dtype=np.int64)
        imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
        imm = np.where(imm & (1 << 20), imm - (1 << 21), imm)
        adr = (w & 0x9F000000) == 0x10000000
        found.append(pcs[adr] + imm[adr])
        idx = np.nonzero((w & 0x9F000000) == 0x90000000)[0]
        page = (pcs & ~0xFFF) + (imm << 12)
        for d in range(1, max_ahead + 1):
            j = idx + d
            j = j[j < len(w)]
            i = j - d
            y = w[j]
            add = ((y & 0xFFC00000) == 0x91000000) & (((y >> 5) & 31) == (w[i] & 31))   # ADD Xd, Xn, #imm12
            found.append(page[i][add] + ((y[add] >> 10) & 0xFFF))
    t = np.concatenate(found) if found else np.zeros(0, np.int64)
    inside = np.zeros(len(t), bool)
    for lo, hi in texts:
        inside |= (t >= lo) & (t < hi)
    t = np.unique(t[inside & (t % 4 == 0)])
    if spans:
        lo = np.array([a for a, _ in spans], np.int64)
        hi = np.array([b for _, b in spans], np.int64)
        # the latest-ending span among those starting at or before t (spans may nest or repeat)
        best = np.maximum.accumulate(hi)
        k = np.searchsorted(lo, t, side="right") - 1
        ok = k >= 0
        within = np.zeros(len(t), bool)
        within[ok] = (t[ok] < best[k[ok]]) & ~np.isin(t[ok], lo)
        t = t[~within]
    return {int(a) for a in t}
