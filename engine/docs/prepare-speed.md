# Preparing the shaders: where the time goes (PREP1, 2026-11-18)

A player's builder showed "Prepare the shaders" at 112:39 and 97%, under "Usually about 45 minutes". Nothing of the game is in
this file: counts and times only. Measured on the PC 3050 (RTX 3050, i5-10400F, 6 cores / 12 threads), a cold slot, the builder's
own `--prepare-shaders` run, nothing else running in the slot. The profile is the `prepare profile:` lines the game now logs.

## Where the time goes

Before (main 9a9962d2, 6 workers = half the CPUs), 550 s wall, all six workers busy the whole time, 6.0 of 12 threads in use:

| work | thread time | share |
|---|---:|---:|
| the driver's compile of the compute programs (7,781 pipelines = 3,769 plain + 3,972 texture variants) | 2,594 s | 79% |
| translation (31,985 programs: SPIR-V, zstd packing 32 s, fingerprints 2 s, cache file wait 0.7 s and writes 5 s) | 678 s | 21% |

By kind (11 workers, same data): plain compute pipelines 3,769 x 223 ms = 839 s; **variant** compute pipelines 3,972 x 769 ms =
3,054 s (78% of the driver's time for half of its pipelines); graphics programs 34,748 x 22 ms = 765 s; mesh programs 1,286 x
132 ms = 170 s (the longest 10 s). Lock contention, disk writes, packing and hashing are under 3% of the translation: not worth
a change. The compute programs go first (460 of the 550 s); the graphics programs are the last 90 s.

With the pipeline notes the precompile adds ~300 s on 12 threads (9,995 graphics pipelines, PIPE1's installs); its compute pass is
milliseconds, the pipeline cache having them from the prepare.

**The mesh fallback** (RX_VK_FORCE_MESH_FALLBACK=1) costs the same in the prepare (514 s, translation 677 s: the 1,286 fallback
programs translate in 132 ms each) and its extra is in the precompile, which builds the compute pipelines of all 1,286 fallback
programs (not only those the notes name): ~3 minutes on an RTX 3050, where an AMD compiler took 3 to 12 s on each. A PC with a
slow CPU, few cores (workers were half of them), an AMD or Intel driver (Intel enters its compiler one thread at a time) and no
mesh shaders adds those factors: the 112 minutes need no other explanation.

## What changed (each has a switch; no translated output changes: no stamp or cache moves, nobody re-prepares)

| change | switch | RTX 3050 cold prepare |
|---|---|---:|
| before: 6 workers, every variant, programs bar | | 550 s (fallback 514 s + a 3 min precompile) |
| workers: all CPUs but an eighth (11), behind a commit gate | `RX_VK_PREPARE_WORKERS=<n>` or `old` | 454 s (fallback 454 s) |
| texture variants of compute programs left to their first dispatch (the default; here and in the precompile) | `RX_VK_WARM_VARIANTS=all` | **174 s** (fallback 174 s, whole run 354 s) |
| with a list of the compute programs play dispatched (`hookshot_computes.bin`: kept from a start that played; 726 keys), only those are built | `RX_VK_WARM_COMPUTES=all` | **116 s** |
| precompile builds only the fallback programs the notes name | `RX_VK_PRECOMPILE_FALLBACKS=noted` (off) | not measured |
| driver builds wait for memory (3 GB commit left) when another worker is in the driver | `RX_VK_PREPARE_COMMIT_MB=<mb>` (0: off) | never waited |
| progress by cost, not by programs | none | bar 0 to 100% in step with the time |
| builders: the step's own time left from its progress | none | no more "usually 45 minutes" for this step |
| `prepare profile:` log lines | none | |

The list that makes the last speed-up possible exists after any play (the game writes it) and the build can ship it as it ships the
pipeline state list (hashes of program keys only); without it a first start builds every compute program's plain variant, as before.
