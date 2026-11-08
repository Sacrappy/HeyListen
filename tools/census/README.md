# Shader census (TICK-002)

`shader_census.py` lists every NVN shader program stored in a Switch 2 `.nup`
package, to size the offline SPIR-V build. It reads the package in place
(nothing is extracted) and writes only a CSV.

```
python tools/census/shader_census.py <a.nup> [<b.nup> ...] [--csv build/shader_census.csv] [--title N|VERSION|HEXID] [--all-titles]
```

Needs Python 3.10+ and `zstandard` (`python -m pip install --user zstandard`).
Do not run it with `python -I`: that hides user-installed packages.
Default CSV is `out/shader_census.csv`; `build/` and `out/` are gitignored.
Never commit the CSV or any game data.

Output: progress per package, then totals per stage (programs, bytes,
references) on stdout. CSV columns: `sha256, stage, stage_name, size,
control_size, code_size, count, packages, sources`. One row per distinct
stage program (SHA-256 of NVN control block + machine code); `count` is how many
variation slots reference it, `sources` the files holding it.

## Format notes

- The `.nup` is a decrypted dump: no keys or decryption step are needed. Layout:
  `NUP\x01` header (file size at 0x40), `TOC\x01` at 0x80, 0x40-byte title
  records (title id, root/exefs/romfs dir, version), 24-byte dir records,
  80-byte file records (size, offset, stored size, hash, codec byte at +64:
  1 = outer zstd frame), child index list, name strings. A package may hold
  several titles; by default only the newest version of each package is scanned.
- Shader files: `Shader/*.bfsha.zs`, `Shader/*.sarc.zs`, `Layout2Shader/*.bfsha.zs`,
  `Effect/*.vsetb.byml.zs`, `Lib/*/*.sarc.zs`. The game `.zs` layer is zstd, some
  members need the dictionaries in `Pack/ZsDic.pack.zs` (a SARC); SARCs nest.
- Inside: `BNSH` (magic, BOM at +12, `grsc` at +96, size at +28, variation count
  at +124 and table offset at +128, 64-byte variations whose program pointer is
  at +16; up to nine stage slots giving an NVN control block (magic
  `34 12 76 98`, stage u16 at +0x56) and code). `BAHS` archives hold compute-only
  programs found by the control magic.
- Stage ids: 0 vertex, 1 fragment, 2 geometry, 3 tess control, 4 tess eval,
  5 compute, 6 task, 7 mesh (6/7 inferred from the Hookshot fork's use, not from
  documentation).

## Results (title newest-per-package)

Both packages give the same totals (36557 distinct programs, ~514 MB, 257876
references combined): vertex 15219, fragment 16862, compute 3968, task 17, mesh 491.
With `--all-titles` the older base title (v65536) in the second package adds
more programs (61939 distinct in total).
