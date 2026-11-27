<!-- Copyright (C) 2026 Sacrappy -->
# Does the lift's cache key need host/hl_*.c? (written check, 27 Nov 2026, no code change)

## What the key covers today

`engine/nx2/make_game.py` `lift_key()` hashes: the Python version; the *names* of the HLE functions host/*.c defines
(`standalone.implemented()`: `void imp_x(Ctx` and `HLE(x)`); every .py/.c/.h/.cpp/.hpp/.txt/.json file of engine/nx2 and
third_party/a64-kit; the exeFS files; the work folder's hot list and chunk list; the lift's environment (`A64_`, `OOT_`,
`RX_`). With the same key and an untouched gen folder (`gen_print`), the lift and split_hot are skipped.

The content of host/hl_*.c is not hashed, on purpose ("a rebuild after a change to host/ alone skips the minutes of the lift").

## Where host/hl_*.c reaches the lifted C

1. **After the lift, every build: `nx2/hl_gates.py` `apply()`.** It reads the HL_GATE, HL_STEP_HOOK, HL_MOVIE_HOOK and lift-hook
   lines of host/hl_*.c and edits the lifted C: stale marked lines out (`MARK.sub`), wanted ones in, each only where the
   function's text (`text_hash`, marks removed) has the recorded hash. It runs whether the lift ran or was skipped
   (make_game.py, "the entry gates host/hl_*.c asks for ... whether the lift ran or was skipped") and keeps its own stamp
   (`hl_gates.state`: what is asked plus the files' sizes and times). So a change of what host/hl_*.c asks is reconciled
   without the key. Covered.
2. **Inside the lift: `nx2/guest_locals.py` `mentioned_addresses()`.** It calls `hl_gates.wanted()`, `wanted_steps()`,
   `wanted_movies()` and `wanted_lifts()`, so every function host/hl_*.c gates or hooks keeps its registers in the context
   (`keep_mentioned`: no locals). That choice changes the lifted text of those functions, and it is made from host/hl_*.c,
   which the key does not cover. **Not covered.**

## What the gap does

A host/hl_*.c change that adds an address (a new gate or hook) on a PC whose gen folder holds a lift under the same key:
the lift is skipped, the new function stays in the locals form, its recorded hash is the context form's, and hl_gates says
"not found with its recorded hash ... not hooked" (or "NOT gated"). One that drops an address leaves that function in the
context form: still correct code, only slower, until something else re-lifts. Nothing breaks silently (both cases are
logged and the gate or hook stays out), but the feature is missing from that build. The 26-27 Nov report of a new
operator-new hook "not found with its recorded hash" on a contributor's lift is the same family (there the lift's two
forms were the cause, fixed by listing both hashes), and the key gap can produce it on any builder that keeps its work
folder.

## What covering it would take

- In `lift_key()`, hash the addresses the lift reads from host/: `sorted(guest_locals.mentioned_addresses())`, or only the
  four hl_gates tables' address sets (`sorted(set(wanted()) | set(wanted_steps()) | ...)`). Addresses only, not the hashes
  or the C around them: a host edit that does not add or drop an address keeps the lift skipped, as today.
- Cost: about 6 lines in make_game.py, plus a test beside test_exefs_mods.py's key check (a new HL_STEP_HOOK address in a
  stand-in host folder changes the key; an edit of a hook's body does not). hl_gates.py would need its `HOST` to follow the
  folder the key is computed for (it reads `../host` from its own path, which is the same tree in every build).
- Effect on players: the commit itself edits nx2/ (already in the key), so every builder lifts once when it lands, as for any
  nx2 change; afterwards only an added or dropped gate or hook re-lifts.
- Not needed: hashing the whole host/hl_*.c text (it would re-lift for every host edit) or host/*.c beyond the HLE names
  already in the key (nothing else of host/ is read by the lift).
- Risk: low; the failure mode of a wrong key is one extra lift (minutes), never a wrong build.

## Done (27 Nov, OLD1)

`lift_key()` now hashes `lift_host_addresses()`: the sorted guest addresses of the four hl_gates tables (addresses only).
Who lifts again: the commit edits nx2/make_game.py, which the key already covers, so every builder lifts once at its first
build or update with it (as for any nx2 change; this release's nx2/nativefp.py change does the same). After that, a lift is
redone only when host/hl_*.c adds or drops a gated or hooked address, which is exactly when the lifted text must change; an
edit of a hook's C or of its recorded hash keeps the lift skipped. tests/test_lift_key_hosts.py checks both.
