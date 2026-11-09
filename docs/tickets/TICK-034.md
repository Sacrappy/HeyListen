# TICK-034 — First public release: HeyListen packaging  · model: Sonnet · when stable
**Goal** (user request, 2026-11-09): once the game is stable, a first release:
- Rename the product to HeyListen (window title, exe names, About, logs, Insert menu header, docs), keeping Hookshot credits and the GPL notices.
- Icon: the user wants Navi. Navi's artwork is Nintendo's; shipping it publicly is an IP risk for the project. Use an original fairy-like icon (a glowing blue orb with wings, own drawing) unless the user decides otherwise.
- A "HeyListen Builder" (fork of engine/builder, Hookshot Builder): the user picks their own .nup, it extracts, lifts, builds and prepares the shaders locally, then launches; nothing from the game is ever shipped.
- Update check: the builder and the game check this repository's GitHub releases (opt-in prompt at first run), offer the update, and rebuild from the kept game files.
- GitHub release: CI build of the builder only (no game data), changelog, checksums.
**Acceptance**: a clean Windows machine with only a .nup can build and play; update prompt works against a test release; no game data in any artifact.
**Branch**: `feature/TICK-034-release`
