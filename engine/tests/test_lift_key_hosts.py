# Copyright (C) 2026 Sacrappy
"""The lift's cache key covers the guest addresses host/hl_*.c gates or hooks (nx2/make_game.py lift_key, lift_host_addresses).

The lift reads them (nx2/guest_locals.py mentioned_addresses: those functions keep the context, so their lifted text differs),
so a gate or hook added or dropped in host/hl_*.c must lift again; an edit that keeps the same addresses (a hook's C, its
recorded hash) must not (docs/research/lift-key-host-files.md). No game: a stand-in exefs folder and the tables mocked.

python -I -B tests/test_lift_key_hosts.py
"""
import os
import sys
import tempfile
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import make_game  # noqa: E402
import hl_gates  # noqa: E402


def check(ok, why):
    if not ok:
        raise SystemExit("FAIL: " + why)
    print("PASS: " + why)


def main():
    check("hl_gates" in make_game.lift_key.__code__.co_names or "lift_host_addresses" in make_game.lift_key.__code__.co_names,
          "lift_key takes host/hl_*.c's addresses")
    real = make_game.lift_host_addresses()
    check(real.startswith("host addresses ") and len(real.split()) > 3, "the real host folder's addresses are read (%d)" % (len(real.split()) - 2))
    with tempfile.TemporaryDirectory(prefix="lift-key-hosts-") as d:
        exefs = Path(d) / "exefs"
        exefs.mkdir()
        (exefs / "main").write_bytes(b"\x00" * 64)
        work = Path(d) / "work"
        (work / "image").mkdir(parents=True)
        env = {"OOT_WORK": str(work)}
        clean = {k: v for k, v in os.environ.items() if not k.startswith(("A64_", "OOT_", "RX_"))}

        def key(gates, steps=None):
            with mock.patch.dict(os.environ, clean, clear=True), \
                    mock.patch.object(hl_gates, "wanted", return_value=gates), \
                    mock.patch.object(hl_gates, "wanted_steps", return_value=steps or {}), \
                    mock.patch.object(hl_gates, "wanted_movies", return_value={}), \
                    mock.patch.object(hl_gates, "wanted_lifts", return_value={}):
                return make_game.lift_key(str(exefs), str(work), env)

        base = key({"8005eca0": "aaaa"})
        check(key({"8005eca0": "aaaa"}) == base, "the same tables: the same key")
        check(key({"8005eca0": "bbbb"}) == base, "a hook's recorded hash changed, same address: the lift stays skipped")
        check(key({"8005eca0": "aaaa", "80706be0": "cccc"}) != base, "a gate added in host/hl_*.c: the lift runs again")
        check(key({}) != base, "a gate dropped: the lift runs again")
        check(key({"8005eca0": "aaaa"}, {"80706be0": ("h", 3, 1)}) != base, "a step hook added: the lift runs again")


if __name__ == "__main__":
    main()
