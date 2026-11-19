"""(HeyListen, STUT1) HL_MEM_COLD backs off under memory pressure (host/hl_mem.c cold_main): a pass of 500 ms or more stops all
trimming for 60 s, and the quiet rule (32 MB came back in a second) holds the pools too, not only the heaps. HL_MEM_COLD_PRESSURE=0
is the old behaviour. Source reading; python -I -B tests/test_cold_pressure.py"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
s = (ROOT / "host/hl_mem.c").read_text(encoding="utf-8")
body = s[s.index("static DWORD WINAPI cold_main"):s.index("void hl_mem_init(void)")]
for need in ("HL_MEM_COLD_PRESSURE", "t2 - t0 >= 500000000LL", "pressure_until = t2 + 60000000000LL",
             "if (pressure_on && hos_now_ns() < pressure_until) continue;",
             "if (pressure_on && quiet_left > 0) hl_mem_cold_pass(HL_MEM_COLD_POOLS);", "else pools_pass();"):
    assert need in body, need
assert "HL_MEM_COLD_PRESSURE=0" in s
print("cold pages back off under pressure: ok")
