"""Read an ArmRecomp crash report (EDEN_NX2_RECOMP_REPORT, run-mkw.ps1's recomp_fault.txt): the lifted function
the host rip is in, and for each guest return address of the backtrace, its function and the call before it.

Usage:  python -I nx2/fault.py work/runs/<run>/recomp_fault.txt
"""
import bisect
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit
import capstone
import dllmap
import image as eden_image


def main():
    text = open(sys.argv[1]).read()
    rows = dllmap.load()
    keys = [r[0] for r in rows]
    entries = sorted(int(e, 16) for e in json.load(open(os.path.join(kit.WORK, "image", "functions.json"))))
    img = eden_image.EdenImage(kit.need_exefs())
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def func_of(pc):
        i = bisect.bisect_right(entries, pc) - 1
        if i < 0:
            return "?"
        m = next((m for m in img.modules if m.base <= pc < m.base + len(m.image)), None)
        return "f_%x+%#x (%s+%#x)" % (entries[i], pc - entries[i], m.name if m else "?", pc - (m.base if m else 0))

    def insn(pc):
        try:
            w = img.word(pc)
        except Exception:
            return "?"
        i = next(md.disasm(w.to_bytes(4, "little"), pc), None)
        return "%s %s" % (i.mnemonic, i.op_str) if i else ".word %#x" % w

    for block in text.split("[recomp] host access violation")[1:]:
        block = "[recomp] host access violation" + block
        print(block.splitlines()[0])
        for line in block.splitlines()[1:]:
            if "backtrace" in line or line.startswith("[recomp]   x"):
                continue
            print(line)
        m = re.search(r"rip in hookshot_game\.dll\+(0x[0-9a-f]+)", block)
        if m:
            t = dllmap.BASE + int(m.group(1), 16)
            i = bisect.bisect_right(keys, t) - 1
            print("  crashed in %s+%#x" % (rows[i][1], t - rows[i][0]))
        m = re.search(r"guest backtrace: (.*)", block)
        if m:
            print("  guest backtrace (return address: the call before it, in its function):")
            for a in m.group(1).replace("lr ", "").split():
                lr = int(a, 16)
                print("    %#x  %-40s %s" % (lr, func_of(lr), insn(lr - 4)))


if __name__ == "__main__":
    main()
