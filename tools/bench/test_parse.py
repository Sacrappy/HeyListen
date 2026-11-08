import json
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import run_bench as rb  # noqa: E402

DATA = Path(__file__).parent / "testdata"
SLEEPER = [sys.executable, "-c", "import time; time.sleep(60)"]


def test_percentile_and_stats():
    s = rb.compute_stats([10.0] * 98 + [20.0, 100.0])
    ft = s["frame_time_ms"]
    assert s["frames"] == 100
    assert ft["p50"] == 10.0 and ft["max"] == 100.0
    assert 10.0 < ft["p99"] < 100.0
    assert s["fps"] == pytest.approx(1000.0 / ft["mean"])
    assert set(ft) == {"mean", "p50", "p95", "p99", "max"}


def test_empty_stats():
    assert rb.compute_stats([])["frames"] == 0


def test_hookshot_trace_skips_first_frame():
    rows = rb.parse_hookshot_trace(DATA / "hookshot_present_trace.csv")
    assert len(rows) == 9
    assert rows[0] == (pytest.approx(0.016), 16.0)
    assert rows[-1][1] == 36.0
    assert rb.compute_stats(rb.window(rows, 0))["frame_time_ms"]["max"] == 36.0


def test_hookshot_trace_truncated_line(tmp_path):
    f = tmp_path / "t.csv"
    f.write_text("frame,return_ns,interval_ns\n1,100,0\n2,200000100,200000000\n3,40")
    assert len(rb.parse_hookshot_trace(f)) == 1


def test_missing_files():
    assert rb.parse_hookshot_trace(Path("nope.csv")) == []
    assert rb.parse_presentmon_csv(Path("nope.csv")) == []
    assert rb.parse_hookshot_log(Path("nope.txt")) == []


def test_hookshot_log_windows():
    assert rb.parse_hookshot_log(DATA / "hookshot_log.txt") == [58.3, 12.5, 57.9]


def test_presentmon_v1():
    rows = rb.parse_presentmon_csv(DATA / "presentmon_v1.csv")
    assert [ms for _, ms in rows] == [16.7, 16.7, 33.4, 16.6]
    assert rows[0][0] == 0.0 and rows[2][0] == pytest.approx(0.05)


def test_presentmon_v2():
    rows = rb.parse_presentmon_csv(DATA / "presentmon_v2.csv")
    assert [ms for _, ms in rows] == [16.7, 16.7, 33.4]


def test_warmup_window():
    rows = rb.parse_presentmon_csv(DATA / "presentmon_v1.csv")
    assert rb.window(rows, 0.04) == [33.4, 16.6]


def test_watchdog_fires_and_is_capped():
    assert rb.Watchdog(10_000).hard_limit_s == 120.0
    dog = rb.Watchdog(1.0)
    p = subprocess.Popen(SLEEPER)
    dog.add(p)
    dog.start()
    t0 = time.monotonic()
    while p.poll() is None and time.monotonic() - t0 < 15:
        time.sleep(0.1)
    dog.stop()
    assert p.poll() is not None and dog.fired


def test_watchdog_kills_on_exception():
    p = subprocess.Popen(SLEEPER)
    dog = rb.Watchdog(100)
    dog.add(p)
    with pytest.raises(RuntimeError):
        try:
            dog.start()
            raise RuntimeError("boom")
        finally:
            dog.stop()
    assert p.poll() is not None


def test_lichii_without_presentmon_refuses_before_launch(tmp_path, monkeypatch):
    monkeypatch.setattr(rb, "find_presentmon", lambda _x: None)
    args = rb.build_parser().parse_args(
        ["--exe", str(tmp_path / "lichii.exe"), "--scene", "s", "--out", str(tmp_path / "o")])
    with pytest.raises(SystemExit):
        rb.run(args)
    assert not (tmp_path / "o").exists()


def test_end_to_end_with_fake_hookshot(tmp_path):
    """A fake game writes a present trace; the harness must kill it and write bench.json."""
    game = tmp_path / "fake_game.py"
    game.write_text(
        "import os, time\n"
        "f = open(os.environ['HOS_PRESENT_TRACE'], 'w')\n"
        "f.write('frame,return_ns,interval_ns\\n')\n"
        "t = 0\n"
        "for i in range(1, 100000):\n"
        "    f.write('%d,%d,%d\\n' % (i, t, 0 if i == 1 else 10000000))\n"
        "    f.flush(); t += 10000000; time.sleep(0.01)\n")
    if sys.platform == "win32":
        exe = tmp_path / "hookshot.cmd"
        exe.write_text('@"%s" "%s"\n' % (sys.executable, game))
    else:
        exe = tmp_path / "hookshot.sh"
        exe.write_text('#!/bin/sh\nexec "%s" "%s"\n' % (sys.executable, game))
        exe.chmod(0o755)
    out = tmp_path / "out"
    t0 = time.monotonic()
    rc = rb.main(["--exe", str(exe), "--build", "hookshot", "--scene", "kokiri-idle",
                  "--out", str(out), "--duration", "2"])
    assert rc == 0 and time.monotonic() - t0 < 20
    data = json.loads((out / "bench.json").read_text())
    assert data["schema_version"] == 1 and data["frames"] > 20
    assert data["frame_time_ms"]["p50"] == 10.0 and data["killed_by_harness"]
