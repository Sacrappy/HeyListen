# a64-kit (vendored subset)

Copied unmodified from NotRexed's a64-kit (Rexcomp's AArch64 kit, `Porting Tools\Rexcomp\a64-kit`, a link to
`Ports\TOTK\TOTKPort\a64-kit`) at TOTKPort commit `60de030` (2026-10-30). Only the parts the lifter pipeline needs:
`lift/` (lifter, suyu's translator and its ctypes binding), `rt/` (guest model), `tools/nso.py`, `tools/toolchain.py`,
`tools/survey.py`, `hos/nx_image.py`, `tests/insn_test.py`.

It is vendored, not referenced in place, so that building `a64emit.dll` and `lld-link.exe` (both written next to
their sources) never writes into the TOTKPort checkout, and so that this project is pinned to one version.
mkw-recomp's changes live outside this folder (`nx2/`): they wrap these modules instead of editing them, so the
copy can be refreshed from upstream by copying the files again.

Licensing: `lift/suyu/arm64_to_c.h` is suyu's (GPL-2.0-or-later, texts in `lift/suyu/`), so `a64emit.dll`, the
generated runtime and all lifted C are GPL. See `NOTICE.TOTKPort.md`.
