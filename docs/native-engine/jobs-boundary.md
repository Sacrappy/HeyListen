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

`HL_JOBS` (`engine/host/hl_bridge.c`; only for the build these addresses are of):

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

Not read yet: the job object itself beyond what the dispatcher uses (function, object, queue level), how dependencies
between jobs are stored, and the job group of the module system (`0x8007fe60`) in the same detail as the actors' stages.
