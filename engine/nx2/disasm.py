"""Disassemble the guest image (Eden layout) around an address, for reading crash reports.

    python -I nx2/disasm.py 0x800bae78            # 16 instructions before and after
    python -I nx2/disasm.py 0x800b9320 0x800b9500  # a range
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit
import capstone
import image as eden_image


def main():
    a = [int(x, 0) for x in sys.argv[1:]]
    img = eden_image.EdenImage(kit.need_exefs())
    lo, hi = (a[0] - 64, a[0] + 64) if len(a) == 1 else (a[0], a[1])
    mod = next((m for m in img.modules if m.base <= lo < m.base + len(m.image)), None)
    names = getattr(img, "names", {}) or {}
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    for pc in range(lo & ~3, hi, 4):
        w = img.word(pc)
        if pc in names:
            print(f"{names[pc]}:")
        ins = next(md.disasm(w.to_bytes(4, "little"), pc), None)
        text = f"{ins.mnemonic} {ins.op_str}" if ins else f".word {w:#010x}"
        mark = " <--" if len(a) == 1 and pc == a[0] else ""
        print(f"  {pc:#x} {mod.name if mod else '?'}+{pc - (mod.base if mod else 0):#x}: {w:08x}  {text}{mark}")


if __name__ == "__main__":
    main()
