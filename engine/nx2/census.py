"""Instruction census: every distinct word in each module's .text, through the lifter's classify() and suyu's
translator (a64emit), counted by capstone mnemonic. Counts occurrences, so data in .text and padding are included;
the per-function numbers come from lift.py (discovered code only).

Usage:  python -I nx2/census.py [--exefs <dir>] [--modules main,sdk] [--top 40] [--nx2] (with the Switch 2 forms)
"""
import argparse
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit
import nso2  # noqa: F401  (DT_RELR)
import nso
import a64emit
import a64_lift
import capstone


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--exefs", default=kit.EXEFS)
    p.add_argument("--modules", default="")
    p.add_argument("--top", type=int, default=40)
    p.add_argument("--nx2", action="store_true", help="apply the Switch 2 instruction extension first")
    a = p.parse_args()
    a.exefs = a.exefs or kit.need_exefs()
    if a.nx2:
        import insn
        insn.install()
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    want = set(a.modules.split(",")) if a.modules else None
    total = collections.Counter()
    for m in nso.load_exefs(a.exefs):
        if want and m.name not in want:
            continue
        lo, hi = m.code_range()
        words = struct.unpack_from("<%dI" % ((hi - lo) // 4), m.image, lo)
        count = collections.Counter(words)
        ops = [w for w in count if a64_lift.classify(w, 0x10000)[0] == "op"]
        _, flags = a64emit.translate([(0x10000, w) for w in ops])
        bad = collections.Counter()
        bad_words = collections.Counter()
        for w, f in zip(ops, flags):
            if f & a64emit.UNHANDLED:
                ins = next(md.disasm(struct.pack("<I", w), 0x10000), None)
                key = ins.mnemonic if ins else "(not an instruction)"
                bad[key] += count[w]
                bad_words[key] += 1
        nbad = sum(bad.values())
        print("== %s: %d words, %d distinct, %d distinct translatable-kind; untranslated %d words (%.3f%%) in %d distinct"
              % (m.name, len(words), len(count), len(ops), nbad, 100.0 * nbad / len(words), sum(bad_words.values())))
        for k, v in bad.most_common(a.top):
            print("   %9d  %-22s (%d distinct)" % (v, k, bad_words[k]))
        total.update(bad)
    print("== total untranslated by mnemonic")
    for k, v in total.most_common(a.top):
        print("   %9d  %s" % (v, k))


if __name__ == "__main__":
    main()
