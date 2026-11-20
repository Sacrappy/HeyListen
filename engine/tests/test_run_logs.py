"""host/hos_run_logs.h: logs/hookshot.log (this run's log, a hard link to hookshot_log.txt) and hookshot.1.log to .4.log (the runs
before), the folder recomplauncher.json declares as "logs" so RecompLauncher attaches the newest .log to a player's crash report;
and the manifest declares it. Made on Windows and Linux alike (on Linux host/posix/windows.h gives the same file calls, and the game
opens hookshot_log.txt whenever it is started with no terminal, as the launcher starts it); a source check covers the Linux side.
Builds a small program with the compiler on PATH (llvm-mingw's clang on Windows); skipped elsewhere.

python -I -B tests/test_run_logs.py
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ENGINE = Path(__file__).resolve().parents[1]
C = r'''
#include <stdio.h>
#include "hos_run_logs.h"
int main(int argc, char **argv) {
    char log[600];
    snprintf(log, sizeof log, "%shookshot_log.txt", argv[1]);
    FILE *f = fopen(log, "w");
    fprintf(f, "%s\n", argv[2]);
    hos_run_logs_make(argv[1], log);
    fprintf(f, "second line\n");   /* (written after the link: the .log has it) */
    fclose(f);
    return 0;
}
'''


class RunLogsTests(unittest.TestCase):
    def test_the_manifest_declares_the_logs_folder(self):
        doc = json.loads((ENGINE.parent / "recomplauncher.json").read_text(encoding="utf-8"))
        self.assertEqual(doc["folders"]["logs"], [{"path": "{install}/HeyListen/logs"}])

    def test_main_calls_it_after_the_log_is_open(self):
        src = (ENGINE / "host" / "hos_main.c").read_text(encoding="utf-8")
        self.assertLess(src.index('freopen(path, "w", stderr)'), src.index("hos_run_logs_make(gdir, path)"))

    def test_the_linux_build_makes_the_folder_too(self):
        """(a source check: this PC is Windows, and Linux's hard link is made by host/posix) The header had a Linux no-op
        before; now one implementation runs on both, and win32_posix.c gives it CreateHardLinkA."""
        header = (ENGINE / "host" / "hos_run_logs.h").read_text(encoding="utf-8")
        self.assertNotIn("static inline void hos_run_logs_make", header)   # (the no-op branch, removed)
        self.assertIn("#include <windows.h>", header)   # (on Linux, host/posix/windows.h through the include path)
        shim = (ENGINE / "host" / "posix" / "windows.h").read_text(encoding="utf-8")
        self.assertIn("BOOL CreateHardLinkA(LPCSTR link, LPCSTR target, void *attr);", shim)
        impl = (ENGINE / "host" / "posix" / "win32_posix.c").read_text(encoding="utf-8")
        link = impl[impl.index("BOOL CreateHardLinkA("):impl.index("BOOL CopyFileA(")]
        self.assertIn("link(b, a)", link)   # (the link's path first, the file it names second, as on Windows)

    @unittest.skipUnless(os.name == "nt", "Windows only (hard links on NTFS)")
    def test_five_runs_keep_the_last_five_logs(self):
        cc = shutil.which("clang") or shutil.which("gcc")
        if not cc:
            self.skipTest("no C compiler on PATH")
        with tempfile.TemporaryDirectory(dir=os.environ.get("TEMP")) as d:
            d = Path(d)
            src = d / "t.c"
            src.write_text(C, encoding="utf-8")
            exe = d / "t.exe"
            subprocess.run([cc, "-I", str(ENGINE / "host"), str(src), "-o", str(exe)], check=True)
            game = d / "game"
            game.mkdir()
            for run in range(1, 7):
                # (what hos_main.c does before the log opens: the .txt files move up one)
                for k in range(4, 0, -1):
                    a = game / ("hookshot_log.txt" if k == 1 else "hookshot_log.%d.txt" % (k - 1))
                    if a.exists():
                        os.replace(a, game / ("hookshot_log.%d.txt" % k))
                subprocess.run([str(exe), str(game) + "/", "run %d" % run], check=True)
            logs = game / "logs"
            self.assertEqual(sorted(p.name for p in logs.iterdir()),
                             ["hookshot.1.log", "hookshot.2.log", "hookshot.3.log", "hookshot.4.log", "hookshot.log"])
            self.assertEqual((logs / "hookshot.log").read_text(), "run 6\nsecond line\n")
            self.assertEqual((logs / "hookshot.1.log").read_text().split("\n")[0], "run 5")
            self.assertEqual((logs / "hookshot.4.log").read_text().split("\n")[0], "run 2")
            self.assertEqual((game / "hookshot_log.txt").read_text(), "run 6\nsecond line\n")   # (the players' log stays)
            self.assertEqual((game / "hookshot_log.1.txt").read_text().split("\n")[0], "run 5")


if __name__ == "__main__":
    unittest.main()
