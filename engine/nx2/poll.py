"""Interrupt polls in lifted code: `A64_POLL(c);` at every loop back-edge (right before each backward goto).

a64-kit's own runtime gives every guest thread a host thread, so the host OS preempts a guest spin loop. Inside
Eglantine guest threads are fibers multiplexed on the emulated cores: a lifted loop that never makes an SVC would
keep its core forever (MKW spins on GPU fences in memory and on CNTPCT, NX2_NOTES rows 35 and 127), and Eden's
preemption, which halts dynarmic at block boundaries, would never get in. So every backward branch target polls the
context's interrupt flag, the same points where dynarmic would see a halt request between blocks.

A64_POLL is defined by the generated guest model (rtgen.py): nothing unless A64_POLLING, else one relaxed load of
c->poll and, when it is set, a store of 0 to it and a call of a64_poll_cur() (the backend's, which hands the core on
and returns when the thread runs again).

Where the poll sits and what it is handed decide whether the compiler can keep a loop's guest registers in host
registers (measured on a lifted-shaped loop, clang 24, -O1 and -O2; nx2/rtgen.py has the macro):
- a call handed the context (a64_poll(c)) may change any of it, so every loop body reloaded and stored its registers;
- a call handed nothing cannot touch the noescape context, but at the loop header it still blocked the promotion: C
  cannot say the call returns (LLVM's willreturn), so the body's stores after it were not guaranteed to execute;
- at the back-edge, after the body's stores, with the call handed nothing: the registers and the invariant loads
  stay in host registers across the loop (4 loads an iteration gone in the sample; the two stores stay).
So the poll goes before each backward goto. One poll an iteration either way. a64_poll_cur reads no registers, so
polls work with A64_LOCAL_REGS too. The emulator-hosted DLL build keeps a64_poll(c), whose host's poll takes the
context (A64_POLL_CTX, build.py).

install() wraps Lifter.emit_function; functions with no loop are unchanged.
"""
import re

import a64_lift

LABEL = re.compile(r"^L_([0-9a-f]{8}):;", re.M)
GOTO = re.compile(r"goto L_([0-9a-f]{8});")


def add_polls(text):
    """A64_POLL before every backward goto (the loop's back-edge), as a block: `{ A64_POLL(c); goto L_x; }` is a
    statement wherever the goto was. Returns the text and the number of polls added."""
    labels = {m.group(1): m.end() for m in LABEL.finditer(text)}
    out = []
    last = n = 0
    for m in GOTO.finditer(text):
        at = labels.get(m.group(1))
        if at is None or at > m.start():
            continue
        out.append(text[last:m.start()])
        out.append("{ A64_POLL(c); " + m.group(0) + " }")
        last = m.end()
        n += 1
    if not n:
        return text, 0
    out.append(text[last:])
    return "".join(out), n


_installed = False
stats = {"functions": 0, "polls": 0}


def install():
    global _installed
    if _installed:
        return
    _installed = True
    base = a64_lift.Lifter.emit_function

    def emit_function(self, e, texts):
        text, refs = base(self, e, texts)
        text, n = add_polls(text)
        if n:
            stats["functions"] += 1
            stats["polls"] += n
        return text, refs

    a64_lift.Lifter.emit_function = emit_function
