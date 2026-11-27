# Copyright (C) 2026 Sacrappy
"""The ids the host PUSHES to the menu (rx_option_set -> gpu/vk/vk_menu.cpp menu_option_set) must not collide: the
performance summary's per-frame pushes (gpu/vk/perf_parts.h: 90, 91 and 94 to 99; host/nvn_native.c pushes 94 + k at every
present) share the number space with the prompts' two pushes (gpu/vk/vk_opt_ids.h HL_OPT_PROMPTS_LIVE and
HL_OPT_PROMPTS_MISSING). At 96 the missing-prompts flag took the wait for a virtual core as its value and the Button
prompts row's hint flipped at every frame; at 95 the prompts' live parts were counted as a wait and never reached the menu.

python -I -B tests/test_option_push_ids.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    perf = (ROOT / "gpu/vk/perf_parts.h").read_text(encoding="utf-8")
    ids = (ROOT / "gpu/vk/vk_opt_ids.h").read_text(encoding="utf-8")
    taken = set()
    m = re.search(r"enum \{ kOptPace = (\d+), kOptLoading = (\d+) \};", perf)
    assert m, "perf_parts.h: kOptPace / kOptLoading not found"
    taken |= {int(m.group(1)), int(m.group(2))}
    m = re.search(r"enum \{ kOptWaitLock = (\d+)((?:, kOpt\w+)+) \};", perf)
    assert m, "perf_parts.h: the wait ids not found"
    first = int(m.group(1))
    taken |= set(range(first, first + 1 + m.group(2).count(",")))
    pushes = {}
    for name in ("HL_OPT_PROMPTS_LIVE", "HL_OPT_PROMPTS_MISSING"):
        m = re.search(r"#define %s (\d+)" % name, ids)
        assert m, name + " not found"
        pushes[name] = int(m.group(1))
    for name, v in pushes.items():
        if v in taken:
            raise SystemExit("FAIL: %s = %d is one of the performance summary's push ids %s" % (name, v, sorted(taken)))
    if len(set(pushes.values())) != len(pushes):
        raise SystemExit("FAIL: the prompts' two push ids are the same")
    # (every other define of the header must differ from them too: an asked id may share a number with a pushed one only by intent)
    others = {int(v) for n, v in re.findall(r"#define (HL_OPT_\w+) (\d+)", ids) if n not in pushes}
    for name, v in pushes.items():
        if v in others:
            raise SystemExit("FAIL: %s = %d is another option's id in vk_opt_ids.h" % (name, v))
    print("PASS option push ids: the prompts' pushes %s are outside the performance pushes %s" % (sorted(pushes.values()), sorted(taken)))


if __name__ == "__main__":
    main()
