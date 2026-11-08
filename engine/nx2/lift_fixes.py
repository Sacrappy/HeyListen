"""Fixes to a64-kit's control-flow analysis (lift/a64_lift.py) found on MKW, applied by wrapping its methods.

1. Jump tables whose table address is set by a plain ADR outside the scan window. a64-kit follows a hoisted table
   register back to ADRP (+ADD) only. rtld's dynamic-section loop (rtld+0x8a0) sets it with `adr x25, table` before
   the loop and dispatches with `adr x9, base; ldrb w10, [x25, x8]; add x9, x9, x10, lsl #2; br x9`. The window
   scan forgets x25 at the loop's `b`, and the reaching search does not know ADR. That left the BR an indirect
   jump, and the first recompiled boot stopped there (a jump to rtld+0xaf4, a case label). function_constant (the
   last fallback for a hoisted table register) now also counts ADR assignments.
2. Jump tables whose hoisted table register is reused between the table load and the BR. main+0xa6b520:
   `adr x9, base; ldrh w10, [x11, x8, lsl #1]; add x9, x9, x10, lsl #2; ...; adrp x11, str; add x11, ...; br x9`,
   x11 set to the table before the window. a64-kit resolves a hoisted table register at the BR, where x11 already
   holds a string address, so the table was never found: the BR stayed an indirect jump, and the boot ran that
   thread in the JIT from there on. The table register is now resolved at the load when the window writes it
   after the load.
3. The symbolic window before a BR ends at ret/b/br, but not at the pointer-authenticated returns and branches
   (RETAA/RETAB, BRAA/BRAB and their Z forms), so register values from the block before a `retab` leaked into
   the dispatch block. zlib's inflate (main+0x1105e70) loses its table register x0 that way, to the
   `mov w0, #-2` before the `retab` above the dispatch. The window now starts after the last such instruction.
"""
import a64_lift

_installed = False


def install():
    global _installed
    if _installed:
        return
    _installed = True
    base_function_constant = a64_lift.Lifter.function_constant

    def function_constant(self, br_pc, reg):
        v = base_function_constant(self, br_pc, reg)
        if v is not None:
            return v
        img = self.img
        lo, hi = self.function_extent(br_pc)
        if hi - lo > 0x40000:
            return None
        page, found = {}, set()
        for pc in range(lo, hi, 4):
            if not img.in_code(pc):
                continue
            w = img.word(pc)
            d = w & 31
            if (w & 0x9F000000) == 0x90000000:                                   # ADRP
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                page[d] = (pc & ~0xFFF) + (a64_lift.sx(imm, 21) << 12)
            elif (w & 0xFF800000) == 0x91000000 and ((w >> 5) & 31) in page:    # ADD Xd, Xn, #imm
                if d == reg:
                    found.add(page[(w >> 5) & 31] + (((w >> 10) & 0xFFF) << (12 if w & (1 << 22) else 0)))
            elif (w & 0x9F000000) == 0x10000000 and d == reg:                    # ADR
                imm = ((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)
                found.add(pc + a64_lift.sx(imm, 21))
        if len(found) == 1:
            self.stats["jt_adr_table"] += 1
            return found.pop()
        return None

    a64_lift.Lifter.function_constant = function_constant

    base_jump_table = a64_lift.Lifter.jump_table

    def table_load(self, br_pc, window):
        """(pc, base register) of the last register-offset integer load in the BR's window, if the window writes
        its base register again after it"""
        img = self.img
        for pc in range(br_pc - 4, br_pc - 4 * window, -4):
            if not img.in_code(pc):
                return None
            w = img.word(pc)
            if (w & 0xFC000000) == 0x14000000 or (w & 0xFFFFFC1F) in (0xD65F0000, 0xD61F0000):
                return None
            if (w & 0x3B200C00) == 0x38200800 and not (w >> 26) & 1 and (w >> 22) & 3:   # LDR* (register offset)
                n = (w >> 5) & 31
                if any(self.writes(img.word(q), n) for q in range(pc + 4, br_pc, 4)):
                    return pc, n
                return None
        return None

    def auth_end(w):
        """RETAA, RETAB, BRAAZ/BRABZ, BRAA/BRAB: the end of a block, as ret and br are"""
        return w in (0xD65F0BFF, 0xD65F0FFF) or (w & 0xFFFFF81F) == 0xD61F081F or (w & 0xFFFFF800) == 0xD71F0800

    def jump_table(self, br_pc, reg, window=32):
        img = self.img
        for k in range(1, window + 1):
            pc = br_pc - 4 * k
            if img.in_code(pc) and auth_end(img.word(pc)):
                window = k - 1
                self.stats["jt_window_cut_at_auth_ret"] += 1
                break
        hit = table_load(self, br_pc, window)
        if not hit:
            return base_jump_table(self, br_pc, reg, window)
        load_pc, treg = hit
        redirected = {}
        for name in ("reaching_constants", "hoisted_constant", "function_constant"):
            bound = getattr(self, name)

            def at_load(at, r, *rest, _f=bound):
                return _f(load_pc if (at, r) == (br_pc, treg) else at, r, *rest)
            redirected[name] = at_load
        self.__dict__.update(redirected)
        try:
            out = base_jump_table(self, br_pc, reg, window)
        finally:
            for name in redirected:
                del self.__dict__[name]
        if out:
            self.stats["jt_table_reg_reused"] += 1
        return out

    a64_lift.Lifter.jump_table = jump_table
