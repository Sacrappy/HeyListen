"""Cross references in the guest image (Eden layout): direct B/BL to an address, and 8-byte words equal to it
(vtables, function pointer tables, GOT entries).

    python -I nx2/xref.py 0x800bae10 [0x800bae20 ...]
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit  # noqa: F401
import image as eden_image
import nx_image


def main():
    targets = {int(x, 0) for x in sys.argv[1:]}
    img = eden_image.EdenImage(kit.need_exefs())
    mem, base = img.mem, nx_image.MAIN_BASE
    for lo, hi, name in img.code:
        for pc in range(lo, hi, 4):
            w = int.from_bytes(mem[pc - base:pc - base + 4], "little")
            if (w & 0x7C000000) == 0x14000000:                  # B / BL
                off = (w & 0x3FFFFFF) << 2
                if off & (1 << 27):
                    off -= 1 << 28
                if pc + off in targets:
                    print(f"{'bl' if w >> 31 else 'b '} at {pc:#x} -> {pc + off:#x}")
    for t in targets:
        needle = t.to_bytes(8, "little")
        i = mem.find(needle)
        while i >= 0:
            if i % 8 == 0:
                print(f"word {t:#x} at {base + i:#x}")
            i = mem.find(needle, i + 1)


if __name__ == "__main__":
    main()
