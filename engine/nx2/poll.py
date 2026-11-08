"""Interrupt polls in lifted code: `A64_POLL(c);` at every loop header (the target of a backward branch).

a64-kit's own runtime gives every guest thread a host thread, so the host OS preempts a guest spin loop. Inside
Eglantine guest threads are fibers multiplexed on the emulated cores: a lifted loop that never makes an SVC would
keep its core forever (MKW spins on GPU fences in memory and on CNTPCT, NX2_NOTES rows 35 and 127), and Eden's
preemption, which halts dynarmic at block boundaries, would never get in. So every backward branch target polls the
context's interrupt flag, the same points where dynarmic would see a halt request between blocks.

A64_POLL is defined by the generated guest model (rtgen.py): nothing unless A64_POLLING, else one relaxed load of
c->poll and a call of a64_poll(c) when it is set (the backend's, which switches to the emulator and returns when the
thread runs again). Polls need the registers in the context: they are not used with A64_LOCAL_REGS.

install() wraps Lifter.emit_function; functions with no loop are unchanged.
"""
import re

import a64_lift

LABEL = re.compile(r"^L_([0-9a-f]{8}):;", re.M)
GOTO = re.compile(r"goto L_([0-9a-f]{8});")


def add_polls(text):
    labels = {m.group(1): m.end() for m in LABEL.finditer(text)}
    back = set()
    for m in GOTO.finditer(text):
        at = labels.get(m.group(1))
        if at is not None and at <= m.start():
            back.add(m.group(1))
    if not back:
        return text, 0
    out = LABEL.sub(lambda m: m.group(0) + (" A64_POLL(c);" if m.group(1) in back else ""), text)
    return out, len(back)


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
