"""Switch 2 additions to a64-kit's NSO loader (tools/nso.py), applied by importing this module.

DT_RELR (tags 35/36/37): every Switch 2 module (main, sdk, subsdk*, rtld of MKW) keeps its relative relocations
in the packed RELR format instead of RELA. Each RELR entry is either an address (even: relocate that word, then
continue after it) or a bitmap (odd: bit i of the 63 bits above bit 0 relocates the word i words further on, then
the cursor moves 63 words). The addend is implicit: the module offset stored at the location. They are added to
Nso.relocs as ordinary RELATIVE entries, so the image builder (hos/nx_image.py) applies them unchanged.
Without them every vtable and function pointer in .data stays a module offset.
"""
import struct

import kit  # noqa: F401  (sys.path)
import nso

DT_RELRSZ, DT_RELR, DT_RELRENT = 35, 36, 37
nso.DT.update({DT_RELRSZ: "RELRSZ", DT_RELR: "RELR", DT_RELRENT: "RELRENT", 21: "DEBUG", 0x6FFFFFFB: "FLAGS_1"})


def relr_offsets(image, start, size):
    out = []
    where = 0
    for off in range(start, start + size, 8):
        e = struct.unpack_from("<Q", image, off)[0]
        if not e & 1:
            out.append(e)
            where = e + 8
        else:
            bits = e >> 1
            i = 0
            while bits:
                if bits & 1:
                    out.append(where + 8 * i)
                bits >>= 1
                i += 1
            where += 63 * 8
    return out


_orig_mod0 = nso.Nso._mod0


def _mod0(self):
    _orig_mod0(self)
    d = lambda k: self.dynamic.get(k, [0])[0]
    start, size = d("RELR"), d("RELRSZ")
    self.relr_count = 0
    if start and size:
        if d("RELRENT") not in (0, 8):
            raise ValueError(f"{self.name}: RELRENT {d('RELRENT')}")
        for off in relr_offsets(self.image, start, size):
            self.relocs.append({"offset": off, "type": "RELATIVE", "sym": None, "addend": self.u64(off)})
            self.relr_count += 1


nso.Nso._mod0 = _mod0
