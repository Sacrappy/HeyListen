# TICK-035 — Headless boot deadlock  · model: Sonnet · later
**Seen** (TICK-031, 2026-11-09): 6 of 7 HOS_HEADLESS=1 runs froze before the first present, with or without RX_VK_SHOTS and with the old texture path too; each log ends with "nn.npln.Worker mutex 0x100ac110c8 ... waiting 10 s; held by nn.npln.Worker (count 2, 1 waiting)". Windowed runs boot 4/4. Earlier today headless runs worked (TICK-027/030), so it may come with the v2.0.10 sync.
**Goal**: find the deadlock (nn.npln worker threads, a lock held across a wait that headless timing exposes) and fix it; headless runs are needed for screenshots in bench runs.
**Branch**: `feature/TICK-035-headless-deadlock`
