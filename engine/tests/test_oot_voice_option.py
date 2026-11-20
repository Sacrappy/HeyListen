"""The game's Voice Language option is applied after a restart (host/oot_voice.c, nx2/oot_patches.py; issue 430).

The game's voice resource picks the voice files' language at its first update (the text language's default) and after that
only when GameDataMgr lists OptionData.VoiceLanguage among the flags written since its last pass (f_80a9d1c0). option.sav is
read after that first update and replaces the option's value without listing it, so a choice saved in an earlier session
showed on the game's Options screen and was never applied: Japanese chosen, English voices.

The update's check, in text shaped as the lift writes it, goes through nx2/oot_patches.py patch_voice_option as a lift does
(and is compiled with host/oot_voice.c as it is) over a guest memory of its own with a made-up GameDataMgr (the enum table
and its entries) and stand-ins for the game's "was it written" and for the branch that reads the option and applies it. Then:

- the start: the default is applied once, when its write is listed; updates with nothing new apply nothing;
- option.sav read: the option's value replaced and not listed is applied, once (this fails without the fix);
- a write from the Options screen is applied once, as before; another flag's write applies nothing;
- another question through the hook (another flag's hash) gets the game's answer alone; a GameDataMgr without the option or
  not made yet: the game's answer alone;
- HOS_VOICE_OPTION_FOLLOW=0: the game's answer alone, as shipped (the saved choice is not applied);
- the edit is made once, at the update's call alone (another caller of f_80a9d1c0 is left), a second run changes nothing,
  another version's text is left alone, and apply() makes it on the supported build's lift only.

python -I -B tests/test_oot_voice_option.py [--cc gcc]
"""
import argparse
import inspect
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

A64_H = r'''#pragma once
#include <stdint.h>
#include <string.h>
#define A64_NOESC
typedef struct { uint64_t x[32]; uint8_t n, z, c, v; } Ctx;
typedef void (*A64Fn)(Ctx *);
#define GUEST 0x80000000ull
extern unsigned char mem[0x10000];
static inline uint64_t recomp_load32(Ctx *c, uint64_t a) { uint32_t v; (void)c; memcpy(&v, mem + (a - GUEST), 4); return v; }
static inline uint64_t recomp_load64(Ctx *c, uint64_t a) { uint64_t v; (void)c; memcpy(&v, mem + (a - GUEST), 8); return v; }
'''

HOS_H = r'''#pragma once
#include "a64.h"
void hos_log(const char *fmt, ...);
'''

# The voice resource's update (f_80a9ca30) from its question to its two ways on, as the lift writes them; voice_apply
# stands for the branch at 0x80a9cd30, which reads the option and applies it. f_80807200: another caller of f_80a9d1c0.
LIFTED = r'''#include "a64.h"
void f_80a9d1c0(Ctx *A64_NOESC);
void voice_apply(Ctx *A64_NOESC);
void f_80a9ca30(Ctx *__restrict A64_NOESC c) {
    c->x[1] = 0xdc9dULL;
    c->x[1] &= 0xFFFFFFFFULL;
    c->x[22] = 0xdc9dULL;
    c->x[22] &= 0xFFFFFFFFULL;
    c->x[20]=recomp_load64(c,c->x[20] + 384);
    c->x[1] = (c->x[1] & ~(0xFFFFULL<<16)) | (0xa2dcULL<<16);
    c->x[1] &= 0xFFFFFFFFULL;
    c->x[22] = (c->x[22] & ~(0xFFFFULL<<16)) | (0xa2dcULL<<16);
    c->x[22] &= 0xFFFFFFFFULL;
    c->x[0]=recomp_load64(c,c->x[20] + 0);
c->x[30] = 0x80a9ca9cULL; f_80a9d1c0(c);
if (((c->x[0] >> 0) & 1)) { goto L_80a9cd30; }
L_80a9caa0:;
    return;
L_80a9cd30:;
    voice_apply(c);
    goto L_80a9caa0;
}
void f_80807200(Ctx *__restrict A64_NOESC c) {
    c->x[1] = 0x8ULL;
c->x[30] = 0x80807210ULL; f_80a9d1c0(c);
}
'''

FIXTURE = r'''#include "hos.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
#define CHECK_WHY(x, why) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, why); exit(1); } } while (0)
unsigned char mem[0x10000];
static int logs, asked, applies;
static uint32_t written, applied;   /* (the flag GameDataMgr lists as written, 0: none; the language the voices are in) */
void hos_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); putchar('\n'); logs++; }

/* GameDataMgr: the page at PAGE + 384 holds the holder's address, the holder holds MGR (as the update loads it). Its enums:
   a table of 7 {hash, entry}, the option's place (its hash % 7 = 1) taken by another flag, so it is one further on; 5
   entries of 0x38 bytes, the option's the fourth */
#define PAGE 0x80000100ull
#define HOLDER 0x80000400ull
#define MGR 0x80001000ull
#define TABLE 0x80002000ull
#define ENTRIES 0x80003000ull
#define OPTION 0xa2dcdc9du
#define OTHER 8u
#define JAPANESE 0x69b014ceu   /* (three of the option's choices: the hashes of their names) */
#define ENGLISH 0x82591fa9u
#define GERMAN 0x1c3498ebu
static void store32(uint64_t a, uint32_t v) { memcpy(mem + (a - GUEST), &v, 4); }
static void store64(uint64_t a, uint64_t v) { memcpy(mem + (a - GUEST), &v, 8); }
static void set_option(uint32_t v) { store32(ENTRIES + 0x38 * 3 + 0x1c, v); }
static void make_mgr(void) {
    store64(PAGE + 384, HOLDER), store64(HOLDER, MGR);
    store32(MGR + 0x250, 5), store64(MGR + 0x258, ENTRIES), store64(MGR + 0x260, TABLE), store32(MGR + 0x26c, 7);
    store32(TABLE + 8 * 1, OTHER), store32(TABLE + 8 * 1 + 4, 0);
    store32(TABLE + 8 * 2, OPTION), store32(TABLE + 8 * 2 + 4, 3);
    store32(ENTRIES + 0x1c, 0x1234);   /* (the other flag's value: never the option's) */
}

/* the game's f_80a9d1c0: x0 GameDataMgr, x1 a flag's hash; x0 bit 0: listed as written. Its scratch registers are not kept */
void f_80a9d1c0(Ctx *c) {
    asked++;
    c->x[0] = written && (uint32_t)c->x[1] == written;
    c->x[1] = c->x[2] = c->x[8] = c->x[9] = 0xdead;
}
/* the update's branch at 0x80a9cd30: the option read and applied */
void voice_apply(Ctx *c) { applied = (uint32_t)recomp_load32(c, ENTRIES + 0x38 * 3 + 0x1c); applies++; }
void f_80a9ca30(Ctx *c);
void f_80807200(Ctx *c);
static void update(void) {
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[20] = PAGE;
    f_80a9ca30(&c);
}
/* The start: the option is the game data's own (Japanese); the first update applies the text language's default (English)
   at once and queues its write, which is listed a pass later */
static void start(void) {
    make_mgr();
    set_option(JAPANESE), applied = ENGLISH;
    update();
    CHECK(applies == 0 && asked == 1);
    set_option(ENGLISH), written = OPTION, update(), written = 0;
    CHECK(applies == 1 && applied == ENGLISH);
    for (int k = 0; k < 3; k++) update();
    CHECK(applies == 1 && !logs);
}
#ifdef HOOKED
void hos_oot_voice_option(Ctx *c, A64Fn game);
#endif

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "off")) {   /* HOS_VOICE_OPTION_FOLLOW=0: as shipped */
        start();
        set_option(JAPANESE), update(), update();
        CHECK(applies == 1 && applied == ENGLISH && !logs);
        set_option(GERMAN), written = OPTION, update(), written = 0;
        CHECK(applies == 2 && applied == GERMAN);
        puts("PASS: HOS_VOICE_OPTION_FOLLOW=0: the game's answer alone, option.sav's choice not applied (as shipped)");
        return 0;
    }
    start();
    /* option.sav read: the option's value replaced, not listed as written */
    set_option(JAPANESE), update();
    CHECK_WHY(applied == JAPANESE, "option.sav's Voice Language (Japanese) is not applied: the voices stay the text "
                                   "language's default (English), issue 430");
    CHECK(applies == 2 && logs == 1);
    for (int k = 0; k < 3; k++) update();
    CHECK(applies == 2 && logs == 1);
    puts("PASS: the default applied once at the start; option.sav's choice applied once when it is read");
    /* the game's Options screen writes the option: listed, applied once */
    set_option(GERMAN), written = OPTION, update(), written = 0;
    CHECK(applies == 3 && applied == GERMAN);
    update();
    written = OTHER, update(), written = 0;
    CHECK(applies == 3 && logs == 1);
    puts("PASS: a write of the option applied once, as before; another flag's write applies nothing");
#ifdef HOOKED
    {   /* another question through the hook: the game's answer, whatever the option does; the option's change is still seen */
        Ctx c;
        memset(&c, 0, sizeof c);
        c.x[0] = MGR, c.x[1] = OTHER, set_option(ENGLISH);
        hos_oot_voice_option(&c, f_80a9d1c0);
        CHECK(!(c.x[0] & 1) && applies == 3);
        c.x[0] = MGR, c.x[1] = OTHER, written = OTHER;
        hos_oot_voice_option(&c, f_80a9d1c0), written = 0;
        CHECK((c.x[0] & 1) && applies == 3);
        update();
        CHECK(applies == 4 && applied == ENGLISH);
    }
    /* a GameDataMgr without the option: the game's answer alone. Back: its value then is the one last seen */
    store32(TABLE + 8 * 2, 0), set_option(GERMAN), update();
    CHECK(applies == 4);
    store32(TABLE + 8 * 2, OPTION), set_option(ENGLISH), update();
    CHECK(applies == 4);
    /* no GameDataMgr yet */
    store64(HOLDER, 0), set_option(JAPANESE), update();
    CHECK(applies == 4);
    store64(HOLDER, MGR), update();
    CHECK(applies == 5 && applied == JAPANESE);
    puts("PASS: another flag's question gets the game's answer; no option or no GameDataMgr: the game's answer alone");
#endif
    return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()

    out, found = oot_patches.patch_voice_option(LIFTED)   # (as a lift of the supported build does)
    hooked = found == 1 and "hos_oot_voice_option(" in out
    cc = shutil.which(a.cc) or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        print("SKIP: no C compiler")
    else:
        with tempfile.TemporaryDirectory(prefix="voice-option-") as tmp:
            tmp = Path(tmp)
            (tmp / "a64.h").write_text(A64_H, encoding="utf-8")
            (tmp / "hos.h").write_text(HOS_H, encoding="utf-8")
            (tmp / "fixture.c").write_text(FIXTURE, encoding="utf-8")
            (tmp / "code_009.c").write_text(out, encoding="utf-8")
            sources = [str(tmp / "fixture.c"), str(tmp / "code_009.c")]
            if hooked:   # (without the edit the update is the game's own: the check below then fails for the bug's reason)
                (tmp / "oot_voice.c").write_text((ROOT / "host" / "oot_voice.c").read_text(encoding="utf-8"), encoding="utf-8")
                sources += [str(tmp / "oot_voice.c"), "-DHOOKED"]
            exe = tmp / ("fixture.exe" if os.name == "nt" else "fixture")
            subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-I", str(tmp)] + sources + ["-o", str(exe)],
                           check=True)
            env = {k: v for k, v in os.environ.items() if k != "HOS_VOICE_OPTION_FOLLOW"}
            for mode, extra in (("", {}), ("off", {"HOS_VOICE_OPTION_FOLLOW": "0"})):
                r = subprocess.run([str(exe), mode], env=dict(env, **extra), capture_output=True, text=True)
                print("\n".join(line for line in r.stdout.splitlines() if line.startswith("PASS")))
                if r.returncode:
                    raise SystemExit("FAIL (%s):\n%s%s" % (mode or "follow", r.stdout, r.stderr))

    assert hooked, "the update's question was not routed through host/oot_voice.c"
    assert out.count(oot_patches.VOICE_OPTION_NEW) == 1 and out.count(oot_patches.VOICE_OPTION_DECL) == 1
    assert "c->x[30] = 0x80807210ULL; f_80a9d1c0(c);" in out, "another caller of f_80a9d1c0 was changed"
    again, found2 = oot_patches.patch_voice_option(out)
    assert again == out and found2 == 1, "a second run changed the text"
    other = LIFTED.replace("0x80a9ca9cULL", "0x80a9caa0ULL")
    assert oot_patches.patch_voice_option(other) == (other, 0), "another version's text was edited"
    apply_src = inspect.getsource(oot_patches.apply)
    assert "if protect_hearts:\n            voice, found = patch_voice_option(out)" in apply_src, \
        "the edit is made on builds other than the supported one"
    print("PASS: nx2/oot_patches.py: the update's call edited once (f_80a9d1c0's other caller left), a second run changes "
          "nothing, other text is left alone, the supported build only")


if __name__ == "__main__":
    main()
