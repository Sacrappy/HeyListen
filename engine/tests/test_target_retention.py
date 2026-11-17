"""The eviction pass's choice of which render-target textures go, run as it stands in vk_texture.cpp over laps of a route.

Render-target images in the game's log grew and were not freed (issue 1443): every image in the render targets' blocks is a
Texture, and a Texture goes only when the pass in `texture_frame` chooses it. This cuts that pass's text (from `order` to the end of its
loop) out of the source, builds tests/test_target_retention.cpp around it and runs laps of small finalized render targets (the terrain
tiles' normal maps) that nothing is ever drawn over: how many are left after 30 laps with the textures within the budget (a sweep) and over
it, and what a game that writes over their storage leaves: finalized_obsolete (the rule that such a texture is gone) is cut out of the
source as well and run on the textures' written bytes.

    python -I -B tests/test_target_retention.py [--cxx g++]
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import between, block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

START = "std::vector<std::pair<uint64_t, Texture*>> order;\n    order.reserve(textures.size());"
END = "    {   // (ported from Longshot 25ee6a83, #323) nor one whose storage another image was drawn into after its own last drawing"   # (what follows the loop)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++", help="C++ compiler")
    a = p.parse_args()
    cxx = shutil.which(a.cxx)
    if not cxx:
        raise SystemExit("test_target_retention: no C++ compiler %r (pass --cxx)" % a.cxx)
    src = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
    cut = between(src, START, END)
    # (a source with no such rule: the pass is run with a stand-in that says no texture is obsolete, which is what it does without one)
    marker = "bool finalized_obsolete(const Texture& t)"
    obsolete = block(src, marker) if marker in src else "bool finalized_obsolete(const Texture&) { return false; }"
    with tempfile.TemporaryDirectory(prefix="target-retention-") as temp:
        d = Path(temp)
        (d / "cut.inc").write_text(cut, encoding="utf-8")
        (d / "obsolete.inc").write_text(obsolete, encoding="utf-8")
        exe = d / "retention"
        subprocess.run([cxx, "-std=c++20", "-O1", "-w", '-DCUT="%s"' % (d / "cut.inc"),
                        '-DOBSOLETE="%s"' % (d / "obsolete.inc"), str(ROOT / "tests" / "test_target_retention.cpp"),
                        "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(result.stdout, end="")
        if result.returncode:
            raise SystemExit("test_target_retention: failed")


if __name__ == "__main__":
    main()
