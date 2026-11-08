# Third-party notices

HeyListen is licensed GPL-3.0-or-later (see `LICENSE`). It incorporates the following; each component keeps its own license and notices in the listed location.

| Component | Location | License | Notes |
|---|---|---|---|
| Hookshot (NotRexed/Hookshot), the engine HeyListen forks; itself a fork of mkw-recomp | `engine/` (imported with history via `git subtree`) | GPL-3.0-or-later | `engine/LICENSE` |
| a64-kit (Rexcomp's AArch64 kit, TOTKPort), vendored unmodified | `engine/third_party/a64-kit/` | includes suyu's translator, GPL-2.0-or-later | `NOTICE.TOTKPort.md`, `PROVENANCE.md`, `lift/suyu/` license texts |
| libopus (Xiph.Org) | `engine/third_party/opus/` | BSD-3-Clause | `COPYING` |
| ZBIC / zstd (Atmosphere-NX single-file zstd) for unpacking only | `engine/third_party/zbic/` | see `LICENSE` there | `PROVENANCE.md` |
| SDL2 | `engine/third_party/sdl2/` | zlib | `LICENSE.txt` |
| stb_truetype | `engine/third_party/stb/` | public domain or MIT | `PROVENANCE.md` |
| Eden shader recompiler and related code, sirit, Dear ImGui, suyu decoders | `engine/gpu/vk/eden`, `engine/gpu/vk/sirit`, `engine/gpu/vk/imgui`, `engine/gpu/third_party/suyu` | GPL-3.0 (Eden), GPL-2.0-or-later (suyu), BSD-3-Clause (sirit), MIT (ImGui) | license files in the respective folders |

No game files are included. Anything derived from the user's game dump (modules, lifted C, shader and pipeline caches) is generated locally and ignored by git.
