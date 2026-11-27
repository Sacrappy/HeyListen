# Copyright (C) 2026 Sacrappy

"""Game-free: the textures the button prompts write for a face (host/prompts.c face_runs). A pad face: its textures.bin's entries
for the pad input each Switch button is bound to. The Keyboard face: the Switch's textures on its default controls (the game's own
pictures), then the ocarina's notes of keyboard/textures.bin for the first key bound to each button (the entries' second byte is a
key code), a key with no note picture leaving the Switch's, a build without keyboard/textures.bin the Switch's alone; the keyboard's
runs come after the Switch's (a later run wins where they meet).

python -I -B tests/test_prompts_face_runs.py [--cc gcc]
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


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu/vk/vk_api.h"
%(parts)s
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%%d %%s\n", __LINE__, #x); return 1; } } while (0)
/* a textures.bin entry: u8 Switch button, u8 pad input (or key), u16, u32 runs; a run: u32 offset, u32 length, the bytes */
static size_t entry(uint8_t *out, int sw, int pad, uint32_t off, const char *bytes) {
    const uint32_t one = 1, len = (uint32_t)strlen(bytes);
    out[0] = (uint8_t)sw, out[1] = (uint8_t)pad, out[2] = out[3] = 0;
    memcpy(out + 4, &one, 4), memcpy(out + 8, &off, 4), memcpy(out + 12, &len, 4), memcpy(out + 16, bytes, len);
    return 16 + len;
}
int main(void) {
    uint8_t sw[256], kb[256];
    const uint32_t two = 2;
    memcpy(sw, "HPTX", 4), memcpy(sw + 4, &two, 4);
    size_t n = 8;
    n += entry(sw + n, HOS_SW_A, HOS_PAD_EAST, 100, "AAAA");    /* (the Switch's A on its default input: the keyboard face's too) */
    n += entry(sw + n, HOS_SW_A, HOS_PAD_NORTH, 200, "BBBB");   /* (on another input) */
    const long swn = (long)n;
    memcpy(kb, "HPTX", 4), memcpy(kb + 4, &two, 4);
    n = 8;
    n += entry(kb + n, HOS_SW_A, 0x45, 300, "EEEE");
    n += entry(kb + n, HOS_SW_A, 0x46, 400, "FFFF");
    const long kbn = (long)n;
    int bind[BINDS];
    for (int i = 0; i < BINDS; i++) bind[i] = -1;
    bind[HOS_SW_A] = HOS_PAD_NORTH;     /* A on the top face button of a pad */
    bind[HOS_SW_N + HOS_SW_A] = 0x45;   /* and on the key E */
    size_t nruns;
    Run *runs = face_runs(HOS_PROMPT_XBOX, bind, sw, swn, kb, kbn, &nruns);   /* (a pad face: its bindings, no keys) */
    CHECK(nruns == 1 && runs[0].off == 200);
    free(runs);
    runs = face_runs(HOS_PROMPT_KEYBOARD, bind, sw, swn, kb, kbn, &nruns);   /* (the Switch's defaults, then the key's note) */
    CHECK(nruns == 2 && runs[0].off == 100 && runs[1].off == 300 && runs[0].idx < runs[1].idx);
    free(runs);
    bind[HOS_SW_N + HOS_SW_A] = 0x47;   /* a key with no note picture: the Switch's alone */
    runs = face_runs(HOS_PROMPT_KEYBOARD, bind, sw, swn, kb, kbn, &nruns);
    CHECK(nruns == 1 && runs[0].off == 100);
    free(runs);
    bind[HOS_SW_N + HOS_SW_A] = 0x45;
    runs = face_runs(HOS_PROMPT_KEYBOARD, bind, sw, swn, NULL, 0, &nruns);   /* (a build without the notes) */
    CHECK(nruns == 1 && runs[0].off == 100);
    free(runs);
    puts("ok");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    parts = [
        re.search(r"#define BUTTON_BINDS .*", src).group(0).split("/*")[0].rstrip(),
        re.search(r"#define BINDS .*", src).group(0).split("/*")[0].rstrip(),
        src[src.index("typedef struct { uint32_t off, len, idx; const uint8_t *data; } Run;"):].split("\n", 1)[0],
        re.search(r"static const unsigned char switch_pad\[HOS_SW_N\] = \{[^}]*\};", src).group(0),
        re.search(r"static uint32_t rd32le\(.*", src).group(0),
        block(src, "static void add_run("),
        block(src, "static void texture_runs_add("),
        block(src, "static int texture_face("),
        block(src, "static const int *texture_bind("),
        block(src, "static Run *face_runs("),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "runs.c", Path(tmp) / "runs"
        c.write_text(HARNESS % {"parts": "\n".join(parts)}, encoding="utf-8")
        subprocess.run([a.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(ROOT), str(c), "-o",
                        str(exe)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
    check(r.returncode == 0 and r.stdout.strip() == "ok", "face_runs: %s%s" % (r.stdout, r.stderr))
    assets = (ROOT / "nx2" / "prompt_assets.py").read_text(encoding="utf-8")
    check("keyboard_layers(" in assets and 'f"{name}.key{k:02x}.png"' in assets, "nx2/prompt_assets.py makes no keyboard notes")
    kb = assets[assets.index("def keyboard_layers("):assets.index("def face_textures(")]
    check("for name, sw in NOTES.items():" in kb and "for name, sw in LETTERS.items():" in kb,
          "the keyboard face must make the Talk ring's letters (KeyTexFont*) as well as the notes: the ring showed the Switch's X Y B")
    check('"%sprompts/keyboard/textures.bin"' in src, "host/prompts.c does not read keyboard/textures.bin")
    print("ok: face_runs: a pad face's bindings; the keyboard face's Switch defaults then its keys' notes, a key without a picture "
          "and a build without the notes leaving the Switch's")


if __name__ == "__main__":
    main()
