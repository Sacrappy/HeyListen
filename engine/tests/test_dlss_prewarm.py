"""(HeyListen, STUT1) NGX is started on a thread of its own at the first present with DLSS chosen (vk_temporal.cpp dlss_prewarm,
called by temporal_idle every present), not at the gameplay upscale's first frame: there NGX's start was 1.8 s of the replay
thread (an RTX 4090, "slow submit: 1870 ms: dispatch 1866"). It is dlss_probe's own call, under the engine's lock, so the
upscale's start() finds it done or waits for what is left. RX_VK_DLSS_PREWARM=0 is the old order. This reads the source; no
GPU/SDK/game.

python -I -B tests/test_dlss_prewarm.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    text = (ROOT / "gpu/vk/vk_temporal.cpp").read_text(encoding="utf-8")
    begin = text.index("static void dlss_prewarm() {")
    end = text.index("\nvoid temporal_idle() {", begin)
    body = text[begin:end]
    for need in ("RX_VK_DLSS_PREWARM", "ur::is_dlss(temporal_mode())", "std::thread(", ".detach()", "dlss_probe();", "done.exchange(true)"):
        if need not in body:
            raise SystemExit(f"vk_temporal.cpp dlss_prewarm: {need!r} missing")
    if "std::lock_guard" in body or "st().mu" in body:
        raise SystemExit("vk_temporal.cpp dlss_prewarm: takes the chain's lock (the gameplay frame would wait for NGX's start)")
    idle = text[text.index("\nvoid temporal_idle() {"):][:200]
    if "dlss_prewarm();" not in idle:
        raise SystemExit("vk_temporal.cpp temporal_idle: no longer asks for the prewarm each present")
    probe = (ROOT / "gpu/vk/vk_dlss.cpp").read_text(encoding="utf-8")
    pb = probe[probe.index("void dlss_probe() {"):][:260]
    if "ngx_init();" not in pb or "std::lock_guard lk(s.mu);" not in probe[probe.index("bool ngx_init() {"):][:200]:
        raise SystemExit("vk_dlss.cpp: dlss_probe does not start NGX under the engine's own lock any more")
    doc = (ROOT / "docs/development.md").read_text(encoding="utf-8")
    if "RX_VK_DLSS_PREWARM" not in doc:
        raise SystemExit("docs/development.md does not say RX_VK_DLSS_PREWARM")
    print("DLSS prewarm: NGX started at the first present on its own thread, switch and doc: ok")


if __name__ == "__main__":
    main()
