"""Exercise flash detection with authored half-float probe samples."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "probe.py"


class ProbeTests(unittest.TestCase):
    def test_nan_is_not_a_brightness_jump(self):
        with tempfile.TemporaryDirectory(prefix="hookshot-probe-") as folder:
            trace = Path(folder) / "probe_1.txt"
            rows = []
            for frame in range(3):
                # R16_SFLOAT: 0x3c00 is 1.0 and 0x7e00 is NaN.
                nan_sample = 0x7E00 if frame == 1 else 0x3C00
                rows.append(f"{frame} 0 D abcd nan_only 1x1 76 {nan_sample:08x} 0 0 0\n")
                # R16G16_SFLOAT: a NaN in R must not hide a real jump in G.
                jump_sample = 0x4C007E00 if frame == 1 else 0x3C003C00
                rows.append(f"{frame} 1 D abcd real_jump 1x1 83 {jump_sample:08x} 0 0 0\n")
            trace.write_text("".join(rows), encoding="utf-8")
            result = subprocess.run([sys.executable, "-I", "-B", str(SCRIPT), str(trace)],
                                    check=True, capture_output=True, text=True)
        self.assertNotIn("nan_only", result.stdout)
        self.assertIn("real_jump", result.stdout)
        self.assertIn("['nan', '16']", result.stdout)
        self.assertIn("\n1 entries far from both neighbours\n", result.stdout)


if __name__ == "__main__":
    unittest.main()
