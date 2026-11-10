# TICK-047 — Start the game's long jobs first  · model: Opus · after TICK-045
**Why**: TICK-042: each stage of the BaseProc calc ends with one long actor job running alone. Link's calc (800b3ce0, 1.5-1.7 ms) starts #13-17 of ~45 in its stage (0.27-0.45 ms late); 802ca820 (1.1-1.3 ms) starts #119-124 of 128. Started first, a stage would end 0.2-0.45 ms sooner a frame (up to ~0.8 in some runs). The scheduler is f_8004ab40's lock-free lists. State after TICK-045 (its three FP lift switches and their profile in `profile/`): Market ~78 fps standing, ~73 walking.
**Goal**: find how the game orders the jobs of a stage (actor list order, a priority field, the push order into the lock-free lists) and the safest place to put the known long jobs first (a priority or sort key, a reorder at push time, or a host-side hook in the dispatch), with the jobs moved proven independent of the rest of their stage. Behind an env switch (`HOS_JOB_ORDER=1`), off by default unless it wins. Prefer a patch outside the hot functions, or one that keeps their PGO records (TICK-041/045).
**Acceptance**: the order explained and the patch point; the chain with `HOS_JOBTRACE` + `jobgraph.py` before and after (start positions, time alone, stage lengths); A/B in kokiri-idle, castle-move and hyrule-walk (`--fps-limit 360`, frame generation off, 66 s cut, 6 interleaved runs a variant), adopted only beyond the 2-4 fps spread; HOS_SPEED_LOG ratio ~60, shots alike the base, no gameplay difference; run_all with no failure beyond the four nup tests; saves identical after every run; the game folder left with the adopted build or the restored backup.
**Branch**: `feature/TICK-047-job-order`

## Result (2026-11-11): not adopted, behind two switches

**Order**: no priority field. f_8004fa40 pushes a job into its level's B ring when it becomes ready (level from the job's
int16 at +0xc); executors take level 0 A, 0 B, 1 A, 1 B, 2 A, 2 B, FIFO. So a stage's jobs start in their driver's
actor-list order. Link's calc is a level-0 job pushed ~15th.

**Patch** (`OOT_JOB_ORDER=1` at lift time, then `HOS_JOB_ORDER=1` at run time; `engine/host/hos_joborder.c`,
`engine/nx2/oot_patches.py`): branch-free statements in f_8004ab40 (time each job by its object) and f_8004fa40
(`x0 -= 0x28` before a level's push puts the job in that level's A ring, ahead of the waiting jobs). Only jobs over
300 us, one runner, any executor, A ring under 3/4 full. The PGO records still match. The default lift is unchanged
(lifted code key e8dc207cb2, as TICK-045).

**Chain** (jobgraph): Link's calc #13-17 -> #5 of its stage, alone 711-738 -> 254-287 us; main waiting 0.7-0.8 ms less
in the marked runs.

**A/B** (6 interleaved runs, order reversed in 4-6; fps): kokiri-idle 76.3 -> 77.4, castle-move 71.6 -> 71.8,
hyrule-walk 74.4 -> 73.4. Inside the spread: not adopted.

**Correctness**: HOS_SPEED_LOG 60.00 / 59.99 (A 60.00 / 59.98); shots alike; run_all 225 pass, 4 fail (the nup tests),
33 skip, new `test_job_order_patch.py` passes. Saves identical after every run.

**Game folder**: the TICK-045 build restored (exe 223765ae, dll 86175aee). Work folder: the default lift with `profile/`.

Docs: engine/docs/perf-audit.md "TICK-047", docs/benchmarks/2026-11-10-tick047-job-order.md.
