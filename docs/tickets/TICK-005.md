# TICK-005 — Import the Hookshot fork into HeyListen (engine/)  · model: Sonnet
**Goal**: HeyListen becomes a fork of Hookshot (decision 2026-11-08, ADR 0001 → GPL-3.0). The engine lives in `engine/`, with Hookshot's history.
**How**: `git subtree add --prefix=engine <E:\OOT\hookshot-fork> <its current branch>` (history kept; the fork's uncommitted `scripts/prof.py` change is not imported). Keep NotRexed/Hookshot as remote `upstream-hookshot` in the doc for future `git subtree pull`.
**Also**: LICENSE → GPL-3.0 text (copy Hookshot's LICENSE), README license section, `THIRD_PARTY_NOTICES.md` (Hookshot, its own third_party/), ADR 0001 status "superseded: GPL-3.0 since TICK-005", `.gitignore` covers the engine's build outputs (check `engine/.gitignore` and the dev-build work folders).
**Acceptance**:
- `powershell -File engine/scripts/dev-build.ps1 -Game E:\OOT\hookshot-fork-game -Jobs 14` builds from the new location (same Builder tools under %LOCALAPPDATA%\HookshotBuilder). If the build writes into the game folder, that is expected.
- `engine/scripts/walk-test.sh` runs and reaches gameplay (screenshots). Kill the game after at most 120 s; never two games at once.
- No game data, generated code or caches committed (`git ls-files | grep -i -E "\.(bin|nso|nup)$|generated/"` is empty).
**Branch**: `feature/TICK-005-import-hookshot`
