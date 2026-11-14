# The main loop

The game's main thread from its entry to the frame, read for brick 3.7 from the guest instructions of the functions named
here and from the main thread's sampled stacks (`build/hotfn`, three castle runs), in the supported build. Addresses are
guest addresses; offsets are in bytes. Nothing here is game code: what each function computes, in plain words.
`engine/native/include/hl/bridge/FrameLoop.h` carries the same numbers.

## From the entry to the loop

The main thread (`sead::MainThread`) runs, outermost first: the game's main `0x81222ce0` (reached from the entry through
`0x81222cd0`), which makes the framework object and calls its run function `0x80f48c30`; that one prepares the framework
through its virtual functions and ends in the framework's **main loop** `0x809667b0`. The main loop is two instructions
long: it calls the framework's virtual function at `+0xe0`, the **frame function** (read from the object's virtual table
at every iteration), and goes round. It holds no state and has **no way out**: the game ends by the process ending
around it (the OS layer's exit), never by leaving the loop. The game's framework's frame function is `0x83b6ea90`: a
prepare, then the base framework's frame function `0x8348c680` (a tail call: the sampled stacks show the loop calling
`0x8348c680` directly).

## One iteration: the frame function `0x83b6ea90`, then `0x8348c680`

On the framework object F, in this order:

0. **Prepare** (`0x83b6ea90`). An SDK query on F's handle `+0x1a8` writes F's `+0x22c` and `+0x230`; when it answers
   not 0, `+0x230` becomes 0 and `+0x22c` 60. A second query writes F's `+0x232`; when it answers not 0, `+0x232` becomes
   0. F's virtual function at `+0x128`. A float at `0x84d61fc8` becomes 60 over F's nominal frame rate `+0x2c8`.
   `0x83be0230` on F's `+0x2f8`. Then the base frame function:
1. **Begin** (`0x8348c694`). A global's address is read from `0x84cb4840`. When F's mode bits (`+0x24f`, bits 0-1) are
   not 1 and F's word `+0x50` is 1, that word becomes 2. The global itself is read (kept for the end). When F's `+0x234`
   is 1, two SDK queries are asked on F's handle `+0x1a8`: the first writes F's `+0x233`, a word of its own and F's
   `+0x236`; F's `+0x23c` becomes the smaller of `+0x236` and `+0x238`; the second gives a 16-bit value, stored at
   `+0x23a`, and `+0x23e` becomes that value times F's float `+0x240`, converted toward zero. Otherwise `+0x233` is 0.
2. **Tasks** (`0x8348c724`): `0x802eaf10` on F's `+0x38`.
3. **Four virtual steps** (`0x8348c72c`): F's virtual functions at `+0xf0`, `+0x70`, `+0x120`, `+0xe8`, in that order,
   each read from F's table at its call. In the game (read by the native loop's check): `0x83b6eb40` (the calc step: it
   reaches `0x802eabd0` > `0x802eacc0`, which calls the frame's delegates, among them the **module system's frame**
   `0x80706be0`, below), `0x8348d430`, `0x81207a50`, `0x83b6eb30` (the draw step: `0x802eb020` > `0x802eacc0`).
4. **Present** (`0x8348c76c`): the system tick (`0x83c61fd0`) minus F's frame start `+0x108` is stored at `+0x148`; then
   F's virtual function at `+0x110` (`0x8042cd10` in the game, then `0x8042cd20`), the present, which reaches the GPU's
   present and the display: **the wait for the display is there**, and it is the OS layer's (below). (`0x8042cd20` also
   runs module stages of its own, through `0x80441560` > `0x80441630` > `0x80070d30` and the wait twin.)
5. **End** (`0x8348c78c`). The tick is read. Unless F's mode bits are both set: when the global read at the beginning is
   not 0, the frame was not over F's budget `+0x138` with F's `+0xaa` bit 0 set, and F's `+0x230` bit 0 is clear, a
   function static (`0x84e415f0`, guard `0x84e415f8`) is made if it is not yet (the code checks it twice); then F's frame
   length `+0x100` becomes the tick minus `+0x108`, and `+0x108` the tick (the next frame's start). Last, F's mode request
   `+0x24e`: 1 with mode bit 0 set clears the mode bits; 2 with mode bit 0 clear sets both; otherwise the request is
   cleared and, with mode bit 0 set, mode bit 1 flips.

Read elsewhere by the game: F's `+0x100`, `+0x108`, `+0x148` (its frame timing), `+0x23a` to `+0x23e`, `+0x24e`,
`+0x24f`, `+0x50`. All of them are written by the frame function itself between its calls, which is why a native loop
that owns the order has to write them too, in place, with the same values.

## The module system's frame `0x80706be0`

Reached once a frame from the calc step, through a delegate (`0x80706ba0`, set up by `0x83b797e0` on the module system's
object, which holds the delegate at `+0x258`). On that object M and the graphics module G (M's `+0xa8`, then `+0x20`; the OS layer's "graphics module" is the framework
object F itself, below):

- a setting change between M's `+0xd4e` and G's `+0x230` is applied first (`0x83b6f370`, `0x83b6f390` or `0x83b6f2e0`
  on G's `+0x270`);
- a light event (at `+0x170` of the object `0x84eb1920` holds) is signalled; right after it (`lr 0x80706c88`) **the
  frame's time step is computed**: 60 (30 when G's `+0x24f` bit 0 is set) divided by G's nominal frame rate `+0x2c8`.
  The OS layer's real-time step (TICK-014, `engine/host/nn_os_sync.c`) writes the measured rate into `+0x2c8` at that
  signal, so the step follows the frame rate actually reached;
- the module stages of the frame: four times the graph's making `0x80070ea0` (which ends in the registration twin
  `0x800711c0`) and the wait twin `0x80070b20` on M's worker set `+0x4b8` and group `+0x4c0`, with M's graph
  descriptions `+0x4c8`, `+0x4d0`, `+0x4c8`, `+0x4d8`, under M's flags `+0xd38` (bit 5 while a stage runs);
- M's **frame counter** `+0xd50` is counted up by one (an atomic add; when it was -1 the time accumulated is reset) and
  the step is added to M's double `+0xd58`; the frame-time object (`*0x84c9e890`) gets the step (`0x83b6e300`: its `+16`
  the ratio, `+0` the step after the game's time scales), which every system of the frame then reads;
- a 128-bit mask gathered from a list at M's `+0x458` (32-byte entries) goes to `0x83b89870`, `0x83b899c0`, `0x80877eb0`
  and, under M's mutex `+0xd60`, to a virtual function of another object.

The time step is therefore computed and read inside one of the frame function's steps, by the game's own code, from
the OS layer's rate: it is separate from the loop and stays where it is.

## The display and the pacing

The present step reaches the GPU's present; the OS layer's side is `engine/host/nn_vi.c` (`hos_vi_native_presented`:
the frame rate limit, the next frame's due time with no burst to catch up, the display queue, the real-time step's
rate) and the frame pacer in `engine/host/nvn_native.c`. This engine already owns the pacing there; a native loop that
calls the present step leaves it as it is.

One OS-layer hook is keyed to a return address inside the frame function: the system tick read before the present (`lr
0x8348c770`) tells `engine/host/hos_thread.c` the framework object (`hos_gfx_module`), which the real-time step needs
(its nominal rate `+0x2c8`). A native loop reads that tick from elsewhere, so it tells the OS layer the object itself;
without it the step stays a whole 1/60 s at any rate (seen in a first run: game speed ratio 33 at 33 fps).

## The cut (brick 3.7)

The loop function is cleanly cut: it holds nothing, and the frame function's steps are calls with a few words written
between them. The smallest honest cut in which the native loop owns the order and the iteration:

- an entry gate on the main loop `0x809667b0` hands the main thread to `hl::app::MainLoop` (through
  `hl::bridge::FrameLoop`), whose phase list is data: **prepare, begin, tasks, the four virtual steps, present, end**, the steps
  of the frame function above, each calling the game's own functions on the main thread's guest context, the words
  between them written in place with the guest's values in the guest's order;
- the frame's time step stays the game's (inside the calc step) and the pacing the OS layer's (inside the present step);
  the native loop keeps no time of its own for the game (no cap, its own step unused);
- when F's frame function is `0x8348c680` itself the prepare is left out; when it is neither (another framework class),
  the loop calls it whole, as the game's loop would, and counts it.

What this gives later: a native subsystem can be given its place in the frame as a phase of the native loop, between two
of the game's steps, without a translated caller.

## Frame rates above 60: the time step and what does not follow it

**How the game keeps time.** The game is written for 30 or 60 frames a second with a time step, not for a fixed frame.
The module system's frame (`0x80706be0`) computes each frame a ratio r = 60 (30 when the framework's `+0x24f` bit 0 is
set) over the nominal frame rate (the framework's `+0x2c8`) and hands it to `0x83b6e300`, which fills the **frame-time
object** (`*0x84c9e890`):

| Offset | What (r: 1.0 at 60 fps, 0.5 at 120, 2.0 at 30) |
|---|---|
| `+0x10`, `+0x14` | r and 1/r |
| `+0x18` | r times a constant (`0x83e6066c`): the step in seconds |
| `+0x1c` | the frame rate as an integer: 60 / r rounded to nearest |
| `+0x00`..`+0x0c` | the step after the game's time scales (16 scales at `+0x140`..`+0x1bc`, a global one at `+0x1c0`): step in frames, its inverse, in seconds, frames a second |
| `+0x20`..`+0x11c` | six more such groups, one a category (each of the 4-bit fields of `+0x1c4` picks its scale) |

133 functions read it; by the fields they load, 63 read the step `+0x00` and 61 the category selector `+0x1c4` (they
take their category's step); one reads `+0x0c`, two `+0x10`, none the integer rate `+0x1c` directly.

**How the port runs above 60.** The game sets its nominal rate back to 60 whenever it is above 60. The OS layer's
real-time step (TICK-014, `nn_os_sync.c`, the Insert menu's Game speed "Real time", `game_speed=1`) writes the rate
actually reached into `+0x2c8` just before the step is computed, so at 120 fps r is 0.5. The game therefore runs its
whole simulation once per presented frame with a smaller step: delta time, no interpolation, no frame generation. Every
system that advances by the frame-time object's step keeps real time (the log's "game 1/60 s a second: ratio 60"
line sums r: 60 at any rate). **A system that advances by one per frame (a frame counter, a fixed per-frame constant)
runs at the frame rate instead**: twice as fast at 120.

**The honest fix: a 60 Hz tick for the systems that count frames.** Running the whole simulation at 60 and presenting
each picture twice would keep every system right but show 60 different pictures a second: nothing gained over the cap.
The engine has no interpolation of its own. The fix that keeps 120 pictures a second is per system: each system found
to advance by one per frame gets its per-frame update called by an accumulator of the frame's step r instead of once a
frame: r is added each frame, and the update runs (and 1 is taken off) while the sum is 1 or more. At 120 fps (r = 0.5)
such a system runs every other frame, at 90 two frames in three, at 60 every frame as now, and below 60 (r > 1) twice in
some frames, which also mends their slowness at low frame rates. One accumulator, kept by the native main loop (it owns
the iteration and sees the step each frame), gives every such system the same ticks so that they stay in step with each
other (a cutscene's sequencer and its voice lines). Each system is answered by an entry gate on its update function
that lets the translated update run, or not, by that tick: the game's code is unchanged, its order too.

**Finding the systems that count frames: `HL_STEP_SCAN`.** With `HL_TRACE=2` (the heaps' allocations recorded) and
`HL_STEP_SCAN=<s>`, the recorder copies every live block of 16 KB or less at <s> seconds and compares it
`HL_STEP_SCAN_SECS` (5) seconds later: a 32-bit word that went up by the number of frames presented counts frames, one
that went up by 60 a second counts the game's time; the words are summed by the allocation site of their block (the
log's `hl step scan` lines, and `hl_step_scan.txt`). It needs a frame rate at least 10% away from 60 during the window
(`HL_FPS_LIMIT=120` on a quiet machine, or `HOS_FRAME_SLEEP_MS` to go below). First run, main PC, the market save
(castle-move walk, 26 fps during the window with two other games running, 559,000 blocks): 78 frame-counting words
against 146 time-counting ones. 72 of the 78 are in blocks made at `0x80ad3278` (in `0x80ad3230`, reached through a
pointer) called from `0x8080b9bc` (`0x8080b990`, whose largest caller `0x80803ef0` is the chemistry system's object
set-up: its strings are the `Chm_Object*` parameters): **the chemistry objects count frames**, so anything they time
(burning, spreading, reactions) runs at the frame rate. The rest are single words in rendering objects (models,
procedural eyes). The time-counting words are mostly floats in seconds. That walk had no cutscene, movie, voice line or
boss: the three systems the owner reported are not in this sample, and finding them takes the same scan in those
situations (below).

**What to measure next, and what to play.** The scan in a cutscene with voice, in the Deku Tree boss fight and during
the opening movie (a new game: a copy of the saves in a slot), each with the frame rate away from 60, names their
frame-counting objects by maker; each maker's update function then gets the 60 Hz tick above. Owner's checks once a
system has its tick, at 120 fps against 60: the opening movie (voice lines one after the other, picture to its end), a
cutscene with voice-over (no overlap), the Deku Tree boss (the same pace as at 60).
