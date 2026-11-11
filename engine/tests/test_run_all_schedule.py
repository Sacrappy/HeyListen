"""tests/run_all.py's order and its --shard split: the slowest checks start first, and the parts of --shard K/N hold
every check exactly once, the same split in every part's run, whatever tests/run_all_times.json says (stale, missing or
unreadable times only make the parts uneven).

python -I -B tests/test_run_all_schedule.py
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("run_all", ROOT / "tests" / "run_all.py")
run_all = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_all)

NAMES = ["tests/test_%02d.py" % i for i in range(40)] + ["(byte-compile every tracked .py)"]
TIMES = {"tests/test_03.py": 500.0, "tests/test_17.py": 250.0, "tests/test_20.py": 240.0, "tests/test_31.py": 30.0,
         "tests/test_gone.py": 99.0}


class Schedule(unittest.TestCase):
    def test_slowest_first_then_by_name(self):
        order = run_all.by_time(NAMES, TIMES)
        self.assertEqual(order[:4], ["tests/test_03.py", "tests/test_17.py", "tests/test_20.py", "tests/test_31.py"])
        self.assertEqual(order[4:], sorted(n for n in NAMES if n not in TIMES))

    def test_parts_hold_every_check_once(self):
        for times in (TIMES, {}, {n: 5.0 for n in NAMES}):
            for count in (1, 2, 3, 7, len(NAMES) + 3):
                part = run_all.shard_of(NAMES, times, count)
                self.assertEqual(sorted(part), sorted(NAMES))
                self.assertTrue(all(0 <= i < count for i in part.values()))
                # (the same split from any order of the names: each part's run lists them itself)
                self.assertEqual(run_all.shard_of(list(reversed(NAMES)), times, count), part)

    def test_parts_are_balanced(self):
        part = run_all.shard_of(NAMES, TIMES, 2)
        load = [sum(TIMES.get(n, run_all.DEFAULT_SECS) for n in NAMES if part[n] == i) for i in range(2)]
        self.assertEqual(part["tests/test_03.py"], 0)
        self.assertEqual({part["tests/test_17.py"], part["tests/test_20.py"]}, {1})
        self.assertLess(abs(load[0] - load[1]), 30)

    def test_times_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "times.json")
            self.assertEqual(run_all.load_times(path), {})   # (missing)
            with open(path, "w", encoding="utf-8") as f:
                f.write("{not json")
            self.assertEqual(run_all.load_times(path), {})   # (unreadable)
            with open(path, "w", encoding="utf-8") as f:
                json.dump({run_all.OS: {"a": 3, "b": "x", "c": True, "d": 2.5}, "other": {"a": 9}}, f)
            self.assertEqual(run_all.load_times(path), {"a": 3.0, "d": 2.5})

            class Ran:
                def __init__(self, name, secs):
                    self.name, self.secs = name, secs
            run_all.write_times([Ran("z", 12.34), Ran("y", 0.5), Ran("x", 2.0)], path)
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            self.assertEqual(data[run_all.OS], {"x": 2.0, "z": 12.3})   # (under TIMES_MIN left out)
            self.assertEqual(data["other"], {"a": 9})                   # (the other OS kept)

    def test_this_repository(self):
        with open(run_all.TIMES, encoding="utf-8") as f:
            data = json.load(f)
        self.assertLessEqual(set(data), set(run_all.OSES))
        for os_name, times in data.items():
            self.assertTrue(all(isinstance(v, (int, float)) and v >= 0 for v in times.values()), os_name)

    def test_shard_arg(self):
        self.assertEqual(run_all.shard_arg("2/3"), (2, 3))
        for bad in ("0/2", "3/2", "1", "a/b", "1/0"):
            with self.assertRaises(argparse.ArgumentTypeError):
                run_all.shard_arg(bad)


if __name__ == "__main__":
    unittest.main(verbosity=1)
