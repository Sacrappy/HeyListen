"""Game-free: the button prompts' Auto face for a controller's name (host/prompts.c face_of_name, contains). A Steam Controller
gets the Steam Deck's face, as the Deck does: SDL3 names it "Steam Controller", which fell through to the Xbox's. Under
Steam, Steam Input's stand-in pad for a Steam Controller has the same name, so it gets the Deck's face too. A pad named
"Steam Virtual Gamepad" stays on the Xbox's, since any controller can be behind it.

python -I -B tests/test_prompt_auto_face.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# (the controller's name as SDL gives it, the face Auto picks)
NAMES = [
    ("", "HOS_PROMPT_SWITCH"),
    ("Steam Controller", "HOS_PROMPT_STEAMDECK"),
    ("Steam Deck", "HOS_PROMPT_STEAMDECK"),
    ("Steam Deck Controller", "HOS_PROMPT_STEAMDECK"),
    ("Steam Virtual Gamepad", "HOS_PROMPT_XBOX"),
    ("DualSense Wireless Controller", "HOS_PROMPT_PS5"),
    ("PS4 Controller", "HOS_PROMPT_PS4"),
    ("Nintendo Switch Pro Controller", "HOS_PROMPT_SWITCH"),
    ("Xbox 360 Controller", "HOS_PROMPT_XBOX"),
    ("8BitDo Ultimate 2C", "HOS_PROMPT_XBOX"),
]


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


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    src = (ROOT / "host" / "prompts.c").read_text(encoding="utf-8")
    check("static int face_of_name(" in src, "host/prompts.c has no face_of_name")
    check("face_of_name(name)" in block(src, "static int choose_face("), "choose_face does not ask face_of_name")

    harness = "\n".join([
        "#include <ctype.h>", "#include <stdio.h>", "#include <string.h>", '#include "gpu/vk/vk_api.h"',
        block(src, "static int contains("),
        block(src, "static int face_of_name("),
        "int main(void) {",
        *('    printf("%%d %%d\\n", face_of_name("%s"), %s);' % nf for nf in NAMES),
        "    return 0;",
        "}", ""])
    with tempfile.TemporaryDirectory() as tmp:
        c, exe = Path(tmp) / "face.c", Path(tmp) / "face"
        c.write_text(harness, encoding="utf-8")
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT), str(c), "-o", str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split("\n")
    for (name, face), line in zip(NAMES, out):
        got, want = line.split()
        check(got == want, '"%s": face %s, not %s (%s)' % (name, got, want, face))
    print("ok: %d controller names, each with its face" % len(NAMES))


if __name__ == "__main__":
    main()
