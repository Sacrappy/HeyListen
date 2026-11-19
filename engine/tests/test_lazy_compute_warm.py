"""(HeyListen, STUT1) The lazy compute start also precompiles the compute programs earlier plays dispatched (hookshot_computes.bin),
off by default (RX_VK_LAZY_COMPUTE_WARM=1 turns it on): no gain measured. Source reading; python -I -B tests/test_lazy_compute_warm.py"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
for need in ('getenv("RX_VK_LAZY_COMPUTE_WARM") && atoi(getenv("RX_VK_LAZY_COMPUTE_WARM")) != 0;', "lazy_compute && !warm ? std::unordered_set<uint64_t>{} : computes_read()",
             "if ((lazy_compute ? warm : !all_computes) && !(which && !strcmp(which, \"none\"))) wanted.insert(used.begin(), used.end());"):
    assert need in s, need
assert "RX_VK_LAZY_COMPUTE_WARM" in (ROOT / "docs/development.md").read_text(encoding="utf-8")
print("lazy compute warm: ok")
