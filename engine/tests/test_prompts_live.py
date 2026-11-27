# Copyright (C) 2026 Sacrappy

"""Game-free: the button prompts following a change while the game runs (host/prompts.c; after Longshot #2366, Gabriel Moisan Matte). put_runs over a copy of a game
file in a guest window: the game's own bytes come back where the runs before were, the new runs are written in their order
(a later one over an earlier), a run past the range found is left alone and counted, and every write is marked
(HOS_WRITTEN: the renderer uploads it again). Guarded (the layouts, whose offsets the game makes pointers where they lie):
a run over bytes the game changed is left alone, the rest written, and only those written are put back at the next change.
The textures' pool finalized (nvn_native.c's nvnMemoryPoolFinalize: hos_prompts_pool_gone): not written any more (put_runs
over a range let go writes nothing), it is let go under the lock a change is written under, and another pool's finalize
leaves it be.

A text's button codes (text_code) after a change: the code of the face's icon of the pad input now, the d-pad's the face's, the
sticks' the keyboard's or the game's, a button on a key a code of its own, anything else left. And Auto's device in use (auto_look:
the hysteresis): a single touch of the other device, or a resting pad's noise, does not change it; real use of it while the one in
use is quiet does, and it does not go back within the dwell.

python -I -B tests/test_prompts_live.py [--cc gcc]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def span(text, first, last):
    """from first to the end of the block that starts at last"""
    start = text.index(first)
    return text[start:text.index(last, start)] + block(text[text.index(last, start):], last)


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PATH 260
typedef long LONG;
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static void hos_log(const char *fmt, ...) { (void)fmt; }
enum { WINDOW = 1 << 20 };
static uint8_t window[WINDOW];   /* (guest addresses: offsets into it, from 0x1000) */
static void *hos_guest_range(uint64_t a, uint64_t n) {
    return a >= 0x1000 && a < WINDOW && n <= WINDOW - a ? window + a : NULL;
}
static size_t marked, marked_bytes;
#define HOS_WRITTEN(p, n) (marked++, marked_bytes += (n))
%(run)s
%(live)s
%(keep)s
%(put)s
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%%d %%s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv) {
    (void)argc;
    /* the game's file: 4096 bytes of i * 7; the guest's copy at 0x2000 as the start left it, a run of 0xAA at 100 (16) */
    enum { N = 4096, AT = 0x2000 };
    uint8_t game[N];
    for (int i = 0; i < N; i++) game[i] = (uint8_t)(i * 7);
    FILE *f = fopen(argv[1], "wb");
    fwrite(game, 1, N, f);
    fclose(f);
    memcpy(window + AT, game, N);
    memset(window + AT + 100, 0xAA, 16);
    static Live l;
    snprintf(l.path, sizeof l.path, "%%s", argv[1]);
    l.base = AT, l.lo = AT, l.hi = AT + N;
    const uint8_t aa[16] = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
    Run before[1] = {{100, 16, 0, aa}};
    keep_ranges(&l, before, 1);
    /* the change: 0xBB at 200 (8), then 0xCC at 204 (8) over its end, and a run past the file's range */
    const uint8_t bb[8] = {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB}, cc[8] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    Run now[3] = {{200, 8, 0, bb}, {204, 8, 1, cc}, {N + 10, 8, 2, bb}};
    size_t written, put_back;
    const size_t outside = put_runs(&l, now, 3, &written, &put_back);
    CHECK(outside == 1 && written == 2 && put_back == 1);
    CHECK(!memcmp(window + AT + 100, game + 100, 16));   /* (the game's own bytes back) */
    CHECK(!memcmp(window + AT + 200, bb, 4) && !memcmp(window + AT + 204, cc, 8));   /* (the later run over the earlier) */
    CHECK(!memcmp(window + AT, game, 100) && !memcmp(window + AT + 212, game + 212, N - 212));
    CHECK(window[AT + N + 10] == 0);   /* (past the range: not written) */
    CHECK(marked == 3 && marked_bytes == 16 + 8 + 8);
    CHECK(l.nranges == 2 && l.ranges[0][0] == 200 && l.ranges[1][0] == 204);   /* (the one not written: not kept) */
    /* guarded: the game made a pointer at 300 (8 bytes); a run over it is left alone, one beside it written */
    l.guarded = 1;
    memset(window + AT + 300, 0x7f, 8);
    Run guarded[2] = {{296, 8, 0, bb}, {400, 8, 1, cc}};
    CHECK(put_runs(&l, guarded, 2, &written, &put_back) == 1 && written == 1 && put_back == 2);
    CHECK(!memcmp(window + AT + 200, game + 200, 12));   /* (the change before: the game's bytes back) */
    CHECK(window[AT + 296] == game[296] && window[AT + 300] == 0x7f && !memcmp(window + AT + 400, cc, 8));
    CHECK(l.nranges == 1 && l.ranges[0][0] == 400);
    /* the next change puts back only what was written: the game's pointer stays */
    Run none[1] = {{N + 10, 8, 0, bb}};
    CHECK(put_runs(&l, none, 1, &written, &put_back) == 1 && put_back == 1 && window[AT + 300] == 0x7f &&
          !memcmp(window + AT + 400, game + 400, 8));
    /* the memory let go (base 0, hos_prompts_pool_gone): nothing written, not even at a run's offset from 0 */
    uint8_t before_all[WINDOW / 16];
    memcpy(before_all, window, sizeof before_all);
    l.base = 0;
    Run low[1] = {{0x1000, 8, 0, bb}};
    CHECK(put_runs(&l, low, 1, &written, &put_back) == 0 && written == 0 && put_back == 0);
    CHECK(!memcmp(before_all, window, sizeof before_all));
    puts("ok");
    return 0;
}
'''

GONE = r'''
#include <stdint.h>
static void hos_log(const char *fmt, ...) { (void)fmt; }
typedef int SRWLOCK;
static SRWLOCK tex_lock;
static int held, taken;
static void AcquireSRWLockExclusive(SRWLOCK *l) { held += l == &tex_lock, taken += l == &tex_lock; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { held -= l == &tex_lock; }
static struct { struct { volatile uint64_t base; uint64_t lo, hi; } l; } tex;
%(gone)s
int main(void) {
    tex.l.lo = 0x5000, tex.l.hi = 0x9000, tex.l.base = 0x4f00;
    hos_prompts_pool_gone(0x6000);   /* (another pool) */
    if (tex.l.base != 0x4f00) return 1;
    hos_prompts_pool_gone(0x5000);
    return tex.l.base != 0 ? 2 : held || !taken ? 3 : 0;
}
'''


def pool_gone(cc, src, tmp):
    nvn = (ROOT / "host" / "nvn_native.c").read_text(encoding="utf-8")
    check("hos_prompts_pool_gone(o->mem);" in block(nvn, "NV(nvnMemoryPoolFinalize)"),
          "nvnMemoryPoolFinalize does not tell the prompts a pool is gone")
    c, exe = Path(tmp) / "gone.c", Path(tmp) / "gone"
    c.write_text(GONE % {"gone": block(src, "void hos_prompts_pool_gone(")}, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)]).returncode
    check(r == 0, "hos_prompts_pool_gone: %s" % {1: "another pool's finalize dropped the textures' pool",
                                                   2: "the textures' pool finalized is still written",
                                                   3: "the pool let go outside tex_lock (a change being written does not wait)"}.get(r, r))

TEXTS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/vk/vk_api.h"
%(defs)s
static int keyboard_built = 1;
static int nslots = 5;
static uint8_t slot_button[320] = {0, 1, 2, 3, 4};
static int key_slot(int b, int k) { (void)b; return k == 0x07 ? -1 : SLOT_BASE + 200 + k; }
%(tables)s
%(fns)s
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%%d %%s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    int bind[BINDS];
    for (int i = 0; i < BINDS; i++) bind[i] = -1;
    for (int b = 0; b < HOS_SW_N; b++) bind[HOS_SW_N + b] = 0;
    bind[BUTTON_BINDS + 0] = 0x57, bind[BUTTON_BINDS + 1] = 0x53, bind[BUTTON_BINDS + 2] = 0x41, bind[BUTTON_BINDS + 3] = 0x44;
    bind[0] = HOS_PAD_NORTH;   /* A on the top face button */
    bind[HOS_SW_N + 0] = 0x45;   /* and on the key E */
    const int xbox = HOS_PROMPT_XBOX, ps5 = HOS_PROMPT_PS5, kb = HOS_PROMPT_KEYBOARD;
    /* a Switch button: the face's icon of its pad input (BIND_BASE: a code of its own for each face, button and input) */
    CHECK(text_code(switch_code[0], xbox, bind) == BIND_BASE + (xbox * HOS_SW_N + 0) * HOS_PAD_ART_N + HOS_PAD_NORTH);
    CHECK(text_code(switch_code[0], ps5, bind) == BIND_BASE + (ps5 * HOS_SW_N + 0) * HOS_PAD_ART_N + HOS_PAD_NORTH);
    /* unbound: its default's icon */
    CHECK(text_code(switch_code[1], xbox, bind) == BIND_BASE + (xbox * HOS_SW_N + 1) * HOS_PAD_ART_N + switch_pad[1]);
    /* a code written before, for another face: the new face's (the button is the code's) */
    CHECK(text_code(BIND_BASE + (xbox * HOS_SW_N + 0) * HOS_PAD_ART_N + HOS_PAD_NORTH, ps5, bind) ==
          BIND_BASE + (ps5 * HOS_SW_N + 0) * HOS_PAD_ART_N + HOS_PAD_NORTH);
    /* the d-pad's icons: the face's */
    CHECK(text_code(static_code[1], ps5, bind) == PAD_BASE + FACE_CODES * ps5 + STATIC_AT + 1);
    /* the keyboard: a button on a key a code of its own; on a key without an icon the pad input's icon */
    CHECK(text_code(switch_code[0], kb, bind) == SLOT_BASE + 200 + 0x45);
    bind[HOS_SW_N + 2] = 0x07;
    CHECK(text_code(switch_code[2], kb, bind) == BIND_BASE + (kb * HOS_SW_N + 2) * HOS_PAD_ART_N + switch_pad[2]);
    /* the sticks: the keyboard's icons (W A S D as built), the game's own on the others */
    CHECK(text_code(stick_code[0], kb, bind) == STICK_KEYS + 0);
    CHECK(text_code(stick_code[0], xbox, bind) == STICK_GAME + 0);
    bind[BUTTON_BINDS + 0] = 0x49;   /* up on I: the one way shows its key's icon */
    CHECK(text_code(stick_code[1], kb, bind) == KEY_BASE + 0x49);
    /* not a button: left alone */
    CHECK(text_code(0x41, xbox, bind) == -1 && text_code(0xE000, ps5, bind) == -1);
    puts("ok");
    return 0;
}
'''

AUTO = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
%(auto)s
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%%d %%s\n", __LINE__, #x); return 1; } } while (0)
#define S(x) ((int64_t)((x) * 1e9))
static AutoState a;
static int64_t last[2];   /* (the pad's last input, the keyboard's: 0 never) */
static int look(double t) { return auto_look(&a, S(t), last); }
int main(void) {
    /* a pad in use, idle since 5 s; the keyboard touched once: no change, however long after */
    last[0] = S(5);
    auto_start(&a, S(10), 0, last);
    last[1] = S(10.0);
    CHECK(look(10.0) == 0);
    for (double t = 10.1; t < 14; t += 0.1) CHECK(look(t) == 0);   /* (one touch: its burst ends) */
    /* two looks of keys: not yet; the third: the keyboard is in use */
    last[1] = S(14.1);
    CHECK(look(14.1) == 0);
    last[1] = S(14.2);
    CHECK(look(14.2) == 0);
    last[1] = S(14.3);
    CHECK(look(14.3) == 1);
    /* and not back within the dwell, whatever the pad does */
    for (double t = 14.4; t < 15.9; t += 0.1) {
        last[0] = S(t);
        CHECK(look(t) == 1);
    }
    /* a pad in use and the keys touched too: the one in use is not idle, no change */
    memset(&a, 0, sizeof a);
    last[0] = last[1] = 0;
    auto_start(&a, S(20), 0, last);
    for (double t = 20.1; t < 25; t += 0.1) {
        last[0] = S(t), last[1] = S(t);
        CHECK(look(t) == 0);
    }
    /* the keyboard in use, the pad touched while the keys are held: no change; the keys let go and the pad used: the pad's */
    memset(&a, 0, sizeof a);
    last[0] = last[1] = 0;
    auto_start(&a, S(30), 1, last);
    for (double t = 30.1; t < 32.5; t += 0.1) {
        last[1] = S(t);
        if (t > 31) last[0] = S(t);
        CHECK(look(t) == 1);
    }
    for (double t = 32.5; t < 34.0; t += 0.1) {
        last[0] = S(t);
        look(t);
    }
    CHECK(a.dev == 0);
    puts("ok");
    return 0;
}
'''

GLYPH = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int logs;
static void hos_log(const char *fmt, ...) { (void)fmt; logs++; }
enum { WINDOW = 1 << 20 };
static uint8_t window[WINDOW];   /* (guest addresses: offsets into it, from 0x1000) */
static void *hos_guest_range(uint64_t a, uint64_t n) {
    return a >= 0x1000 && a < WINDOW && n <= WINDOW - a ? window + a : NULL;
}
static struct { volatile uint64_t at; uint32_t n; uint8_t head[64]; } font;
%(glyph)s
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%%d %%s\n", __LINE__, #x); return 1; } } while (0)
/* a glyph slot at 0x1000 (its face at +8), the face at 0x2000 (its stream pointer at +192), the stream at 0x3000 (base, size) */
static void stream(uint64_t face, uint64_t base, uint64_t size) {
    *(uint64_t *)(window + 0x1008) = face;
    *(uint64_t *)(window + face + 192) = 0x3000;
    *(uint64_t *)(window + 0x3000) = base, *(uint64_t *)(window + 0x3008) = size;
}
int main(void) {
    enum { N = 4096 };
    font.n = N;
    for (int i = 0; i < 64; i++) font.head[i] = (uint8_t)(0x40 + i);
    /* FreeType reading the font from its OpenType start (the game's buffer + 8, the file's size given): the font is at the base */
    memcpy(window + 0x5000, font.head, 64);
    stream(0x2000, 0x5000, N);
    hos_prompts_glyph(0x1000);
    CHECK(font.at == 0x5000);
    /* the stream's base at the file's header: the font 8 bytes on */
    font.at = 0;
    memset(window + 0x5000, 0, 64);
    memcpy(window + 0x6008, font.head, 64);
    stream(0x2800, 0x6000, N);
    hos_prompts_glyph(0x1000);
    CHECK(font.at == 0x6008);
    /* another font (another size): not it, said once for the face, and not looked at again */
    font.at = 0, logs = 0;
    stream(0x2400, 0x5000, N + 100);
    hos_prompts_glyph(0x1000);
    hos_prompts_glyph(0x1000);
    CHECK(font.at == 0 && logs == 1);
    /* the size of the font but other bytes: not it */
    memset(window + 0x7000, 0x11, 64);
    stream(0x2c00, 0x7000, N);
    hos_prompts_glyph(0x1000);
    CHECK(font.at == 0);
    puts("ok");
    return 0;
}
'''


def font_find(cc, src, tmp):
    c, exe = Path(tmp) / "glyph.c", Path(tmp) / "glyph"
    c.write_text(GLYPH % {"glyph": block(src, "void hos_prompts_glyph(")}, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(c), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout.strip() == "ok", "hos_prompts_glyph: %s%s" % (r.stdout, r.stderr))


def texts(cc, src, tmp):
    consts = ["PAD_BASE", "FACE_CODES", "STATIC_AT", "BIND_BASE", "KEY_BASE", "SLOT_BASE", "SLOTS", "STICK_KEYS", "STICK_GAME",
              "BUTTON_BINDS", "BINDS", "HOS_PAD_ART_N"]
    defs = []
    for name in consts:
        m = re.search(r"#define %s .*" % name, src)
        check(m is not None, "host/prompts.c has no %s" % name)
        defs.append(m.group(0).split("/*")[0].rstrip())
    tables = [re.search(r"static const %s\[[^\]]*\] = \{[^}]*\};" % pat, src).group(0) for pat in (
        "uint16_t switch_code", "unsigned char switch_pad", "uint16_t static_code", "uint16_t stick_code",
        "unsigned char stick_ways", "unsigned char move_default")]
    fns = [block(src, "static unsigned stick_shows("), block(src, "static int text_code(")]
    c, exe = Path(tmp) / "texts.c", Path(tmp) / "texts"
    c.write_text(TEXTS % {"defs": "\n".join(defs), "tables": "\n".join(tables), "fns": "\n".join(fns)}, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(ROOT), str(c), "-o", str(exe)],
                   check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout.strip() == "ok", "text_code: %s%s" % (r.stdout, r.stderr))

BOXES = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef long LONG;
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
#define HOS_PROMPT_N 6
#define BINDS 32
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static int64_t now_ns;
static int64_t hos_now_ns(void) { return now_ns; }
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }
static int logs;
static void hos_log(const char *fmt, ...) { (void)fmt; logs++; }
enum { WINDOW = 1 << 20 };
static uint8_t window[WINDOW];   /* (guest addresses: offsets into it, from 0x1000) */
static void *hos_guest_range(uint64_t a, uint64_t n) { return a >= 0x1000 && a < WINDOW && n <= WINDOW - a ? window + a : NULL; }
static volatile LONG font_changes, text_seen, text_with_codes, text_rewritten, text_codes_told;
static int live_face_now = 2, live_bind_now[BINDS];
static int text_code(unsigned c, int face, const int *bind) { (void)face; (void)bind; return c == 0xE040 ? 0xF123 : c == 0xF123 ? 0xE040 : -1; }
%(code)s
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%%d %%s\n", __LINE__, #x); return 1; } } while (0)
/* a box at 0x2000: its status pointer at +8 (0x3000, the word 0 and the allowed bits after it), its text at +0xf0 (0x4000), its length at +0x124 */
static void make(uint64_t box, uint64_t status, uint64_t text, const char *s, uint32_t n) {
    *(uint64_t *)(window + box + 8) = status;
    *(uint32_t *)(window + status + 4) = 0xFFFFFFFF;
    *(uint64_t *)(window + box + 0xf0) = text;
    *(uint32_t *)(window + box + 0x124) = n;
    memcpy(window + text, s, n);
}
int main(void) {
    /* "Back \xEE\x81\x80" (U+E040) seen before any change: noted, not rewritten, laid out as it was */
    make(0x2000, 0x3000, 0x4000, "Back \xEE\x81\x80", 8);
    make(0x2400, 0x3100, 0x4100, "no icon here", 12);
    CHECK(text_rewrite(0x2000) == 0 && seen_n == 1);
    CHECK(text_rewrite(0x2400) == 0 && seen_n == 1 && ret_codes == 1);
    CHECK(text_rewrite(0x2000) == 0 && seen_n == 1);   /* (the same box: one entry) */
    CHECK(!memcmp(window + 0x4000, "Back \xEE\x81\x80", 8));
    /* the change: the hook is not called again, the table reaches the box at once and marks it to be laid out again */
    font_changes = 1;
    int looked = 0;
    CHECK(boxes_rewrite(&looked) == 1 && looked == 1);
    CHECK(!memcmp(window + 0x4000, "Back \xEF\x84\xA3", 8));   /* (U+F123) */
    CHECK((*(uint32_t *)(window + 0x3000) & 0x120000) == 0x120000);
    /* the next change finds it as it is, and puts the text's code back */
    *(uint32_t *)(window + 0x3000) = 0;
    CHECK(boxes_rewrite(&looked) == 1 && !memcmp(window + 0x4000, "Back \xEE\x81\x80", 8));
    /* the game sets the bar's text again (another string, codes in it): the same box, rewritten */
    *(uint64_t *)(window + 0x2000 + 0xf0) = 0x4800;
    memcpy(window + 0x4800, "x \xEE\x81\x80", 6);
    *(uint32_t *)(window + 0x2000 + 0x124) = 6;
    *(uint32_t *)(window + 0x3000) = 0;
    CHECK(boxes_rewrite(&looked) == 1 && looked == 1 && !memcmp(window + 0x4800, "x \xEF\x84\xA3", 6));
    /* a box gone (its class another now): let be, not even looked at */
    *(uint64_t *)(window + 0x2000) = 0x9999;
    memcpy(window + 0x4800, "x \xEE\x81\x80", 6);
    *(uint32_t *)(window + 0x3000) = 0;
    CHECK(boxes_rewrite(&looked) == 0 && looked == 0 && !memcmp(window + 0x4800, "x \xEE\x81\x80", 6) && *(uint32_t *)(window + 0x3000) == 0);
    /* a box rewritten at a change is laid out again as it is drawn within 20 s: every 250 ms, for one hidden then shown */
    *(uint64_t *)(window + 0x2000) = 0;   /* (its class as it was) */
    *(uint64_t *)(window + 0x2000 + 0xf0) = 0x4000;
    memcpy(window + 0x4000, "Back \xEE\x81\x80", 8);
    *(uint32_t *)(window + 0x2000 + 0x124) = 8;
    CHECK(boxes_rewrite(&looked) == 1 && seen_box[0].pend == 1);
    *(uint32_t *)(window + 0x3000) = 0;
    now_ns = 1000000000;
    box_drawn(0x2000);
    CHECK((*(uint32_t *)(window + 0x3000) & 0x120000) == 0x120000 && boxes_relaid == 1);
    *(uint32_t *)(window + 0x3000) = 0;
    now_ns += 100000000;
    box_drawn(0x2000);
    CHECK(*(uint32_t *)(window + 0x3000) == 0);   /* (not within 250 ms) */
    now_ns += 200000000;
    box_drawn(0x2000);
    CHECK((*(uint32_t *)(window + 0x3000) & 0x120000) == 0x120000 && boxes_relaid == 2);
    /* a box whose text the game draws from a texture of its own (+380 bit 2: the hint bars' icons): its codes are rewritten but its
       status is left alone (bits written there alone drop its glyphs for good: PROMPT4) and it is not laid out again when drawn */
    *(uint32_t *)(window + 0x2000 + 380) = 4;
    memcpy(window + 0x4000, "Back \xEE\x81\x80", 8);
    *(uint32_t *)(window + 0x3000) = 0;
    seen_box[0].pend = 0;
    CHECK(boxes_rewrite(&looked) == 1 && memcmp(window + 0x4000, "Back \xEE\x81\x80", 8) != 0);
    CHECK(*(uint32_t *)(window + 0x3000) == 0 && seen_box[0].pend == 0);
    now_ns += 1000000000;
    box_drawn(0x2000);
    CHECK(*(uint32_t *)(window + 0x3000) == 0);
    *(uint32_t *)(window + 0x2000 + 380) = 0;
    /* more boxes than the table holds: the oldest entry's place is taken, the table never grows past it */
    for (int i = 0; i < BOXES + 5; i++) {
        make(0x10000 + (uint64_t)i * 0x200, 0x3200, 0x7000 + (uint64_t)i * 0x10, "a \xEE\x81\x80", 6);
        text_rewrite(0x10000 + (uint64_t)i * 0x200);
    }
    CHECK(seen_n == BOXES);
    puts("ok");
    return 0;
}
"""


def boxes(cc, src, tmp):
    code = src[src.index("#define BOXES"):src.index("/* nx2/oot_patches.py, as the game lays a text box out")]
    c, exe = Path(tmp) / "boxes.c", Path(tmp) / "boxes"
    c.write_text(BOXES % {"code": code}, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                    "-Wno-unused-but-set-global", str(c), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout.strip() == "ok", "the boxes seen: %s%s" % (r.stdout, r.stderr))



def auto(cc, src, tmp):
    defs = [m.group(0).split("/*")[0].rstrip() for m in re.finditer(r"#define AUTO_\w+ .*", src)]
    enum = re.search(r"enum \{ AUTO_LOOKS = \d+ \};", src)
    check(enum is not None and len(defs) == 3, "prompts.c's Auto hysteresis constants are gone")
    parts = [enum.group(0)] + defs + [re.search(r"typedef struct \{\n    int dev;.*?\} AutoState;", src, re.S).group(0),
                                       block(src, "static void auto_start("), block(src, "static int auto_look(")]
    c, exe = Path(tmp) / "auto.c", Path(tmp) / "auto"
    c.write_text(AUTO % {"auto": "\n".join(parts)}, encoding="utf-8")
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout.strip() == "ok", "auto_look: %s%s" % (r.stdout, r.stderr))
    live = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    check("hos_prompts_live();" in block(live, "static void npad_states("), "the game's controller read does not call hos_prompts_live")
    check('"!prompts "' in live and '"!bind "' in live, "the live input has no !prompts or !bind command")
    check("rx_option_set(HL_OPT_PROMPTS_LIVE" in src, "host/prompts.c does not tell the menu what is live")
    check("HL_OPT_PROMPTS)" in (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8"),
          "the renderer does not answer the Button prompts row")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    parts = {
        "run": src[src.index("typedef struct { uint32_t off, len, idx; const uint8_t *data; } Run;"):].split("\n", 1)[0],
        "live": span(src, "typedef struct {\n    char path[MAX_PATH];", "typedef struct {") .rsplit("}", 1)[0] + "} Live;",
        "keep": block(src, "static void keep_ranges("),
        "put": block(src, "static size_t put_runs("),
    }
    harness = HARNESS % parts
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "live.c", Path(tmp) / "live"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(c), "-o", str(exe)],
                       check=True)
        out = subprocess.run([str(exe), str(Path(tmp) / "game.bin")], capture_output=True, text=True)
        pool_gone(a.cc, src, tmp)
        texts(a.cc, src, tmp)
        auto(a.cc, src, tmp)
        boxes(a.cc, src, tmp)
        font_find(a.cc, src, tmp)
    check(out.returncode == 0 and out.stdout.strip() == "ok", "exit %d: %s%s" % (out.returncode, out.stdout, out.stderr))
    print("ok: text codes follow the face and the controls; Auto's device in use has its hysteresis; put_runs puts the game's bytes back, writes the runs in order, leaves one past the range; guarded, never over "
          "bytes the game changed; the textures' pool finalized is not written, under its lock; the font is found from FreeType's glyph at the stream's base or 8 bytes on; the boxes seen with button codes are rewritten and laid out again at a change, one gone left be")


if __name__ == "__main__":
    main()
