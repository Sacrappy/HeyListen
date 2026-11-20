"""Game-free: the button prompts' font patch at run time (host/prompts.c cmap4_glyph, cmap4_point, cmap4_bind) on a cmap made the
way the build makes it (nx2/prompt_assets.py cmap4): a bound Switch button's code point gets its pad input's icon, an unbound one
keeps its own, and the pad icons stay. The keyboard face (nx2/prompt_keys.py): a Switch button gets the icon of the first key bound
to it (the second slot's when the first is clear), one whose key has no icon (or no key at all) its pad input's, and the sticks the
keys that move and the mouse. Also the faces' order: host/prompts.c faces[], gpu/vk/vk_api.h HOS_PROMPT_* and nx2/prompt_faces.py
FACES (after the Switch, then the keyboard), and the defaults: host/prompts.c switch_pad and stick_code, nx2/prompt_assets.py
DEFAULT, nx2/prompt_keys.py STICKS and the code points.

python -I -B tests/test_prompt_font_bind.py [--cc gcc]
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import prompt_assets  # noqa: E402
import prompt_faces  # noqa: E402
import prompt_keys  # noqa: E402

# (the Switch buttons bound in the test: A to the bottom face button, B to the right shoulder; the rest unbound)
BIND = {0: 0, 1: 10}
# (and their keys: A on E, B on the mouse's left button in the second slot (the first clear), X on a key the font has no icon for;
# the rest none)
KEYS = {0: (0x45, 0), 1: (0, 0x01), 2: (0x07, 0)}
KEY_ICONS = (0x45, 0x01, 0x49)   # (the keys with an icon in the test's cmap)
KEYBOARD = len(prompt_faces.FACES) + 1   # (HOS_PROMPT_KEYBOARD)
# (the movement keys the test tries, each way's two slots: up, down, left, right)
MOVES = [
    ("the defaults", [(0x57, 0), (0x53, 0), (0x41, 0), (0x44, 0)]),
    ("up on I", [(0x49, 0), (0x53, 0), (0x41, 0), (0x44, 0)]),
    ("up on I in the second slot", [(0, 0x49), (0x53, 0), (0x41, 0), (0x44, 0)]),
    ("up on a key without an icon", [(0x07, 0), (0x53, 0), (0x41, 0), (0x44, 0)]),
    ("up unbound", [(0, 0), (0x53, 0), (0x41, 0), (0x44, 0)]),
]


def first_key(b):
    k = KEYS.get(b, (0, 0))
    return k[0] or k[1]


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


def defines(src, names):
    """host/prompts.c's #defines, comments off"""
    out = []
    for name in names:
        d = re.search(r"#define %s .*" % name, src)
        check(d is not None, "host/prompts.c has no %s" % name)
        out.append(d.group(0).split("/*")[0].rstrip())
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")

    faces = re.search(r"faces\[HOS_PROMPT_N\] = \{([^}]*)\}", src)
    check(faces is not None, "host/prompts.c has no faces[HOS_PROMPT_N]")
    names = re.findall(r'"([a-z0-9]+)"', faces.group(1))
    check(names == prompt_assets.ALL_FACES and names[-1] == "keyboard",
          "faces[] %s is not the Switch, prompt_faces.FACES, then the keyboard" % names)
    enum = re.search(r"enum \{ (HOS_PROMPT_SWITCH[^}]*)\}", api)
    check(enum is not None, "gpu/vk/vk_api.h has no HOS_PROMPT_* enum")
    ids = [e.strip() for e in enum.group(1).split(",") if e.strip()]
    check(ids == ["HOS_PROMPT_" + n.upper() for n in names] + ["HOS_PROMPT_N"], "HOS_PROMPT_* %s is not faces[]" % ids)

    pads = re.search(r"switch_pad\[HOS_SW_N\] = \{([^}]*)\}", src)
    check(pads is not None, "host/prompts.c has no switch_pad[HOS_SW_N]")
    check([e.strip() for e in pads.group(1).split(",")] == ["HOS_PAD_" + p.upper() for p in prompt_assets.DEFAULT],
          "host/prompts.c switch_pad is not nx2/prompt_assets.py DEFAULT")
    sticks = re.search(r"stick_code\[15\] = \{([^}]*)\}", src)
    check(sticks is not None and [int(c, 16) for c in sticks.group(1).split(",")] == [c for c, _, _ in prompt_keys.STICKS],
          "host/prompts.c stick_code is not nx2/prompt_keys.py STICKS")
    for name, value in (("KEY_BASE", prompt_keys.KEY_BASE), ("STICK_KEYS", prompt_keys.STICK_KEYS),
                        ("STICK_GAME", prompt_keys.STICK_GAME)):
        d = re.search(r"#define %s (0x[0-9A-Fa-f]+|\d+)" % name, src)
        check(d is not None and int(d.group(1), 0) == value, "host/prompts.c %s is not nx2/prompt_keys.py's" % name)
    check(prompt_keys.KEY_BASE + 256 <= prompt_keys.STICK_KEYS and prompt_keys.STICK_KEYS + 16 <= prompt_keys.STICK_GAME,
          "the keyboard face's code points overlap")
    check(len({c for c, _, _ in prompt_keys.STICKS}) == len(prompt_keys.STICKS), "nx2/prompt_keys.py STICKS repeats a code")

    # the codes: the Switch buttons on glyphs 100.., the pad icons on 200.., the keyboard's keys' icons on 900 + key, its sticks on
    # 1200.. (theirs) and 1300.. (the game's), each stick's code a segment of its own on 1400.. (as face_font: each Switch code, and
    # on the keyboard face each stick's, a segment of its own)
    nfaces = len(names)
    mapping = {c: 100 + b for b, c in enumerate(prompt_assets.SWITCH)}
    mapping.update({prompt_assets.PAD_BASE + i: 200 + i for i in range(len(prompt_assets.PADS))})
    stick_codes = [c for c, _, _ in prompt_keys.STICKS]
    mapping.update({prompt_keys.KEY_BASE + k: 900 + k for k in KEY_ICONS})
    mapping.update({c: 1400 + i for i, c in enumerate(stick_codes)})
    mapping.update({prompt_keys.STICK_KEYS + i: 1200 + i for i in range(len(stick_codes))})
    mapping.update({prompt_keys.STICK_GAME + i: 1300 + i for i in range(len(stick_codes))})
    # (the keyboard face's cmap: the Switch codes and the sticks' a segment of their own; a pad face's: the Switch codes)
    subs = {f: prompt_assets.cmap4(mapping, set(prompt_assets.SWITCH) | (set(stick_codes) if f == KEYBOARD else set()))
            [20:] for f in range(nfaces)}   # (one format 4 subtable, after the cmap's header and its two records)
    codes = list(prompt_assets.SWITCH) + [prompt_assets.PAD_BASE + i for i in range(len(prompt_assets.PADS))] + stick_codes

    harness = "\n".join([
        "#include <stdio.h>", "#include <stdlib.h>", "#include <string.h>", '#include "gpu/vk/vk_api.h"',
        *defines(src, ["PAD_BASE", "KEY_BASE", "STICK_KEYS", "STICK_GAME"]),
        "static int move_keys[4][2];",
        "static int hos_binding(int set, int button, int slot) {",
        "    if (set == HOS_BIND_KEY) {",
        "        if (button >= HOS_KEY_MOVE_UP && button < HOS_KEY_MOVE_UP + 4) return move_keys[button - HOS_KEY_MOVE_UP][slot];",
        *("        if (button == %d) return slot ? %d : %d;" % (b, k[1], k[0]) for b, k in KEYS.items()),
        "        return 0;",
        "    }",
        *("    if (button == %d) return %d;" % kv for kv in BIND.items()),
        "    return -1;",
        "}",
        re.search(r"static const uint16_t switch_code\[HOS_SW_N\] = \{[^}]*\};", src).group(0),
        re.search(r"static const unsigned char switch_pad\[HOS_SW_N\] = \{[^}]*\};", src).group(0),
        re.search(r"static const uint16_t stick_code\[15\] = \{[^}]*\};", src).group(0),
        re.search(r"static const unsigned char stick_ways\[7\] = \{[^}]*\};", src).group(0),
        re.search(r"static const unsigned char move_default\[4\] = \{[^}]*\};", src).group(0),
        block(src, "static int first_key("),
        block(src, "static unsigned stick_shows("),
        re.search(r"static unsigned rd16\(.*", src).group(0),
        re.search(r"static void wr16\(.*", src).group(0),
        block(src, "static unsigned cmap4_glyph("),
        block(src, "static int cmap4_point("),
        block(src, "static int cmap4_bind("),
        "int main(int argc, char **argv) {",
        "    (void)switch_pad;",
        "    for (int m = 0; m < 4; m++) move_keys[m][0] = (int)strtol(argv[3 + 2 * m], NULL, 0),",
        "                                      move_keys[m][1] = (int)strtol(argv[4 + 2 * m], NULL, 0);",
        "    FILE *f = fopen(argv[1], \"rb\");",
        "    static unsigned char st[65536];",
        "    const size_t n = fread(st, 1, sizeof st, f);",
        "    fclose(f);",
        "    printf(\"%d\\n\", cmap4_bind(st, n, atoi(argv[2])));",
        "    for (int i = 11; i < argc; i++) printf(\"%u\\n\", cmap4_glyph(st, n, (unsigned)strtoul(argv[i], NULL, 0)));",
        "    return 0;",
        "}", ""])
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "bind.c", Path(tmp) / "bind"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(c), "-o", str(exe)], check=True)

        def run(face, moves):
            bin_ = Path(tmp) / ("cmap4-%d.bin" % face)
            bin_.write_bytes(subs[face])
            args = [str(exe), str(bin_), str(face)] + [str(k) for pair in moves for k in pair] + [str(x) for x in codes]
            return subprocess.run(args, check=True, capture_output=True, text=True).stdout.split()

        outs = [run(f, MOVES[0][1]) for f in range(nfaces)]
        moved = [(name, moves, run(KEYBOARD, moves)) for name, moves in MOVES]
    nsw = len(prompt_assets.SWITCH)
    for f, out in enumerate(outs):
        done, got = int(out[0]), [int(x) for x in out[1:]]
        keyed = {b: first_key(b) for b in range(nsw) if f == KEYBOARD and first_key(b) in KEY_ICONS}
        want_done = len(keyed) if f == KEYBOARD else len(BIND)
        check(done == want_done, "face %d: %d buttons bound, not %d" % (f, done, want_done))
        for b, code in enumerate(prompt_assets.SWITCH):
            want = 900 + keyed[b] if b in keyed else (200 + BIND[b] if b in BIND else 100 + b)
            check(got[b] == want, "face %d: Switch button %d (U+%04X): glyph %d, not %d" % (f, b, code, got[b], want))
        for i in range(len(prompt_assets.PADS)):
            check(got[nsw + i] == 200 + i, "face %d: pad icon %d moved" % (f, i))
        for i in range(len(stick_codes)):
            want = (1200 if f == KEYBOARD else 1300) + i
            check(got[nsw + len(prompt_assets.PADS) + i] == want,
                  "face %d: stick icon %d: glyph %d, not %d" % (f, i, got[nsw + len(prompt_assets.PADS) + i], want))
    # (the movement keys: each stick icon as nx2/prompt_keys.py's table and host/prompts.c stick_shows say)
    check([e.strip() for e in re.search(r"stick_ways\[7\] = \{([^}]*)\}", src).group(1).split(",")] ==
          [str(sum(1 << m for m, w in enumerate("udlr") if w in ways)) for _, _, ways in prompt_keys.STICKS[:7]],
          "host/prompts.c stick_ways is not nx2/prompt_keys.py STICKS' ways")
    check([e.strip() for e in re.search(r"move_default\[4\] = \{([^}]*)\}", src).group(1).split(",")] ==
          ["0x%02X" % prompt_keys.MOVE[w] for w in "udlr"], "host/prompts.c move_default is not nx2/prompt_keys.py MOVE")
    for name, moves, out in moved:
        got = [int(x) for x in out[1 + nsw + len(prompt_assets.PADS):]]
        for i in range(len(stick_codes)):
            ways = prompt_keys.STICKS[i][2]
            want = 1200 + i   # (the build's picture: the keys that move as they are, or the mouse for the camera)
            for m, w in enumerate("udlr") if prompt_keys.STICKS[i][1] == "move" else ():
                key = moves[m][0] or moves[m][1]
                if w in ways and key != prompt_keys.MOVE[w]:
                    want = 900 + key if len(ways) == 1 and key in KEY_ICONS else 1300 + i
                    break
            check(got[i] == want, "keyboard face, movement keys %s: stick icon %d (%s): glyph %d, not %d" %
                  (name, i, prompt_keys.STICKS[i][2], got[i], want))
    print("ok: on each of %d faces, %d of %d Switch buttons pointed at their pad icons (the keyboard face: the first key bound to "
          "it), the rest and the pad icons kept; %d stick icons, and the movement keys in %d sets" %
          (nfaces, len(BIND), nsw, len(stick_codes), len(MOVES)))


if __name__ == "__main__":
    main()
