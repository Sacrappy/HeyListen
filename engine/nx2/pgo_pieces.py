"""The profile from the whole code_hot.c made to fit its pieces (nx2/split_hot.py).

    python -I nx2/pgo_pieces.py <in.prof> <out.prof> [pieces, default 8]

A file's static functions go by "<file>:<name>" in a profile, so the helpers of a64.h would match only in a file of the same name. Each
such record is written once per piece. The rest go by name alone and stay as they are.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kit  # noqa: E402,F401
import toolchain  # noqa: E402

PREFIX = "code_hot.c:"


def main():
    src, dst = sys.argv[1:3]
    pieces = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    tool = toolchain.need(toolchain.find("llvm-profdata"))
    text = subprocess.run([tool, "merge", "--text", "-o", "-", src], capture_output=True, text=True, encoding="utf-8", check=True).stdout
    head, _, rest = text.partition("\n\n")
    records = rest.split("\n\n")
    out = []
    moved = 0
    for r in records:
        if r.startswith(PREFIX):
            moved += 1
            out.extend("code_hot_%d.c:%s" % (i, r[len(PREFIX):]) for i in range(pieces))
        elif r.strip():
            out.append(r)
    tmp = dst + ".txt"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(head + "\n\n" + "\n\n".join(out) + "\n")
    subprocess.run([tool, "merge", "--text-format" if False else "--binary", "-o", dst, tmp], check=True)
    os.remove(tmp)
    print("pgo_pieces: %d records, %d helpers for %d pieces" % (len(records), moved, pieces))


if __name__ == "__main__":
    main()
