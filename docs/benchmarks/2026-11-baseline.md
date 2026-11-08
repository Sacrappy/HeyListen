# Baseline — Hookshot vs Lichii (TICK-004, 2026-11-08)

## Machine and settings
- Ryzen 7 7800X3D, RTX 5070 12 GB, Windows 11 Pro 26200.
- Hookshot: the user's fork build `E:\OOT\hookshot-fork-game` (fork of NotRexed/Hookshot v2.0.0+), window 1280x720, **frame generation off**, `HOS_FPS_LIMIT=120`, audio off. Source: the game's `HOS_PRESENT_TRACE` (present-call return intervals).
- Lichii: `E:\OOT\lichii-windows-x86_64-pgo\lichii.exe`, settings as the user had them. Source: PresentMon 2.6.0 `MsBetweenPresents` (host swap chain), 60 s per scene, the user walking Link.
- Scenes checked from the game's own screenshots for Hookshot; positioned by the user for Lichii.

## Results (gameplay only)

| Build | Scene | fps | p50 | p95 | p99 | max | frames > 16.7 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| Hookshot | Kokiri Forest (scripted walk) | **58.9** | 16.9 ms | 21.0 ms | 26.0 ms | 92 ms | 54% |
| Hookshot | Hyrule Field (scripted walk) | **54.3** | 18.0 ms | 24.8 ms | 30.3 ms | 92 ms | 67% |
| Lichii | Kokiri Forest (manual) | **41.3** | 22.1 ms | 36.8 ms | 49.8 ms | 778 ms | 87% |
| Lichii | Hyrule Field (manual) | **53.6** | 17.0 ms | 23.7 ms | 31.1 ms | 792 ms | 62% |

Hookshot: from 45 s after the first present to the end of a 115 s run. Lichii: the whole 60 s capture.

## Reading
- **Neither build holds 60 fps.** Both sit at 54-59 fps at best, with a p99 around 26-31 ms.
- **Hookshot** loses ~8% from Kokiri to Hyrule Field, as expected from the per-draw replay cost (docs/BENCHMARK_ANALYSIS.md §1). Its worst frames stay under 100 ms once in gameplay; the 0.7-1 s hitches are on scene entry (first run, excluded by the 45 s cut).
- **Lichii** has ~0.8 s hitches *during* gameplay in both scenes (24 frames over 50 ms in Kokiri): shader translation on the GPU thread (§2).
- **Lichii was slower in Kokiri than in Hyrule Field here**, the opposite of the brief (60 Kokiri / 30-45 Hyrule). The manual captures are not the same path or the same moment (Kokiri may include a first visit with shader compiles), so this is not a like-for-like comparison. Re-capture both from a warm shader cache before drawing conclusions on Lichii's scene scaling.
- Earlier figure of ~85-90 fps for Hookshot in Kokiri was **with frame generation on** and is discarded.

## Limits
- One run per scene: the ticket's "two runs within 5%" is not checked yet.
- Hookshot and Lichii are measured with different tools (in-game trace vs PresentMon) and different inputs (script vs manual).
- Raw data (not committed): `build/bench/hookshot-kokiri-2`, `hookshot-hyrule-1`, `lichii-kokiri`, `lichii-hyrule`.

## Update 2026-11-09 — engine = Hookshot v2.0.5 + TICK-006 (same machine, frame generation off, 60 s after launch)

| Replay | Scene | fps | p50 | p95 | p99 | max | frames > 16.7 ms |
|---|---|---:|---:|---:|---:|---:|---:|
| Serial | Kokiri Forest | 54.2 | 18.0 ms | 23.2 ms | 25.8 ms | 264 ms | 63% |
| Parallel (4 threads) | Kokiri Forest | **64.8** | 15.1 ms | 17.9 ms | 19.6 ms | 71 ms | 19% |
| Serial | Hyrule Field | 49.1 | 18.5 ms | 22.7 ms | 65.9 ms | 1290 ms | 80% |
| Parallel (4 threads) | Hyrule Field | **62.1** | 16.5 ms | 17.9 ms | 18.8 ms | 29 ms | 42% |
| Parallel, default build (no env) | Hyrule Field | 59.4 | 17.1 ms | 19.1 ms | 20.0 ms | 70 ms | 52% |

Pictures checked from the game's screenshots in both scenes (fog, exposure, shadows as serial). Parallel recording is now the default (4 threads on 8+ hardware threads; `RX_VK_PARALLEL=0` off). One run each; run-to-run spread not measured yet (the two parallel Hyrule runs differ by ~3 fps).
