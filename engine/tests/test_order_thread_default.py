# Copyright (C) 2026 Sacrappy
"""The order thread's default in the test build v0.4.4-test2: on again by the rule (off with HL_ORDER_THREAD_AUTO=0): the rule of
DECOUPLE3 (docs/research/decouple-cross-frame.md 4.9) decides: RX_VK_ORDER_THREAD on where the host keeps a
core for the replay, off elsewhere, and the switch always wins.

By the text: host/hos_main.c sets RX_VK_ORDER_THREAD=1 only when it is not set and hos_reserved_cpus() names a core; it does so
after hookshot_env.txt is read (the file's RX_VK_ORDER_THREAD=0 and HOS_RESERVE_CORE=0 count) and before anything reads the switch
(nvn_native.c caches it at the game's first frames, the renderer when it loads), in both of main's paths; the renderer and the
host's present poll and event join read that same variable.

python -I -B tests/test_order_thread_default.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(text, head):
    i = text.index(head)
    j = text.index("{", i)
    depth = 0
    for k in range(j, len(text)):
        depth += {"{": 1, "}": -1}.get(text[k], 0)
        if depth == 0:
            return text[i:k + 1]
    raise SystemExit("FAIL: unbalanced " + head)


def need(c, msg):
    if not c:
        raise SystemExit("FAIL: " + msg)


def main():
    hm = (ROOT / "host/hos_main.c").read_text(encoding="utf-8")
    f = function(hm, "static void order_thread_default(void) {")
    # (test build v0.4.4-test2: the rule decides again unless HL_ORDER_THREAD_AUTO=0; that check comes first)
    auto = 'if (getenv("HL_ORDER_THREAD_AUTO") && atoi(getenv("HL_ORDER_THREAD_AUTO")) == 0) return;'
    need(auto in f and f.index(auto) < f.index('_putenv("RX_VK_ORDER_THREAD=1");'),
         "HL_ORDER_THREAD_AUTO=0 no longer keeps the order thread off")
    need('if (getenv("RX_VK_ORDER_THREAD") || !hos_reserved_cpus()) return;' in f,
         "the default overrides a set switch, or is on without a core kept for the replay")
    need('_putenv("RX_VK_ORDER_THREAD=1");' in f and 'setenv("RX_VK_ORDER_THREAD", "1", 0);' in f, "the default does not set the switch")
    need(hm.count("    load_hookshot_env();\n    order_thread_default();\n") == 2,
         "the default is not decided right after hookshot_env.txt in both of main's paths")
    need("RX_VK_ORDER_THREAD" not in function(hm, "__attribute__((constructor)) static void heylisten_env_defaults(void) {"),
         "the default is set before hookshot_env.txt is read (its =0 and HOS_RESERVE_CORE=0 would not count)")
    rp = (ROOT / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    need('getenv("RX_VK_ORDER_THREAD") && atoi(getenv("RX_VK_ORDER_THREAD")) != 0' in function(rp, "bool order_thread_on() {"),
         "the renderer does not read the same variable")
    nv = (ROOT / "host/nvn_native.c").read_text(encoding="utf-8")
    need(nv.count('getenv("RX_VK_ORDER_THREAD") && atoi(getenv("RX_VK_ORDER_THREAD")) != 0') >= 2,
         "the host's present poll and event join do not read the same variable")
    print("PASS order thread default")


if __name__ == "__main__":
    main()
