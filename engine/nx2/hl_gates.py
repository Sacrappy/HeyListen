"""(HeyListen native engine) Entry gates: a guest function the game calls directly, answered by a native module.

    python -I nx2/hl_gates.py <gen>            make the lifted code's gates what host/hl_*.c asks for (idempotent)
    python -I nx2/hl_gates.py <gen> --hash 80ba6d80 [...]    print the hash of those functions' lifted text

A function the game reaches only through a pointer needs no gate (host/hl_host.h HL_ANSWER: the runtime's lookup asks the
OS layer first). One that lifted code calls by name does: a module writes, in its host/hl_<module>.c,

    HL_GATE(80ba6d80, "3dc3022f619d8950") { ... return 1; }     /* int hl_gate_80ba6d80(Ctx *c): 1 when it answered */

and this script puts one line at the head of the lifted f_80ba6d80:

    { extern volatile int hl_gate_on_80ba6d80; extern int hl_gate_80ba6d80(Ctx *A64_NOESC);
      if (__builtin_expect(hl_gate_on_80ba6d80, 0) && hl_gate_80ba6d80(c)) return; }   /* hl gate 80ba6d80 */

so the translated body stays, runs whenever the gate is off or says 0, and is there to compare the native answer with.
The second argument is the hash of the lifted function's text the native code was written against (--hash prints it): the
gate is put in only when the text hashes so. Another game version or another lift leaves the function alone, with a
warning, and the module's switch then does nothing (hl_gate_on_<address> is never read).

With the gate, one line goes right before the function, which tells the OS layer at start that the gate is in:

    __attribute__((constructor)) static void hl_gate_in_80ba6d80(void) { ... hl_gate_present(0x80ba6d80ULL); }   /* hl gate 80ba6d80 */

so that a module whose gates must be on together (functions sharing state, half native and half translated otherwise)
switches them on only when every one of them is in the lifted code (host/hl_host.h hl_gates_in). Every function's hash
leaves out every gate's lines, its own and the next function's presence line.

The gates wanted are read from the host sources, so there is no second list to keep. A gate no source asks for any more is
taken out. Only the files whose text changes are written (and so recompiled). Applied after a lift by oot_patches.apply,
and before a build by the development scripts.
"""
import glob
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "..", "host")
WANT = re.compile(r'^HL_GATE\(\s*([0-9a-f]{8})\s*,\s*"([0-9a-f]*)"\s*\)', re.M)
MARK = re.compile(r"^.*/\* hl gate ([0-9a-f]{8}) \*/\n", re.M)
HEAD = "void f_%s(Ctx *__restrict A64_NOESC c) {\n"
FORM = "form 2: gate line and presence line"
NEXT = re.compile(r"^void f_[0-9a-f]{8}\(Ctx \*__restrict A64_NOESC c\) \{\n", re.M)


def gate_line(addr):
    return ("    { extern volatile int hl_gate_on_%s; extern int hl_gate_%s(Ctx *A64_NOESC); "
            "if (__builtin_expect(hl_gate_on_%s, 0) && hl_gate_%s(c)) return; }   /* hl gate %s */\n" % ((addr,) * 5))


def present_line(addr):
    return ("__attribute__((constructor)) static void hl_gate_in_%s(void) { extern void hl_gate_present(unsigned long long); "
            "hl_gate_present(0x%sULL); }   /* hl gate %s */\n" % ((addr,) * 3))


def wanted(host=HOST):
    """{address: hash} of every HL_GATE in host/hl_*.c"""
    out = {}
    for path in sorted(glob.glob(os.path.join(host, "hl_*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for addr, h in WANT.findall(f.read()):
                if addr in out and out[addr] != h:
                    raise SystemExit("hl_gates: %s is gated twice with different hashes" % addr)
                out[addr] = h
    return out


def body_of(s, addr):
    """(start, end) of f_<addr>'s lifted text in a file's text, its gate line left out of neither; None when not there"""
    head = HEAD % addr
    start = s.find(head)
    if start < 0:
        return None
    m = NEXT.search(s, start + len(head))
    return start, (m.start() if m else len(s))


def text_hash(s, addr):
    """the hash of f_<addr>'s lifted text, without any gate's lines: the same before and after gates are put in"""
    span = body_of(s, addr)
    if span is None:
        return None
    body = MARK.sub("", s[span[0]:span[1]])
    return hashlib.sha1(body.encode("utf-8")).hexdigest()[:16]


def apply_text(s, want):
    """(new text, {address: status}) for one file: gates of `want` whose function is here put in, others' taken out"""
    status = {}
    # (stale gates out first)
    def stale(m):
        if m.group(1) in want:
            return m.group(0)
        status[m.group(1)] = "taken out"
        return ""
    s = MARK.sub(stale, s)
    for addr, h in want.items():
        span = body_of(s, addr)
        if span is None:
            continue
        got = text_hash(s, addr)
        line, present = gate_line(addr), present_line(addr)
        has = line in s[span[0]:span[1]]
        has_present = s[:span[0]].endswith(present)
        if got != h:
            if has:
                s = s[:span[0]] + s[span[0]:span[1]].replace(line, "", 1) + s[span[1]:]
            if has_present:
                s = s[:span[0] - len(present)] + s[span[0]:]
            status[addr] = "NOT gated: its lifted text hashes %s, the native code was written against %s" % (got, h or "(none given)")
            continue
        if has and has_present:
            status[addr] = "gated already"
            continue
        if not has:
            at = span[0] + len(HEAD % addr)
            s = s[:at] + line + s[at:]
        if not has_present:
            s = s[:span[0]] + present + s[span[0]:]
        status[addr] = "gated"
    return s, status


def apply(gen, host=HOST, quiet=False):
    """Returns the number of gates wanted that are in place."""
    want = wanted(host)
    state_path = os.path.join(gen, "hl_gates.state")
    # (FORM: the gates' lines as this script writes them; a new form makes a gen folder stamped by the old one read again)
    state = FORM + "\n" + "\n".join("%s %s" % kv for kv in sorted(want.items()))
    files = sorted(glob.glob(os.path.join(gen, "code_*.c")))
    stamp = state + "\n" + "\n".join("%s %d %d" % (os.path.basename(p), os.path.getsize(p), os.stat(p).st_mtime_ns) for p in files)
    if os.path.isfile(state_path):   # (nothing asked for changed and no file was written since: nothing to read)
        with open(state_path, encoding="utf-8") as f:
            if f.read() == stamp:
                return len(want)
    found = {}
    for path in files:
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        if not want and "/* hl gate " not in s:
            continue
        out, status = apply_text(s, want)
        for addr, st in status.items():
            found[addr] = st
            if not quiet:
                print("hl_gates: f_%s in %s: %s" % (addr, os.path.basename(path), st))
        if out != s:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(out)
    for addr in want:
        if addr not in found:
            print("hl_gates: WARNING: f_%s is not in the lifted code: not gated" % addr)
    files = sorted(glob.glob(os.path.join(gen, "code_*.c")))
    stamp = state + "\n" + "\n".join("%s %d %d" % (os.path.basename(p), os.path.getsize(p), os.stat(p).st_mtime_ns) for p in files)
    with open(state_path, "w", encoding="utf-8") as f:
        f.write(stamp)
    return sum(1 for a in want if found.get(a, "").startswith("gated"))


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    gen = argv[0]
    if len(argv) > 1 and argv[1] == "--hash":
        left = set(argv[2:])
        for path in sorted(glob.glob(os.path.join(gen, "code_*.c"))):
            if not left:
                break
            with open(path, encoding="utf-8", newline="") as f:
                s = f.read()
            for addr in sorted(left):
                h = text_hash(s, addr)
                if h:
                    print("%s %s %s" % (addr, h, os.path.basename(path)))
                    left.discard(addr)
        for addr in sorted(left):
            print("%s not found" % addr)
        return 1 if left else 0
    apply(gen)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
