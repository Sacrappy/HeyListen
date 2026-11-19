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
FORM = "form 3: gate line, presence line, step hooks"
NEXT = re.compile(r"^void f_[0-9a-f]{8}\(Ctx \*__restrict A64_NOESC c\) \{\n", re.M)


def gate_line(addr):
    return ("    { extern volatile int hl_gate_on_%s; extern int hl_gate_%s(Ctx *A64_NOESC); "
            "if (__builtin_expect(hl_gate_on_%s, 0) && hl_gate_%s(c)) return; }   /* hl gate %s */\n" % ((addr,) * 5))


def present_line(addr):
    return ("__attribute__((constructor)) static void hl_gate_in_%s(void) { extern void hl_gate_present(unsigned long long); "
            "hl_gate_present(0x%sULL); }   /* hl gate %s */\n" % ((addr,) * 3))


# Step hooks (host/hl_step.c HL_STEP_HOOK(80706be0, "<hash>", <vreg>, <probe>)): at the one float divide of f_<addr> that
# makes a time step from the engine's nominal frame rate (base / nominal), one line after it, so that the step is base x
# the host's measured frame time when the host publishes one (hos_frame_seconds_bits, a float's bits; 0: the game's own
# divide). With probe 1 it also tells the host the object (x19) whose +0xd58 is the game clock, and that the hook is in.
# The line carries the marker of the address plus one ("hl gate 80706be1": no function starts at an odd address), so a
# tree that does not ask for it (an older hl_gates.py too) takes it out as a stale gate: the lifted work folder is shared
# by every worktree, and a tree whose host lacks the two symbols must still link. Both or neither: all the step hooks a
# tree asks for go in only when every one of them can (one divide timed and the other stepping 1/60 s would run the
# game's two clocks apart).
WANT_STEP = re.compile(r'^HL_STEP_HOOK\(\s*([0-9a-f]{8})\s*,\s*"([0-9a-f]*)"\s*,\s*(\d+)\s*,\s*([01])\s*\)', re.M)
STEP_DIVIDE = "    _v=a64_fpx_div32_div32(c,_a,_b);\n    a64_vset64(c->vreg[%s],_v);}"


def step_id(addr):
    return "%08x" % (int(addr, 16) + 1)


def step_line(addr, probe):
    return ("    { extern volatile uint32_t hos_frame_seconds_bits; const uint32_t _hs = hos_frame_seconds_bits; if (_hs) { "
            "float _base, _secs; uint32_t _w; memcpy(&_base,&_a,4); memcpy(&_secs,&_hs,4); _base *= _secs; "
            "memcpy(&_w,&_base,4); _v = _w; }%s }   /* hl gate %s */\n"
            % (" extern volatile uint64_t hos_step_probe; hos_step_probe = c->x[19];" if probe else "", step_id(addr)))


def wanted_steps(host=HOST):
    """{address: (hash, vreg, probe)} of every HL_STEP_HOOK in host/hl_*.c"""
    out = {}
    for path in sorted(glob.glob(os.path.join(host, "hl_*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for addr, h, reg, probe in WANT_STEP.findall(f.read()):
                out[addr] = (h, reg, probe == "1")
    return out


def step_site(s, addr, reg):
    """the offset in s where f_<addr>'s step hook line goes (right after its divide), or None"""
    span = body_of(s, addr)
    if span is None:
        return None
    body = s[span[0]:span[1]]
    divide = STEP_DIVIDE % reg
    if body.count(divide) != 1 or body.count("a64_fpx_div32_div32") != 1:
        return None
    return span[0] + body.index(divide) + len("    _v=a64_fpx_div32_div32(c,_a,_b);\n")


# Movie hooks (host/hl_step.c HL_MOVIE_HOOK(83b7c710, "<hash>", <offset>)): at the one "+1" of the count a movie player keeps
# at +<offset> of itself (x20), one line after the add, so that the count grows by hos_movie_frames(player) instead: the
# frames' time in 60ths above a 60 cap, 1 otherwise. A call and no branch (the function's PGO record stays). The marker is
# the address plus one, as a step hook's, so a tree that does not ask for it takes it out.
WANT_MOVIE = re.compile(r'^HL_MOVIE_HOOK\(\s*([0-9a-f]{8})\s*,\s*"([0-9a-f]*)"\s*,\s*(\d+)\s*\)', re.M)
MOVIE_ADD = ("    c->x[8]=recomp_load32(c,c->x[20] + %d);\n"
             "    { uint64_t _a=c->x[8], _b=1ULL; uint64_t _r=_a+_b; _r&=0xFFFFFFFFULL; c->x[8]=_r; }\n")


def movie_line(addr):
    return ("    { extern uint32_t hos_movie_frames(uint64_t); c->x[8] = (c->x[8] - 1 + hos_movie_frames(c->x[20])) & 0xFFFFFFFFULL; }"
            "   /* hl gate %s */\n" % step_id(addr))


def wanted_movies(host=HOST):
    """{address: (hash, offset)} of every HL_MOVIE_HOOK in host/hl_*.c"""
    out = {}
    for path in sorted(glob.glob(os.path.join(host, "hl_*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for addr, h, off in WANT_MOVIE.findall(f.read()):
                out[addr] = (h, int(off))
    return out


def movie_site(s, addr, off):
    """the offset in s where f_<addr>'s movie hook line goes (right after the count's add), or None"""
    span = body_of(s, addr)
    if span is None:
        return None
    body = s[span[0]:span[1]]
    add = MOVIE_ADD % off
    if body.count(add) != 1:
        return None
    return span[0] + body.index(add) + len(add)


# Lift hooks: the ports of upstream Hookshot's fixes for the game's systems that count updates instead of time above a 60
# cap (upstream edits the lifted text in nx2/oot_patches.py; the lifted work folder is shared here, so each is lines put
# in after an anchor the function's text has once, its body and hash unchanged). A kind a macro in host/hl_*.c:
# - HL_SOUND_HOOK(80435300, "<hash>"): the sound manager's job. At its head: return at once while hos_oot_sound_gate
#   (host/hl_high_fps.c) says so (the job at 60 Hz above a 60 cap); after its load of the game step: that step through
#   hos_oot_sound_step (a whole one while the gate holds the job to whole steps).
# - HL_BGM_HOOK(83373400, "<hash>"): (Longshot eb5ba6e2 / Hookshot bf802502, CraftyCorvid) the music manager's update, run inside
#   the sound job, so at 60 Hz too: at its head hos_oot_bgm_begin (takes the change flags the sound gate held back); after its
#   two loads of the time object's step (+16) and seconds (+24) both through hos_oot_sound_step / hos_oot_sound_seconds (a
#   whole step while the gate holds the job); after each of its four reads of the battle manager's change flags (x26 + 2200)
#   that byte with the held ones, hos_oot_bgm_flags (the battle music stopped after a fight above 60 fps).
# - HL_TURN_HOOK(8263e8e0, "<hash>"): (Longshot 3ab8cae6 / Hookshot 93a8bff3, PirateKingIsh; Gohma) the steering update's two
#   clamps of the saved rate (+36), vreg 1 = turn x rate and vreg 0 = turn x step (vreg 8): above 60 fps, 0 < step < 1, each is
#   the division instead (the turn was already scaled by the step, so the saved rate kept rate x step squared). Each multiply
#   is jumped over by a line before it that does the division when the step is under 1, and a label after it.
# - HL_HORSE_HOOK(82c59eb0, "<hash>"): (Longshot 8e8a06db, CraftyCorvid; Epona's fences) between the jump state machine's test of
#   its step (vreg 10) against 1 and its branch past the slow-motion branch: a step that is the whole frame's (the time
#   object's, hos_oot_horse_whole_step) takes that branch too.
# - HL_FLOOR_HOOK(80159dc0, "<hash>", "L_8015a39c"): (Longshot 4f755527, CraftyCorvid; the Shadow Temple's barge) a character
#   module's test of the physics' 1/dt (+40) against 100 and its branch to the tenth of a moving floor's velocity (the label
#   given): that branch is skipped above a 60 cap on a frame under a 60 fps one unless hos_oot_floor_carry_slow says time is slow.
# - HL_SOLVER_HOOK(838ceb80, "<hash>"): after the cut of frame step x the solver's steps a 60 fps frame + 0.001, stored to
#   the solver's +712: the count through hos_oot_solver_steps (rounded up while the step is under 1).
# - HL_WHOLE_HOOK(80a87e50, "<hash>", <reg>, "<key>"): after the function's cut of an amount of the frame step (vreg 0's
#   float) to whole frames into x<reg>, that count made hos_frame_whole(<key>, the amount, the game's count) (host/hl_step.c):
#   the fractions carried per counter (<key>, the counter's or its object's guest address) above a 60 cap. The cut is the
#   function's one; where it makes the same cut more than once, the one stored to <key> right after it. A third argument
#   "u" is for an unsigned cut (the conversion's flag 0: a counter the game keeps unsigned), "u<N>" for the unsigned cut that
#   the addition to x<N> follows (a function with another unsigned cut followed by another register's addition).
# - HL_HEAP_HOOK(804566e0, "<hash>"): (Hookshot 1b695334, #2451) the four allocations of the material block's rebuild that the
#   game does not check: its two CPU buffers (the heap's tryAlloc through a pointer, return addresses 0x80456910 and
#   0x80456da4) and its two GPU-memory calls (f_80522050, 0x80456f1c and 0x80457098), each tried again in the heap's parents
#   when it failed (host/hl_heap_retry.c). Upstream replaces those lines; here the game's own line stays, between a line
#   before it (which saves the arguments and may refuse the call, for the test switch: "if (pre) x0 = 0; else {") and a line
#   after it ("}" and the retry), so that taking the hook out leaves the lift as it was.
# Each goes in only where its function's text hashes as recorded, both lines of a kind or none. The marker is the address
# plus one, as a step hook's (one hook a function), so a tree that does not ask for it takes it out as a stale gate.
WANT_LIFT = re.compile(r'^HL_(SOUND|SOLVER|WHOLE|HEAP|BGM|TURN|HORSE|FLOOR)_HOOK\(\s*([0-9a-f]{8})\s*,\s*"([0-9a-f]*)"((?:\s*,\s*(?:\d+|"[^"\n]*"))*)\s*\)',
                       re.M)
CUT = "    c->x[%d]=recomp_fp_to_int(c->vreg[0][0]&UINT64_C(0xffffffff),32,32,1,3,c->fpcr,&c->fpsr);\n"
SOUND_SLOT = "g_module_base + 0x80435000ULL + (int64_t)75927552"   # (the page of the step's pointer, whose +2192 holds it)
SOUND_LOAD = ("    c->x[8]=" + SOUND_SLOT + ";\n"
              "    { uint64_t _r = ((uint64_t)0 | ((c->x[0]) << 0)); c->x[19] = _r; }\n"
              "    c->x[8]=recomp_load64(c,c->x[8] + 2192);\n"
              "    c->x[8]=recomp_load64(c,c->x[8] + 0);\n"
              "    { { uint64_t _v=recomp_load32(c,(c->x[8]+16ULL)); a64_vset64(c->vreg[0],_v); } }\n")
BGM_TIME = ("    { { uint64_t _v=recomp_load32(c,(c->x[8]+16ULL)); a64_vset64(c->vreg[8],_v); } }\n"
            "    { { uint64_t _v=recomp_load32(c,(c->x[8]+24ULL)); a64_vset64(c->vreg[9],_v); } }\n")
BGM_FLAGS = "    c->x[8]=recomp_load8(c,c->x[26] + 2200);\n"   # (the update's four reads of the change flags)
TURN_MULS = ("    {uint64_t _a=0,_b=0,_z,_v;memcpy(&_a,c->vreg[0],4);memcpy(&_b,c->vreg[1],4);_z=(_a^_b)&0x80000000ULL;\n"
             "    _v=a64_fpx_mul32_fma32(c,_a,_b,_z);\n"
             "    a64_vset64(c->vreg[1],_v);}\n")
TURN_MULS2 = ("    {uint64_t _a=0,_b=0,_z,_v;memcpy(&_a,c->vreg[0],4);memcpy(&_b,c->vreg[8],4);_z=(_a^_b)&0x80000000ULL;\n"
              "    _v=a64_fpx_mul32_fma32(c,_a,_b,_z);\n"
              "    a64_vset64(c->vreg[0],_v);}\n")
HORSE_TEST = "    a64_vset64(c->vreg[9],0x3f800000ULL);\n    { if (!a64_fcmp32(c,c->vreg[10],c->vreg[9])) "   # (its line goes on)
HORSE_BRANCH = "if (!c->n) { goto L_82c5ae48; }\n"
FLOOR_TEST = ("    { { uint64_t _v=recomp_load32(c,(c->x[8]+40ULL)); a64_vset64(c->vreg[0],_v); } }\n"
              "    c->x[8] = 0x42c80000ULL;\n"
              "    c->x[8] &= 0xFFFFFFFFULL;\n"
              "    { a64_vset64(c->vreg[1],0); memcpy(&c->vreg[1][0],&c->x[8],4); }\n")
FLOOR_FCMP = "    { if (!a64_fcmp32(c,c->vreg[0],c->vreg[1])) "   # (its line goes on, then the branch)
FLOOR_BRANCH = "if ((c->n == c->v) && !c->z) { goto %s; }\n"
SOLVER_STORE = "    recomp_store32(c,c->x[0] + 712,c->x[1]);\n"
WHOLE_STORE = "    recomp_store32(c,%s,c->x[%d]);\n"
# (the unsigned cut's addition of the elapsed counter: "u<N>" anchors on the cut followed by the addition to x<N>)
WHOLE_ADD = ("    { uint64_t _a=c->x[%d],_b=((uint64_t)(uint32_t)(((uint32_t)(c->x[8])) << 0)),_r=_a+_b; _r&=0xFFFFFFFFULL; "
             "c->x[8]=_r; }\n")


HEAP_CPU = (0x80456910, 0x80456da4)   # (f_804566e0's tryAlloc calls, by their return addresses, and the GPU memory's)
HEAP_GPU = (0x80456f1c, 0x80457098)
HEAP_CPU_LINE = "{ uint64_t _t = c->x[8]; c->x[30] = %#xULL; A64_ICALL(c, _t); }\n"
HEAP_GPU_LINE = "c->x[30] = %#xULL; f_80522050(c);\n"


def lift_lines(kind, addr, args):
    """the lines a lift hook puts in, in the order of their places"""
    mark = "   /* hl gate %s */\n" % step_id(addr)
    if kind == "sound":
        return ["    { extern int hos_oot_sound_gate(Ctx *A64_NOESC, uint32_t, uint64_t); "
                "if (!hos_oot_sound_gate(c, (uint32_t)c->x[2], " + SOUND_SLOT + " + 2192)) return; }" + mark,
                "    { extern uint32_t hos_oot_sound_step(uint32_t); "
                "a64_vset64(c->vreg[0], hos_oot_sound_step((uint32_t)c->vreg[0][0])); }" + mark]
    if kind == "bgm":
        return (["    { extern void hos_oot_bgm_begin(void); hos_oot_bgm_begin(); }" + mark,
                 "    { extern uint32_t hos_oot_sound_step(uint32_t); extern uint32_t hos_oot_sound_seconds(uint32_t, uint32_t); "
                 "const uint32_t _st = (uint32_t)c->vreg[8][0]; a64_vset64(c->vreg[9], hos_oot_sound_seconds((uint32_t)c->vreg[9][0], _st)); "
                 "a64_vset64(c->vreg[8], hos_oot_sound_step(_st)); }" + mark] +
                ["    { extern uint64_t hos_oot_bgm_flags(uint64_t, uint64_t); "
                 "c->x[8] = hos_oot_bgm_flags(c->x[26] + 2200, c->x[8]); }" + mark] * 4)
    if kind == "turn":
        cond = "(uint32_t)c->vreg[8][0] > 0 && (uint32_t)c->vreg[8][0] < 0x3f800000u"   # (0 < step < 1, by its bits: no host compare)
        out = []
        for tag, src, dst in (("a", 1, 1), ("b", 8, 0)):
            out += ["    if (" + cond + ") { {uint64_t _a=0,_b=0,_v;memcpy(&_a,c->vreg[0],4);memcpy(&_b,c->vreg[%d],4);"
                    "_v=a64_fpx_div32_div32(c,_a,_b);a64_vset64(c->vreg[%d],_v);} goto L_hl_%s_%s; }" % (src, dst, step_id(addr), tag) + mark,
                    "L_hl_%s_%s:;" % (step_id(addr), tag) + mark]
        return out
    if kind == "horse":
        return ["    { extern int hos_oot_horse_whole_step(uint32_t, uint64_t); "
                "if (hos_oot_horse_whole_step((uint32_t)c->vreg[10][0], " + SOUND_SLOT + " + 2192)) { goto L_82c5ae48; } }" + mark]
    if kind == "floor":
        return ["    { extern int hos_oot_floor_carry_slow(uint32_t, uint64_t); if ((c->n == c->v) && !c->z && "
                "!hos_oot_floor_carry_slow((uint32_t)c->vreg[0][0], " + SOUND_SLOT + " + 2192)) goto L_hl_%s; }" % step_id(addr) + mark,
                "L_hl_%s:;" % step_id(addr) + mark]
    if kind == "solver":
        return ["    { extern uint32_t hos_oot_solver_steps(uint32_t, uint32_t, uint32_t); c->x[1] = "
                "hos_oot_solver_steps((uint32_t)c->x[1], (uint32_t)c->vreg[0][0], (uint32_t)c->vreg[1][0]); }" + mark]
    if kind == "whole":
        reg, key = args[:2]
        return ["    { extern uint32_t hos_frame_whole(uint64_t, uint32_t, uint32_t); c->x[%d] = hos_frame_whole(%s, "
                "(uint32_t)c->vreg[0][0], (uint32_t)c->x[%d]); }%s" % (reg, key, reg, mark)]
    if kind == "heap":
        lines = []
        for lr in HEAP_CPU:
            lines += ["    { extern int hos_oot_heap_pre(Ctx *A64_NOESC); extern void hos_oot_heap_post(Ctx *A64_NOESC, uint64_t); "
                      "if (hos_oot_heap_pre(c)) c->x[0] = 0; else {" + mark,
                      "    } hos_oot_heap_post(c, %#xULL); }" % lr + mark]
        for lr in HEAP_GPU:
            lines += ["    { extern int hos_oot_gpu_pre(Ctx *A64_NOESC, uint64_t); extern void hos_oot_gpu_post(Ctx *A64_NOESC, A64Fn); "
                      "if (hos_oot_gpu_pre(c, %#xULL)) c->x[0] = 0; else {" % lr + mark,
                      "    } hos_oot_gpu_post(c, f_80522050); }" + mark]
        return lines
    raise ValueError(kind)


def lift_places(body, addr, kind, args):
    """the offsets in f_<addr>'s body (without its hook) where the lines of lift_lines go, or None when an anchor is not
    there once"""
    if kind == "sound":
        if body.count(SOUND_LOAD) != 1:
            return None
        return [len(HEAD % addr), body.index(SOUND_LOAD) + len(SOUND_LOAD)]
    if kind == "bgm":
        if body.count(BGM_TIME) != 1 or body.count(BGM_FLAGS) != 4:
            return None
        places, at = [len(HEAD % addr), body.index(BGM_TIME) + len(BGM_TIME)], 0
        for _ in range(4):
            at = body.index(BGM_FLAGS, at) + len(BGM_FLAGS)
            places.append(at)
        return places if places == sorted(places) else None
    if kind == "turn":
        if body.count(TURN_MULS + TURN_MULS2) != 1:
            return None
        at = body.index(TURN_MULS + TURN_MULS2)
        mid, end = at + len(TURN_MULS), at + len(TURN_MULS + TURN_MULS2)
        return [at, mid, mid, end]
    if kind == "horse":
        if body.count(HORSE_TEST) != 1 or "L_82c5ae48:;" not in body:
            return None
        line_end = body.find("\n", body.index(HORSE_TEST) + len(HORSE_TEST))
        if line_end < 0 or not body.startswith(HORSE_BRANCH, line_end + 1):
            return None
        return [line_end + 1]
    if kind == "floor":
        branch = FLOOR_BRANCH % args[0]
        if body.count(FLOOR_TEST) != 1 or body.count(branch) != 1:
            return None
        fcmp = body.index(FLOOR_TEST) + len(FLOOR_TEST)
        line_end = body.find("\n", fcmp)
        if not body.startswith(FLOOR_FCMP, fcmp) or line_end < 0 or not body.startswith(branch, line_end + 1):
            return None
        return [line_end + 1, line_end + 1 + len(branch)]
    if kind == "solver":
        anchor = CUT % 1 + SOLVER_STORE
        if body.count(anchor) != 1:
            return None
        return [body.index(anchor) + len(CUT % 1)]
    if kind == "whole":
        reg, key = args[:2]
        cut = CUT % reg
        mode = args[2] if len(args) > 2 else ""
        if mode:
            cut = cut.replace(",32,32,1,3,", ",32,32,0,3,")
            if mode[1:]:
                anchor = cut + WHOLE_ADD % int(mode[1:])
                return [body.index(anchor) + len(cut)] if body.count(anchor) == 1 else None
        for anchor in (cut, cut + WHOLE_STORE % (key, reg)):
            if body.count(anchor) == 1:
                return [body.index(anchor) + len(cut)]
        return None
    if kind == "heap":
        places = []
        for lines in ((HEAP_CPU_LINE, HEAP_CPU), (HEAP_GPU_LINE, HEAP_GPU)):
            for lr in lines[1]:
                text = lines[0] % lr
                if body.count("\n" + text) != 1:
                    return None
                at = body.index("\n" + text) + 1
                places += [at, at + len(text)]
        return places
    raise ValueError(kind)


def wanted_lifts(host=HOST):
    """{address: (hash, kind, args)} of every lift hook (HL_SOUND_HOOK, HL_SOLVER_HOOK, HL_WHOLE_HOOK) in host/hl_*.c"""
    out = {}
    for path in sorted(glob.glob(os.path.join(host, "hl_*.c"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            for kind, addr, h, rest in WANT_LIFT.findall(f.read()):
                args = tuple(int(a) if a.isdigit() else a.strip('"')
                             for a in re.findall(r'\d+|"[^"\n]*"', rest))
                out[addr] = (h, kind.lower(), args)
    return out


def apply_lift(s, addr, kind, args):
    """s with f_<addr>'s lift hook in (made again from the lift's text: a hook of an older form is replaced), or None when
    its anchors are not there"""
    span = body_of(s, addr)
    if span is None:
        return None
    mark = "/* hl gate %s */" % step_id(addr)
    lines = s[span[0]:span[1]].split("\n")
    body = "\n".join(line for i, line in enumerate(lines) if i == len(lines) - 1 or not line.endswith(mark))
    places = lift_places(body, addr, kind, args)
    if places is None:
        return None
    # (the lines in reverse order of place, those of one place the last first, so that they end up in their order)
    for _, at, line in sorted(((i, at, line) for i, (at, line) in enumerate(zip(places, lift_lines(kind, addr, args)))),
                              key=lambda t: (t[1], t[0]), reverse=True):
        body = body[:at] + line + body[at:]
    return s[:span[0]] + body + s[span[1]:]


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


def apply_text(s, want, steps=None, movies=None, lifts=None):
    """(new text, {address: status}) for one file: gates of `want` whose function is here put in, others' taken out; the
    step hooks of `steps` ({address: (vreg, probe)}, all of them found in the lift: apply) put in, others' taken out; the
    movie hooks of `movies` ({address: offset}, each found with its hash: apply) and the lift hooks of `lifts` ({address:
    (kind, args)}, the same) too"""
    status = {}
    steps = steps or {}
    movies = movies or {}
    lifts = lifts or {}
    keep = set(want) | {step_id(a) for a in steps} | {step_id(a) for a in movies} | {step_id(a) for a in lifts}
    # (stale gates out first)
    def stale(m):
        if m.group(1) in keep:
            return m.group(0)
        status[m.group(1)] = "taken out"
        return ""
    s = MARK.sub(stale, s)
    for addr, (reg, probe) in steps.items():
        line = step_line(addr, probe)
        if line in s:
            status["step " + addr] = "hooked already"
            continue
        at = step_site(s, addr, reg)
        if at is None:
            continue
        s = s[:at] + line + s[at:]
        status["step " + addr] = "hooked"
    for addr, off in movies.items():
        line = movie_line(addr)
        if line in s:
            status["movie " + addr] = "hooked already"
            continue
        at = movie_site(s, addr, off)
        if at is None:
            continue
        s = s[:at] + line + s[at:]
        status["movie " + addr] = "hooked"
    for addr, (kind, args) in lifts.items():
        out = apply_lift(s, addr, kind, args)
        if out is None:
            continue
        status["%s %s" % (kind, addr)] = "hooked already" if out == s else "hooked"
        s = out
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


def apply(gen, host=HOST, quiet=False, skip=None):
    """Returns the number of gates wanted that are in place. skip: {address: mod name} of functions an exeFS mod patches (the builder's
    code mods): no gate and no step or movie hook goes into those (the translated, patched body runs), and a gate already there is
    taken out; the build log says so for each. The step hooks go in both or neither, so one patched step function leaves both out."""
    skip = {a: m for a, m in (skip or {}).items()}
    want = wanted(host)
    asked = wanted_steps(host)
    movies_asked = wanted_movies(host)
    lifts_asked = wanted_lifts(host)
    for addr in sorted(a for a in set(want) | set(asked) | set(movies_asked) | set(lifts_asked) if a in skip):
        print("hl_gates: f_%s NOT gated: an exeFS mod patches it (mods/%s)" % (addr, skip[addr]))
    if any(a in skip for a in asked):
        asked = {}
    want = {a: h for a, h in want.items() if a not in skip}
    movies_asked = {a: v for a, v in movies_asked.items() if a not in skip}
    lifts_asked = {a: v for a, v in lifts_asked.items() if a not in skip}
    state_path = os.path.join(gen, "hl_gates.state")
    # (FORM: the gates' lines as this script writes them; a new form makes a gen folder stamped by the old one read again)
    state = FORM + "\n" + "\n".join("%s %s" % kv for kv in sorted(want.items())) + "\n" + \
        "\n".join("step %s %s" % (a, v) for a, v in sorted(asked.items())) + \
        "".join("\nmovie %s %s" % (a, v) for a, v in sorted(movies_asked.items())) + \
        "".join("\nlift %s %s" % (a, v) for a, v in sorted(lifts_asked.items())) + \
        "\n".join("skip %s %s" % kv for kv in sorted(skip.items()))
    files = sorted(glob.glob(os.path.join(gen, "code_*.c")))
    stamp = state + "\n" + "\n".join("%s %d %d" % (os.path.basename(p), os.path.getsize(p), os.stat(p).st_mtime_ns) for p in files)
    if os.path.isfile(state_path):   # (nothing asked for changed and no file was written since: nothing to read)
        with open(state_path, encoding="utf-8") as f:
            if f.read() == stamp:
                return len(want)
    found = {}
    # (the step hooks: each asked-for divide found with its recorded hash, or none of them hooked)
    steps, seen = {}, set()
    if asked:
        for path in files:
            with open(path, encoding="utf-8", newline="") as f:
                s = f.read()
            for addr, (h, reg, probe) in asked.items():
                if body_of(s, addr) is not None and text_hash(s, addr) == h and \
                        step_site(MARK.sub("", s[body_of(s, addr)[0]:body_of(s, addr)[1]]), addr, reg) is not None:
                    seen.add(addr)
        if seen == set(asked):
            steps = {a: (reg, probe) for a, (h, reg, probe) in asked.items()}
        else:
            print("hl_gates: WARNING: the step hooks' divides found at %d of %d (%s): none hooked" %
                  (len(seen), len(asked), " ".join(sorted(set(asked) - seen))))
    movies_hooked, lifts_hooked = set(), set()
    for path in files:
        with open(path, encoding="utf-8", newline="") as f:
            s = f.read()
        # (a movie or lift hook goes in only where its function's text hashes as recorded; elsewhere a stale one is taken out)
        movies = {a: off for a, (h, off) in movies_asked.items() if body_of(s, a) is not None and text_hash(s, a) == h}
        lifts = {a: (k, v) for a, (h, k, v) in lifts_asked.items() if body_of(s, a) is not None and text_hash(s, a) == h}
        if not want and not steps and not movies and not lifts and "/* hl gate " not in s:
            continue
        out, status = apply_text(s, want, steps, movies, lifts)
        movies_hooked |= {a for a in movies if movie_line(a) in out}
        lifts_hooked |= {a for a, (k, v) in lifts.items() if status.get("%s %s" % (k, a), "").startswith("hooked")}
        for addr, st in status.items():
            found[addr] = st
            if not quiet or st.startswith("NOT gated"):   # (a gate left out is always said: a code mod changed that function's text)
                print("hl_gates: f_%s in %s: %s" % (addr, os.path.basename(path), st))
        if out != s:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(out)
    for addr in want:
        if addr not in found:
            print("hl_gates: WARNING: f_%s is not in the lifted code: not gated" % addr)
    for addr in sorted(set(movies_asked) - movies_hooked):
        print("hl_gates: WARNING: f_%s's movie count not found with its recorded hash: not hooked" % addr)
    for addr in sorted(set(lifts_asked) - lifts_hooked):
        print("hl_gates: WARNING: f_%s (%s hook) not found with its recorded hash and anchors: not hooked" %
              (addr, lifts_asked[addr][1]))
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
