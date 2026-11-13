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
case's chunks are decoded on another thread, reached through a callback object in the decompressor (`+1640`); the
`ParallelDecompThread`s of `0x8069a5e0` are, by their creator, those threads (not traced further).

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

## 5. The chunked case and its hand-off

A file whose compressed size is at least a chunk (902,912 bytes) is decoded while it is read: the loading thread fills the
descriptor (state 1, destination, decompressed size, workspace, the two source buffers, the DDict), waits on and clears its
events, then in a loop: hands the chunk just read to the decoding thread (count at `+1624`, the callback's slot 0 with the
descriptor), reads the next chunk into the other buffer, and waits for the decoding thread to finish the previous one. A
short read ends the loop (a last partial chunk is handed over and waited for). The decoding thread (`0x80115bf0`) decodes
the chunk from the buffer its index chooses with the game's own block decoder (`0x80115c80`), counts the chunk, and
signals; an error goes to the status. Success is the state byte at 4.

**What the native path does with it**: the chunked case is decided only after the first read (the read filled the
chunk). At that point the native path has opened the file and read one chunk, and nothing else: no header was acted on, no
memory allocated. It closes the file as the translated function closes it, and gives the request, unchanged, to the
translated function, which opens and reads the file again from its start. What the game can see of that: one more
prepare/open/read/close of the same file object, with the same arguments, before the real one; the read buffer's first
chunk written twice with the same bytes. Nothing else changes (the request, the heap and the results are untouched until
the translated function runs). Reproducing the hand-off natively would mean reproducing the game's block decoder's
progress fields in the descriptor; it was not done.

## 6. The other callers of the decoder (not this brick)

- `0x80c17040` (through a vtable made by `0x80d331e0`): decodes a block embedded in data already in memory (magic `TARB`)
  into a work buffer of its caller: not resource storage.
- `0x80f700d0`: the ROM cache and the heap size table, at start.

## 7. `hl::stream` (2026-11-13)

| Piece | Where |
|---|---|
| The path a request takes, decided before anything is done for it (`hl::stream::choose`), the counters | `engine/native/include/hl/stream/Decode.h`, `src/stream/Decode.cpp` |
| The provenance table: destination range -> file name (the per-file heap's), file object, compressed size (0 for a chunked file, whose later chunks the native side does not see) and decompressed size, dictionary id, chunked or not, a fingerprint of the decoded bytes; lookup by address; a new decode drops the ranges it overlaps; at most 65,536 ranges, oldest dropped | `engine/native/include/hl/stream/Provenance.h`, `src/stream/Provenance.cpp` |
| The C surface | `engine/native/include/hl/c/hl_stream.h`, `src/c/hl_stream.cpp` |
| The answer of `0x80108da0` (and of `0x80115bf0` for the check), the native function, the dictionaries, the zstd calls | `engine/host/hl_stream.c` |
| zstd's decoder in the game's executable (the vendored `third_party/zstd`, decoder only) | `engine/nx2/build.py` |
| Tests without the game | `engine/native/tests/stream_test.cpp` |

Switches: `HL_STREAM=1` the native function (the default; 0 the translated function, as before), `HL_STREAM_CHECK=1` the check (the translated function answers,
then the same compressed bytes are decoded natively and compared), `HL_STREAM_LOG=1` the counters every 10 s.

The native path takes a request when it has a file object of the usual kind, a heap, a granularity that is a multiple of
32, and a decompressor with its own usable read buffer: every request seen in the three scenes. Everything the function
does on that path goes through the game's own objects (the file object's slots, the heap's allocation, the guard, the
close) on the calling thread's guest context, in the translated function's order, with its stack frame at the same
address; only the header read and the decode are native (zstd 1.5.7). What is not reproduced: the decoder context's
fields in the read buffer (scratch, section 3), and the chunked case (section 5).

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
