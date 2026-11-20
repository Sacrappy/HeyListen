# Crash reports: names, not offsets

HeyListen's executable is compiled on each player's PC, so `HeyListen.exe+0x12f44593` is a different function in
every build: the same crash on two PCs looked like two unrelated crashes, and no one could say which code was
running. A crash report now names each frame on the player's own PC, at the moment of the crash:

```
CRASH: exception 0xc0000005 (read 0x150, guest 0x150) at HeyListen.exe+0x12f44593 f_80580b38+0x1c4
CRASH: build v0.4.3 (source 0c007049), lifted code 3f1a9c2b04
CRASH: host backtrace: HeyListen.exe+0x12f44593 HeyListen.exe+0x32acd6c ...
CRASH: host frame 0: HeyListen.exe+0x12f44593 f_80580b38+0x1c4
CRASH: host frame 1: HeyListen.exe+0x32acd6c f_8057fd20+0x18dac
CRASH: host frame 2: HeyListen.exe+0x1ad4cf5b hos_job_step+0x2d
CRASH: guest backtrace: lr 0x80580b38 0x8057b1b8 ...
CRASH: guest frame 0: 0x80580b38 f_80580b38+0x1c4
CRASH: guest frame 1: 0x8057b1b8 f_8057af40+0x278
```

`f_<guest address>` is the game's own function (the lift's name, the same in every build); a host function keeps
its C/C++ name (`hos_crash_helper_main`, `hl_trace_gpu_record`, `operator_new`); a system library stays
`module+offset`. The module+offset is still printed first, so older readers keep working.

## Where the names come from

The linker already writes a map beside the executable (`HeyListen.map`; `hookshot.map` in older builds) and
`hookshot_vk.map` for the renderer. The map is 85 MB: too large to ship and too large to read at a crash. The build
therefore compacts it, right after each link, with `nx2/symtab.py` into `HeyListen.sym` and `hookshot_vk.sym`:

- one entry per address: the function's RVA and its name, sorted by RVA;
- the lifted code's `f_<guest address>` names as the map has them, host functions demangled without parameters
  (`llvm-cxxfilt -n`) when the toolchain has it, local labels and `<absolute>` symbols dropped;
- the identity of the binary it was made for (PE TimeDateStamp, SizeOfImage and CheckSum), so a table left beside a
  newer executable is refused rather than naming the wrong functions.

`nx2/build.py` writes the two `.sym` files beside the executable; `nx2/make_game.py` ships them into the game
folder's `bin/` with the exe and DLLs. A game folder that has neither (an update from an older release, or a map
that was not written) is not a failure: the frames stay module+offset, and host frames in the lifted code are still
named from `a64_fn_addr`/`a64_fn_ptr`, the table every executable already carries.

The tables are a Windows build product: the PE linker's map has the symbol form `nx2/symtab.py` reads. The Linux
ELF linker writes another shape, so a Linux build writes no table and its frames stay module+offset (the compiled-in
lifted table still names host frames in lifted code). The `bin/<name>` folder a developer's `build/s5/mkg.ps1`
makes is not installed by the builder and holds no tables (it copies the exe and DLLs only); copy
`HeyListen.sym` and `hookshot_vk.sym` beside the exe when running from one.

## When it is resolved

`hos_crash_record_start` (normal start-up, before the crash handlers are installed) calls
`hos_crash_names_start`, which reads the two tables once with ordinary file I/O and one allocation each. The crash
path (`host/mkw_glue.c report_crash`, `host/hos_crash_record.c`) then only binary-searches those tables and formats
strings:

- no allocation, no lock, no file, no loader call (`tests/test_crash_names.py` counts the allocations and the
  reads around the lookups; the crash path in `report_crash` is checked for `malloc`/`fopen`);
- the identity is checked against the module's mapped PE header, read in place;
- a lookup costs `O(log n)`, and the lifted fallback a linear scan of the compiled-in table, both a few
  milliseconds at most for 20 frames.

The guest backtrace names each return address with `f_<guest address>` from `a64_fn_addr`, which is already in every
build (`hl_host.c`).

## The telemetry report and its signature

`Report automatically` sends the run log's `CRASH:` lines in `crash[]`, so the resolved lines travel unchanged,
and adds explicit fields for the relay:

| Field | Meaning |
|---|---|
| `signature` | **new**: the grouping key. The resolved top host frame (`f_80580b38+0x1c4`, `hos_crash_helper_main+0x10`, `crash_now+0x10`); when nothing resolved, the exception's own `module+offset` as before. |
| `frames` | **new**, when non-empty: every host frame the game resolved (`f_...`, a host function, or a system `module+offset`). |
| `guest_frames` | **new**, when non-empty: every guest return address's function (`f_80580b38+0x0`, ...). |
| `lifted_code` | **new**, when the log has it: the lifted code key (`nx2/build.py`'s `object keys: lifted code <key>`), so a report says which game code ran. |
| `crash[]` | unchanged shape: the log's crash lines, now with the resolved names in them. |
| `log_tail[]` | unchanged. |
| `version`, `release`, `os`, `renderer`, `gpu`, `cpu`, `map`, `options` | unchanged. |

The server should group on `signature` (plus the exception code, which the report's first crash line still holds)
when it is present, and fall back to today's parse of the first `CRASH: exception ... at module+offset` line when it
is not. The old fields stay, so nothing has to change at once. The same crash on a PC whose build laid the code out
differently now lands in one group; the `+0x...` inside a function is the same for two builds of the same sources.

The crash window's manual report ("Report manually") shows the crash record, whose `host_backtrace` and
`guest_backtrace` lines carry the names too, and whose `lifted_code:` line says which game code ran. The record's
`version:` still has the release and its source fingerprint.

## Trying it without sending anything

`HOS_CRASH_TEST=start` (or `<seconds>`) makes the game crash on purpose once the handlers are set up
(`host/hos_crash_dialog.c`); `HOS_CRASH_WATCH=0` starts no crash window. With no address stamped in
`hookshot_version.txt` and `HL_TELEMETRY_URL` unset nothing can be sent; the local record is in
`bug-reports/pending/` and the log in `hookshot_log.txt`.
