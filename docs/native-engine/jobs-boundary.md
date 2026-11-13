# The job system's boundary

What the gameplay code sees of the game's own job system, in the supported build: the entry points a native job system
has to answer, the structures it has to keep, and the rules it has to hold. Read from the user's local translation during
sprint 5 (`docs/tickets/SPRINT-5.md`, sessions 5 and 6; TICK-039, TICK-042). Addresses are guest addresses; offsets are in
bytes. `engine/native/include/hl/bridge/JobBridge.h` carries the same numbers.

## Threads

| Threads | Count | Role |
|---|---|---|
| main thread | 1 | the frame: runs its own phases, then waits for the frame's job group |
| module workers | 5 | run the module system's jobs; one of their jobs drives the actors' stages |
| executors | 6 | run the actors' jobs of a stage, one executor a core |
| actor create/delete | 6 | make and destroy actors off the frame |

A frame on the test PC (castle scene): the main thread runs about 3.4 ms, waits about 7 ms for the job group, about 1.1 ms
for the last stage. In the job group six stages run on the executors; all six executors are busy together for about a
third of that wait, and the rest is held by the jobs' order (a few long jobs that run alone).

## Entry points

| Address | What it does |
|---|---|
| `0x80b5e9e0` | makes the job system: its schedulers and its worker sets |
| `0x80b5ef60` | makes one scheduler: a list of contexts with room for six, three levels of job queues |
| `0x8069b020` | makes a worker set: one worker thread for each core of a mask (cores 0 to 5) |
| `0x806af9d0`, `0x806afae0`, `0x806afe70` | begin a stage on a set of cores (three forms) |
| `0x806b0260` | registers the stage's workers with the stage's scheduler; may send them their message at once |
| `0x806afc40` | waits for the stage; the owner runs jobs itself meanwhile, with the set's own context |
| `0x83acfb90` | the frame's last stage: each owner thread starts the executor of its own core, waits for it, and the last one out ends the stage |
| `0x8004aae0` | a worker's message handler: message 1 runs the dispatcher, then signals the worker's end |
| `0x80df2680`, `0x80df25c0`, `0x834a2e50` | the job threads' loop: a message becomes a job |
| `0x8004ab40` | the dispatcher: takes jobs from a scheduler's queues, runs them, sleeps when there is none, wakes sleeping contexts when jobs arrive, leaves when the stage is over |
| `0x8004fa40` | pushes a job to a scheduler |
| `0x8007fe60` | runs a job group and waits for it (the main thread's long wait) |
| `0x80706be0` | reads the frame's time step |

## Structures

**Worker set**: `+0` workers (count), `+8` their array, `+16` the set's own dispatch context, `+232` the owner's core,
`+240` pointer to the lane status word, `+248` the stage's cores (mask), `+252` workers registered and not yet done
(atomic), `+256` registered workers (mask by index), `+260` flags (bit 0: the owner takes part with the set's own
context), `+264` the stage's scheduler.

**Worker** (a thread object): `+40` its message queue, `+172` state, `+176` its core (mask), `+216` its dispatch context,
`+488` its "done" event, `+504` its core (index), `+512` the scheduler it serves.

**Dispatch context**: `+8` state (1: asleep), `+16` flags (bit 0: sent its message; bit 1: not to run), `+176` its wake
event, `+216` its lane, `+224` pointer to the lane status word.

**Scheduler**: `+48` contexts registered (count), `+52` room, `+56` their array, `+64` the job queues (three levels, 80
bytes each), `+388` flags (bit 1: the stage is over).

**Lane status word**: one 64-bit word, a 4-bit count of running contexts for each lane in its own byte; lane 0 has none.

**Identity table** (from the actor manager, at `+128`): `+32` the six executors' thread ids, `+56` each one's current
actor, `+104` and `+112` the same for a seventh thread. The manager also has `+1100` its state word and `+1104` a thread
id.

## Rules the gameplay code relies on

1. **Six identities.** Gameplay code asks "which executor am I, and which actor am I running" by comparing the current
   thread's id with the six in the identity table. That lookup is inlined: at 217 places in 152 functions at least. A
   thread that is none of the six is refused the links between actors, and the code does not expect the refusal.
2. **One thread a core at a time.** 193 functions read the current core from a thread-local value and index tables of six
   with it, without a lock. Two threads must never run with the same core number at once.
3. **A stage's protocol.** Begin on a set of cores; register the workers of those cores with the scheduler; each worker,
   once sent its message, runs the dispatcher until the stage is over and signals; the stage's owner waits for every
   registered worker. In the frame's last stage the owner of each core starts its own executor and the count of registered
   workers decides who ends the stage.
4. **Sleeping contexts are woken from the scheduler's list.** A context that is not in the list is never woken.
5. **A job may wait for its own children** (the player's update does): the waiting thread keeps running jobs.

## What is switched on today

`HL_JOBS` (`engine/host/hl_jobs.c`; only for the build these addresses are of):

- `0`: the game's own threads, untouched.
- `1`, through: the worker message function `0x8004aae0` is answered natively. The stage's work for that worker (the
  dispatcher called on the worker's dispatch context) is submitted to the native job system as one guest job under the
  lane of the worker's core, and the worker's own thread runs it while it waits for it.
- `2`, pool: the same job is run by a thread of the native pool. That thread goes on as the worker's guest thread, on the
  worker's own guest context (its registers, its stack from where it stands, its thread-local values, its thread id and
  core), while the worker's host thread stands still in the wait. Nothing is copied: rule 1 and rule 2 hold because the
  lane is held by one host thread at a time.

Checked in the game (castle scene): in pool mode every stage of every job thread ran on a pool worker, hundreds of
thousands of them, with the picture and the game running. What is still the game's own: the dispatcher picks the jobs of
a stage from the scheduler's queues, the stages are begun, registered and waited for by the game's functions, and the
module system's job group and the main loop do not go through the native side.

## What the native job system does with them

- The six identities become six **lanes**. A native worker takes a free lane to run a gameplay job: the bridge binds the
  worker to that executor's guest context (stack, thread-local values, core number, thread id) for the job and unbinds
  it. Rule 1 and rule 2 hold by construction: a lane is held by one worker at a time.
- The stage protocol, the scheduler's list and the sleeping and waking disappear into the native scheduler; the entry
  points that drive them are answered by it. A thread waiting for a job keeps running jobs, under the lane it already
  holds if it is inside a gameplay job (rule 5).
- The bridge keeps the identity table's "current actor" of a lane and the lane status word as the gameplay code reads
  them.

Not read yet: the job group of the module system (`0x8007fe60`) in the same detail as the actors' stages.

## The dispatcher's protocol (`0x8004ab40`, read whole for brick 3.1)

Read from the guest instructions of `0x8004ab40` (5,056 instructions; the lifted text follows them one for one), the push
`0x8004fa40`, the forward `0x810cc640`, the stage wait `0x806afc40` and the waiting helper `0x8012f790`. Of the
dispatcher's 5,056 instructions about 2,000 are the protocol below; the other 3,000 are copies of four job bodies and of
two observer methods that the compiler put inline behind a check that the object's virtual function is that one (a
guarded devirtualisation): calling the virtual function instead does the same thing. `engine/native/src/bridge/Dispatcher.cpp`
is written from this section; `HL_DISPATCH=1` runs it instead of the translated dispatcher (below).

### The objects

**Scheduler** S (the dispatcher's first argument): `+0x30` contexts in its list (int), `+0x38` the list (an array of
context pointers), `+0x40` three levels of 0x50 bytes, `+0x140` the stage's **end job** (a job record embedded in the
scheduler), `+0x180` the **polling lane** (a lane number), `+0x184` flags (bit 1: the stage is over).

**Level** L (at S + 0x40 + L x 0x50): an A ring at `+0x00` and a B ring at `+0x28`. **Ring**: `+0x00` size (int, a power
of two), `+0x08` its cells (0: the ring does not exist and is skipped), `+0x10` head, `+0x18` tail (64-bit counters that
only grow). **Cell** (16 bytes): `+0` the job record's address, `+8` a sequence word.

**Job record**: `+0x00` the job's object; bit 0 set: a tagged object (bit 0 cleared to use it), `+0x0c` the level (16-bit
signed), `+0x0e` the job's kind for the lane status word (0 or not), `+0x10` the lane it must run on (0x21: any), `+0x14` an
identifier for the observer, `+0x18` a hook called before it is pushed (0: none), `+0x20` its list of dependents (a node
of the list sits there: `+0x28` first node, `+0x30` last; a node is `+0` the dependent job, `+8` the next node; the list is
empty when the first node is the head node itself), `+0x38` the **runner word** (16 bits: bits 0-6 runners still to start,
bits 7-13 runners running, bit 14 a runner finished, bit 15 complete when the last running one finishes even with runners
left; bits 14-15 both set: the job is not to be started), `+0x3c` the count of jobs it still waits for.

**Dispatch context** C (the second argument): `+0x08` its **state word** (64-bit: 0 awake, 1 asleep, anything else a job
handed to it), `+0x10` flags (bit 0: sent its message, cleared when it leaves; bit 1: not to run), `+0x18` its own queue of
handed jobs (an array of job addresses), `+0x20` that queue's room, `+0x24` its first index, `+0x28` its count, `+0x70` a
mutex guarding that queue, `+0xb0` its wake event, `+0xc0` an observer (0: none), `+0xc8` what the observer is shown (the
job's object, `+0xd0` the job's identifier, `+0xd4` 1 before the job runs, 0 after), `+0xd8` its lane, `+0xe0` the lane
status word's address.

**Lane status word**: one byte for each lane (lane L at bits 8L..8L+7); its low four bits count the contexts inside a
dispatcher on that lane plus the running jobs of kind 0, its high four bits the running jobs of another kind. Both are
changed by compare-and-swap of the whole word, counting up to 15 and staying there, down to 0 and staying there, and only
when the context's lane is not 0 and the word's address is set.

### A ring (bounded, many producers and many consumers)

The sequence word of the cell a counter value p maps to (p modulo the size) says what may happen to it: p means free for
the push at tail p; p + 1 means it holds the job pushed at p, for the pop at head p; bit 63 set means a pop owns it now.

- **Push** at tail p: read the cell's sequence word (acquire). Equal to p: compare-and-swap the tail from p to p + 1 (on
  failure take the tail it read and start again); then store the job in the cell, store p + 1 in the sequence word
  (release), and notify the word's waiters. Below p: when tail - head has reached the size the ring is full and the push
  **returns without pushing** (the job is not queued anywhere); otherwise a pop of the last lap has not finished: wait for
  the word to change (acquire loads) and start again. Above p: with bit 63 set, a pop is finishing: wait for the word to
  change (plain loads); otherwise the tail read was stale: read it again.
- **Pop** at head p: read the sequence word (acquire). Equal to p + 1: compare-and-swap the head from p to p + 1 (on failure
  take the head it read and start again); then set bit 63 of the word (if it was already set, wait until it is clear and
  set it again), swap the cell's job with 0, store p + size in the sequence word (release; bit 63 goes with it) and notify
  the word's waiters. A popped job of 0 is skipped (pop again). Below p + 1: the ring is empty when its tail equals the
  head read (go on to the next ring); otherwise a push is between its tail swap and its store: wait for the word to change
  (acquire loads), then read the head again. Above p + 1: with bit 63 set, wait for the word to change (plain loads);
  otherwise the head read was stale: read it again.
- **Waiting** for a word to change is the C++ library's atomic wait: about a hundred polls, then polls with a clock until
  4 microseconds have passed, then the library's wait on the address (a table of 256 waiter counts hashed by address, and
  a futex), woken by the notify every push and pop makes. A native waiter must use the same table to be woken by the
  translated code's notifies: the native dispatcher calls the guest's own functions for it (`0x83c61e80`, `0x83c61e70`,
  `0x83c61e50`).
- Every read-modify-write is an atomic instruction (compare-and-swap, swap, add, set or clear of bits); loads marked
  acquire and stores marked release above are such on the guest; everything else is plain. x64's ordering is at least as
  strong for each.

### The push (`0x8004fa40`, scheduler and job)

When the job has a pre-push hook, the hook's function is called on the job's object and its 64-bit answer sets the runner
word (its low 16 bits as the runners, and bit 15 when its high half is 1); when that gives no runner the job is complete
at once (its dependents are made ready, see completion) and nothing is pushed. Then the job goes to a **B ring**: level 2's
when level 2's B ring exists and the job's level is 0, level 0's when level 0's B ring exists and its level is -2, level
1's otherwise. Callers: the dispatcher (dependents made ready), `0x8389af90`, `0x838cf8e0`, and the push itself.

### The dispatcher (scheduler S, context C)

1. **Enter**: the lane status of C's lane goes up by one.
2. **Loop** (with the job J just run, none the first time):
   - **Finish J**: the runner word loses one running runner and gets bit 14. If J was its last running runner and either
     bit 15 was set or no runner is left to start, J is **complete**: each dependent's waiting count goes down by one, and
     a dependent whose count was 1 is pushed (the push above); then the list is emptied. When J is the end job (S + 0x140)
     the **stage is over**: S's flags get bit 1, and every context in S's list whose state is 1 is set to 0, and every
     context's wake event is signalled.
   - **Leave** when C's flags have bit 1 or S's flags have bit 1 (below). (A one-time initialisation of a time constant,
     a static of the function, comes first the first time the dispatcher runs in the process.)
   - **A handed job**: when C's state word holds a job, take C's mutex, put the next job of C's own queue (or 0) in the state
     word, release the mutex, and run the handed job (after waking others: the count is every ring's jobs).
   - **Take a job**: rings in the order level 0 A, 0 B, 1 A, 1 B, 2 A, 2 B, the first job popped. With the job's runner word
     read and, unless both bits 14 and 15 are set, added 0x7f (one more running, one fewer to start) by compare-and-swap:
     - from an **A ring**: both bits set: the job is dropped and the ring popped again. Runners to start (before) 1: run
       it. 0: dropped, the ring popped again. More: push it back at the A ring's tail (a full ring drops the push) and run
       it.
     - from a **B ring**: runners to start (before) 1: when the job's lane is neither "any" (0x21) nor C's lane, it is
       **forwarded** (`0x810cc640`: to the context of S's list with that lane, unless that one is not to run: its state
       word is set to the job when it is 0 or 1, and its wake event signalled when it was 1; when it already holds a job,
       the job goes to the end of its own queue under its mutex, dropped when that queue is full; no such context: dropped),
       and the loop goes on without a job. Otherwise run it. Anything else (and both bits set): push it into the same level's
       A ring (a full ring drops the push) and run it.
     - **Wake others** before running: a count n = the jobs between the ring's head (before this pop) and its tail as read
       just after the pop, plus 1 when the job was pushed back into an A ring, plus the jobs of every ring after this one in
       the order of service (32-bit sums of tail - head). While n > 0, for each context of S's list in order: skipped if
       not to run; if its state is 1, compare-and-swap it to 0, and on success signal its wake event and lower n.
   - **Run**: a job whose object word is 0 is finished at once. Otherwise the lane status goes up by one in its low or high
     four bits (by the job's kind); the observer, if any, is shown the job (`+0xc8`, `+0xd0`, `+0xd4` = 1) and its first
     virtual function called with C + 0xc8; the job's object's third virtual function is called: tagged, with the object
     and the address of three words (S, the job record, 1); untagged, with the object; the observer, if any, is shown the
     end (`+0xd4` = 0) and called again; the lane status goes down by one in the same four bits.
   - **Nothing found**: if S's polling lane is C's lane, sleep about 100 microseconds (a time constant made once) and go
     round. Otherwise C's state is set from 0 to 1 by compare-and-swap (a handed job found there instead is taken as
     above). Then every ring's count of jobs is summed again: if there are jobs, C's state is set back from 1 to 0 (when
     another context has already set it to 0 it will signal: C first consumes that signal by waiting on its event, its lane
     status down by one around the wait); after 9 or more such rounds without a job, C sleeps about 100 microseconds before going
     round. If there are none: the lane status goes down by one, C waits on its wake event, the lane status goes up by one,
     and the loop goes round.
3. **Leave**: a full barrier, C's flags lose bit 0, the lane status of C's lane goes down by one, return.

### What else uses these structures

- **The stage wait** `0x806afc40` (the owner thread): when the set's owner takes part, the workers of the owner's core get
  "not to run", the set's own context gets the owner's core as its lane, and the translated dispatcher is called on it;
  then it waits for each registered worker's done event, its lane status down by one around each wait.
- The translated dispatcher also runs whenever a job body itself calls it (a job that waits for its children).
- Both dispatchers can run at once on one scheduler: every shared word is changed by the atomic operations above, so a
  native and a translated context are each other's peers exactly as two translated ones are.

### The native dispatcher in the game (`HL_DISPATCH`)

`HL_DISPATCH=1` (the default, with `HL_JOBS` on; `0` calls the translated one as before): the job bridge runs a stage's
work with `hl::bridge::Dispatcher` (native) instead of calling `0x8004ab40`. Job bodies, the push's hook, forwarding, events, the mutex, the timed sleep and the atomic
wait's table are the game's own functions, called on the worker's guest context; the rings, the runner words, the counts,
the state words and the lane status word are read and written in place. The owner's path (`0x806afc40`) and nested calls
reach it through the entry gate of the next section (`HL_DISPATCH_ALL`); with it off they run the translated dispatcher
on the same scheduler. The dispatcher leaves to the translated one, as a whole call,
when the function's one-time initialisation has not been done yet (the first call in the process). `HL_DISPATCH_CHECK=1`
counts the invariants listed in `Dispatcher.h` and logs the first violations.

Validated 2026-11-13 (function and picture; no speed measured): 18 runs of 110 s with `HL_DISPATCH=1
HL_DISPATCH_CHECK=1`, 11 on the PC 3050 (the three scenes, castle-move 6 times) and 7 on the main PC (the three scenes
with shots, castle-move 4 more times): every run to the end, game speed ratio 56-61, pictures right, and in every run
zero on each invariant (pops not run, runs not finished, finishes with no runner, waiting counts below zero, one-runner
jobs run twice, stage ends with jobs left or contexts asleep with jobs queued). A main-PC castle run is about 320,000
native dispatches and 6.4-7.5 million jobs run; forwarding to an executor and handed jobs happen a few times a run, the
dispatcher's wait on a ring word 3 to 27 times. The same stage-end checks after the translated dispatcher
(`HL_DISPATCH=0 HL_DISPATCH_CHECK=1`, one castle run) also read zero. `tests/dispatch_test.cpp` runs it with no game: the
order of service, a parallel job's runners, the observer, a full ring, the first-call fallback, and six contexts with
two pushers over 30 stages of 600 jobs (dependencies, parallel and tied jobs, a polling lane).

What the checks do not see: the translated contexts' own pops and runs (the owner's path and nested calls), so "every
job popped is run once" is counted for the native side only; the shared words' conservation (rings empty, no context
left asleep with work, no waiting count below zero) covers both.

### Every caller on the native dispatcher (`HL_DISPATCH_ALL`, brick 3.3)

The dispatcher `0x8004ab40` has three direct callers in the lifted code and no address taken anywhere: the worker message
`0x8004aae0` (answered natively already), the stage wait `0x806afc40` (the stage's owner taking part, on the worker set's
own context) and the frame's last stage `0x83acfb90` (the thread of the set's own lane, on the same context). Every
"nested" dispatch is one of the last two reached from inside a job's body: thirteen functions call the stage wait, most of
them job bodies that begin a stage and wait for it. What differs from a worker's call is only which context: the set's
own context (set + 16) has the same layout as a worker's, its lane is the owner's core (written by the stage wait, or by
the registration), its lane status word is the set's (`+240`), it is in the scheduler's list whenever the owner takes part
(the registration puts it there), and its flags are cleared by the registration (bit 0 is set by the last stage before
its call, and cleared by the dispatcher's leave as for a worker). The native dispatcher needed nothing new for it.

`HL_DISPATCH_ALL=1` puts an entry gate on `0x8004ab40` (`HL_GATE` in `engine/host/hl_jobs.c`; `nx2/hl_gates.py` writes it
into the lifted file at build time; the translated body stays under it). The gate calls `hl::bridge::Dispatcher::gated`
on the calling thread's own guest context. When the native dispatcher has to call the translated one (the first call of
the process, before the guest has made its timed sleep's constant), it sets a per-thread bypass before the call; the
gate takes the bypass and lets that one call into the translated body, so that calls nested inside it are native again.
The bridge's worker path does not go through the gate. With the gate on, every context's pops, runs and finishes are the
native dispatcher's except those of the calls let through, so the counts "pops not run" and "runs not finished" are
complete.

Validated 2026-11-13 (function and picture; no speed measured), `HL_DISPATCH_ALL=1 HL_DISPATCH_CHECK=1`: on the PC 3050
eight runs (castle-move 4 times, kokiri-idle once, hyrule-walk 3 times; shots looked at in one hyrule run), on the main PC
the three scenes with shots (all right against the reference shots) and castle-move 3 more times. Every invariant read
0 in every run (pops not run, runs not finished, bad finishes, bad counts, double runs, stage ends with jobs left or
contexts asleep); game speed ratio 57-61. One PC 3050 hyrule-walk run stayed on a menu screen for the whole run (the
game presenting at its menu's rate, the job system running and its checks clean); the three other hyrule runs reached the
scene. It is not attributed to the gate, which in these scenes only ever let calls through (below), but it is noted.

**What the gate sees in the game**: in every run, the only calls that reached `0x8004ab40` other than through the bridge
were the bridge's own first calls let through (2 to 6 a run, while the first threads to dispatch race to the guest's
one-time initialisation). The stage wait and the last stage never call the dispatcher in the three scenes: none of the
worker sets the game begins stages on has its owner taking part (W's flags bit 0), so the owner only waits for the done
events, and every last-stage thread finds its core's worker. Every pop and run of the scenes is therefore the native
dispatcher's after those first calls, and was already in the first session's runs; the gate makes that hold by
construction, for any set that does have its owner take part. `HL_DISPATCH_ALL=1` is the default.

## The stages' protocol (read whole for brick 3.4)

Read from the guest instructions of the begin forms `0x806af9d0`, `0x806afae0`, `0x806afe70`, `0x83acfa30` and their
shared tail `0x806afa20`, the registration `0x806b0260`, the stage wait `0x806afc40`, the last stage `0x83acfb90` and its
caller `0x83b001c0`, the graph's making `0x806b08a0` and the seed `0x806affc0`, `0x8389aef0` / `0x8389af40` (the seed and
the registration in one), and the module system's twins `0x800711c0` / `0x80070b20`.

### More of the objects

**The current core** is read the way rule 2 says: a thread-local value holding the core plus one (`0x83c61cf0` is the
thread-local read, its key the 32-bit word at `0x84e9f7ac`), and when it is 0 the OS's current core (`0x83c61f60`, used
as it is). Every function below reads it this way (the registration twice, the same value both times).

**A stage's holder** P (the actor manager M has it at `M + 0x80`): `+0x08` the scheduler, `+0x10` the worker set, `+0x68`
who waits for the stage (a field of the current thread's object, stored by the begin forms around the wait, 0 after).
M itself: `+0x44c` a state word whose low byte is 2 while a stage begun by the second and third forms runs, 0 after.

**Scheduler** S, as the stage functions use it: `+0x00` the room for job records, `+0x08` their array (64 bytes each),
`+0x10` the room for dependents' nodes, `+0x18` their array (16 bytes each), `+0x20` the room for roots, `+0x28` the
roots' list, `+0x130` records used,
`+0x134` nodes used, `+0x138` jobs in the graph, `+0x140` the end job, `+0x184` flags: bit 0 the graph is made, bit 1 the
stage is over, bit 2 the owner takes part and polls (see the registration).

**Worker set** W, all of it now: `+0x00` workers (count), `+0x08` their array (each entry points to an object whose
`+0x08` is the worker), `+0x10` its own dispatch context, `+0xe8` that context's lane, `+0xf0` the lane status word's
address, `+0xf8` the stage's cores (a mask of 6 bits), `+0xfc` workers registered and not yet done (atomic), `+0x100`
registered workers (mask by index), `+0x104` flags (bit 0: the owner takes part with the own context; bit 1: the own
context's lane is kept when it already is the owner's core), `+0x108` the stage's scheduler (0: no stage).

**Worker** (as the stage functions use it): `+0x00` its virtual table (`+0x10` its message function), `+0x28` its message
queue, `+0xa0` and `+0xac` its run state (a message is not queued when `+0xac` is 3 or 4, or `+0xa0` is 1),
`+0xb0` its core as a one-bit mask, `+0xd8` its dispatch context (`+0xe0` the context's state word, `+0xe8` its flags,
`+0x188` its wake event), `+0x1e8` its done event, `+0x200` its scheduler.

### The graph and the seed

`0x806b08a0` (P, S) makes a stage's graph on S from a description: every ring emptied (head set to the tail, each cell's
sequence word made free for its next lap), the counts at `+0x130` to `+0x138` set to 0, the end job made (one runner,
nothing waited for, an empty list of dependents, identifier 0x00ff00ff), S's flags set to 0; then one job record for each
job of the description (its object, level, kind, lane, identifier, runner word, no hook, a waiting count of 0 or 1 as the
description marks it, and the end job as its dependent, so the end job waits for every job), then S's flags get bit 0, then the description's edges are
linked (a node in the earlier job's list, one more waiting count on the later one). The guest's observer of the frame is
told the stage's numbers last.

`0x806affc0` (S), the seed: every job of the roots' list that waits for none is pushed into a B ring, as the push
`0x8004fa40` does (the hook first; level 2's ring for level 0, level 0's for level -2, level 1's otherwise; a full ring
skips it).

### The begin forms

All four first set W's stage cores (the call's mask, 6 bits). `0x806afae0`, `0x806afe70` and `0x83acfa30` also set the
low byte of M's state word to 2 first and back to 0 at the end. `0x806af9d0`, `0x806afe70` and `0x83acfa30` make the graph
first (`0x806b08a0`; the second form expects it made). Then, with S the holder's scheduler (or the one passed):

- when S's graph is made and has no job, W's scheduler is set to 0 (the wait then returns at once);
- otherwise W's scheduler is set to S, the seed pushes the roots, and the registration runs: with its message sent
  (`send` 1) when the call waits, followed by the wait `0x806afc40` with P's `+0x68` set around it; or without the message
  (`send` 0) and no wait when the call's last argument says so: the workers are then started by the last stage.

`0x806afa20` is the common tail of the first form; `0x8389aef0` and `0x8389af40` are the seed and the registration with
`send` 1 and 0, for callers that made the graph themselves. `0x83afff70` (read for brick 3.4) is the begin of the frame's
last stage: the stage cores, M's state byte 2, the graph, then the seed and the registration without the message
(through `0x8389af40`, or directly, with W's scheduler set to 0 when the graph has no job); the job `0x83b001c0` then
calls the last stage on every core.

### The registration `0x806b0260` (W, send)

1. S = W's scheduler: S's count of contexts, W's registered mask and W's registered count set to 0.
2. When W has no stage cores or the owner takes part (flags bit 0): the own context's lane is set to the owner's core, its
   state word and flags to 0, its wake event cleared; it is appended to S's list (unless the list is full); S's flags get
   bit 2 when the owner takes part and W's flags bit 1 is clear, else lose it; the registered count goes up by one. When W
   has no stage cores it stops here. Otherwise S's flags lose bit 2.
3. Each worker whose core is in the stage's cores, except the worker of the owner's own core when the owner takes part:
   its context appended to S's list, its bit set in the registered mask, the registered count up by one.
4. Each registered worker, between two full barriers: its context's state word and flags set to 0, its wake event and its
   done event cleared, its scheduler set to S.
5. With `send` (its bit 0): each registered worker's context gets bit 0 of its flags ("sent its message"), and the worker
   is sent message 1 (its message function, `+0x10` of its virtual table, called with (worker, 1, 1); when it is the
   usual one, `0x801c5ea0`, whose address the guest reads from `0x84cab408`, inlined: the message is queued on the
   worker's queue `+0x28` by `0x83c62d20` unless `+0xac` is 3 or 4 or `+0xa0` already equals the message).

The writes of steps 1 to 3 are plain stores but the registered count's increments and the scheduler's flag bit 2, which
are atomic; the mask's bits are set by plain read-modify-write (only the owner writes it).

### The stage wait `0x806afc40` (W)

Returns at once when W has no scheduler. Otherwise, with c the owner's core:

1. When the owner takes part: every worker whose core is c, registered or not, gets bit 1 of its context's flags ("not to
   run"), the own context's lane is set to c (the store is skipped when W's flags have bit 1 and the lane already is c),
   and the dispatcher runs on S with the own context, on the owner's thread.
2. For each registered worker in index order: its done event is waited for; when W has a lane status word and c is not 0,
   the low four bits of lane c's byte go down by one before the wait and up by one after it (staying within 0..15), as the
   dispatcher's own sleep does.

The owner does not wait for the stage's end itself: the workers leave their dispatcher when the end job completes, and
each one's done event is signalled by its message function after it leaves (`0x8004aae0`: the dispatcher, then the event).

**Nothing lowers the registered count of a stage that is waited for**: neither the wait nor the worker message touch W's
`+0xfc` or `+0x100`; both keep the registration's values until the next registration empties them. Only the last stage
lowers the count. (Brick 3.4's brief asked for "the owner's wait returns only when the registered count is back to zero":
that is not the game's protocol; the check counts instead that every registered worker was started and waited for once,
and that the mask and the count are unchanged at the wait's end.) The lane status is changed on the owner's lane c
whether or not the owner is counted in it: an owner that is not (not inside a job of its lane) leaves its lane's count
one higher for each worker waited for, the floor at 0 absorbing the decrement.

### The frame's last stage `0x83acfb90` (M)

Reached from `0x83b001c0`, a job body run on every core: unless conditions of its own skip it, it records its core in a
64-bit word (one bit a core, or plus one once the word's top bit is set) and calls the last stage when the word it wrote
is negative. With W = P's set
and S its scheduler (registered without the message, above), on the calling thread's core c:

- When W has no scheduler: M's state word's low byte set to 0, return.
- When a registered worker of core c has not been sent its message (flags bit 0 clear; W's worker count, registered mask
  and array are read once before the search): its flags get bit 0, it is sent message 1 (as the registration sends
  it), the lane status of W's own lane (not c) goes down by one around the wait for its done event (the word's address
  and the lane read again after the wait, nothing done when either is 0), then W's registered count goes down by one.
- Otherwise, when the owner takes part, the own context has not been sent its message and its lane is c: its flags get
  bit 0 and the dispatcher runs on S with it, on this thread; then the registered count goes down by one.
- Otherwise (another thread already took this core): return.
- The thread that brings the registered count from 1 to 0 sets M's state word's low byte to 0.

### What else shares these objects

- The module system has its own twins: `0x800711c0` (registration) and `0x80070b20` (wait) read the same worker set layout
  and the same worker fields (the done event, the context's flags, the lane status word), and the wait runs the module
  system's job group `0x8007fe60` on the own context instead of `0x8004ab40`. They are called from `0x80706be0`, the frame
  function, and from `0x80070d30`, `0x839fbe40` and `0x83b7b130`.
- The workers' wake events are also signalled by translated code that is not the dispatcher: the forward `0x810cc640`,
  the pushers `0x8389af90` and `0x838cf8e0`, and the module system's job group. The done events are cleared by both
  registrations, waited for by both waits and the last stage, and signalled by the worker message.

### The recommended cut (brick 3.4)

The reading gives three facts that decide the cut:

1. **The handshakes are the guest's events, and other translated code holds them.** A worker's wake event is signalled by
   the forward, the pushers and the module system's group, which stay translated; its done event is cleared, waited for and
   signalled by six functions, two of them the module system's twins. A native sleep or wake that is not the guest's event
   would miss those. So the event objects stay the medium until every function that touches one is native; what can be
   native now is the policy around them (when to wait, on what, with the lane status kept), in one place.
2. **The registration, the wait and the last stage share state that cannot be half-native**: the registered mask and
   count, the "sent its message" bit and the done events are written by the registration and read by the two others (and
   by the worker message, native already), in an order the protocol relies on (bit 0 set before the message, the count
   lowered after each done event). They are answered together or not at all; the begin forms, the graph and the seed
   need not be (they only call them, and write the scheduler before any of them reads it).
3. **The begin forms and the graph are not a hand-off**: they are single-threaded work by the owner before the stage
   starts, with no sleeping or waking; answering them natively gains nothing for the later speed work and widens the
   exactness surface (the graph's records are read by the dispatcher and by gameplay code).

The smallest unit worth answering natively, and exact: **`0x806b0260`, `0x806afc40` and `0x83acfb90` together**, three
`HL_GATE`s behind one switch, beside the worker message the bridge already answers, written as one native class (a
`hl::bridge::Stage`) that keeps W, S and the workers' fields in place exactly as above and owns the policy of the
hand-offs: how a worker is started (the message, or a native job on the worker's lane as `HL_JOBS` already does), how the
owner waits for each registered worker (today the done event, the lane status down and up around it), with counters on
each (messages sent, waits that found the event already set, waits that slept). The module system's twins stay
translated and keep working because nothing they read changes form. Its differential check: the registration's writes
(S's list and count, W's mask and count, the workers' words) are a function of W, S, the workers' cores and the current
core, so a check switch can compute them both ways before any message is sent and compare them; the wait and the last
stage write only flags and counts that the same check can read after each call. A later step takes the twins and the
module system's group `0x8007fe60` (a second dispatcher of the same shape, to be read like `0x8004ab40`); only then can
the events themselves become the native side's own.

### The stages natively (`HL_STAGES`, brick 3.4)

`hl::bridge::Stages` (`engine/native/include/hl/bridge/Stages.h`, `src/bridge/Stages.cpp`) answers the registration
`0x806b0260`, the stage wait `0x806afc40` and the last stage `0x83acfb90` together, through three entry gates
(`HL_GATE` in `engine/host/hl_jobs.c`, switched on together by `HL_STAGES=1` once the job bridge has started). It writes
every word above in place with the guest's operations in the guest's order (plain stores where the guest has plain
stores, the same atomic operations, the full barriers around each worker's reset), calls the guest's own functions for
the thread-local core, the events (clear, wait), the message queue and a worker's message function when it is not the
usual one, and runs the owner's part of a stage (the set's own context, in the wait or in the last stage) with the native
dispatcher. The hand-off policy is three functions with counters: `start` (a registered worker's message),
`await_done` (a wait for a worker's done event, the lane given up around it), `dispatch` (the owner's part). The module
system's twins stay translated and keep working on the same objects.

`HL_STAGES_CHECK=1` counts the protocol's invariants, logged every 10 s with the dispatcher's counters: each registration
is first made by the translated function on the same set (without the message: its gate lets that one call through) and
its writes read back (the scheduler's list, count and flags, the set's mask, count and own context, each worker's
context state, flags and scheduler), then made natively over it and read back again, and the two compared; a set is not
registered again before its stage is over; every registered worker is started once (by the registration's message or by
its core's last stage) and waited for once; after its done event a worker's context no longer says it is inside its
dispatcher; at a wait's end, and when the last stage's count reaches zero, the scheduler's stage is over and (for a wait)
the registered mask and count are still the registration's; the last stage never lowers the count below zero.
Observations beside them: stages of sets registered before the gates were on (`untracked`), and the owner's lane byte
after its wait against before its registration (`lane drift`, which the guest's floor at 0 makes legitimate for an owner
not counted in its lane). `tests/stages_test.cpp` runs it on a model with no game: stages in a row with the message and
the wait (the OS core read when the thread-local one is 0, an owner inside a job of its lane), an owner that takes part
(its own context listed first, its core's worker left out and told not to run), the last stage on six threads at once
with and without the own context, no stage, no stage cores, another message function, a worker not running.
