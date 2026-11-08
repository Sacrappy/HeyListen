"""The guest image at the addresses Eglantine (Eden) loads it to, so lifted code can run inside the emulator.

a64-kit's image builder (hos/nx_image.py) places main, subsdk*, sdk from 0x08000000, 1 MB aligned, and leaves rtld
out: its own loader is rtld. Eden instead loads rtld, main, subsdk0..9, sdk back to back (loader/nso.cpp,
LoadModule): each module takes PageAlign(.data end + header bss size), plus 0x9000 for rtld's argument block when
program arguments are set, from the process code address. For a 39-bit process that is 0x80000000 + the ASLR
offset, (rng_seed << 12) & 0xfff000 (deconstructed_rom_directory.cpp), so recomp runs set rng_seed and lift with
the same offset.

rtld is part of this image: in the emulator it runs first and binds the GOTs itself, to the same values this
builder binds them to (same module order, same lookup).
"""
import kit  # noqa: F401  (sys.path)
import nso2  # noqa: F401  (DT_RELR)
import nso
import nx_image

PAGE = 0x1000
CODE_39BIT = 0x80000000
ARGS_SIZE = 0x9000


def page_align(v):
    return (v + PAGE - 1) & ~(PAGE - 1)


def eden_size(m):
    """LoadModule's image size: the .data segment's end plus the header's bss size, page aligned"""
    data_off, data_size, _ = m.segments[2]
    return page_align(data_off + data_size + m.bss_size)


class EdenImage(nx_image.NxImage):
    def __init__(self, exefs, aslr_offset=0, program_args=False, hle=()):
        base = CODE_39BIT + aslr_offset
        nx_image.MAIN_BASE = base     # (NxImage's word/read/write index the image from this module global)
        self.modules = nso.load_exefs(exefs)
        self.skipped = []
        for m in self.modules:
            m.base = base
            size = eden_size(m) + (ARGS_SIZE if m.name == "rtld" and program_args else 0)
            if len(m.image) < size:
                m.image.extend(bytes(size - len(m.image)))
            base += size
        self.end = base
        self.mem = bytearray(self.end - nx_image.MAIN_BASE)
        for m in self.modules:
            self.mem[m.base - nx_image.MAIN_BASE:m.base - nx_image.MAIN_BASE + len(m.image)] = m.image
        self.hle = set(hle)
        self.exports = {}
        for m in self.modules:
            for s in m.symbols:
                if s["defined"] and s["name"] and s["name"] not in self.exports:
                    self.exports[s["name"]] = (m, s)
        self.hooks, self.hook_syms, self.tramp, self.unresolved = {}, {}, {}, {}
        for sym in sorted(self.hle):
            hit = self.exports.get(sym)
            if hit:
                m, s = hit
                name = nx_image.c_ident(sym)
                self.hooks[m.base + s["value"]] = name
                self.hook_syms[name] = sym
        self._relocate()
        self.code = [(m.base + lo, m.base + hi, m.name) for m in self.modules for lo, hi in [m.code_range()]]
        self._entries()
        self.names = self.symbol_names()
