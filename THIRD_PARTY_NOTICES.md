# Third-party notices

HeyListen is licensed GPL-3.0-or-later (see `LICENSE`). It incorporates the following; each component keeps its own license and notices in the listed location.

| Component | Location | License | Notes |
|---|---|---|---|
| Hookshot (NotRexed/Hookshot), the engine HeyListen forks; itself a fork of mkw-recomp | `engine/` (imported with history via `git subtree`) | GPL-3.0-or-later | `engine/LICENSE` |
| a64-kit (Rexcomp's AArch64 kit, TOTKPort), vendored unmodified | `engine/third_party/a64-kit/` | includes suyu's translator, GPL-2.0-or-later | `NOTICE.TOTKPort.md`, `PROVENANCE.md`, `lift/suyu/` license texts |
| libopus (Xiph.Org) | `engine/third_party/opus/` | BSD-3-Clause | `COPYING` |
| ZBIC / zstd (Atmosphere-NX single-file zstd) for unpacking only | `engine/third_party/zbic/` | see `LICENSE` there | `PROVENANCE.md` |
| SDL2 | `engine/third_party/sdl2/` | zlib | `LICENSE.txt` |
| Kenney Input Prompts 1.5 (kenney.nl), the Xbox, PlayStation and Steam Deck button fonts the builder draws the Button prompts from | `engine/third_party/kenney_input/` | CC0 | `License.txt`, `PROVENANCE.md` |
| Kenney Input Prompts 1.5 Keyboard & Mouse (kenney.nl), the keys the builder draws the Keyboard Button prompts' key icons from (the word keys and the mice are drawn) | `engine/third_party/kenney_input_keyboard/` | CC0 | `License.txt`, `PROVENANCE.md` |
| stb_truetype | `engine/third_party/stb/` | public domain or MIT | `PROVENANCE.md` |
| Eden shader recompiler and related code, sirit, Dear ImGui, suyu decoders | `engine/gpu/vk/eden`, `engine/gpu/vk/sirit`, `engine/gpu/vk/imgui`, `engine/gpu/third_party/suyu` | GPL-3.0 (Eden), GPL-2.0-or-later (suyu), BSD-3-Clause (sirit), MIT (ImGui) | license files in the respective folders |
| AMD FidelityFX SDK v2.4.0, the FSR API headers only (AMD's signed runtime is never bundled: the player's own, beside the game) | `engine/third_party/fidelityfx/` | MIT | `LICENSE.md`, `README.md` |
| PsychoV35, the zelda-oot-oz tone mapper of RenoDX (clshortfuse/renodx, ShortFuse, Carlos Lopez Jr.), ported through Hookshot #2481 and Longshot #400 and #1393 | `engine/gpu/vk/eden/shader_recompiler/ir_opt/psychov35.h` | MIT (RenoDX), GPL-3.0-or-later (the port) | `LICENSE-renodx.md` beside it |
| Unrolled software matrix multiply for the upscale network (sheep, BunnyHood commit 01f9b690d5ef058cae970c8b6ce3ca2970d562f3), on Lichii's loop (zortos293/Lichii); ported from Longshot `4f2da515` | `engine/gpu/vk/eden/shader_recompiler/backend/spirv/mma_software.h`, `engine/gpu/vk/vk_coopmat.cpp` | GPL-3.0 | header of `mma_software.h` |

No game files are included. Anything derived from the user's game dump (modules, lifted C, shader and pipeline caches) is generated locally and ignored by git.
