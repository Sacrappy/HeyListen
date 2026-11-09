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
                  "--out", str(out), "--duration", "2", "--warmup-s", "0"])
    assert rc == 0 and time.monotonic() - t0 < 20
    data = json.loads((out / "bench.json").read_text())
    assert data["schema_version"] == 3 and data["frames"] > 20
    assert data["fps_1pct_low"] == pytest.approx(100.0) and data["fps_0_1pct_low"] == pytest.approx(100.0)
    assert data["frame_time_ms"]["p50"] == 10.0 and data["killed_by_harness"]
    # The fake game's return_ns is not the perf_counter clock, so the offset falls back
    # to the time its first row was seen: a small positive number, not the trace clock.
    assert data["warmup_origin"] == "launch" and 0.0 <= data["launch_offset_s"] < 10.0
    assert any("trace clock is not the launch clock" in w for w in data["warnings"])


def test_fake_hookshot_repeat_writes_spread(tmp_path):
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
    rc = rb.main(["--exe", str(exe), "--build", "hookshot", "--scene", "kokiri-idle",
                  "--out", str(out), "--duration", "2", "--repeat", "2", "--warmup-s", "0.5"])
    assert rc == 0
    for i in (1, 2):
        data = json.loads((out / f"run{i}" / "bench.json").read_text())
        assert data["frames"] > 5 and data["warmup_s"] == 0.5
    rep = json.loads((out / "repeat.json").read_text())
    assert rep["runs"] and len(rep["runs"]) == 2 and rep["spread_pct"] is not None


# ------------------------------------------------------ TICK-009: warm-up, offset, repeat
def test_warmup_defaults_per_build():
    assert rb.resolve_warmup(None, "hookshot") == 60.0
    assert rb.resolve_warmup(None, "lichii") == 0.0
    assert rb.resolve_warmup(0.0, "hookshot") == 0.0
    assert rb.resolve_warmup(45, "hookshot") == 45.0
    assert rb.build_parser().parse_args(["--exe", "x", "--scene", "s", "--out", "o"]).warmup_s is None


def test_skip_counts_from_launch_when_offset_known():
    # 60 s after launch, first present 12 s after launch -> measure from 48 s after first present
    assert rb.skip_from_first_present(60, "launch", 12.0) == pytest.approx(48.0)
    # a warm-up shorter than the offset discards nothing
    assert rb.skip_from_first_present(5, "launch", 12.0) == 0.0
    # no offset, or the old origin: the warm-up is counted from the first present
    assert rb.skip_from_first_present(45, "launch", None) == 45.0
    assert rb.skip_from_first_present(45, "first_present", 12.0) == 45.0
    assert rb.cut_label(60, "launch", 12.0) == "60 s after launch"
    assert rb.cut_label(45, "first_present", None) == "45 s after first present"
    assert rb.cut_label(0, "launch", 12.0) == "whole capture"


def test_resolve_offset_from_matching_clock(tmp_path):
    trace = tmp_path / "t.csv"
    launch_ns = 5_000_000_000_000
    trace.write_text("frame,return_ns,interval_ns\n1,%d,0\n2,%d,16000000\n"
                     % (launch_ns + 12_500_000_000, launch_ns + 12_516_000_000))
    off, note = rb.resolve_launch_offset(trace, launch_ns, None)
    assert off == pytest.approx(12.5) and note is None


def test_resolve_offset_falls_back_to_first_seen(tmp_path):
    trace = tmp_path / "t.csv"
    trace.write_text("frame,return_ns,interval_ns\n1,100,0\n2,116000000,16000000\n")  # other clock
    launch_ns = 5_000_000_000_000
    off, note = rb.resolve_launch_offset(trace, launch_ns, launch_ns + 7_000_000_000)
    assert off == pytest.approx(7.0) and "first trace row" in note
    off, note = rb.resolve_launch_offset(trace, launch_ns, None)
    assert off is None and "unknown" in note


def test_trace_has_row_and_first_return(tmp_path):
    trace = tmp_path / "t.csv"
    assert not rb.trace_has_row(trace)
    trace.write_text("frame,return_ns,interval_ns\n")
    assert not rb.trace_has_row(trace) and rb.hookshot_first_return_ns(trace) is None
    trace.write_text("frame,return_ns,interval_ns\n1,123456789,0\n2,")
    assert rb.trace_has_row(trace) and rb.hookshot_first_return_ns(trace) == 123456789


def test_fps_spread_limit():
    s = rb.fps_spread([60.0, 62.0, 61.0])
    assert s["spread_pct"] == pytest.approx(100 * 2 / 61) and s["within_limit"]
    s = rb.fps_spread([50.0, 60.0])
    assert s["spread_pct"] == pytest.approx(100 * 10 / 55) and not s["within_limit"]
    s = rb.fps_spread([0.0, 0.0])
    assert s["spread_pct"] is None and not s["within_limit"]
    assert rb.fps_spread([58.0])["spread_pct"] is None


def _synthetic_trace(path: Path, seconds: int = 60, step_ns: int = 10_000_000) -> None:
    """Frames every 10 ms (100 fps) from a first present at a fixed return_ns."""
    base = 1_000_000_000_000
    lines = ["frame,return_ns,interval_ns"]
    n = seconds * 100
    for i in range(1, n + 1):
        lines.append("%d,%d,%d" % (i, base + (i - 1) * step_ns, 0 if i == 1 else step_ns))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_summarize_warmup_from_launch_and_override(tmp_path, capsys):
    import summarize as sm
    d = tmp_path / "hookshot-x"
    d.mkdir()
    _synthetic_trace(d / "present_trace.csv")  # first present at t=0, 60 s of frames
    (d / "bench.json").write_text(json.dumps({
        "build": "hookshot", "scene": "kokiri-walk", "warmup_s": 45.0,
        "warmup_origin": "launch", "launch_offset_s": 5.0}))
    # from launch: 45 s after launch = 40 s after first present -> frames at t>=40 s: 2000
    assert sm.main([str(d)]) == 0
    row = [ln for ln in capsys.readouterr().out.splitlines() if "hookshot-x" in ln][0]
    assert "45 s after launch" in row and "| 2000 |" in row and "**100.0**" in row
    # override: 45 s after first present -> 1500 frames
    assert sm.main(["--warmup-s", "45", "--origin", "first_present", str(d)]) == 0
    row = [ln for ln in capsys.readouterr().out.splitlines() if "hookshot-x" in ln][0]
    assert "45 s after first present" in row and "| 1500 |" in row


def test_summarize_old_layout_without_bench_json(tmp_path, capsys):
    import summarize as sm
    d = tmp_path / "lichii-y"
    d.mkdir()
    (d / "presentmon.csv").write_bytes((DATA / "presentmon_v2.csv").read_bytes())
    assert sm.main([str(d)]) == 0
    out = capsys.readouterr().out
    assert "Lichii" in out and "whole capture" in out and "| 3 |" in out


def test_summarize_rejects_empty_dir(tmp_path):
    import summarize as sm
    d = tmp_path / "empty"
    d.mkdir()
    with pytest.raises(SystemExit):
        sm.summarize_dir(d, None)


# ------------------------------------------------------ TICK-013: low fps and budget shares
def _frames(ms_list: list[float]) -> list[float]:
    return sorted(ms_list)


def test_low_fps_is_mean_of_slowest_share():
    # 1000 frames: 990 at 10 ms, 10 at 50 ms -> slowest 1% (10 frames) mean 50 ms -> 20 fps
    vals = _frames([10.0] * 990 + [50.0] * 10)
    assert rb.low_fps(vals, 0.01) == pytest.approx(20.0)
    # slowest 0.1% = 1 frame (50 ms) -> 20 fps
    assert rb.low_fps(vals, 0.001) == pytest.approx(20.0)
    # 1000 frames, one 100 ms frame among 10 ms: 0.1% low = 10 fps, 1% low (10 frames) mean 19 ms
    vals = _frames([10.0] * 999 + [100.0])
    assert rb.low_fps(vals, 0.001) == pytest.approx(10.0)
    assert rb.low_fps(vals, 0.01) == pytest.approx(1000.0 / ((100.0 + 9 * 10.0) / 10))


def test_low_fps_short_run_uses_at_least_one_frame():
    # 100 frames: 1% = 1 frame, 0.1% = 1 frame (the worst one)
    vals = _frames([10.0] * 99 + [40.0])
    assert rb.low_fps(vals, 0.01) == pytest.approx(25.0)
    assert rb.low_fps(vals, 0.001) == pytest.approx(25.0)
    assert rb.low_fps([], 0.01) == 0.0


def test_low_fps_is_ceiling_of_share():
    # 250 frames: 1% = ceil(2.5) = 3 frames
    vals = _frames([10.0] * 247 + [20.0, 30.0, 40.0])
    assert rb.low_fps(vals, 0.01) == pytest.approx(1000.0 / 30.0)


def test_over_budget_shares_120_and_144_and_60():
    vals = _frames([6.0, 7.0, 8.0, 8.5])
    assert rb.over_budget_pct(vals, rb.FRAME_BUDGET_120HZ_MS) == pytest.approx(25.0)  # only 8.5
    assert rb.over_budget_pct(vals, rb.FRAME_BUDGET_144HZ_MS) == pytest.approx(75.0)  # 7.0, 8.0, 8.5
    assert rb.over_budget_pct(vals, rb.FRAME_BUDGET_60HZ_MS) == 0.0
    # strictly above: a frame exactly on the budget is not over it
    assert rb.over_budget_pct([8.33], 8.33) == 0.0
    assert rb.over_budget_pct([], 8.33) == 0.0


def test_compute_stats_new_keys_on_synthetic_trace():
    # 120 Hz-ish capture: 90 frames at 8.0 ms, 10 frames at 12 ms
    s = rb.compute_stats([8.0] * 90 + [12.0] * 10)
    assert s["frames"] == 100
    assert s["over_8_33_ms_pct"] == 10.0          # the 12 ms frames only
    assert s["over_6_94_ms_pct"] == 100.0        # every frame is above 6.94 ms
    assert s["over_16_7_ms_pct"] == 0.0
    assert s["fps_1pct_low"] == pytest.approx(1000.0 / 12.0)
    assert set(rb.compute_stats([])) >= {"fps_1pct_low", "fps_0_1pct_low", "over_8_33_ms_pct", "over_6_94_ms_pct"}


def _write_trace(path: Path, intervals_ms: list[float]) -> None:
    """Hookshot-format trace: frame 1 (interval 0), then one row per interval in intervals_ms."""
    lines = ["frame,return_ns,interval_ns", "1,1000000000000,0"]
    t = 1_000_000_000_000
    for i, ms in enumerate(intervals_ms, start=2):
        t += int(round(ms * 1e6))
        lines.append("%d,%d,%d" % (i, t, int(round(ms * 1e6))))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def test_summarize_new_columns_on_synthetic_traces(tmp_path, capsys):
    import summarize as sm
    d = tmp_path / "hookshot-120"
    d.mkdir()
    # 100 frames: 99 at 8.0 ms and one at 40 ms (frame 1 has no interval and is skipped)
    _write_trace(d / "present_trace.csv", [8.0] * 98 + [40.0])
    (d / "bench.json").write_text(json.dumps({"build": "hookshot", "scene": "kokiri-idle",
                                              "schema_version": 3, "warmup_s": 0.0}))
    assert sm.main([str(d)]) == 0
    out = capsys.readouterr().out
    header = [ln for ln in out.splitlines() if ln.startswith("| Build")][0]
    for col in ("1% low", "0.1% low", "> 8.33 ms", "> 6.94 ms", "> 16.7 ms"):
        assert col in header
    row = [ln for ln in out.splitlines() if "hookshot-120" in ln][0]
    # 8.0 ms frames are inside the 120 Hz budget but outside the 144 Hz one; the 40 ms frame is over all
    assert "| 1.0% | 100.0% | 1% |" in row
    # 1% low and 0.1% low: the single 40 ms frame -> 25.0 fps
    assert "| 25.0 | 25.0 |" in row
