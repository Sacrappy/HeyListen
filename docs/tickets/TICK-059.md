# TICK-059 — Let the CPU's readbacks wait only for what they need, not for the replay's whole lock  · model: Opus · after TICK-058
**Why**: TICK-058 found the main thread's two nvnSyncWait first looks a frame (lr 8042cde0 / 8042ce34, f_8042cd20) waiting 1.4 ms
a frame for the renderer's `sync_order_mu`, which `submit_q` / `present` hold for whole replays on the Presentation Thread.
Skipping it (`RX_VK_FENCE_POLL_FAST=1`) gave +1.5..+3 fps but blew the standing Market out to white: the lock also ordered the
exposure readback after the replay's write-back registrations. Moving a whole-lock wait into every readback with write-backs
(v3) kept the picture but cost 6 fps (castle-move 69.6 -> 63.2).
**Goal**: give the readbacks that order without the replay's whole lock; the correctness argument in the code; shots and speed
before any long benchmark; castle-move screen, then the full confirm only to adopt (bar: above base in at least 15 of 18 rounds);
then TICK-056's `RX_VK_MSYNC_FAST` + `RX_VK_SEG_EARLY` on top.
**Benchmarks**: main PC (7800X3D, RTX 5070), `run_bench.py` 110 s, 66 s cut, `--fps-limit 360`, frame generation off; one binary,
variants by environment.
**Branch**: `feature/TICK-059-readback-order` (from main af1cc93). Local only.

## Result (2026-11-12): the order kept by a ticket, the picture right, +1.4..+1.7 fps on castle-move; not above base in enough rounds: not adopted
**Where the registrations are** (vk_memory.cpp `wb_pages_note`): at a small binding written in place (during the recording, in
`gpu_buffer_bind`), at a submission's write-back (`memory_end_submission`, i.e. `finish_recording`, which with the sync-waits replay
runs at `burst_end`: the very end of `submit_job`) and at a lazy mirror's release (same place). A submission's write set is known
only once its recording ends: no point in the section before its end can say "this replay writes no range the readback reads".
So a readback that must see every registration the locked poll gave it must wait for the end of the section the poll would have
waited for, and no earlier. What can be avoided is waiting for any other section (v3's cost) and waiting at the poll.
**Built** (`RX_VK_READBACK_TICKET`, gpu/vk/vk_replay.cpp, off): `sync_order_mu` is a counted mutex (`entered` after it is taken,
`left` before it is let go). A fast poll (TICK-058's path) notes `entered` as a ticket (a global maximum); `invalidate` waits,
first thing, until `left` reaches the newest ticket (a condition variable; at once when no section is pending). The argument is in
the comment above `invalidate`: every registration is made inside a section; the sections numbered up to the ticket took the lock
before the poll looked, and their registrations are sequenced before their `left` increment, which the wait acquires; the next
section counts itself before it records anything, so none of its registrations is one the poll was ordered after; a newer ticket
only makes a readback wait for a later section, as a later locked poll would have. `=2` waits only for ranges with write-backs (a
diagnostic, not the locked path's order: a range written back for the first time by the pending section would be missed).
Trace marks 36/37 with 1: the ticket's wait. `tests/test_readback_ticket.py` (the counted mutex and the ticket compiled with
stubs: a readback after a ticket sees its section's writes over 60,000 random polls, no wait without one, none for a later section).
**Correctness**: shots (every 300 presents, last 12): standing base 102-106, ticket 102-105, near-white at most 0.08% in both;
walking base 55-122, ticket 63-106 (the walk, same range), no white. Speed (`HOS_SPEED_LOG`, last 40 s): base 59.97 / 59.95 median,
ticket 60.02 / 60.01. The title -> Market load in every ticket run.
**Measured**: the trace (castle-move, 3 s): base, the first looks 0.80 ms a frame on the main thread (mean 0.40, max 7.5);
ticket, first looks 0.012 ms, the readbacks' ticket waits on the main thread 0.97 ms a frame (93 waits, 2.05 ms each) and on
module workers 1-5 0.97 / 0.93 / 0.61 / 0.28 / 0.12 (the jobs the main thread hands out right after its poll read back too). The
main thread waits about as long as before, at its first readback after the poll instead of in the poll: the gain is that
`invalidate`'s wrapper (host/rx_backend.c) gives the virtual core up while it waits, which the first look (nvn_native.c) never
did (core 0, shared with BaseProcExecutor0).
Screen (castle-move x3): base 69.5 69.3 67.4 = 68.7, ticket 70.5 68.6 71.3 = 70.1 (1% low 17.5 -> 38.3). Confirm (rounds 1-3 of
6, stopped when the bar could no longer be met): kokiri-idle 73.9 -> 73.6, castle-move 66.5 -> 67.7, hyrule-walk 63.3 -> 69.3
(base r1 51.3, a stray `find /` from another session using a core); above base in 5 of 9 rounds: at most 14 of 18. Castle-move
over every paired round (7): 67.7 -> 69.4, above in 5.
**Also tried** (`HOS_NVN_POLL_AWAY`, host/nvn_native.c, off): the first look without the virtual core, the locked order kept as is.
Of two castle-move runs one stayed on the title (the save never loaded; cause not found), the other 69.3. Not pursued.
**Decision**: nothing adopted; both switches off; every default as on main. TICK-056's switches on top were not screened (the
ticket did not pass). Game folder: TICK-054's build (exe b6462ab3, dll 07b8dcd6). Saves identical after every run; no game left.
**Not verified**: rounds 4-6 of the confirm; the combo with MSYNC_FAST + SEG_EARLY; `HOS_NVN_POLL_AWAY`'s load failure (once in two).
**Next**: the readback's wait can only end before the section does if a submission's write set is known earlier: (1) take the
capture out of `sync_order_mu` (it opens every `submit_q` section, under `submit_order_mu` it would keep its order), shortening
every section the poll or a ticket waits for; (2) count the registrations per submission and publish "S's write-backs registered"
at `finish_recording`, with `schedule()` / `burst_end` after it shown to register nothing more, so a ticket ends there; (3) the
waiting threads' virtual core: the ticket's gain is the core given up, and the module workers' waits are new ones (a per-thread
ticket handed on with the game's jobs would spare the workers that never follow a poll).
