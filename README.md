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

GPL-3.0-or-later (see [LICENSE](LICENSE)). HeyListen is a fork of [Hookshot](https://github.com/NotRexed/Hookshot) (GPL-3.0), which lives in `engine/` with its history; its lifter is derived from suyu (GPL-2.0-or-later) and its Vulkan back end contains Eden code (GPL-3.0). Third-party sources and their licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). See [docs/adr/0001-license.md](docs/adr/0001-license.md).

### Syncing with upstream

```
git remote add upstream-hookshot https://github.com/NotRexed/Hookshot.git   # once
git fetch upstream-hookshot
git subtree pull --prefix=engine upstream-hookshot main
```

The engine was imported with `git subtree add --prefix=engine E:\OOT\hookshot-fork main` (TICK-005; the fork is itself a clone of NotRexed/Hookshot).
