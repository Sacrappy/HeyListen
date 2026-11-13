# The streaming boundary: the decode of a resource file

What the game does to turn a compressed resource file into its bytes in memory, in the supported build, and what the
first native streaming brick (`hl::stream`, `engine/host/hl_stream.c`) answers of it. Read from the user's local
translation on 2026-11-13, then measured in the game with the brick's check. Addresses are guest addresses; offsets are in
bytes. `engine/host/hl_stream.c` carries the same numbers. Section 9 of `memory-boundary.md` was the starting point; this
document corrects and completes it.

## 1. Entry points and threads

| Address | What it does |
|---|---|
| `0x80108da0` | **the decode of one file** (slot 2 of the resource decompressor's vtable `0x84c377e8`; no direct caller: reached only through the vtable, so a native answer needs no change to lifted code) |
| `0x80109790` | reads a zstd frame header (zstd's own frame-header reader: content size, window, dictionary id, ...) |
| `0x8010b380` | zstd's multi-frame decoder with a dictionary (decoder context, destination, its capacity, source, its size, a raw dictionary and its size, a DDict); called from `0x80108da0` for a file that fits one read |
| `0x80115bf0` | the chunked case's step: one chunk handed to the decoding thread is decoded (slot 2 of the parallel descriptor's vtable `0x84c377c0`; reached only through it) |
| `0x80115c80` | the chunk decoder behind it: a block-by-block zstd decoder of the game's own over the same decoder context, keeping its progress in the descriptor |
| `0x834ac540` | the decompressor's dictionary by id (a search tree; it assumes the id is there) |
| `0x80108c90` | closes a file opened for the decode (through the file object's own slots) |
| `0x836dea10` | the decompressor's destructor (slot 0); its constructor was not found by address and is not needed |

Threads: the decode runs on the resource system's four loading threads (`ARes Loading 0`..`3`), each with a decompressor
of its own (its own read buffer), and once on the main thread at start (the resource system's own archive). The chunked
case's chunks are decoded on another thread, reached through a callback object in the decompressor (`+1640`): the
decompressor's own `ParallelDecompThread`, made with it by `0x8069a5e0` (section 5).

## 2. The decompressor and the request

The decompressor (first argument):

| Offset | What |
|---|---|
| `+112` | its dictionaries: the root of a search tree by dictionary id; a node is `+0` left, `+8` right, `+24` id (32 bits), `+32` the dictionary (a zstd DDict in guest memory) |
| `+1424` | the chunk: the largest read (32 bits; 902,912 bytes in every decompressor seen) |
| `+1432` | its read buffer (0: a temporary one is allocated from the request's heap and freed after); it holds the decoder's 290,816-byte workspace, then two chunks; the source starts at the buffer + 291,351 rounded down to 32 |
| `+1440` | the chunked case's descriptor, an object of its own (vtable `0x84c377c0`): state byte `+1456`, destination `+1464`, decompressed size `+1472`, workspace `+1480`, source buffers `+1608`, chunk `+1616`, chunk index `+1620`, the chunk's byte count `+1624`, status `+1628` (negative: an error), DDict `+1632`, events `+1544` and `+1592`, the callback `+1640` |

The request (second argument), as `0x80108da0` reads it:

| Offset | What |
|---|---|
| `+16` | the heap to allocate from (0: the calling thread's current heap); for resource files the per-file heap, named by the file's path (`memory-boundary.md`, section 10) |
| `+24` | (32 bits, signed) its sign chooses the allocation's direction (the alignment is passed negated) |
| `+28` | (32 bits, signed) the alignment asked; at least 32, and at least what the allocator kind says |
| `+32` | a destination given (0: allocate one) |
| `+40` | (32 bits) the largest decompressed size accepted (0: any) |
| `+44` | (32 bits) the size granularity: the destination's size is the decompressed size rounded up to it (0: to 32); its low five bits must be clear |
| `+56` | the file object (0: the file is opened by a path the request names, through `0x809d79d0`) |
| `+64` | (32 bits) the largest single read (0: one read of a whole chunk) |
| `+69` | bit 0: some errors leave no result code |
| `+80` | where a result code goes (0: nowhere) |

The other arguments: the third an allocator-kind object (asked for its kind and its alignment), the fourth, fifth and
sixth pointers the results go to: the decompressed size (32 bits), the destination's size (32 bits), and a byte that says
whether the destination was allocated here. The return value is the destination, 0 on an error.

## 3. What the function does (main path)

1. Takes the request's heap (or the calling thread's), checks the size granularity, and builds a file holder on its stack
   (a small object: its class word, the open handle, the owner).
2. Prepares the file object (its slot at `+32`, devirtualized in two known cases: nothing, or forwarding to the object at
   `+96`), opens it (slot `+104`, given the holder and the request): a handle, or code 8.
3. Checks the read buffer (code 10 when it is too small or misaligned), checks the handle (its kind byte `+88` must be 1,
   its slot `+72` must say it is ready, code 9 otherwise) and reads one chunk into the read buffer after the workspace
   (slot `+144`: handle, a count out, the holder, the buffer, the size; with `+64` set, several reads of at most that).
4. Reads the frame header from what was read: the decompressed size (code 4 if the header is bad, code 5 if the size is not
   in it), the dictionary id; looks the dictionary up in the decompressor when the frame names one.
5. Refuses a size over the request's limit (code 2), computes the destination's size, and gets the destination: the one
   given, or an allocation: a function-local static (the allocator kind's id) is set once under a guard, the alignment is
   worked out from the request and the allocator kind, and the request's heap allocates (its slot `+48`, size and
   alignment), or `aligned_alloc` before the heap system exists (code 10 when nothing is given).
6. **If the read filled the whole chunk**, the file is larger than one read: the chunked case (section 5).
7. Otherwise one call of the decoder decodes the whole file into the destination (capacity: the rounded size; source: the
   bytes read; the DDict). On a decoder error the code comes from a table of three codes (`0x83f44d60`; zstd's
   "destination too small" is one, every other error another), and a destination allocated here is freed.
8. Writes the decompressed size and the destination's size to the fourth and fifth arguments' targets, the "allocated" byte
   to the sixth's, frees a temporary read buffer, closes the file, and returns the destination.

Result codes stored at the request's `+80`: 2 (over the limit), 4 (bad header), 5 (no size), 7 (granularity), 8 (not
opened), 9 (not readable or a read failed), 10 (no buffer or no destination), the decoder's table codes, and in the chunked
case 1 and 3 (a decode that did not end) or the table code of the decoding thread's status. With bit 0 of `+69` set,
codes 2, 4, 5, 1 and 3 and the decoder's codes are not stored.

Two error paths of the translated function write the "allocated" byte through a stack slot that is not set yet on that
path (a handle that is not of the right kind or not ready): the byte goes to whatever pointer that slot held. The native
path keeps the same frame at the same address and reads the same slot, so it does the same.

What the caller reads afterwards: the returned destination and the three results. The decoder's context inside the read
buffer (the workspace) is scratch: every decode sets up what it needs before using it (the one-read path writes a dozen
of its fields before the call; the chunk decoder resets it at a frame's start).

## 4. Dictionaries

The game's dictionaries come from `Pack/ZsDic.pack.zs` (loaded at start; it is itself decoded by this function, without a
dictionary). Four were seen, ids 1 to 4, 131,072 bytes each; files use 0 (no dictionary), 1, 2, 3 or 4. The decompressor
holds them as zstd DDicts in guest memory, by id; a DDict's `+8` and `+16` are its dictionary's bytes and their size
(zstd's own layout).

**The native side takes the bytes from the game's DDict** (`guest_ddict` + `host_ddict` in `hl_stream.c`): it looks the
frame's id up in the same tree the translated function uses, checks that the bytes start with zstd's dictionary magic and
carry the same id, and makes a DDict of its own from them once (kept for the process's life). Chosen over reading the
archive from the romfs because these are by construction the bytes the game decodes with, whatever archive, update or
mod they came from, and because nothing has to parse the archive: the same id can never mean other bytes. A frame whose
dictionary is not in the tree fails to decode (the translated function would read through a null node there).

## 5. The chunked case and its hand-off (read whole 2026-11-13, second session)

A file whose compressed size is at least a chunk (902,912 bytes) is decoded while it is read. Everything before the
decision is the main path's: the header, the dictionary, the limit, the destination (section 3, steps 1 to 5); the
chunked case is chosen after the destination is had, when the first read filled the chunk.

**Who takes part.** Each decompressor owns one decoding thread (made with it by `0x8069a5e0`, the `ParallelDecompThread`
of its creator: a message-queue thread, class `0x84c376e0`). The decompressor's `+1640` is a small callback object in
place (vtable `0x84c37578`, `+8` the thread): its slot 0 (`0x801c5e80`) posts the descriptor to the thread's queue
(the thread's slot 2, `0x801c5ea0`). The thread's loop (`0x80df2680`) takes a message and calls its slot 14
(`0x836dda20`), which calls the message's slot 2: for the descriptor, the step `0x80115bf0`. The thread does nothing
else with the descriptor before or after. The descriptor is made in place by `0x8069a8d0` (its events: the one at
`+1544` is made signalled, the one at `+1592` not); the decompressor's destructor (`0x836dea10`) calls the callback's
slot 3 (which does nothing) and finalizes the events. No other code was found that reads the descriptor.

**The descriptor** (offsets in the decompressor; the step sees it at `+1440`, its decoder state at `+1456`):

| Offset | Set by the loading thread | What the step does with it |
|---|---|---|
| `+1456` (8 bits) | 1 | the state: 1 a frame to start (the step reads the frame header, resets the decoder context in the workspace, sets 2), 2 under way, 3 failed, 4 decoded (the input ended at the frame's end) |
| `+1464` | the destination | advanced by every byte written |
| `+1472` (32 bits) | the decompressed size | room left: decreased by every byte written |
| `+1476` (32 bits) | 0 | set to the header's content size at the frame's start, decreased by every byte written; **the step's result** |
| `+1480` | the read buffer (the workspace) | the decoder context |
| `+1488` | the read buffer + `0x271f8` | a staging area for a block split between two chunks |
| `+1496` (32 bits), `+1500` (32 bits) | 128 KB, 0 | the staging area's size and fill |
| `+1504` | 0 | the decoder context, set at the frame's start |
| `+1544`, `+1592` | (light events) | `+1544` signalled when a step ends the frame or fails; `+1592` signalled at the end of every step |
| `+1608` | the source (the first buffer; the second is a chunk after it) | |
| `+1616` (32 bits) | the chunk | |
| `+1620` (32 bits) | 0 | chunks decoded: the step picks the buffer by its parity, then counts the chunk |
| `+1624` (32 bits) | the byte count of the chunk handed over (before each hand-off) | |
| `+1628` (32 bits) | 0 | the step's error, when negative |
| `+1632` | the guest DDict (0: none) | |

**The hand-off.** The loading thread waits on the event `+1544` (the previous frame finished: it is signalled when
idle), clears it, fills the descriptor (above), then for each full chunk: stops if the status is negative, clears
`+1592`, stores the chunk's count, posts the descriptor (the callback), reads the next chunk into the other buffer (the
same read calls and checks as the first read: the handle's kind and readiness each time, one read of a chunk or reads
of at most `+64`), waits on `+1592`. A chunk shorter than the chunk ends the loop; a last non-empty one is handed over
and waited for the same way. Then: a negative status gives the result code of the decoder's error, state 4 is success,
state 2 code 3, anything else code 1; on every failure a destination allocated here is freed and 0 returned. A failed
read in the loop gives code 9 (whatever the quiet bit), frees an allocated destination, and returns without waiting for
the chunk handed over last (the decoding thread may still be writing into the freed destination: the game's own
behaviour, never seen).

**The step** (`0x80115bf0`): when the status is not negative, decodes the chunk (`0x80115c80` on `+1456`, the source
chosen by the index, the count, the DDict), counts it, and with the result r (the frame's bytes still to come, `+1476`):
r < 0 stores the status and signals both events, r = 0 (the frame ended) signals both, r > 0 signals `+1592` only. The
chunk decoder decodes block by block with the game's own decoder over the context in the workspace; a block split
between two chunks waits in the staging area; once the frame has ended a further chunk is an error (-3); what follows
the frame's end in the chunk that ended it is ignored.

**What later code reads.** The loading thread reads the state byte and the status after the last wait; nothing reads the
other fields after a decode (the next chunked decode sets every one of them before use, and the step's frame start resets
the decoder context and the fields it reads). The two events stay as the last decode left them: `+1544` signalled,
`+1592` signalled.

**What the native path does** (`HL_STREAM=1`, `decode_chunked` in `hl_stream.c`): the same function, on the loading
thread, with the step's decode done in place instead of on the decoding thread. It waits on and clears `+1544`, fills
every field the translated function fills with the same values, then for each chunk clears `+1592`, stores the count,
decodes the chunk from the buffer its index chooses with zstd's streaming decoder (`ZSTD_decompressStream`, the
destination as a stable output buffer: no window copy whatever the frame's window, any window accepted as the game's
decoder accepts it, a context of its own per thread, the game's dictionary bytes as for one-read files), counts it,
signals as the step signals (`+1544` once the frame's bytes are all written, `+1592` each time), reads the next chunk
into the other buffer with the same calls, and waits on `+1592` (already signalled: it returns at once). After the
frame, the descriptor is left as the step leaves it (state 4, the destination advanced by the size, room and bytes to
come 0, the decoder context set). The decoding thread is never given the descriptor: it only runs the step for a
descriptor it is given, so nothing of it is missing. What differs: the decoder context in the workspace and the staging
area are not written (scratch: the next frame start resets them), the staging area's fill (`+1500`) stays 0, and the
reads and the decode alternate on one thread instead of overlapping on two.

If zstd fails, a chunk comes after the frame's end, or the input ends before it (never seen; the game's decoder gives an
error code there), the native path signals `+1544`, frees a destination it allocated, closes the file and gives the
request whole to the translated function, which opens and reads the file again and reproduces the game's own result;
counted as `chunked-translated`.

## 6. The other callers of the decoder (not this brick)

- `0x80c17040` (through a vtable made by `0x80d331e0`): decodes a block embedded in data already in memory (magic `TARB`)
  into a work buffer of its caller: not resource storage.
- `0x80f700d0`: the ROM cache and the heap size table, at start.

## 7. `hl::stream` (2026-11-13)

| Piece | Where |
|---|---|
| The path a request takes, decided before anything is done for it (`hl::stream::choose`), the counters | `engine/native/include/hl/stream/Decode.h`, `src/stream/Decode.cpp` |
| The provenance table: destination range -> file name (the per-file heap's), file object, compressed size and decompressed size, dictionary id, chunked or not, a fingerprint of the decoded bytes; lookup by address; a new decode drops the ranges it overlaps; at most 65,536 ranges, oldest dropped | `engine/native/include/hl/stream/Provenance.h`, `src/stream/Provenance.cpp` |
| The measurement of brick 6.3: a hash of every whole 4 KB page of every decode, checked again at 30 s and 60 s, by file kind (section 9) | `engine/native/include/hl/stream/Pages.h`, `src/stream/Pages.cpp` |
| The C surface | `engine/native/include/hl/c/hl_stream.h`, `src/c/hl_stream.cpp` |
| The answer of `0x80108da0` (and of `0x80115bf0` for the check), the native function and its chunked case, the dictionaries, the zstd calls, the pages' thread and the restore test | `engine/host/hl_stream.c` |
| zstd's decoder in the game's executable (the vendored `third_party/zstd`, decoder only) | `engine/nx2/build.py` |
| Tests without the game | `engine/native/tests/stream_test.cpp` |

Switches: `HL_STREAM=1` the native function, the chunked case included (the default; 0 the translated function, as
before), `HL_STREAM_CHECK=1` the check (the translated function answers, then the same compressed bytes are decoded
natively and compared: a chunked file by the native path's streaming decode fed the same chunks), `HL_STREAM_CHECK=2`
the native function answers and each chunked file it decoded is decoded again in one call from the bytes it read and
compared, `HL_STREAM_LOG=1` the counters every 10 s, `HL_STREAM_PAGES=1` the measurement of section 9 (off by default).

The native path takes a request when it has a file object of the usual kind, a heap, a granularity that is a multiple of
32, and a decompressor with its own usable read buffer: every request seen in the three scenes. Everything the function
does on that path goes through the game's own objects (the file object's slots, the heap's allocation, the guard, the
close, the descriptor's events) on the calling thread's guest context, in the translated function's order, with its
stack frame at the same address; only the header read and the decode are native (zstd 1.5.7). What is not reproduced:
the decoder context's fields in the read buffer and the chunked case's staging area (scratch, sections 3 and 5), and the
overlap of reads and decoding on two threads in the chunked case.

The provenance table sees no free: a range that was freed and given to another decode is replaced when that decode
records; until then a lookup returns the old entry, whose fingerprint (the decoded bytes' first and last 4 KB) tells the
user whether the bytes are still the decode's.

## 8. Measurements

### The check (`HL_STREAM_CHECK=1`): the translated decode, then the same bytes natively, byte for byte

PC 3050, builds `s6-a`, one run a scene (110 s); the counts are the last 10-second line of each run.

| Scene | Files checked | Of them chunked | MB decoded and compared | Mismatches | Not checked |
|---|---:|---:|---:|---:|---:|
| castle-move | 8,525 | 451 | 4,287 | **0** | 0 |
| kokiri-idle | 7,655 | 371 | 3,956 | **0** | 0 |
| hyrule-walk | 7,793 | 371 | 3,969 | **0** | 0 |

One file (`Tex/CmnTex_Map_Soil_FairyFountain_Floor_D_Nrm.bntx`) ends on the bad-header path (code 4) every time it is
asked, in the translated function as in the native one: the game's own behaviour, not counted as a mismatch.

### Share of the native path

In every scene about 95% of the files and about 40% of the decompressed bytes are one-read files (native); the chunked
files are about 5% of the files and 60% of the bytes (castle-move with `HL_STREAM=1`: 9,685 files and 2,206 MB native,
554 files and 3,333 MB chunked, translated).

### Validation (main PC, `valid.sh s6v1 s6-v1 HL_STREAM=1 HL_STREAM_LOG=1`, 2026-11-13)

| Scene | Ran | Game speed ratio | Files (native / chunked) | MB decompressed (native / chunked) | Game RAM | Process RAM | Commit | VRAM |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| kokiri-idle | to the end, no crash | 59.95 | 9,407 / 482 | 2,133 / 3,112 | 5,158 | 9,686 | 19,225 | 5,979 |
| castle-move | to the end, no crash | 61.13 | 9,488 / 584 | 2,149 / 4,206 | 5,260 | 9,921 | 20,079 | 6,857 |
| hyrule-walk | to the end, no crash | 60.42 | 9,526 / 619 | 2,158 / 4,567 | 5,289 | 10,100 | 20,205 | 6,648 |

(MB; the counts are the last 10-second line of each run.) The last shots of each scene were looked at and are right
(kokiri-idle's matches its reference shot; the two walking scenes show the market with its geometry and textures, nothing
black, missing or tinted). Every error path logged was the texture file of section 8 (code 4), as in the translated
function; the runs log the first 12 only, and the count grows with time in the scene (55 to 195), so the later ones are
not named. With this the default is on (`HL_STREAM=1`).

### Not verified

- The rare paths given to the translated function whole (no file object, a temporary read buffer, no heap, a bad
  granularity) were never taken in the three scenes; the native error paths other than code 4 were never taken either:
  they are written from the reading, not seen working.
- The chunked case is translated, after one extra open, read and close of the file (section 5); that this extra read is
  invisible to the game is reasoned, not shown beyond the runs being right.
- The provenance table is recorded only: nothing reads it, so its use (decommit and restore) is not tested in the game.
- Other scenes, a scene change, and a save loaded elsewhere than the castle town market.

## 9. A decoded resource's pages given back for good (brick 6.3)

### Step 1: how much is eligible (`HL_STREAM_PAGES=1`)

A page can be decommitted and made again by decoding the file again only while its bytes are exactly what the decoder
wrote. The unit is the 4 KB page (what Windows decommits); only the whole pages inside a decoded range count (the two
end pages are shared with the heap's other data). With the switch, every decode's whole pages are hashed when it is
recorded (`hl::stream::page_hash`: four multiply-xor lanes over 8-byte words, a bijection per word, so one changed word
always changes the hash), and a thread of the OS layer hashes them again when the range is 30 s old and 60 s old
(`hl::stream::Pages`), through `VirtualQuery` first (a page of a freed region is counted as gone, not read: reading it
would commit a zero megabyte through the commit-on-touch handler). A range given to a new decode before its second check
is counted as replaced. The table holds at most 8 Mi page hashes (64 MB). Reading the pages brings back those the
cold-pages thread had taken out (`memory-boundary.md` section 12): the measurement runs are measurement only.

EligibilityTablePlaceholder

### Step 2: the restore path

The provenance table gives, for a range, the name of the file (its per-file heap's), its dictionary id and its sizes. A
name maps to the RomFS file `<RomFS>/<name>.zs` (the mods' file first when a mod has one, as `nn_fs.c` resolves the
game's own opens); the game's file object may be gone by then and is not needed. At its 60 s check a range is read
whole from that file through the host's own file access, decoded in one call with the dictionary of its id, and
compared: every page with the hash taken at decode time, and byte for byte with memory for the pages that still match
it (a page that differs is hashed again: written since the check, it is counted apart, not as a mismatch). At most 64
MB of ranges a second are tested, so the thread keeps up with its checks.

RestoreTablePlaceholder

### Step 3: decommit and restore on touch: not built, and why

What it needs to coordinate, found by reading the OS layer:

- **Every commit over guest memory would make a dropped page a zero page.** The heap and the address regions are
  committed lazily (`mkw_glue.c` `commit_on_touch`: a megabyte at a time on any touch of a reserved page) and on demand
  (`hos_commit`: before the kernel reads or writes a guest buffer, `nn_fs.c`, `hos_svc.c`, `hos_mem.c`), and the back end
  commits the ranges it imports (`gpu/vk/vk_memory.cpp`). Each would have to ask the stream module to restore the dropped
  pages of its range first (a megabyte committed by a touch of its neighbour included), and the decommits and resets of
  the OS layer (`hos_mem.c` unmap and discard, `hos_svc.c`) would have to forget the dropped pages of their range (their
  contents are given up there: zero is then right). The per-file heap's destruction reaches the OS layer as such an unmap
  of its address region: that is the reliable signal that a dropped range is gone. A plain reuse of a dropped range by the
  game's allocator (the heap's free `0x8021dea0`, then an allocation over it) needs no signal: the first touch restores
  the old bytes and the new owner's write goes on, which is what the memory would have held had it not been dropped.
- **The restore cannot be made atomic with the guest window as it is reserved today, and this is what stops the
  brick.** A decommitted page is made again by committing it and copying its bytes in. Committing makes a zero page
  visible to every thread at once: a thread that touches it between the commit and the end of the copy does not fault and
  reads zeros (silent corruption). Committing it `PAGE_NOACCESS` first does not help: the copy needs the page writable,
  and the moment it is, every thread can read it. Windows has no "commit with these contents" for plain private memory.
  What would close it: (a) the guest window reserved as placeholders, so that a restored page is a section view filled
  through a second view and then mapped in place in one call (`VirtualAlloc2` / `MapViewOfFile3` with
  `MEM_REPLACE_PLACEHOLDER`: 64 KB granularity, a change to how the whole window is reserved and committed); (b) AWE
  physical pages (`MapUserPhysicalPages`: atomic, but needs the lock-pages privilege); (c) stopping every other thread
  of the process (game, back end, driver) for the copy, with the page committed `PAGE_NOACCESS` until all are suspended.
  None was built in this session.
- The rest of the races are closable and are written down for whoever builds it: two threads touching one dropped range
  (a lock per range; the second waits, then runs its instruction again); a kernel touch (through `hos_commit`, above); the
  back end's threads (user-mode copies come through the handler; imports through `hos_noaccess_import`, which would have
  to restore before the driver locks the pages); a write to a dropped page (restored first, then the write goes on: the
  handler returns to the faulting instruction); the decommit itself racing a write (pages made read-only, hashed against
  the decode-time hash, decommitted only if equal: a writer faults on the read-only page and waits for the decision).

`HL_STREAM_DROP` was therefore not added: nothing is decommitted.
