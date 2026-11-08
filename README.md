# HeyListen

Static recompilation of *The Legend of Zelda: Ocarina of Time* (**Nintendo Switch 2** build, title 0400486024DEC000) to native Windows/Linux, with a Vulkan backend. Goal: a locked 60+ fps everywhere (Kokiri Forest, Hyrule Field) with no shader-compile stutter.

This repository contains **no game code, keys, ROM or generated files**. Everything derived from the game is built locally from the user's own copy.

## Targets
| Metric | Hookshot (today) | Lichii (Eden fork) | HeyListen target |
|---|---|---|---|
| Kokiri Forest | 45-55 fps | 60 fps | 60+ locked |
| Hyrule Field | untested | 30-45 fps | 60+ locked |
| First-visit stutter | yes | yes (shader translation) | none (prebuilt pipelines) |

See [docs/BENCHMARK_ANALYSIS.md](docs/BENCHMARK_ANALYSIS.md) and [docs/tickets/](docs/tickets/).

## Layout
- `docs/` — ADRs, benchmarks, ticket specs
- `tools/` — worker (opencode) orchestration, REA/Ghidra scripts
- `references/` — manifest of the comparison repos and builds (kept in place, not committed)

## License
MIT for original code. If substantial GPL-derived code (e.g. from Hookshot or Eden) is integrated, the project moves to GPL-3.0; see [docs/adr/0001-license.md](docs/adr/0001-license.md).
