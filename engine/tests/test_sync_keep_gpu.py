# Copyright (C) 2026 Sacrappy

"""The GPU's newer words kept at a mirror sync (FLICK1; gpu/vk/shaders/sync_keep.comp, vk_memory.cpp "the GPU's newer words kept at
a sync"; no game/GPU). With the order thread a submission's capture is taken when the game hands it over, before the order thread
has recorded the submissions handed over just before it; a page the CPU wrote is then copied with the GPU's words as guest memory
had them before those submissions' write-backs, and the sync put those older words back over the mirror's newer ones (a real rollback,
rare; not the one-frame flash, which is the meter pace's). Checks the shader's rule and its wiring (the capture's check value, the sync's test, the switch,
off by default), that the SPIR-V header is the shader's (when glslangValidator is there), and on a model of two
submissions: the whole copy rolls the GPU's word back, the merge keeps it and still takes the CPU's word.

python -I -B tests/test_sync_keep_gpu.py
"""
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu/vk"


def check_shader():
    src = (VK / "shaders/sync_keep.comp").read_text(encoding="utf-8")
    for needle in ("const bool keep = cv != mv && g.w[i] == mv;",
                   "else m.w[i] = cv;",
                   "if (has_shadow) sh.w[i] = keep ? mv : cv;"):
        if needle not in src:
            raise SystemExit("sync_keep.comp no longer has: " + needle)
    glslang = shutil.which("glslangValidator") or next(
        (p for p in (r"C:\VulkanSDK\1.4.363.0\Bin\glslangValidator.exe",) if os.path.exists(p)), None)
    if not glslang:
        print("sync_keep.comp: glslangValidator not found, the header's match not checked")
        return
    with tempfile.TemporaryDirectory(prefix="sync-keep-") as tmp:
        out = Path(tmp) / "h.h"
        subprocess.run([glslang, "-V", "--quiet", "--target-env", "vulkan1.3", "--vn", "SYNC_KEEP_COMP_SPV", "-o", str(out),
                        str(VK / "shaders/sync_keep.comp")], check=True, stdout=subprocess.DEVNULL)
        words = lambda t: re.findall(r"0x[0-9a-f]{8}", t)
        if words(out.read_text(encoding="utf-8"))[5:] != words((VK / "shaders/sync_keep_comp_spv.h").read_text(encoding="utf-8"))[5:]:
            raise SystemExit("shaders/sync_keep_comp_spv.h is not sync_keep.comp's: regenerate it (the command is in the shader)")
    print("shader: the rule, and its header")


def check_wiring():
    mem = (VK / "vk_memory.cpp").read_text(encoding="utf-8")
    for needle in ('#include "shaders/sync_keep_comp_spv.h"',
                   "c->wb_seen = wb_noted_max;   // (FLICK1)",
                   "b.wb_seen = c->wb_seen;   // (FLICK1)",
                   "wb_noted_max = std::max(wb_noted_max, value);   // (FLICK1)",
                   "if (pv > b->second.wb_seen) {",
                   'const int m = e && *e ? atoi(e) : 0;',
                   "if (sk_record(upload_command_buffer(), sk_chunks)) {"):
        if needle not in mem:
            raise SystemExit("vk_memory.cpp no longer has: " + needle)
    # the capture's value is taken before its late check reads the write-backs' pages (a write-back recorded between the two would be
    # neither late nor after the check)
    cm = mem[mem.index("c->wb_seen = wb_noted_max;"):]
    if cm.index("c->wb_seen = wb_noted_max;") > cm.index("const uint64_t pv = wb_pages.get(pages[i]);"):
        raise SystemExit("the capture's wb_seen must be read before the late check")
    print("wiring: the capture's check value, the sync's test, the switch (off by default)")


def merge(capture, mirror, guest):
    """sync_keep.comp for one page: the mirror after the sync"""
    return [m if (c != m and g == m) else c for c, m, g in zip(capture, mirror, guest)]


def model():
    # word 0: a counter the GPU carries from frame to frame; word 1: a word the CPU stores each frame
    guest = [100, 7]
    capture_s2 = [guest[0], 8]          # S2 handed over: the CPU stored 8 in word 1; S1 not replayed yet: word 0 still 100
    mirror = [101, 7]                   # the order thread replays S1: the GPU's counter goes to 101 ...
    guest = [101, 8]                    # ... and its write-back lands (word 1 holds the CPU's 8)
    whole = list(capture_s2)            # the whole copy at S2's sync
    assert whole[0] == 100, "the whole copy puts the GPU's older counter back (the flash)"
    kept = merge(capture_s2, mirror, guest)
    assert kept == [101, 8], kept       # the GPU's newer counter kept, the CPU's word taken
    # the CPU writes the page again after S2's hand-over (the next frame): guest memory no longer holds the mirror's word, the
    # capture's word is the submission's own and goes in
    assert merge([5, 8], [6, 7], [9, 9]) == [5, 8]
    # nothing changed: the merge is the copy
    assert merge([3, 4], [3, 4], [3, 4]) == [3, 4]
    print("model: the whole copy rolls the GPU's word back; the merge keeps it and takes the CPU's")


def main():
    check_shader()
    check_wiring()
    model()


if __name__ == "__main__":
    main()
