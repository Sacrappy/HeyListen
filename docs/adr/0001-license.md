# ADR 0001 — License

Status: superseded: GPL-3.0 since TICK-005 (originally accepted 2026-11-08). HeyListen forked Hookshot; `LICENSE` is GPL-3.0 and the engine lives in `engine/`.

- Original HeyListen code is MIT.
- Hookshot (NotRexed/Hookshot) and Eden/Lichii are GPL. Reading them for analysis is fine; **copying or porting substantial code** from them makes HeyListen a derivative work.
- Rule: the first PR that imports GPL-derived code must switch `LICENSE` to GPL-3.0-or-later in the same PR, and list the sources in `THIRD_PARTY_NOTICES.md`.
- Practical note: forking Hookshot's lifter (`nx2/lift.py`) and host layer is the fastest path; if chosen, HeyListen is GPL from day one.
