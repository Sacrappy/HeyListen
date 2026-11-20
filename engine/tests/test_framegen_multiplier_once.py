"""Frame generation takes the multiplier its sets were made for (HeyListen issue 55: nvoglv64.dll read of 0x590 on the Presentation
Thread, in framegen_record's last vkCmdCopyImage; Longshot 1bf4284a, notquiteog).

framegen_record runs on the replay thread, the Insert menu on the display thread (menu_tick from present_image), and a wait for a
free set sits between ensure_size (which makes the sets' images for the multiplier then) and the rest. A step up in that window,
with the multiplier read again, made the last copy's target set->frame[made] an image that was never made (a null handle in the
driver). Checked here: after ensure_size, framegen_record reads the multiplier from fg.mult (what the images were made for) and
the Auto row (framegen_now() == 1) once, and never calls multiplier() again.
python -I -B tests/test_framegen_multiplier_once.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, head):
    at = text.find(head)
    if at < 0:
        raise SystemExit("FAIL: not found: " + head)
    depth, i = 0, text.index("{", at)
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[at:j + 1]
    raise SystemExit("FAIL: unbalanced: " + head)


def main():
    src = (ROOT / "gpu" / "vk" / "vk_framegen.cpp").read_text(encoding="utf-8")
    body = block(src, "bool framegen_record(")
    after = body[body.index("ensure_size(") + len("ensure_size("):]
    if "multiplier()" in after:
        raise SystemExit("FAIL: framegen_record reads multiplier() after ensure_size made the sets")
    if "m = fg.mult" not in after:
        raise SystemExit("FAIL: framegen_record does not take m from fg.mult")
    # (the row as it runs, framegen_now: the mode's clamp and the limit's lowering, read once)
    if after.count("framegen_now()") != 1 or "auto_rate = framegen_now() == 1" not in after or "opts.framegen" in after:
        raise SystemExit("FAIL: the Auto row must be read once (auto_rate), not again")
    if "if (auto_rate)" not in after:
        raise SystemExit("FAIL: the Auto branch does not use the single read")
    print("ok: framegen_record takes the sets' multiplier from fg.mult and reads the Auto row once")


if __name__ == "__main__":
    main()
