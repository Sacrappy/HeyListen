"""The Linux builder's details box is read-only, as the Windows builder's: typing, paste, cut and delete leave the build's
lines as printed, and the build still adds to them (issue 1955).

python -I -B tests/test_builder_log_read_only.py
"""
import importlib.util
from pathlib import Path
import tkinter as tk
import unittest

RUN_ALL_MODULES = ["tkinter"]

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("hookshot_builder_read_only", ROOT / "builder" / "linux" / "hookshot_builder.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)


class FakeText:
    """a Text's widget command in a bare Tcl interpreter (no display)"""

    def __init__(self, interp):
        self.tk = interp.tk
        self.tk.eval('set ::text ""; proc .log {op args} {switch -- $op {insert {append ::text [lindex $args 1]} '
                     'delete - replace {set ::text ""} get {return $::text}}}')

    def __str__(self):
        return ".log"

    def bind(self, sequence, func):
        pass


class ReadOnlyTests(unittest.TestCase):
    def test_edits_are_refused_and_the_build_still_writes(self):
        interp = tk.Tcl()
        add = builder.read_only(FakeText(interp))
        add("line one\n")
        for edit in ("insert end typed", "delete 1.0 end", "replace 1.0 end other"):
            interp.eval(".log " + edit)
        add("line two\n")
        self.assertEqual(interp.eval(".log get 1.0 end"), "line one\nline two\n")


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))