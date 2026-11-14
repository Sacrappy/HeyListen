# The memory boundary

What the game's memory looks like from the host, what the gameplay code sees of the game's own heaps, how a resource's bytes
travel from a file to the GPU, and where the same bytes are held twice or three times. Read on 2026-11-13 from the user's
local translation (the lifted C, `functions.json`, the relocated image and its export table) and from this repository's
`engine/host` and `engine/gpu/vk`; nothing was run. Addresses are guest addresses of the supported build; offsets are in
bytes; counts of call sites were made by scripts over the whole lifted code (direct calls and tail calls; indirect calls are
not in them unless said). A number that was measured cites its ticket; one that was not is marked **to measure**. What is
inferred rather than read is marked *(inference)*.

## 1. Guest memory on the host today

### The window

| Range (guest) | What | Committed |
|---|---|---|
| `0x01000000`-`0x07000000` | system region: the host's own guest objects (main stack, TLS blocks, thread regions) | all of it at start (`hos_mem_init`) |
| `0x80000000`-`0x86018000` | the image: rtld, main (`0x80020000`, 82 MB with data), subsdk0, sdk | the file's size at load |
| `0x1000000000`-... | the heap (`nn::os::SetMemoryHeapSize`) | the whole size, when the game sets it |
| ...-`0x8000000000` | address regions (`nn::os::AllocateAddressRegion`), handed out top-down | each region whole, when handed out |

- The host reserves 512 GB at a fixed host address (`A64_HOST_BASE`) and commits what it hands out (`hos_commit`,
  `host/mkw_glue.c`), because host APIs given guest buffers (a file read into a game buffer) fail on reserved pages. A touch
  of a page nobody committed is committed 64 KB at a time by an exception handler (`commit_on_touch`, logged as "committed on
  first touch"). Committed pages cost commit charge at once and RAM only when touched.
- The default build tracks guest stores in software (`A64_WRITE_TRACK`, `nx2/build.py`): every lifted store marks its 4 KB
  page in a byte array of 2^27 entries; the window is not write-watched by Windows then. The renderer's page write
  generations come from these marks.
- `HOS_LARGE_PAGES=1` (experiment, needs "Lock pages in memory") commits the whole window in 2 MB pages; freed pages are then
  not discarded.
- The header comment of `host/hos_mem.c` still says the whole 4 GB window is committed up front: that is the TOTK layout it
  came from, not this one (`hos.h`, `hos_main.c reserve_guest`).

### What the game asks for

- `nn::os::QueryMemoryInfo` answers 9809 MB available (`HOS_MEMORY_MB` overrides; the Switch 2 application budget) and
  program + 16 MB used (`host/hos_mem.c`).
- The game's memory set-up `0x8262bb10` (91 lines; reached from the SDK's start-up `0x850d51a0`) sets the CPU boost mode, then
  asks for a heap of `align_down(available - used, 2 MB) - 0xAF200000` (2802 MB left out for address regions and the
  system), stores that constant and the heap size in globals (`0x84eb0e80`..`0x84eb0ea0`), calls
  `SetMemoryHeapSize`, takes the whole heap as one block (`AllocateMemoryBlock`), gives its first 36 MB (`0x2400000`) to an
  `nn::mem::StandardAllocator` (through `0x814e6c30`), records where the rest starts (`0x84df1058`), and installs the
  thread-local-storage allocator (`nn::os::SetMemoryAllocatorForThreadLocal`).
- With the host's defaults that is a heap of **6894 MB** at `0x1000000000`: the `heap:` line of every run log
  (`build/s5/base1/game_log.txt`, sprint 5). The arithmetic agrees: 9809 - ~112 MB used (image + 16 MB), aligned, - 2802 = 6894.
- `SetMemoryHeapSize` commits all 6894 MB at once (`hos_mem.c`). `AllocateAddressRegion` commits the whole region at once.
  `AllocateMemoryPages` above 4 GB commits (a no-op inside a region already committed); `FreeMemoryPages` decommits
  (`MEM_DECOMMIT`), except pages the Vulkan back end has imported, which are zeroed and stay committed.

### Where RAM goes (measured)

`engine/docs/perf-audit.md` "TICK-038" (Market, Ryzen 7 7800X3D, 32 GB, RTX 5070, before TICK-038's changes; castle-move in
brackets):

| Category | Committed | Resident |
|---|---:|---:|
| guest window (the game's memory) | 11.2 GB (12.3) | 7.75 GB (9.2); ~3.5 GB committed and never touched |
| HeyListen.exe image (lifted code, the mark array) | 1.03 GB | 0.27 GB |
| NVIDIA driver arenas | 5.18 GB | 5.0 GB (2.83 after TICK-038) |
| 165-210 driver regions beside VRAM | 5.5 GB (6.9) | 0.4 GB (0.6) |
| capture buffers (host-cached Vulkan memory) | 0.47 GB (0.61) | |
| C heaps (the engine's own objects) | 0.45 GB | |
| process private / working set | 23.7 GB (25.7) | 14.2 GB (15.5); 11.9 / 13.6 after TICK-038 |

VRAM by kind, same ticket: textures 1.27 GB (1.6-1.7), render targets 1.0-1.1 GB (0.96-2.2), buffer-pool mirrors 1.52-1.54
GB (1.7), write-back shadows 224 MB, BC scratch 160 MB, staging 76-140 MB and streams 96 MB in BAR VRAM, free inside VMA
blocks 0.4-1.9 GB, the driver's other ~1.03 GB. TICK-051 adds ~900 MB of system memory held by the BC cache on a castle walk
(PC 3050).

### How the game's memory in RAM splits (measured 2026-11-13)

`HOS_MEM_SPLIT=<seconds>` (`host/hos_memsplit.c`) counts the resident pages of the guest window once, by part of the
window, and those inside an NVN memory pool with storage, by the pool's flags. Castle scene, 95 s after the start, main
PC, with the heap committed on first touch (`HOS_LAZY_COMMIT`, the default since that day: the process's commit went
from 24.9 GB to 21.9 GB, the working set stayed at 13.9-14.0 GB):

| Part | Committed | In RAM | Of which inside GPU pools |
|---|---:|---:|---:|
| heap | 3943 MB | 3810 MB | 2932 MB |
| address regions | 4468 MB | 4332 MB | 1645 MB |
| image, system region | 192 MB | 97 MB | 0 |
| **the game's memory** | | **8240 MB** | **4578 MB** in 4,542 pools; 3662 MB outside |

| Pool flags | Meaning (NVN's flag bits) | Pools | Storage | In RAM |
|---|---|---:|---:|---:|
| `0x024` | CPU cached, GPU cached | 3,225 | 3896 MB | 3350 MB |
| `0x0a1` | no CPU access, GPU cached, compressible | 70 | 547 MB | 417 MB |
| `0x064` | CPU cached, GPU cached, shader code | 195 | 315 MB | 315 MB |
| `0x021` | no CPU access, GPU cached | 80 | 293 MB | 247 MB |
| `0x022` | CPU uncached, GPU cached | 949 | 175 MB | 104 MB |
| `0x18c` | CPU cached, no GPU access, compressible, physical | 1 | 512 MB | 96 MB |
| others | | 22 | 91 MB | 44 MB |

What it says:
- **More than half of the game's RAM (4.6 of 8.2 GB) is the CPU-side storage of GPU data**, which also exists in VRAM.
  This is the Switch's shared memory seen from a PC: one copy there, two here.
- **664 MB are in pools the game declared "no CPU access"** (`0x0a1`, `0x021`): by NVN's contract the game's code never
  reads or writes them. They are in RAM only because this engine's back end writes there (GPU results written back, and
  the pool's storage read for the mirror). That is the first saving to take: it depends on the back end alone.
- 3.35 GB are CPU-cached pools (`0x024`): the game may read them; dropping those pages after upload needs the proof by
  page trap described in section 4.
- 3.66 GB are outside any pool: the game's own heaps (actors, resources' CPU side, the allocator's 36 MB).

## 2. The game's heap system

### Global allocation entry points

| Address | Name (export table) | What it does | Call sites / callers |
|---|---|---|---|
| `0x80f471c0` | `operator new(size)` | current heap (below), its virtual alloc (slot 6) with alignment 16; no heap manager yet: `0x8005eca0` | 343 / 186 |
| `0x80f404a0` | `operator new[](size)` | `0x8005ea70` with heap 0, alignment 16 | 19 / 16 |
| `0x8005ea70` | (none) | alloc(heap, size, alignment): heap 0 means the current heap; slot 6 | 1,286 / 1,017 |
| `0x8005eca0` | `aligned_alloc` | `StandardAllocator::Allocate(size, align)` on the global allocator `0x84d68660` (ready flag `0x84d68658`); errno 12 on failure | 6,328 / 4,241 |
| `0x814e6c90` / `0x814e6ce0` / `0x814e6d50` | `malloc` / `calloc` / `realloc` | the same allocator | 335 / 187; 15 / 11; 433 / 135 |
| `0x80709580` | `free` | `StandardAllocator::Free` through the GOT | 236 / 129 |
| `0x8021dea0` | `operator delete(ptr, size)` | finds the heap that holds the pointer (below), its virtual free (slot 7) | **19,350 / 16,301** |
| `0x8021f7c0` | `operator delete[](ptr)` | the same | 2,593 / 2,391 |
| `0x83488c40`..`0x83488e70` | the nothrow and `align_val_t` forms | thin forms of the above | 0 to 16 each |

The `StandardAllocator` (36 MB at the heap's start) serves libc's `malloc` and the fallback of `operator new` before the heap
manager exists. Everything else goes to the sead-like heaps.

**The current heap.** The heap manager's pointer is at `0x84e9f498` (through the GOT entry `0x84c9dff0`); null means "before
the heaps". The thread manager's pointer is at `0x84e9f668`; its `+72` is the TLS slot of the current game thread object
(`nn::os::GetTlsValue`). That thread object's `+136` is its current heap; with no thread object the heap is the manager's
`+8` *(inference: the root heap)*. `operator delete` checks the current thread's heap first (`+136`, with an atomic flag at
`+144` held during the check), then falls back to a search under the manager's lock; it compares two vtable entries against
one heap class's own functions (`0x806a0ef0`, `0x83495020`, class `0x84c178e0` below) to skip a virtual call.

**Inlined.** `operator new` is inlined almost everywhere; `operator delete` is not. Counted over the lifted code (script
over every call of `GetTlsValue`):

| Pattern | Sites | Functions |
|---|---:|---:|
| calls of `nn::os::GetTlsValue` | 12,103 | |
| ... with the slot read at `+72` of the thread manager (current game thread) | 8,300 | |
| ... then the thread's `+136` read (current heap) | 6,339 | 4,195 |
| ... then a vtable `+48` load and an indirect call (inline `new`) | 5,210 | 3,754 |
| ... with the `aligned_alloc` fallback next to it | 3,462 | |

So most of `aligned_alloc`'s 6,328 call sites are the cold branch of an inlined `new`. The largest users: `0x83ae22c0` (142
inline allocations), `0x82e64600` (107), `0x836d0880` (58), `0x82c24db0` (48), `0x836d8ad0` (30, the resource system's
set-up).

### Heap objects

Base constructor `0x809dfed0` (15 sites; arguments: this, name, parent, start, size, direction, thread-safe flag, a second
parent). Fields read in place by inlined code (`operator delete`, inline `new`) are in bold:

| Offset | Field |
|---|---|
| +0 | vtable |
| +8, +16..+24 | the disposer part: its heap, a list node |
| +32 | name |
| **+40** | arena start |
| **+48** | arena size |
| +56 | parent heap |
| +64..+72 | a self-linked list head |
| **+80** | a word `operator delete` requires to be 0 before it trusts the range test |
| +64..+72, +84 | the child heaps' list (node at +84 in the child; section 10) |
| +104..+120 | the disposers' list: child heaps and other objects that end with the heap; +120 their count (raised under the parent's lock; section 10) |
| **+128** | direction (1: the heap object sits at the arena's front) |
| +136 | `nn::os::MutexType` (initialized in the constructor) |
| +168 | flags: bit 0 thread-safe (lock taken), bits 16 and up an id from a global counter |

The free-list heap (class `0x84c172c8`) adds `+172` (16 bits), `+176`..`+184` and `+200`..`+208` two self-linked lists *(inference:
free and used blocks)*. Its object is 224 bytes and lives inside its own arena: its range test excludes the first or last
224 bytes depending on the direction (`0x80517d30`).

### The virtual interface

Thirteen vtables share one layout (slot 28 is the common dump function `0x83492f20`, which names heaps
`$SEAD_META_HEAP_%03d`; slot 29 is the common `0x809dfe30`). Slots read function by function:

| Slot | Offset | Meaning | How it is known |
|---:|---:|---|---|
| 6 | +48 | alloc(size, alignment) | inline `new` and `operator new` pass size and 16 |
| 7 | +56 | free(ptr) | `operator delete`; the frame-style classes leave it empty |
| 13 / 14 / 15 | +104 / +112 / +120 | start (`+40`), end (`+40 + +48`), size (`+48`) | one-line bodies |
| 16 | +128 | free size | |
| 17 | +136 | size - free size (calls 15 and 16) | |
| 18 / 19 | +144 / +152 | 19 calls 18 *(inference: max allocatable size, with and without an alignment)* | |
| 20 | +160 | contains(ptr) | the range test above |
| 22-24 | +176..+192 | constant 1 in the free-list class *(inference: freeable / resizable / adjustable)* | |
| 4, 5, 8-12, 21, 25-27 | | *(inference: destroy, adjust, resize, realloc, free all, ...)* not read | |

| Vtable | alloc / free | Made by | *(inference)* kind |
|---|---|---|---|
| `0x84c172c8` | `0x809df950` / `0x803b44e0` | create `0x809df710` (68 sites, 51 callers) and its wrapper `0x809df700` (44 sites, 39 callers); in place `0x809de1f0` (the root heap) | free-list heap (the general one) |
| `0x84c17400` | `0x834921a0` / empty | create `0x809e00b0` (19 / 14) and wrapper `0x809e00a0` (14 / 11) | frame heap |
| `0x84c177a8` | `0x801e8460` / `0x80504840` | `0x809dfb20` (4) and wrapper `0x809dfb10` (3) | |
| `0x84c178e0` | `0x8080fe20` (calls other heaps' slot 6) / empty | `0x8069d980` <- `0x8080cf10` (actor packs, chemistry, renderer); `0x8080cf10` also takes address regions and pages through `0x8080d440` | its empty free is what `operator delete` short-cuts |
| `0x84c17628` | `0x8080fd70` / `0x834931b0` | `0x809e2210`; "NinVirtualAddressHeap" `0x809e20c0` | address region with pages mapped on demand: `AllocateAddressRegion` / `AllocateMemoryPages` in `0x8080d440` (called by this alloc) and `0x809e2180`; `FreeMemoryPages` / `FreeAddressRegion` in `0x83493210` and `0x8080ec40` (slots 12 and 10) |
| `0x84c179e8` | `0x808101d0` / `0x806a22f0` | `0x809e05c0` (terrain heaps) | |
| `0x84c17c08` | `0x834991f0` / `0x83499320` | `0x809e1180` (actor instance heaps) and wrapper `0x809e1170` | |
| `0x84c1acb8` | `0x834bdd90` / `0x834bdfc0` | `0x809df570`: over an NVN memory pool ("ForResourceG", `0x809de920`) | GPU resource heap |
| `0x84c41fe0`, `0x84c77660`, `0x84c78328`, `0x84c79200` | | `0x809e13d0`, -, `0x80a38110`, `0x83acd9d0` | not read |
| `0x84c17508` | pure virtual | | the abstract base |

### Who makes which heap

- **Root**: `0x809de180` makes "RootHeap" in place (free-list class) over the arena whose descriptor `0x84e9f3e0` points to,
  and adds it to the root array at `0x84e9f400` (count `+0`, capacity `+4`, pointers `+8`). Two callers: `0x809de110` (arena
  given) and `0x834930b0` (arena taken with `AllocateMemoryBlock` through `0x83490bf0`), both from the thread manager's set-up
  `0x809ddf00` <- `sead::SystemManagers` `0x809db6b0` <- `0x81222ce0`.
- **Sizes are data**: `0x81222ce0` reads `System/HeapSize/` and a `HeapSizeDB` (also `%s%s.Nin_Ounce_NVN2.HeapDef.byml.zs`), so
  most heap sizes are not constants in the code. Not read: the values.
- Named heaps by creator (strings formed in the creating function), free-list class unless said: `sead::ResourceMgr`,
  `sead::FileDeviceMgr`, `sead::ThreadMgr`, `sead::TaskMgr`, `sead::MethodTreeMgr`, `agl::WorkHeap` / `agl::FrameHeap`
  (`0x80c49ab0`, the second a frame heap), `TextureHeap`, `TextureCompactionWorkHeap`, `ModelResourceMgrGpuHeap` (`0x831dd780`),
  `aglTextureDataMgr::CompactibleHeap` (`0x80f6c580`), `gsys::ModelResourceMgr(Heap)` / `(GpuHeap)` (`0x80f5b720`),
  `tera::FileResourceMgr` (`0x80f6cf50`), `ActorInstanceHeap` / `BaseProcCreateOrderHeap` / `DynamicParamHeap` (`0x83afb830`),
  `GeneralHeap` (module system, `0x83b797e0`), `DefaultHeap` (`0x814e5540`), `MovieHeap`, `MultimediaHeap`, `PhiveSystemHeap`,
  `NavMeshSystem`, `EventHeap`, `LayoutArchiveHeap` / `StaticScreenHeap`, `Foliage Model Heap` / `Foliage Scene Heap`,
  `Terrain Resource Heap` / `Terrain Temp Heap` / `Create Terrain Scene Heap` (`0x809e05c0` class), `ExtraArgHeap` /
  `PauseMgrHeap` (`0x83b7a2c0`, which also makes eight heaps through `0x83b7b340`), `ResourceU` / `ResourceS` / `ResourceL` /
  `ResourceG` (`0x83bb6eb0`, the resource module), and ~30 more of one creator each.
- Which thread: the boot heaps are made on the main thread during the system managers' set-up; the resource, sound and
  effect heaps by their modules' initialisation *(inference: main thread, from the module system's init; not traced)*.

### What a native allocator can answer, and what it must keep

- **Real calls, answerable at their address**: `operator delete` (19,350 sites), `operator delete[]`, `operator new`,
  `operator new[]`, `0x8005ea70`, `malloc`/`calloc`/`realloc`/`free`/`aligned_alloc`, each heap class's virtual functions
  (reached through the vtables above, so answering a slot means answering it for one class), the creators `0x809df710`,
  `0x809e00b0`, `0x809dfb20`, `0x809e05c0`, `0x809e1180`, `0x809df570`, `0x809de1f0`.
- **Read in place, must be kept as the game expects**: the heap manager pointer `0x84e9f498` and its `+8`; the thread
  manager's TLS slot (`0x84e9f668` -> `+72`); each game thread object's `+136` current heap and `+144` flag; each heap's
  vtable pointer at `+0` (the inline `new` calls slot 6 through it, 5,210 sites), `+40`/`+48`/`+80`/`+128` (the range test
  inlined in `operator delete`, which a native `delete` would replace). A native heap must therefore be a guest object with a
  guest vtable whose slot 6 lands in native code; its arena can be anywhere in the window.
- Not counted: virtual calls through slots other than 6 from gameplay code (heap pointers held in objects), and inlined reads
  of `+40`/`+48` outside `operator delete`.

## 3. The resource system and the life of GPU data

### Entry points found

| Address | What |
|---|---|
| `0x83bb6eb0` | the resource module's initialisation (reached only through a vtable): makes the `ResourceU/S/L/G` heaps, `ResourceRecordFallbackHeap` (`0x83bb8290`), `GeneralFormatAsync_` (`0x83bb3eb0`), the `ZsDic` set-up (`0x811fdd00`), and calls `0x836d8ad0` |
| `0x836d8ad0` | `ares::System` set-up (3,076 lines): "Resource Control" and "Resource Memory" threads, `LoadingThreads` ("ARes Loading %@", four), `ComposeThreads` ("ARes Compose %@"), `DecompSystem`, `zsdic`; calls `0x8069a5e0` |
| `0x8069a5e0` | `DecompSystem`: makes the `ParallelDecompThread`s (two in the logs) |
| `0x83bada70` | the "ForResourceG" GPU heap: a memory pool through `0x80f6c9d0` over the module's memory, a heap of class `0x84c1acb8` over it (`0x809df570`), then `0x80b13100` with it (which makes a `NinVirtualAddressHeap`, `0x809e20c0`; how the two heaps divide the work was not read) |
| `0x80b13100` | a resource heap maker used by `ForResourceV` (`0x80b13af0`, which registers the file kinds: pack, zsdic, sarc, bfres, bfsha, byml, ...), sound (`0x83740cd0`) and `ForResourceG` |
| `0x8010b380` | the zstd frame decoder with dictionary support (the frame magic and the dictionary magic in its text; 5,646 lines); callers `0x80f700d0` (rom cache, `HeapSizeDB`) and two reached only through a vtable: `0x80108da0` (vtable at `0x84c377e8`, made by `0x836dea10`) and `0x80c17040` |
| `0x80109790` | zstd frame header read (7 sites in 5 functions) |

Candidate thread bodies, formed as code pointers by their creators, not confirmed: `0x80971c00` and `0x80971a90` (in
`0x836d8ad0`), vtables `0x84c376e0` -> `0x80e6fb10` and `0x84c37578` -> `0x801c5e80` (in `0x8069a5e0`), `0x80a83e90` and
`0x801b2a00` (in `0x83bb3eb0`). **Not found**: the request entry points the rest of the game calls to load, wait for and
release a resource. The most-called functions near the resource system (`0x801b06d0`, 193 callers; `0x801b0950`, 139) turned
out to be data-parsing helpers, not requests.

### NVN calls the game makes

The game resolves its NVN procedures once (`0x812be290`, 735 names, each stored in a global) and calls them through those
globals. Call sites, counted per procedure by following the global's load to the indirect call:

| Procedure | Sites / functions | Notes |
|---|---:|---|
| `nvnMemoryPoolInitialize` | 21 / 17 | few pools, made by managers: `0x80f6c9d0` (the gfx-layer pool init; 5 callers incl. `ForResourceG`, `aglTextureDataMgr`, `tera::FileResourceMgr`), `0x809de920` (3 pools), `0x812c42c0` (10 sites), `0x80f69040`, `0x8005ed00` (`agl::utl::DynamicTextureAllocator`) ... |
| `nvnMemoryPoolBuilderSetFlags` constants seen | | `0x21` (CPU no access, GPU cached), `0xa1` (+compressible), `0x22`, `0x24`, `0x12`, `0x62` and `0x64` (shader code); `0x80f6c9d0` computes its flags from a property mask (not resolved) |
| `nvnMemoryPoolMap` | 7 / 5 | incl. `0x80f6c9d0`: the resource pools are CPU-mapped |
| `nvnMemoryPoolFlushMappedRange` | 117 / 57 | CPU writes into pools |
| `nvnMemoryPoolInvalidateMappedRange` | **36 / 12** | CPU reads of GPU results: `0x80021ca0`, `0x8005ed00`, `0x800662e0`, `0x8007a8f0`, `0x8009bb80`, `0x80293190`, `0x80445020`, `0x8044e8e0`, `0x80665980`, `0x809f43c0`, `0x812342c0`, `0x83647b20` |
| `nvnTextureInitialize` / `nvnTextureBuilderSetStorage` | 14 / 11; 17 / 14 | `0x834bf750` (render targets: 298 sites in 121 functions), `0x80798920` / `0x807982f0` (gfx-layer textures, 7 + 2 callers), `0x81503090` (packaged texture data: the anti-aliasing tables) |
| `nvnBufferInitialize` / `BufferBuilderSetStorage` | 28 / 19; 29 / 20 | |
| `nvnBufferMap` / `BufferInvalidateMappedRange` | 7 / 6; 3 / 3 | |
| `nvnTextureReadTexels` / `WriteTexels` / `FlushTexels` / `InvalidateTexels` | 2 / 2 / 2 / 1 | the CPU touches texels directly in very few places |
| `CopyBufferToTexture` / `CopyTextureToBuffer` / `CopyTextureToTexture` | 2 / 2 / 4 | the game does not upload textures with GPU copies: textures are made in place in pool memory |

### From file to the GPU *(inference where marked)*

1. The file is read by the loading threads and decompressed (zstd, dictionary `zsdic`) by the decompression threads into the
   heap of its resource class. GPU resources (models' `bfres`, textures) go to `ForResourceG`, a heap whose arena **is** an NVN
   memory pool, CPU-mapped (`0x83bada70`, `0x80f6c9d0`). *(inference: the read buffer of the compressed file is a second,
   short-lived copy in a work heap; where it lives was not traced.)*
2. Textures and buffers are made at an offset of that pool (`nvnTextureBuilderSetStorage` / `nvnBufferBuilderSetStorage`): no
   copy on the game's side. NVN requires the storage to stay while the texture lives, so the game keeps the decompressed
   bytes in guest RAM for the resource's lifetime *(inference from NVN's rules; no free of the storage before the texture's
   finalize was seen, but none was looked for)*.
3. The host's `nvnMemoryPoolInitialize` (`host/nvn_native.c`) makes the pool's GPU address its guest address, tells the
   back end (`rx_nvn_pool_created`, `rx_nvn_pool_flags`) and does not copy. Flush is a no-op (the write marks carry CPU
   writes); invalidate makes the CPU wait for the GPU's write-backs of that range.
4. The Vulkan back end (`gpu/vk/vk_memory.cpp`, `vk_texture.cpp`):
   - imports guest memory by 16 MB chunk around what is bound (`VK_EXT_external_memory_host`, in the background): the same
     pages, pinned, no copy;
   - on a discrete GPU (default `RX_VK_MIRROR` there) gives every pool bound as a buffer a **device-local mirror**
     (budget `RX_VK_MIRROR_MB`, 3072 by default; a whole pool, *(inference)* including whatever textures share it); CPU-written
     pages are copied in at a submission's first use, GPU writes are copied back to guest memory at its end, through a
     **write-back shadow**; since TICK-019 the GPU reads no imported host memory at all;
   - makes a `VkImage` per texture storage (VRAM only), uploaded through **staging arenas** (host-visible; kept up to 256 MB,
     arenas over 64 MB freed) at first use and again when the CPU wrote the storage; ASTC is transcoded on the GPU to BC7/BC1
     (or RGBA8) and the BC blocks are kept in a **BC cache** in system memory (`RX_VK_BC_CACHE_MB`, 1024 by default);
     evicted textures are uploaded again **from guest memory** when made again;
   - copies every page written since the last submit into a **capture** (25 kept buffers and 12 spares) so that the replay
     reads memory as it was at the submit; texture-storage pages taken by a capture are kept as a **shadow** (4 KB each, at
     most 65,536 pages = 256 MB).

### Copies of the same bytes

| Kind | Copy 1 | Copy 2 | Copy 3 and more |
|---|---|---|---|
| Static textures | the decompressed file in `ForResourceG` (guest RAM, resident, kept for the resource's life) | the `VkImage` (VRAM, transcoded) | while loading: a staging copy (transient), the capture's copy and its shadow (up to 256 MB in all), the BC cache's blocks (system memory, ~900 MB measured, TICK-051); the compressed read buffer (transient, *inference*) |
| Static geometry (vertices, indices in pools bound as buffers) | the pool in guest RAM | the VRAM mirror of the whole pool | the capture's copy when written (load time) |
| Dynamic buffers (per-frame constants, instances, the light meter) | the pool in guest RAM, rewritten every frame | the capture (~46 MB a frame at the Market, ~11,900 pages, 51% of them byte-identical to their last copy: sprint 5) | the VRAM mirror; the write-back shadow and the copy back to guest memory for what the GPU writes |
| Pools the CPU reads back | guest RAM | the mirror | the write-back's copy into guest memory, waited for at the invalidate |

Does the game read pool memory back? Yes for 12 functions (36 invalidates; the light meter of sprint 5 is one). For textures
the CPU reads texels in two places (`ReadTexels`). **Not established**: whether any code reads a static texture's or a static
vertex buffer's pool bytes after it was made (a direct read needs no NVN call, so it cannot be counted from the calls).

## 4. What a native memory manager could do, ranked

Ranked by the saving existing measurements can back. None of these has been tried.

1. **Give up the guest copy of static GPU data once it is on the GPU** (RAM). Evidence: the decompressed texture and
   geometry data stay in `ForResourceG` for the resource's life, and the back end keeps its own VRAM copy (textures 1.3-1.7
   GB, mirrors 1.5-1.7 GB, TICK-038). Saving: **to measure** (the resident size of the resource pools: a residency scan of the
   guest window labelled by pool and heap ranges, which `tools/bench/ressample.py` can do with the pool list the host already
   sees). Rules that must hold: no CPU read or write of those pages afterwards; the back end never reads them again (today it
   does: evicted textures are uploaded again from guest memory, and a mirror is made again from guest memory after it was given
   back). How to prove it rather than hope: protect the pages (`PAGE_NOACCESS`) after upload and catch any touch in a handler
   like `commit_on_touch` that restores them and logs who (lifted code, host, or driver); a run of the three scenes with no
   trap is the evidence. Risks: heap block headers and small allocations share pages with resource data; imported pages must
   stay committed (`FreeMemoryPages` already keeps them); eviction then needs a native copy (compressed, or on disk).
2. **Commit on touch instead of at hand-out** (commit charge, not RAM). Evidence: ~3.5 GB of the guest window committed and
   never touched (TICK-038); the host commits the whole 6894 MB heap at `SetMemoryHeapSize` and every address region whole,
   although the game's own region heaps (`0x84c17628`, `0x84c178e0`) map pages on demand. Saving: up to ~3.5 GB of commit
   charge, ~0 of working set. Rule: host code that hands a guest buffer to an OS API (file reads) commits that buffer first.
   Risk: low; the touch handler exists. Matters on PCs with a small page file (the out-of-memory box in `mkw_glue.c`).
3. **The BC cache** (RAM): up to 1024 MB by default, ~900 MB measured on a castle walk (TICK-051). Engine-side, not the
   game's memory: a lower cap or a compressed/disk store. Risk: more transcodes (2107 -> 327 a run is what it bought).
4. **Mirror only what is bound as a buffer** (VRAM): mirrors are whole pools; a native allocator that owns the pools could
   keep buffers and textures in separate pools so that textures are never mirrored. Saving: **to measure** (the bytes of
   mirrored pools that no buffer binding covers, from the back end's bindings).
5. **Dynamic buffers in memory the GPU reads directly** (RAM bandwidth, the 46 MB capture a frame). Evidence: sprint 5's
   capture numbers; `RX_VK_MIRROR=0` (the GPU reading system memory over PCIe) gave 5.5 fps. Host-visible device-local (BAR)
   memory cannot be placed at fixed guest addresses on Windows, and the gameplay code addresses guest memory by its window, so
   this needs the per-frame pools moved out of guest-written memory or a write path that targets BAR: **to research**, not a
   switch.
6. **Capture buffers and the texture shadow** (RAM, 0.47-0.61 GB and up to 256 MB): already tuned (TICK-038 tried aging the
   spares: no steady gain). Small.

Not for RAM but noted: large pages for the guest heap (TLB reach, `docs/research/lifter-x86.md`), and a native allocator's
speed (the inline `new` is a virtual call into lifted heap code at 5,210 sites; not measured as a cost).

## 5. Not read yet

- The resource request entry points (load, wait, release) and their objects; the loading, compose and decompression threads'
  bodies (candidates above, not confirmed); where the compressed read buffer lives; which heap each resource kind lands in
  beyond `ForResourceG`.
- The heap classes' slots other than 6, 7, 13-17, 20 and 22-24; the four vtables not read; the heap manager's object beyond
  `+8`; the free-list heap's block header layout.
- Heap sizes (data files, not code) and the pool flags `0x80f6c9d0` computes.
- Whether static texture or geometry pool bytes are read by the CPU after creation (only NVN calls were counted).
- Virtual calls to heap slots other than alloc from gameplay code; inlined range tests outside `operator delete`.
- The SDK's own allocators (`nn::lmem` exp/frame/unit heaps, used by the SDK and middleware) and Havok's, audio's and the
  effect library's allocators: they sit on top of the heaps above but were not followed.
- How the 7.75 GB of resident guest memory splits by heap and pool: measured in section 10.
- The header of `gpu/vk/vk_memory.cpp` still describes the pre-mirror design ("no mirrored copies"); the mirror section further
  down is the current one.

## 6. GPU pools without CPU access (2026-11-13)

NVN's `CPU_NO_ACCESS` pools (flags `0x0a1` and `0x021`; 70 + 80 pools, 547 + 293 MB of storage) held 664 MB of RAM on a castle
walk. Main PC, the three scenes, 95 s after the start, one run each unless said; the runs' logs are in `build/s5/gpr*`.

### Why their pages were in RAM

Measured with `HOS_POOL_TRAP_NEW=1 HOS_POOL_TRAP_FLAGS=a1,21` (`host/hos_memsplit.c`): every page of each such pool is made
`PAGE_NOACCESS` when the pool is made, and the first touch of each is counted by who made it (castle, one run, `gpr1-new2`):

| | `0x0a1` | `0x021` |
|---|---:|---:|
| storage of the pools made | 875 MB (render targets, made again at scene changes) | 331 MB |
| in RAM already when the pool was made | 9 MB | 277 MB |
| first touched by lifted code | 0 | 0 |
| read by the back end's texture uploads (`upload_inner` -> `UnswizzleTexture`, on its threads and the Presentation Thread) | 289 MB | 6 MB (ASTC sources: `astc_pieces`) |
| read by the capture's copies | 0 | 32 MB |
| locked by the driver: an import of a 16 MB chunk around a binding | 137 MB | 18 MB |
| pool ended before it was touched | 320 MB | 0 |

And `HOS_MEM_SPLIT` (which now also says how much of the resident pool memory holds only zeros) on the unchanged build:
**the 417 MB of `0x0a1` in RAM are zeros, every page**; of the 247 MB of `0x021`, 16 MB are.

- `0x0a1` (render targets): the game never writes them, but the back end read their storage to upload a texture made over it
  before the GPU had drawn it (a page nobody wrote, read, gets a zero page), and imports of 16 MB chunks (and the spans over
  pools across their ends) around a binding of a *neighbouring* pool (`0x022`, `0x024`) took them in: the driver locks an
  import's pages into RAM. `RX_VK_LOG_NOACCESS_IMPORT=1` (`gpu/vk/vk_memory.cpp`) logs those imports: 11 on the castle walk,
  173 MB of no-access pools in them, all but one for a binding in another pool.
- `0x021`: the game fills the memory before it makes the pool (277 of 331 MB in RAM at `nvnMemoryPoolInitialize`; ASTC
  textures among them, uploaded from there by the back end). That is the textures' only copy outside VRAM, read again when an
  evicted texture is made again: not given up here (it is the `0x024` question of the next section, for 231 MB).
- GPU results are not written back into either: the write-backs left out for pools the CPU does not read (TICK-015) cover
  them, and the trap saw no write by the back end.

### What changed (`HOS_NOACCESS_RAM`, on; `=0` off)

1. `host/hos_memsplit.c` `discard_main`: every 5 s, the resident pages of the `CPU_NO_ACCESS` pools that hold only zeros are
   decommitted (`MEM_DECOMMIT`): they read as zeros again (`commit_on_touch` commits a zero page at a touch), so whoever reads
   them reads what was there. Under `HOS_LAZY_COMMIT` only (the default), heap and address regions, not with large pages.
2. `gpu/vk/vk_texture.cpp` `texture_touch`: a first upload of a texture whose storage lies in such a pool and is all reserved
   (`VirtualQuery`, no page touched) is not read: the image is made cleared to zero, which is what the storage reads as
   (formats stored as they are only: not BC/ASTC, not widened, not sparse).
3. `gpu/vk/vk_memory.cpp` `clip_noaccess`: an import made for a binding leaves out the `CPU_NO_ACCESS` pools the binding is
   not in (cut at the import alignment, never into the binding). A binding inside such a pool imports it as before.

What it relies on, and how each is held:
- the game does not touch those pools while they live (NVN's contract; the trap above: no lifted-code touch). The check and
  the decommit are done under the pool list's lock, so a pool that ends meanwhile (its memory going back to the heap) waits;
- the back end writes guest memory through `nvn_gpu_write` (texture write-backs before an eviction or a virtual remap, replay
  copies and reports): it holds the discard lock shared, the check-and-decommit holds it exclusive, so no write lands between;
- pages the driver locked are never decommitted: `import_pool` calls `hos_noaccess_import` (exported) before it commits and
  imports, and pages of any range ever imported are left alone.

The back end's other reads of that storage, checked: re-upload of an evicted texture (a render target the GPU drew is written
back first through `nvn_gpu_write`, so its pages hold its texels, or zeros that read as zeros); the captures and
`memory_read_as_submitted` (`guarded_copy` and plain reads of a decommitted page commit a zero page: the same bytes); copies
between pools on the CPU (`gpu_read` + `gpu_write`, the write under the lock); `nvnMemoryPoolMap` (still NULL for these pools,
`nvn_native.c`); virtual pools mapped onto them (`nvn_gpu_host` reaches the same guest pages: the same reads; their textures
are never skipped by 2, which is for addresses below 1 TB); textures and buffers made later at offsets in the pool (2 is per
texture, on its own storage; a buffer binding imports as before); mirrors and write-backs (GPU copies to and from imported
memory only, which 1 never touches); `FreeMemoryPages` (imported pages zeroed, never decommitted by 1; others decommitted by
the host as before).

### Before and after

`gpr0` is the build before, `gpr2` the change (castle: `gpr4`, the final build, with the switch on and off in turn). "Game
RAM" and the pools are `HOS_MEM_SPLIT=95`; working set and mirrors (VRAM) are `RX_VK_RES_LOG=1`'s last lines.

| Scene | Build | Game RAM | in GPU pools | `0x0a1` in RAM | `0x021` in RAM | working set | VRAM mirrors |
|---|---|---:|---:|---:|---:|---:|---:|
| kokiri-idle | before | 8214 MB | 4577 MB | 417 MB | 247 MB | 13792 MB | 1632 MB |
| | after | 7779 MB | 4151 MB | 0 | 231 MB | 13339 MB | 1462 MB |
| castle-move | before | 8236 MB | 4577 MB | 417 MB | 247 MB | 14155 MB | 1824 MB |
| | switch off (`gpr4`) | 8239 MB | 4578 MB | 417 MB | 247 MB | 14014 MB | 1776 MB |
| | after (`gpr4`) | 7832 MB | 4162 MB | 0 | 231 MB | 13760 MB | 1669 MB |
| hyrule-walk | before | 8232 MB | 4588 MB | 417 MB | 247 MB | 13971 MB | 1712 MB |
| | after | 7789 MB | 4154 MB | 0 | 231 MB | 13511 MB | 1526 MB |

- **The game's RAM: -407 to -443 MB in each scene** (433 MB of it the two pool kinds: 417 + 16). The working set moves by
  250-460 MB (one run each, and it varies between runs). The mirrors are 100-190 MB smaller (the imports, and so the mirrors
  made of them, no longer take the no-access pools in), one run each.
- The decommitting thread gave back 363-385 MB in all, 14-32 ms a round; after the scene load nothing came back (0 MB a
  round from there).
- Picture and speed (`check.sh gprc mx gpr3`: the reference build against this change, 3 scenes, interleaved): the shots
  match (luma differences are the camera's position at a shot; both builds' pictures correct); fps 77.8 / 79.5 (kokiri,
  this / reference), 76.4 / 76.6 (castle), 81.0 / 77.9 (hyrule): within the runs' spread.
- `0x021` keeps 231 MB: the game's own data, written before the pool was made.

## 7. Does game code touch CPU-cached pools after upload? (measured)

`HOS_POOL_TRAP=70 HOS_POOL_TRAP_REPORT=105` (`host/hos_memsplit.c`, off by default, nothing done when off): 70 s after the
start, every resident page of the pools with flags `0x024`, `0x064` and `0x022` that exist for 2 s or more is made
`PAGE_NOACCESS` (3.7 GB, in ~0.2 s); the first touch of each page gives it back its access and is counted by flags,
read/write, thread and host stack; `tools/bench/pooltrap.py` resolves the stacks with `HeyListen.map` (lifted `f_<guest
address>`) and the back end's symbols. Classes: **game** (a lifted function, or a copy routine it called), **back end** (the
back end's threads, and its own work on guest threads: the replay and present on the Presentation Thread), **kernel/driver**
(pages handed to `hos_commit` or to an import: given back their access first and counted). One run per scene (`gprt-*`,
kokiri `gprt2-*`), the trap armed once, read 35 s later.

| Scene | Flags | Trapped | Never touched again | Game: read / write | Back end (its threads + on guest threads) | Kernel / driver |
|---|---|---:|---:|---:|---:|---:|
| kokiri-idle | `0x024` | 3351 MB | 3331 MB | 0.5 / 0.4 MB | 17.6 + 0.1 MB | 1.2 MB (imports) |
| | `0x064` | 316 MB | 315 MB | 0 / 0 | 0.8 + 0.1 MB | 0.2 MB |
| | `0x022` | 104 MB | 75 MB | 0.8 / 24.5 MB | 1.9 + 1.5 MB | 0 |
| castle-move | `0x024` | 3350 MB | 3119 MB | 0.5 / 0.4 MB | 148.2 + 20.4 MB | 61.6 MB (imports) |
| | `0x064` | 316 MB | 308 MB | 0 / 0 | 6.8 + 0.6 MB | 0.7 MB |
| | `0x022` | 104 MB | 72 MB | 1.2 / 26.1 MB | 2.3 + 1.5 MB | 0.5 MB |
| hyrule-walk | `0x024` | 3351 MB | 3213 MB | 0.5 / 0.4 MB | 109.1 + 24.2 MB | 3.6 MB (imports) |
| | `0x064` | 316 MB | 308 MB | 0 / 0 | 5.9 + 1.7 MB | 0.1 MB |
| | `0x022` | 104 MB | 73 MB | 1.2 / 26.4 MB | 2.9 + 0.8 MB | 0.4 MB |

Of the trapped `0x024` pages, 750-756 MB lie in chunks the back end imported (bound as buffers, mirrored).

- **The game's code touched under 1 MB of the 3.35 GB of `0x024` in 35 s, in every scene**, and none of `0x064`. The same
  ~17 functions in the three scenes, on the module system's workers and the main thread: reads by `0x80354060` (~0.2 MB),
  `0x82c1d340`, `0x80361c80`, `0x80362ff0`, `0x82c1d750`, `0x80364080`, `0x803678e0`, `0x80365950`, `0x80357060`,
  `0x8034f440`, `0x80586fd0`, `0x80365f00`, `0x8034cea0`, `0x82692290`, `0x80361a00`; writes by `0x80105170` (~0.2 MB) and
  `0x8575cdc0` (~0.1 MB).
- `0x022` (CPU uncached: per-frame data) is the game's write traffic, as expected: `0x80255be0` (7-8 MB), `0x802a8fc0` (5),
  `0x80364ee0` (4.3), `0x80658660` (3.4), `0x8572f300` (1.5-2.4), `0x8575cdc0` (1-2); reads by `0x812f0fc0` (0.6-0.8 MB).
- The back end reads `0x024` again: texture uploads (`UnswizzleTexture`), the BC cache's hash of a texture's source
  (`bcc_bytes_hash`), the ASTC alpha scan: 18-170 MB in 35 s while textures are made, mostly in the moving scenes; the driver
  locks 1-62 MB more for new imports. Those are the reads a release of these pages after upload has to serve another way
  (a native copy, or keeping the pages of a texture not uploaded yet).
- What this does not show: a page first touched by the back end is given back then, so a game touch after it is not seen
  (18-170 MB of the 3.35 GB); only the pages resident at the arming, in pools older than 2 s, in one 35 s window each; no
  scene change was in the window. `HOS_POOL_TRAP_EVERY=<s>` re-arms, for longer runs.

## 8. Cold pages of CPU-cached pools leave the working set (`HL_MEM_COLD`, 2026-11-13)

### What it is, and what it is not

The pages of the CPU-cached pools (flags `0x024`: textures, vertex and index data; `0x064`: shader code) hold the CPU side
of data the back end already has in VRAM, and section 7 showed game code touching under 1 MB of their 3.6 GB in 35 s. A
thread of the OS layer now **takes those pages out of the process's working set**: `VirtualUnlock` on a range that is not
locked removes its pages from the working set (Windows' documented behaviour; the call fails with `ERROR_NOT_LOCKED` and
trims all the same, and every take-out is checked right after with `QueryWorkingSetEx`). Nothing is decommitted:

- the pages keep their contents. They go to the standby list (through the modified list and a write to the page file, as
  they are dirty: the game wrote them); a later touch by anyone (the game, the back end, the kernel or the driver) is a
  soft fault that maps the same page back, or a hard fault that reads it back from the page file if Windows gave its RAM
  away meanwhile. No handler of ours is involved: **no one can read a wrong byte**, whatever the policy does;
- **RAM is freed, the commit charge is not**: the pages stay committed, backed by the page file (the commit column below
  does not move). On a PC with no page file the dirty pages cannot leave the modified list: the working set falls, the RAM
  in use does not;
- the cost of a wrong guess is a fault per page (a soft one, microseconds; a hard one if RAM was short), and the page file
  writes of the pages taken out (~3.5 GB in a 110 s run, by Windows' modified page writer, not on a game thread).

### How (`host/hl_mem.c`, `native/src/mem/ColdPages.cpp`)

- `host/hl_mem.c`: one thread, a pass every 500 ms. A pass takes the pools with the chosen flags made 10 s ago or more
  (`hos_pools_with_flags`, `host/hos_memsplit.c`: the pool list copied under its lock, nothing else done under it), merged
  into ranges, and goes on from where the last pass stopped: at most 256 MB looked at a pass (the 3.6 GB in ~7 s), in
  slices of 1 MB. It asks `QueryWorkingSetEx` which pages of each 64 KB unit are resident, asks the policy, takes out the
  resident runs of the units it says (at most 64 MB a pass) and tells the policy how many pages left. It holds no lock that
  a game thread or the back end waits for (the only one is the kernel's working-set lock, for each call over at most 64 KB).
- `hl::mem::ColdPages` (native, no guest knowledge; `native/tests/mem_test.cpp`): a 32-bit word a 64 KB unit (pages out,
  times it came back, the pass it may be taken out again). A unit taken out and seen resident again **came back**: it is
  counted and left in for 30 s, 60 s, 120 s after its first, second, third come-back, and for good after the fourth, so
  that pages someone reads all the time are left alone. Units the back end imported are never taken out (the driver pins
  them: `hos_noaccess_import`, which `import_pool` calls before each import, marks them); a pool made over units forgets
  their history.
- Not with large pages (they cannot leave the working set), never while `HOS_POOL_TRAP` measures (it has pages
  `NOACCESS`): the thread is then not started. Windows only; stubs elsewhere.
- Switches: `HL_MEM_COLD=1` (**the default**), `=0`: off, nothing above runs (the two hooks are a load and a branch);
  `HL_MEM_COLD_FLAGS` (hex, default `24,64`; `0x022`, the per-frame pools the game writes ~25 MB of a scene, is left out
  for want of evidence it pays), `HL_MEM_COLD_AGE` (seconds, default 10). Every 10 s a line `hl mem: cold pages: ...`
  gives the counters.

### Validation (main PC, `valid.sh s1v1 s1-cold` against `refv`, the reference build of main, 2026-11-13)

All three scenes ran to the end (110 s), no crash line, game speed ratio 58.5 / 60.4 / 60.0 (kokiri, castle, hyrule; ref
59.1 / 60.0 / 59.3); the last shots of each scene looked at beside the reference shots: the same scenes, complete and right
(the camera and the characters are elsewhere at a given frame, as between any two runs). No visible stutter in the logs'
speed lines.

| Scene | Build | Game RAM | of it GPU pools | `0x024` in RAM | `0x064` in RAM | Process RAM (working set) | Process commit | VRAM | Vulkan in system memory |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| kokiri-idle | ref | 7512 | 3920 | 3129 | 315 | 13102 | 19700 | 5981 | 1391 |
| | `HL_MEM_COLD` | **5435** | 1817 | 1323 | 9 | **10575** | 19965 | 6059 | 1431 |
| castle-move | ref | 7830 | 4163 | 3353 | 315 | 13718 | 21425 | 7137 | 1614 |
| | `HL_MEM_COLD` | **5704** | 2037 | 1535 | 23 | **11342** | 21435 | 6810 | 1668 |
| hyrule-walk | ref | 7801 | 4146 | 3352 | 315 | 13512 | 20687 | 6376 | 1456 |
| | `HL_MEM_COLD` | **5299** | 1772 | 1277 | 18 | **11032** | 20596 | 6491 | 1518 |

MB; game RAM and pools from `HOS_MEM_SPLIT=95`, the rest from `RX_VK_RES_LOG=1`'s last lines (`tools/bench/steptable.py`);
one run each.

- **The game's RAM: -2.1 to -2.5 GB in every scene; the process's working set: -2.4 to -2.5 GB. The commit charge does not
  move** (+265 / +10 / -91 MB: run-to-run noise), as it should not.
- What stays of `0x024` (1.3-1.5 GB) is mostly in the 16 MB chunks the back end imported around buffer bindings (26,000 to
  28,000 units of 64 KB, ~1.7 GB of address space, marked imported by the end of the runs): the driver has them locked.
  Imports cut to the bound ranges would let more go (a back-end change, for brick 1.2).
- The counters (`hl mem: cold pages:`, end of each run): taken out 3.4-3.8 GB in all; out of the working set at the end
  2.1-2.5 GB; came back 1.06-1.37 GB in 18,000-23,000 units. The come-backs are not spread over the run: **~1 GB of them
  in one 10-20 s window at the scene's load** (memory of long-lived pools reused for the new scene's resources: the
  decompression writes the new data over pages taken out, which brings them back and overwrites their old bytes, a soft
  fault each), then 0-15 MB per 10 s while the scene plays, with bursts of 100-175 MB per 10 s in the moving scenes
  (castle, hyrule: new textures made from storage taken out, read by the back end's uploads). No unit came back four times
  (none left in for good); no take-out failed (0 MB stayed in). A pass took at most 15-22 ms of its own thread.
- Not measured, by the rule of the night (no speed study): the time of the faults on the threads that take them, and what
  a scene change costs when the pages taken out were written to the page file and their RAM reused (hard faults on the
  loading threads). On a PC short of RAM that is the case to watch.

### Not verified

- The PC 3050 (8 GB of VRAM, evicts textures: more re-uploads read pages taken out) was not run.
- Long sessions (more than 110 s), several scene changes in a row, a PC with a small page file or none.
- `HOS_LARGE_PAGES=1` and `HOS_POOL_TRAP` with the switch on: by reading only (the thread is not started then).

## 9. Specification: true decommit with an exact restore (the next brick; read, not built)

Section 8 frees RAM but keeps the commit charge and writes the pages to the page file. Giving the pages back for good
(`MEM_DECOMMIT`) needs a way to make the same bytes again when someone touches a page, from a copy this engine controls.
Two exist, for two kinds of data.

### (i) Buffer storage mirrored in VRAM: restore by reading the mirror back

- What is there: on a discrete GPU the back end gives every pool bound as a buffer a device-local mirror of the **whole
  pool** (`gpu/vk/vk_memory.cpp`, `mirror_make`; budget `RX_VK_MIRROR_MB`). CPU-written pages reach the mirror at a
  submission's first use (from the capture of the submit that wrote them); GPU writes come back to guest memory through
  the write-back shadow. So for a page with no CPU write since its last mirror sync (`PageX::syn` against the page's
  generation, `vk_memory.cpp`) and no GPU write pending, the mirror holds its bytes exactly.
- What a restore needs: (1) the pages made `PAGE_NOACCESS` before the decommit, and a handler for the touch that commits
  the page and copies it from the mirror (a GPU copy to a host-visible buffer and a wait); the handler can run on the
  replay's own thread (texture uploads read pool memory, section 7) and must then not wait for work that thread would
  submit; (2) **the imports dropped first**: every mirrored pool lies in 16 MB chunks the driver locked (`import_pool`), and
  a locked page cannot be decommitted. The mirror syncs copy from the captures since TICK-019, but guest memory through the
  import is still the source for pages not in a kept capture (`cap_entry`: "else from guest memory") and for the
  write-backs: both would have to go through staging; (3) a page the CPU writes after the decommit is committed again and
  written (its write-track mark makes the next sync copy it).
- What it is worth: the `0x024` pages that stay resident under section 8 are mostly these (1.3-1.5 GB, in imported chunks),
  plus the commit of every mirrored pool.

### (ii) Data the game decompresses from its files into pool memory: restore by decoding again

What was established by reading the lifted code (no run):

- **The zstd frame decoder `0x8010b380`** (dictionary support, 5,646 lines) has three callers:
  - `0x80108da0`, the resource loader's decode of a file (reached only through a vtable: slot 2 of `0x84c377e8`, made by
    `0x836dea10`, in the `ares` decompression system). Its second argument is a request whose fields give: `+16` a heap,
    `+28` an alignment, `+32` a destination already given (or 0), `+40` a size limit, `+56` the file object, `+64` a value
    passed on to the read. It reads the frame header (`0x80109790`), takes the **decompressed size from the frame header**,
    rounded up to 32, and gets the destination: the given buffer (`+32`), else an allocation from the request's heap (its
    virtual slot 6, the heap's alloc, with the alignment), else `aligned_alloc`. The **source is the decompressor's own
    work buffer** (in the object at its first argument's `+1432`, 32-byte aligned past a 290,816-byte workspace), filled by
    the file object's virtual read (`+144`): never pool memory. Two cases:
    - the compressed file fits in one chunk (the object's `+1424`): one read, then one call
      `0x8010b380(workspace, destination, size, source, compressed size, 0, 0, ...)`: **the whole file decoded into the
      destination at once**;
    - it does not: the loading thread fills a descriptor in the object (`+1456`..`+1640`: destination `+1464`, its size
      `+1472`, the workspace `+1480`, the source buffers from `+1608`, a step of 128 KB at `+1496`), hands it to another
      thread through a callback (`+1640`) and light events (`+1544`, `+1592`: `nn::os::WaitLightEvent` and
      `ClearLightEvent`, answered by the OS layer), and reads the file chunk after chunk into two buffers in turn while that
      thread decodes them (by their creator, the `ParallelDecompThread`s of `0x8069a5e0`; the decoding side's body was not
      read). The output is still one destination for the whole file, written in order.
  - `0x80c17040` (through a vtable made by `0x80d331e0`): decodes a block embedded in data already in memory (its fourth
    argument, checked for the magic `TARB`; compressed size at `+16`, data at the offset at `+10`) into a buffer of a ring
    in the caller's object (`+31216`/`+31224`): a work buffer, not resource storage.
  - `0x80f700d0`: the ROM cache and the heap size table (`HeapSizeDB`), at start.
- **So a resource file's decoded bytes land in one buffer, from the request's heap or given by the caller, written by one
  decoder call or one ordered chunked decode, from one file read in order.** Whether that buffer is pool memory is not read
  on this path: the GPU resource heap `ForResourceG` is a heap over an NVN pool (section 3: `0x83bada70`, `0x809df570`), and
  section 6 found the `0x021` pools filled before `nvnMemoryPoolInitialize` was called on them, which is what a pool made
  over a file already decoded into a heap looks like *(inference: a resource file's GPU section made a pool in place, so the
  pool's bytes are the file's decoded bytes at an offset)*. Section 8's come-backs at a scene load (the decode writing into
  long-lived pools' pages) agree.
- **The other two writers the trap named are not the decoder**: `0x8575cdc0` is the SDK's `memcpy` (its callers into
  pools were not identified); `0x80105170` is reached through a function table at `0x84b400e8`, makes virtual calls on its
  objects and writes 64-bit words in unrolled loops: an update of data in place, not a decompressor (it is only near the
  zstd code by address). Together 0.3 MB in 35 s (section 7): pages they write after a decode can no longer be made again
  by decoding.

What the next brick (native streaming) needs, in order:

1. **Provenance**, measured before anything is decommitted: an entry gate (`HL_GATE`) on `0x8010b380` answered by a native
   zstd decoder giving the same bytes (compared byte for byte with the translated decoder on live inputs, rule 7), which
   records for each output range: the file (from the request's file object, `+56`: how it names its file and archive member
   is still to be read), the offset and length of the compressed frame, the dictionary (`zsdic`, by the dictionary id in
   the frame header), the destination and the decompressed size. A table of ranges by guest address. The chunked path
   decodes on the other thread: to be confirmed that it goes through the same decoder.
2. **Which ranges are pool storage and still the decoder's bytes**: a range is a candidate when it lies in a CPU-cached
   pool and no page of it was written since its decode (the write-track marks, `A64_WRITE_TRACK`, per 4 KB page: the
   writers above and any relocation of the CPU part of a file show there).
3. **Decommit and restore**: candidate pages decommitted once the back end has consumed them (section 8's policy says which
   stay cold). A touch reaches `commit_on_touch` (`host/mkw_glue.c`), which today commits a zero page; it would look the
   page up in the provenance table, read the frame from the file, decode it into a scratch buffer and copy the page (or the
   whole range) in. That handler can run on any thread, the back end's included, and does file I/O; a frame is the unit of
   a zstd decode, so a page costs reading and decoding up to its whole frame. A wrong decommit costs milliseconds on the
   thread that touched.
4. **Not covered**: data copied into pools with `memcpy` (no decode on the way), pools the GPU fills, and files read
   uncompressed straight into their destination (the same provenance without the decode, cheaper).

Not read, and needed before building: the decoding thread's body on the chunked path; the file object's read (`+144`) and
how a request names its file; which requests allocate from `ForResourceG` (the request's heap at `+16`, by resource kind);
where `0x80c17040`'s input comes from.

## 10. What is in the heaps (measured 2026-11-13)

`HOS_HEAP_SPLIT=<seconds>` (`host/hos_heapsplit.c`, off by default; with it unset a constructor reads the variable and
returns) walks the game's heap tree once from a host thread and counts, for every heap, the resident pages that are its own
(in its range and in no child's), outside and inside GPU pools; `HOS_HEAP_SPLIT_COLD=<seconds>` then takes every resident
heap page outside the pools out of the working set once (`VirtualUnlock` of an unlocked range: the contents stay) and counts,
that long after, which came back. Main PC, the three scenes (`build/s5/s4v2-*`, validation runs: no crash, game speed ratio
59.6-60.4, the last shots of each scene looked at and right), the walk at 74 s after the start, a cold window of 20 s,
`HL_MEM_COLD=0` (stream 1's cold-page thread off, so that the two do not take pages out at the same time). The save puts all
three scenes in the castle town market; the scenes differ by what the player does there.

### The heap tree, as read (additions to section 2)

- **Three roots**: the root array at `0x84e9f400` (count `+0`, pointers at `+8`: one heap, `RootHeap`, free-list class, over
  the 6.86 GB heap after the 36 MB `StandardAllocator`), the second array at `0x84e9f430` (empty in these runs), and the
  pointer at `0x84e9f460`, set by `0x809e20c0`: the **`NinVirtualAddressHeap`** (class `0x84c17628`), in no array.
- **Child heaps** are in the list whose sentinel is at `+64` (node: `+0` previous, `+8` next; `+72` the first node), the node
  at `+84` in the child (88): this is the list `0x80503f00` walks to find the heap that holds an address. The list at `+104`
  (node at `+16` in the child, count `+120`) is the **disposers'** list: child heaps and other objects that end with the heap
  (section 2's table called it the children list; it holds non-heap objects too).
- Every heap vtable has the base's `0x809dfe30` in slot 29: that is how the walk tells a heap from another object.
- **Free-list heap blocks** (class `0x84c172c8`, alloc `0x809df950` -> `0x801e7ff0` / `0x801e8d00` / `0x801e7b90` /
  `0x801e8b00`; `0x84c177a8` shares its free list): a 32-byte header (the list node at the offset the heap's `+196` gives;
  `+16` a byte: the tail slack under 33 bytes left in the block; `+18` 16 bits: the heap's id, `0xffff` when free; `+20` 16
  bits: the alignment padding; `+24` the size after the header), the data at header + 32 + padding. Free blocks are in the
  list at `+176` (first node at `+184`, count `+192`), used blocks in the list at `+200` (node offset `+220`, count `+216`).
  `+172` (a byte) chooses how the free list is searched *(inference: first fit, best fit)*. The free size (slot 16, `0x834916d0`) is the sum of the free blocks'
  sizes. No fill of a block's data was seen in alloc or free (free's two helpers `0x83491650` / `0x83491690`, *(inference)* the
  merging of neighbours, were not read).
- **Free size of the other classes** (their slot 16): frame `0x84c17400` and `0x84c178e0`: `+184` minus `+176`;
  `0x84c17c08`: `+192`; `0x84c1acb8`: the size minus the sizes 8 bytes before each node of a list at `+176`; `0x84c179e8`:
  `+7912` of the object at `+176`.
- **`NinVirtualAddressHeap`**: `+40`/`+48` are a budget (9.47 GB), not a range. Its alloc (slot 6, `0x8080fd70` ->
  `0x8080d440`) takes an address region of its own for each large allocation (`nn::os::AllocateAddressRegion` +
  `AllocateMemoryPages`, the import stubs `0x8510a1e0` / `0x8510a220`; a list at `+248`, count `+256`) and packs allocations
  of 15 pages or less into 64 KB regions with a 16-bit page map (list at `+176`, count `+192`); `+264` adds up the bytes
  mapped; `+224` / `+232` hold the lowest and highest address of its regions. Its children are the **resource files' heaps**:
  one heap per loaded file, named after the file (`Model/<name>.bfres`, `Tex/<name>.bntx`, `Pack/...`, `Effect/...`), a frame
  heap or a free-list heap sized to the file, in its own region. 17,300-19,600 heaps in all, ~19,000 of them resource files.
- **Locks**: each heap has an `nn::os` mutex at `+136` (taken when `+168` bit 0 is set). The host's mutex records its owner as
  the calling guest thread (`nn_os_sync.c`), which a host thread does not have, so the tool does not take them: a walk can race
  a change. Every read is checked against committed readable memory, every pointer against the heap's own range, every walk is
  capped; a walk that leaves its range is marked torn (none were in these runs). What it can miscount: blocks taken or given
  back during the walk, heaps made or ended during the enumeration (~2 s).

### Totals (MB in RAM, 74 s)

| | kokiri-idle | castle-move | hyrule-walk |
|---|---:|---:|---:|
| the game's memory in RAM | 7775 | 7804 | 7788 |
| in heaps, outside GPU pools | 3531 | 3535 | 3537 |
| ... of it in the heap (the 6.9 GB `SetMemoryHeapSize` block) | 858 | 858 | 866 |
| ... of it in address regions (`NinVirtualAddressHeap` and its children) | 2673 | 2677 | 2671 |
| in GPU pools (all inside heaps) | 4145 | 4153 | 4151 |
| in neither (the image 96, the system region 2, and the rest) | 99 | 116 | 100 |
| free blocks' pages in RAM (inside free blocks of free-list heaps) | 159 | 160 | 168 |
| heap pages outside pools taken out once | 3422 | 3413 | 3423 |
| **... not back in the working set 20 s later (cold)** | **3328** | **3242** | **3215** |

`HOS_MEM_SPLIT=70` in the same runs counted 4144-4152 MB in pools and 3627-3634 MB outside: the two tools agree. **The 3.6 GB
outside the pools is 97% inside heaps, and 94% of it was not touched in 20 s.**

### By kind (castle-move; heaps grouped by resource folder and file kind, or by name)

| Group | Heaps | Size MB | RAM outside pools MB | RAM in pools MB | Cold of taken out MB | What it is |
|---|---:|---:|---:|---:|---:|---|
| `Tex/*.bntx` | 5,167 | 1703 | **1702** | 0 | **1702 of 1702** | texture files, each in a frame heap of its own sized to the file, in address regions; nothing of them in a GPU pool |
| `Model/*.bfres` | 1,358 | 1522 | 467 | 1053 | 372 of 443 | model files (free-list heaps): their GPU part in pools, the rest (and the `*_Animation.anim.bfres`, 40 MB each for the player) outside |
| `agl::TextureDataMgr::InitializeHeap` | 1 | 2443 | 1 | 2116 | - | the textures the GPU reads: a pool-backed heap |
| `Effect/*.byml` | 174 | 134 | 102 | 31 | 102 of 102 | effect files |
| `Graphics` (+ its sub-heap) | 2 | 1044 | 103 | 47 | 96 of 99 | the graphics module's heap (17,000 blocks) |
| `NinVirtualAddressHeap` own | 1 | 4474 (span) | 113 | 0 | 101 of 109 | its small allocations, and *(inference)* the host's own regions in its span (the renderer's guest memory, the window image) |
| `Movie` | 1 | 206 | 96 | 0 | 96 of 96 | **one free block of 206 MB, 96 MB of it in RAM** (a movie played at start and freed) |
| `Npc_*_Child` actor heaps | 52 | 105 | 104 | 0 | 76 of 90 | actor instances (class `0x84c178e0`) |
| `Actor` | 2 | 404 | 70 | 6 | 64 of 68 | |
| `Pack/*.pack` | 1,416 | 75 | 72 | 0 | 70 of 71 | |
| `Sound/*.bars` | 150 | 68 | 68 | 0 | 65 of 67 | |
| `Bake/*.bkprb` | 35 | 45 | 44 | 0 | 44 of 44 | light probes |
| `Shader/*.bfsha` | 4 | 311 | 36 | 275 | 34 of 35 | |
| `GameUIWorkHeap` | 1 | 25 | 25 | 0 | 25 of 25 | **one free block of 25 MB, all in RAM** |
| `ares::System` | 1 | 39 | 28 | 0 | 21 of 25 | the resource system's own |
| everything else | | | ~420 | ~630 | | |

The other two scenes give the same groups within a few MB (`Tex/*.bntx`: 1708 MB in RAM, 1708 / 1700 MB cold).

### Top 15 heaps by RAM (castle-move, a heap's own pages, outside and inside pools)

| # | Heap | Class | Size MB | Used / free MB | RAM own MB | RAM in pools MB | Free blocks' pages in RAM MB | Cold in 20 s (of taken out) MB |
|---:|---|---|---:|---:|---:|---:|---:|---:|
| 1 | `agl::TextureDataMgr::InitializeHeap` | free-list | 2443 | 2443 / 0 | 1 | 2116 | 0 | 1 (of 1) |
| 2 | `Shader/Model.Product.110.product.Nin_Ounce_NVN2.bfsha` | frame | 306 | 306 / 0 | 34 | 272 | | 32 (of 33) |
| 3 | `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.bntx` | frame | 233 | 233 / 0 | 1 | 217 | | 1 (of 1) |
| 4 | `Graphics` | free-list | 1044 | 1044 / 0 | 103 | 47 | 0 | 96 (of 99) |
| 5 | `gsys::SystemTask` | free-list | 639 | 639 / 0 | 24 | 123 | 0 | 6 (of 21) |
| 6 | `NinVirtualAddressHeap` | virt-addr | 4474 | ? | 113 | 0 | | 101 (of 109) |
| 7 | `Foliage Scene Heap` | free-list | 248 | 98 / 150 | 7 | 97 | 6 | 7 (of 7) |
| 8 | `Movie` | free-list | 206 | 0 / 206 | 96 | 0 | 96 | 96 (of 96) |
| 9 | `Actor` | free-list | 399 | 399 / 0 | 68 | 6 | 0 | 62 (of 66) |
| 10 | `CreateModelScene` | free-list | 711 | 606 / 105 | 20 | 37 | 6 | 14 (of 15) |
| 11 | `Effect/static.Nin_Ounce_NVN2.vsetb.byml` | frame | 50 | 50 / 0 | 32 | 18 | | 32 (of 32) |
| 12 | `Model/Player_Child_01_Animation.anim.bfres` | free-list | 42 | 42 / 0 | 42 | 0 | 0 | 42 (of 42) |
| 13 | `Model/Player_Child_03_Animation.anim.bfres` | free-list | 42 | 42 / 0 | 42 | 0 | 0 | 42 (of 42) |
| 14 | `ProbeHeap` | free-list | 240 | 37 / 203 | 4 | 37 | 4 | 4 (of 4) |
| 15 | `Model/Land_HrlCastle_GardenInside_A...bfres` | free-list | 37 | 37 / 0 | 1 | 36 | 0 | 0 (of 1) |

"Used" is the size minus the heap's own free count (it includes block headers and the heap object). A container heap
(`RootHeap`, `sead::TaskMgr`, `ModuleSystem`) owns almost nothing itself: its children do. The individual `Tex/*.bntx` heaps
are small (70 KB-1 MB each); as a group they are the largest owner of RAM outside the pools.

### What it says

- **A native allocator is not where the memory is.** The heaps' own slack is small: 160 MB in RAM inside free blocks (96 MB of
  it the `Movie` heap's one free block, 25 MB `GameUIWorkHeap`'s); the large free spans (`ModuleSystem` 589 MB, `ProbeHeap` 203
  MB, `Foliage Scene Heap` 150 MB, `CreateModelScene` 105 MB) are not in RAM (committed on first touch, never touched). What
  is in RAM is **data the game loaded and keeps**: resource files (textures 1.7 GB, models' non-GPU part 0.47 GB, effects,
  packs, sounds, probes: ~2.5 GB) and module heaps.
- **1.7 GB of texture files sit in RAM outside any GPU pool and were not touched in 20 s, in every scene.** The textures the
  GPU reads are in the `agl::TextureDataMgr` pool (2.1 GB in RAM); the `Tex/*.bntx` heaps hold the files as loaded.
  *(inference, not established: the texture data was copied from the file into the TextureDataMgr pool and the file is kept
  only for its metadata or until the resource is released; whether any code reads a `.bntx` file's bytes after the copy was not
  traced.)*
- Cold within 20 s is not "never read": the window is short, and a page read once a minute is cold here.

### First recommendation for `hl::mem` (brick 4.2 specifies the rest)

1. **Leave the resource files' cold pages to the system**, starting with `Tex/*.bntx`: expected **~1.7 GB** of working set
   (1702 MB measured cold in each scene), then `Model/*.bfres` outside pools (~0.37-0.39 GB cold), `Effect`, `Pack`, `Sound`,
   `Bake` (~0.28 GB). Mechanism: stream 1's cold-page thread (`HL_MEM_COLD`) extended from the GPU pools to the heaps whose
   name is a resource path (the tool's walk gives their ranges; their memory is in address regions, whole pages). Pages taken
   out stay committed and come back on a touch (a soft fault), so this needs no proof that the game never reads them, only
   that it rarely does. Evidence: the cold column above; to settle before switching it on: a longer window (60-120 s, or
   re-arming) and a scene change.
2. **Decommit the inside of large free blocks** of free-list heaps: ~0.12 GB (`Movie` 96 MB, `GameUIWorkHeap` 25 MB; 160 MB
   in all). What must stay: the first 32 bytes of each free block (its header and list node); the rest of the block's pages can
   be decommitted, and a later allocation that touches them gets zero pages through `commit_on_touch` (`mkw_glue.c`: the heap
   and the regions commit on first touch since `HOS_LAZY_COMMIT`). No fill was seen in alloc or free (free's merge helpers are still to
   read). Small and low risk.
3. Not worth acting on for RAM: shrinking heaps (their untouched free space costs commit charge, not RAM) and a native
   allocator for a heap class (no slack in RAM to win).

Not established here: whether `.bntx` file bytes are read after the texture is made (only 20 s of coldness); per-heap
allocation rates (no counter: the allocation paths are lifted code and were not hooked); the address-region heap's
large-allocation list (`+248`) was not walked, so its 113 MB of own pages are not split by user; in two of the three scenes the
per-heap `heaptsv` lines were cut by the run's end (the tables above come from the `heaptop`, `heapgroup` and `heapcold`
lines, complete in all three).

## 11. The back end's own caches, its imports, and Vulkan memory in system memory (2026-11-13)

Before: the validation runs of section 8 (main PC, `HL_MEM_COLD` on). After: `valid.sh s1c3v s1-cache3` (main PC, the three
scenes, 110 s each, `HOS_MEM_SPLIT=95 RX_VK_RES_LOG=1`); the previous behaviour with this build: one castle run with the
three switches below off (`s1c2off`). Function and picture: the three scenes ran to the end, no crash line, game speed ratio
60.1 / 60.2 / 60.0; the last shots looked at beside the reference shots: the same scenes, complete and right. Counters are
from the runs' last `RX_VK_RES_LOG` and `BC cache` lines. One run each.

| Scene | Build | Game RAM | of it GPU pools | Process RAM | Commit | VRAM | Vulkan in system memory (heap 1 blocks) | of it BC cache | of it capture buffers |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| kokiri-idle | before | 5435 | 1817 | 10575 | 19965 | 6059 | 1431 | 756 | 619 |
| | after | 5252 | 1616 | **9619** | 19013 | 5936 | **422** | 0 | 402 |
| castle-move | before | 5704 | 2037 | 11342 | 21435 | 6810 | 1668 | 890 | 670 |
| | switches off (`s1c2off`) | 5221 | 1547 | 11069 | 20947 | 6417 | 1650 | 890 | 716 |
| | after | 5211 | 1540 | **9974** | 20445 | 6941 | **553** | 0 | 525 |
| hyrule-walk | before | 5299 | 1772 | 11032 | 20596 | 6491 | 1518 | 853 | 652 |
| | after | 5147 | 1497 | **9860** | 19290 | 6100 | **528** | 0 | 446 |

MB. **Vulkan memory in system memory: -1.0 to -1.1 GB in every scene; the process's working set: -1.0 to -1.4 GB; the
commit charge: -1.0 to -1.3 GB.** The game's RAM moves by -0.1 to -0.5 GB between runs whatever the build (the cold pages'
come-backs at the scene load, section 8): not this change's.

### 11.1 The BC cache (`RX_VK_BC_CACHE_LAZY`, `gpu/vk/vk_texture.cpp`)

- What it holds and why (TICK-051): the BC blocks of every ASTC texture the GPU transcodes, in host-cached system memory, keyed
  by a hash of the ASTC bytes, so that a texture evicted by the VRAM budget and made again is copied from there instead of
  decoded and encoded again (82% of a Market walk's transcodes on an RTX 3050 were of bytes transcoded before). Cap 1024 MB.
- Counted: on the RTX 5070 it stored 1016-1308 textures (756-890 MB) a scene and served **0**: the texture budget (6144 MB)
  is never reached (textures at most 3760 MB), nothing transcoded is evicted. A new counter, the transcodes of bytes
  transcoded before (`bcc_seen`, counted with the switch on or off): 179 in the castle run with every transcode stored and 0
  served (the same bytes made again before the GPU had written their first blocks, which the cache cannot serve), 137-266
  with the change. **No work redone.**
- The change: the cache stores nothing until the textures pass 3/4 of their budget or an ASTC texture uploaded from memory is
  evicted; from then on it stores every transcode, as before. The default is on where the driver allows more than 9.5 GB of
  VRAM, off below: on the PC 3050 (7273 MB allowed, castle walk, one run each) the textures made again were transcoded at the
  scene's load, before either trigger: storing all, 416 came from the cache and 274 were transcoded again; storing from the
  first eviction, 360 and 404; from 3/4 of the budget, 12 and 208 (of 220 made again). There the cache earns its memory and
  keeps it. Storing the bytes transcoded before alone (tried first) held 76-104 MB on the 5070 for no hit: left out.
- Switch: `RX_VK_BC_CACHE_LAZY=1` / `=0` (0: every transcode stored, as before); `RX_VK_BC_CACHE_MB` unchanged. The log line
  `vk: BC cache (RX_VK_BC_CACHE_MB): ...` now gives the transcodes of bytes transcoded before, those not stored, and whether it
  stores; a line says when and why storing began.

### 11.2 The capture buffers (`RX_VK_CAP_FIT`, `RX_VK_CAP_SPARE_SECS`, `gpu/vk/vk_memory.cpp`)

- What they hold and why: each submit's capture copies the pages written since the last one into a host-cached buffer; the
  last 25 are kept (`kKeep`) because the mirror syncs copy from them (TICK-019); retired buffers are kept as spares (up to 12)
  because making one on the submitting thread every submit cost the replay (comment at `cap_free`).
- Counted (new line `vk: capture buffers since the last line`): with best fit alone, 68-886 takes per 600 frames gave a
  spare of over twice the need's size class (the load's 40-64 MB buffers to captures of a few MB), so the 25 kept buffers were
  283-369 MB of which their captures filled 150-173 MB; the fewest spares left after a take was 8-10 of 11 (one or two in use
  at a time); 45-48 buffers made in a run.
- The change, `RX_VK_CAP_FIT=1` (default): a capture takes a spare at most twice its need's size class, else a buffer of its
  size is made; a returned buffer, when 12 spares are kept, takes the place of the largest spare left untaken for 2 s if it
  is smaller (any largest at first: a castle walk's ~50 MB capture a frame then lost its spare every frame, 728-1130 buffers
  made in a run; with the 2 s rule, 93-113). And `RX_VK_CAP_SPARE_SECS=20` (default): a spare untaken for 20 s is freed. It
  freed 0-2 spares a run: in these scenes every spare is taken again within 20 s; it is there for scenes left behind.
- Result: kept + spares 402-525 MB (619-716 before). **Work redone: 93-113 buffers made in a run instead of 45-48** (about
  2 a second while walking, after the load), each a host-cached allocation on the thread that takes the capture. Not measured
  as time (rule of the night).
- Switches: `RX_VK_CAP_FIT=0` and `RX_VK_CAP_SPARE_SECS=0` give the previous behaviour.

### 11.3 The imports that pin game memory: measured, not changed

- What they are for: the back end imports guest memory into Vulkan (`VK_EXT_external_memory_host`) by 16 MB chunk around a
  binding (`import_chunk`, `RX_VK_IMPORT_CHUNK_MB`), because an import cost 5-45 ms in the driver and the game makes ~13 pools a
  second; each import gets a VRAM mirror of its size (section 9 (i)). The driver locks an import's pages.
- Counted: 50-60 live imports of 1622-1832 MB on the main PC. **What stays of `0x024` in RAM under section 8 is not mostly
  these**: on the PC 3050, castle walk, `RX_VK_IMPORT_CHUNK_MB=4` against 16 (one run each): live imports 1444 MB against 1846,
  mirrors 1292 MB against 1686 (VRAM -394 MB), imports 95 against 73 in 315 ms against 344 ms in all, but `0x024` in RAM 1344
  MB against 1355. The pages that stay are mostly the cold pages' come-backs at the scene's load (1.06-1.29 GB come back in the
  10-20 s of the load, section 8), held in for 30 s by the policy, which the memory split at 95 s still sees. This corrects
  section 8's sentence that they sit "mostly in the 16 MB chunks".
- Not changed: smaller chunks would save VRAM (the mirrors) rather than RAM, and the cost to watch is imports made on the
  replay's thread at a binding (a hitch history: 130-147 ms, `import_span`), a timing question this brick does not measure.
  Left for the decommit brick (section 9 (i)), which needs the imports dropped anyway.

### 11.4 Vulkan memory in system memory: what it was

Heap 1 (system) of the `vk: memory heaps` line held 1431-1668 MB in blocks before: the BC cache's 756-890 MB, the capture
buffers' 619-716 MB, and 7 MB of other host memory (the imports are counted in the heap's "used", not in its blocks). After
11.1 and 11.2: 422-553 MB, of which the capture buffers are 402-525 MB. The staging arenas (140 MB) and the streams (96 MB)
are VRAM behind the BAR, not here. Nothing else to take.

### Not verified

- The PC 3050 with the final build (its default keeps the BC cache as before; 11.2 ran there in an earlier form only).
- Long sessions, several scene changes, a scene that evicts on a 12 GB card (the BC cache then stores from its trigger on,
  as before; the textures transcoded before it are transcoded again if made again).
- The time taken by the extra capture buffers made (11.2) and by smaller imports (11.3).

## 12. Cold pages of the game's heaps leave the working set too (`HL_MEM_COLD_HEAPS`, 2026-11-13)

Section 8's mechanism, extended from the CPU-cached GPU pools to the game's heap memory outside the pools (section 10: ~3.5 GB
in RAM, ~94% of it untouched in 20 s). What section 8 says of the mechanism holds unchanged: pages are taken out of the working
set with `VirtualUnlock` on an unlocked range, they stay committed and keep their bytes, a touch brings them back with no
handler of ours (a soft fault; a hard one if Windows gave the RAM away), so **no one can read a wrong byte whatever the policy
does**. **RAM is freed, the commit charge is not** (the pages are page-file-backed; the commit column below does not move).
What it costs when RAM is short (hard faults on the threads that touch pages taken out, a scene change that reads back
memory written to the page file) is **not measured here**.

### Design (`host/hl_mem.c`, `native/src/mem/ColdPages.cpp`)

- **Which ranges: the simple form.** Every committed page of the heap and of the address regions
  (`[HOS_HEAP_BASE, HOS_GUEST_SPACE)`: the 6.9 GB heap, `NinVirtualAddressHeap`'s regions with the resource files' heaps,
  the host's own regions there), except: any GPU pool of any flags or age (the pool policy's, or left alone on purpose:
  `0x022` per-frame, the no-access pools the zero-page thread handles), every guest thread's stack and every live fiber's
  stack (`hos_guest_stacks`, `host/hos_thread.c`: the game gives them, from its heaps), each widened to whole 64 KB units.
  The thread-local blocks, the system region and the image are below the heap and never looked at; units the back end
  imported are never taken out (as section 8). Why not "only heaps named by a resource path" through the heap walk: section 10
  measured the general heaps (`Graphics`, `Actor`, actor heaps, `Movie`'s free block) as cold as the files, the walk races the
  game's allocator (it cannot take the heaps' `nn::os` mutexes, section 10) and would have to run on a timer, and the back-off
  calibrates the simple form by itself: what is touched comes back, is counted, and is left in for 30 s, 60 s, 120 s, then for
  good.
- **What is being written is left alone by residency, not by the write-track marks.** The marks (`A64_MARK`) are read and
  cleared only over the GPU ranges (`host/rx_backend.c` `host_collect`, over `hos_gpu_guest_ranges`); outside them nobody
  clears them, so a heap page's mark says "written once since the start", which every loaded file's page is. Clearing them
  from this thread to make them say "written lately" would hide writes from the back end: when a pool is later made over
  that memory, a new mirror takes from a retired one the pages whose page generations say unchanged (`gpu/vk/vk_memory.cpp`,
  TICK-036 inheritance), and a mark cleared here would never become a generation: stale texels. So instead the heap policy
  has a **settle rule** (`ColdPages::Config::settle_passes`, 20 passes = 10 s): a unit is taken out only at a look 10 s or
  more after an earlier look, and only if it has not gained resident pages since; a unit that gained pages (memory being
  filled: a file decoded into fresh pages) waits again. A unit rewritten in place while fully resident is not caught by this:
  if it is taken out it comes back at once and the back-off holds it.
- **Quiet while a scene loads.** The come-back rate is the signal: when 32 MB or more came back in the last second (pools
  and heaps together; section 8 saw ~1 GB come back in 10-20 s at a scene's load, 0-15 MB per 10 s while playing), no heap
  page is looked at or taken out for 5 s, renewed while it lasts. The pools' scan is unchanged (validated as it is).
- **Budget and pacing.** The same thread and pass (500 ms) as section 8, after the pools: at most 256 MB of heap pages looked
  at a pass, 64 MB taken out, 1,024 `VirtualQuery` calls (the address regions are ~20,000 pieces); the walk goes on from where
  the last pass stopped and starts again at the bottom after a sweep (a sweep took ~5-6 s). No lock a game thread waits for:
  the pool list's (shared, copied out) and the thread and fiber lists' (shared, copied out) only. Not with large pages, not
  while `HOS_POOL_TRAP` measures (the thread is not started), and no heap work while `HOS_HEAP_SPLIT_COLD` measures.
- **Two policies, one per kind**, each with its own counters (`hl_mem_cold_*(kind, ...)`, `HL_MEM_COLD_POOLS`,
  `HL_MEM_COLD_HEAPS`); `forget` (a pool made over units) and the imports go to both. A pool made over heap units the heap
  policy had taken out drops them from its "out now" count.
- **Counters**, every 10 s, one line each: `hl mem: cold pages (pools): ...` and `hl mem: cold pages (heaps): ...`: MB out of
  the working set now, taken out in all and since the last line, came back (MB and units, in all and since the last line,
  and **the most that came back between two lines**: the worst 10 s), stayed in when taken out, units left in for good,
  imported; for the heaps also the seconds spent quiet and the sweeps; the longest pass of each.
- Switches: `HL_MEM_COLD=1` (default) now covers the pools and the heaps; `HL_MEM_COLD_HEAPS=0` keeps the pools only (the heap
  policy is then not even made: the previous behaviour, but for the log line's name); `HL_MEM_COLD=0` turns everything off.
  `HL_MEM_COLD_HEAPS` defaults to 1 after the validation below. Tests: `native/tests/mem_test.cpp` (`settle`,
  `report_worst`).

### Validation (main PC, `valid.sh s4h s4-heaps1` against `valid.sh s4b s4-base`, main built from the same tree, 2026-11-13)

All three scenes ran to the end (112 s), no crash line, game speed ratio at the end 60.2 / 58.5 / 59.7 (kokiri, castle,
hyrule; the base's runs 66.2 / 59.8 / 60.2, the base's kokiri line taken during a slow second). The last shots of each scene
looked at beside the reference shots (`refshots-*`): the market complete and right in all three, no missing geometry, no
tint, no black or white frames. No large slowdown in the runs' own speed lines.

| Scene | Build | Game RAM | of it GPU pools | outside pools | Process RAM (working set) | Process commit | VRAM |
|---|---|---:|---:|---:|---:|---:|---:|
| kokiri-idle | s4-base (pools only) | 5229 | 1599 | 3630 | 10821 | 19396 | 5361 |
| | heaps too | **3298** | 1523 | **1775** | **8309** | 20130 | 5900 |
| castle-move | s4-base | 5690 | 2044 | 3646 | 11243 | 21161 | 6935 |
| | heaps too | **3472** | 1734 | **1738** | **8533** | 21154 | 6805 |
| hyrule-walk | s4-base | 5308 | 1655 | 3653 | 10734 | 20430 | 6291 |
| | heaps too | **3348** | 1754 | **1594** | **8902** | 20619 | 6188 |

MB; game RAM from `HOS_MEM_SPLIT=95` (95 s), the rest from `RX_VK_RES_LOG=1`'s last lines (`tools/bench/steptable.py`); one
run each.

- **The game's RAM: -1.9 to -2.2 GB in every scene (5.2-5.7 GB to 3.3-3.5 GB); outside the pools -1.9 to -2.1 GB; the
  process's working set: -1.8 to -2.7 GB. The commit charge does not move** (+734 / -7 / +189 MB: run-to-run noise), as it
  should not. The pools' part is unchanged within run-to-run differences.
- The heaps' counters at the end of each run (kokiri / castle / hyrule): taken out 2535 / 2612 / 2552 MB in all; out of the
  working set at the end 2105 / 2137 / 2030 MB, and still growing (350 MB taken out in the last 10 s in two scenes: a
  longer run frees more); came back 240 / 266 / 301 MB in 7,700-9,200 units (~10% of what was taken out); the worst 10 s
  60 / 58 / 78 MB; 0.5-0.9 MB stayed in when taken out; no unit left in for good; quiet 24.5 / 32.5 / 24.5 s in all (the
  start's loading, and the bursts in the moving scenes); 16-19 sweeps; a pass took at most 91-108 ms of its own thread (the
  `VirtualQuery` walk of the fragmented address regions and the working-set queries; no game thread waits on it).
- What stays outside the pools (1.6-1.8 GB): the image (96 MB), units not yet taken out at 95 s (the take-out was still
  going on), what came back and is held, the stacks, and the host's own regions the back end touches.

### Not verified

- The PC 3050 was not run; long sessions, several scene changes in a row, a PC short of RAM or with a small page file or none
  (the dirty pages taken out then cannot leave the modified list: the working set falls, the RAM in use does not).
- The time of the soft faults on the threads that take them (no speed study by the rule of the night): the come-back counts
  bound how many there were (60-78 MB, ~15,000-20,000 pages, in the worst 10 s).
- `HOS_HEAP_SPLIT_COLD` and `HOS_LARGE_PAGES=1` with the switch on: by reading only.

### Free blocks' insides decommitted (the brief's second item): not done

Read: `0x83491650` and `0x83491690`, which section 10 took for the free path's merge helpers, are the heap's lock and unlock
(the `nn::os` mutex at `+136`, taken when `+168` bit 0 is set); the merging is in the free function itself, and it writes
only block headers (the size at `+24`, the `0xffff` id at `+18`, the list node). So nothing in the allocator reads a free
block's inside. It is still **not safe from this thread**: decommitting must exclude the allocator, which can hand that block
out and have its new owner write it between the check and the decommit (the bytes written would be lost: a zero page on
the next touch), and a host thread cannot take the heap's `nn::os` mutex (section 10). A gate in the free path would be needed
to do it on the game's thread under its own lock. Meanwhile section 12's take-out already moves those free blocks' pages
(`Movie` 96 MB, `GameUIWorkHeap` 25 MB, cold) out of RAM; only their commit charge stays.

## 13. The guest window's substrate: granules given back and restored atomically (`HOS_GUEST_PAGES`, 2026-11-13)

Section 9 and `stream-boundary.md` section 9 (step 3) found what stopped a true decommit: with the window reserved as plain
private memory, a page made again is committed first and filled after, and every thread sees it as zeros in between. This
brick is the substrate that closes it (`host/hos_guestpages.{h,c}`); which pages are dropped and where their bytes come back
from is the policy's (stream M2), through the interface in the header: `hos_gp_drop`, `hos_gp_restore` with a fill
callback, one owner hook called for touched dropped granules, `hos_gp_is_dropped`.

### What Windows allows (a game-free experiment on the main PC, Windows 11 26200)

| Question | Answer |
|---|---|
| A 448 GB placeholder at a fixed address (`VirtualAlloc2` `MEM_RESERVE_PLACEHOLDER`) | yes |
| One private reservation over all of it (`MEM_REPLACE_PLACEHOLDER`), committed in parts later | yes: commit, decommit, `MEM_RESET`, `VirtualProtect`, `VirtualUnlock`, `ReadFile` into it work as on an ordinary reservation |
| A 64 KB piece **inside** that private allocation released to a placeholder (`VirtualFree` `MEM_RELEASE \| MEM_PRESERVE_PLACEHOLDER`) | **yes**: the allocation splits in two around the hole, both halves keep their bytes and commit state (also with the piece only reserved) |
| One release call over two pieces | no (`ERROR_INVALID_PARAMETER`) |
| One `VirtualAlloc(MEM_COMMIT)`, `VirtualProtect`, `MEM_RESET` across two allocations | no (`ERROR_INVALID_ADDRESS`); `VirtualFree(MEM_DECOMMIT)` across two: no (87) |
| `VirtualUnlock` across two allocations; `ReadFile` across two views | yes |
| A section view mapped over a placeholder (`MapViewOfFile3` `MEM_REPLACE_PLACEHOLDER`), filled through another view first | yes, its bytes are there when it appears |
| On a view: `VirtualProtect`, `VirtualUnlock`, `MEM_RESET`, `ReadFile` into it, `VirtualAlloc(MEM_COMMIT)` (no-op) | yes; `MEM_DECOMMIT` no |
| Half of a view unmapped | no: a view goes whole (`UnmapViewOfFile2` `MEM_PRESERVE_PLACEHOLDER`, 5 us) |
| Cost of a restore | 36 us per 64 KB with a section per granule; 25 us per 64 KB as one 8 MB section (copy included) |
| Cost of a private 64 KB piece released to a placeholder | 2.5 us |

So option (a) of the specification holds and nothing needed the fallback (c). The granule is 64 KB.

### How (`host/hos_guestpages.c`)

- **Reservation.** With `HOS_GUEST_PAGES=1`, `[0, HOS_HEAP_BASE)` (system region, image) is reserved as before and
  `[HOS_HEAP_BASE, HOS_GUEST_SPACE)` (heap and address regions, 448 GB) as a placeholder replaced at once by one private
  reservation (`mkw_glue.c` `hos_reserve_guest_pages`). With nothing dropped that reservation behaves as the old one in every
  respect. Only with the soft write watch (`A64_WRITE_TRACK`, the default build): a placeholder cannot be write-watched.
- **Drop**: each granule released to a placeholder of its own (a restored one: its view unmapped to a placeholder). No RAM,
  no commit charge. A granule the back end ever imported is refused (the driver locks those pages).
- **Restore**: a page-file-backed section the size of the range, mapped privately, filled by the callback, then one view per
  dropped granule mapped in place. A view per granule so that any granule can be dropped again alone; the section's commit
  goes with its last view.
- **Touches.** Three bitmaps (dropped, apart from the big reservation, imported) and one of granules being worked on; a drop,
  restore, forget or import claims its granules, and an overlapping one waits (a lock and a condition variable). The
  exception handler (`commit_on_touch`) hands a touched dropped granule to the owner, whose restore waits for whatever holds
  it, and runs the instruction again. Kernel and driver do not fault: `hos_commit` (before a system call gets a guest buffer)
  and `hos_noaccess_import` (before an import) ask the owner first.
- **Split reservation.** A dropped or restored granule splits the reservation, and one commit, decommit or reset call cannot
  cross it: `hos_commit`, `FreeMemoryPages`, `UnmapPhysicalMemory`, the freed regions' discard and the no-access pools'
  zero-page decommit take `hos_gp_commit` / `hos_gp_decommit` / `hos_gp_reset` (piece by piece) over a range that holds such
  granules (`hos_gp_touches`), and the plain call of before elsewhere. The lazy commit's megabyte already falls back to 64 KB.
  A decommit or a discard forgets the dropped granules wholly inside (a reservation of their own again: zero on the next
  touch, the owner is not asked) and restores those it only cuts first.
- **Back end.** `memory_all_reserved` (no upload of texture storage that is all reserved) does not take a dropped granule for
  reserved (`hos_gp_dropped_in`, exported).
- Linux: `hos_gp_ready()` is 0 (stubs); mmap could do the same later.

Tests: `tests/test_guestpages.py` (`tests/guestpages_test.c`, Windows): a 256 MB window reserved the same way; 8 threads read
a dropped range word by word while another restores it with a 200 ms fill (2,097,152 words read, none but the filled bytes);
touch restores from 8 threads alone; a restore next to memory committed on first touch; a dropped range restored, dropped
and restored again; a write to a dropped page (restored, then written); a failed fill leaves the range dropped; imported
granules refused, an import restores dropped granules first; commit, `ReadFile` across views and private pieces, and a
decommit over dropped, restored and cut granules; a 4 MB region holding dropped granules given up whole, used again,
dropped whole and restored; 20 rounds of 2 MB dropped and touched by 4 threads.

### A trap found in the game: commits fail while a drop splits the reservation

The first self-test runs crashed in two of four runs (castle-move): a lazy commit of a page nothing had dropped failed with
`ERROR_INVALID_ADDRESS`. A game-free test reproduced it: **while one thread splits the private allocation (releases a
granule inside it to a placeholder), `VirtualAlloc(MEM_COMMIT)` of reserved pages elsewhere in the same allocation fails**:
84,561 of 479,668 page commits on two threads during 65,536 splits. First touches of pages already committed are not
affected (no fault in the same test), and nor are restores and re-drops of views (they do not split the big allocation).
So a drop of a private granule takes a lock exclusive (`split_lock`), and every commit, decommit and reset of the window
by the OS layer (`commit_on_touch`'s lazy commit, `hos_commit`, `FreeMemoryPages`, `UnmapPhysicalMemory`, the freed regions'
discard, the no-access pools' zero-page decommit, the piecewise paths) and the back end's commit before an import hold it
shared (`hos_gp_hold`, exported; nothing when off). Never held while the owner is asked (its restore may wait for a drop).
The game-free test has the case (9).

### The self-test in the game (`HOS_GUEST_PAGES_TEST=1`, `host/hos_gpcheck.c`)

No policy: every 3 s a thread takes granules of pools with flags `0x024` / `0x064` made 20 s ago or more that the cold-pages
thread has already taken out of the working set (none of their 16 pages resident, all committed), makes each read-only,
copies its bytes and their hash to a host buffer, drops it, and gives the write access back only by the drop (a writer
that faults meanwhile waits for the drop, then runs its write again over the dropped granule, which restores it). It is
the owner: a touch restores the granule from its buffer, then the granule in guest memory is hashed against the saved hash.
At most 256 MB saved (`HOS_GUEST_PAGES_TEST_MB`), 32 MB dropped a pass. A line every 10 s: drops, restores, mismatches,
refusals, writers held, and how much of the pools' storage imports made undroppable in the last whole sweep.

### Validation (main PC, build `m1-gp`, main 528b17c7 merged, 2026-11-13)

`valid.sh` in the three scenes, three ways: the switch off (`m1off`), on with nothing dropped (`m1on`), on with the self-test
(`m1t`). All nine runs went to the end (112 s), no crash line, game speed ratio 59.8-60.5. The last shots of each `m1on` and
`m1t` scene looked at beside the reference shots: the market complete and right, no missing geometry, no tint, no black or
white frames.

| Scene | Run | Game RAM | of it GPU pools | Process RAM | Process commit | VRAM |
|---|---|---:|---:|---:|---:|---:|
| kokiri-idle | off | 3232 | 1556 | 7616 | 18446 | 5415 |
| | on, nothing dropped | 3119 | 1506 | 7239 | 18861 | 5661 |
| | self-test | 3380 | 1852 | 7599 | 17630 | 5329 |
| castle-move | off | 3596 | 1727 | 7692 | 19802 | 6435 |
| | on, nothing dropped | 3236 | 1543 | 7631 | 20304 | 6969 |
| | self-test | 4003 | 2073 | 8470 | 20221 | 6973 |
| hyrule-walk | off | 2738 | 1631 | 7104 | 19925 | 6565 |
| | on, nothing dropped | 3127 | 1627 | 7512 | 19408 | 6153 |
| | self-test | 4042 | 1934 | 8783 | 19311 | 6279 |

MB; `HOS_MEM_SPLIT=95` and `RX_VK_RES_LOG=1`; one run each. On with nothing dropped is within the run-to-run spread of off
(the reservation is the same but for its two parts). The self-test holds up to 256 MB of saved bytes in host memory and
reads every granule it drops back into RAM first (to copy it), so its RAM is not a saving: it exercises the path.

Self-test counters at the end of each run (kokiri / castle / hyrule): granules dropped 4,670 / 4,241 / 3,729; restored
574 / 1,665 / 1,283, by the handler 539 / 1,554 / 1,172 and before an import 35 / 111 / 111 (none through `hos_commit`: no
system call was given a dropped granule); **hash mismatches 0**; restores refused, drops failed, fills failed, maps failed,
owner declines: 0; writers held during a drop: 0. Drops refused because the range is imported: 0, because a granule is a
candidate only when none of its pages is resident, and pages the driver locked for an import stay resident: the imports
keep those granules out of reach without a refusal being counted (the per-sweep figure of imported granules is not
reliable: the sweep stops at the 256 MB cap). Before the split lock, two of four castle-move self-test runs crashed on a
failed lazy commit; after it, three castle-move runs and the three scenes above ran clean.

### Not verified

- Linux (stubs: `hos_gp_ready()` is 0); `HOS_LARGE_PAGES=1` (not with large pages: the window is reserved as before) and the
  hardware write-watch build (refused, logged).
- `hos_commit`'s path with a dropped granule in the game (0 restores by that path: no system call was given one), the
  forget path in the game (0 forgotten: the self-test drops only pool storage and no pool's memory was given up while
  dropped); both are covered by the game-free test only.
- Kernel writes into a granule the self-test has made read-only for its copy (a `ReadFile` there would fail instead of
  faulting; `hos_commit` before it would make it writable again): not seen, not excluded. A policy that drops granules
  the game may still write must prove them unchanged the same way.
- Long sessions, several scene changes, the PC 3050, a PC short of RAM; the time a restore costs on the thread that touches
  (no speed study by the rule of the night).

## 14. GPU data's system-memory copy given back for good (`HL_MEM_DROP`, `RX_VK_NO_HOST_IMPORT`, 2026-11-13)

Section 13's substrate makes a dropped granule (no RAM, no commit charge) come back atomically on any touch. This brick
is the policy over it: which granules go, where their bytes come back from, and the back end's imports that pinned the
game's memory. Two sources of exact bytes were specified (section 9); (ii), a resource file decoded again, is built;
(i), reading a buffer's VRAM mirror back, is not (see the end).

### Decoded resources dropped (`hl::mem::Drop`, `host/hl_drop.c`)

- **Candidates.** Every decode the stream module records (`stream-boundary.md` 7) is told to the policy with what makes
  it again: the file's name (its per-file heap's), its dictionary id, its compressed and decoded sizes. Only the whole
  64 KB granules inside the range count (the two ends are shared with the heap's other data).
- **When a granule goes**, checked in this order, from a thread of its own every 500 ms: the range is 30 s old or more
  (`HL_MEM_DROP_AGE`), and not held back (a restored range waits 60 s, then 120 s, 240 s, and is left alone for good after
  its fourth restore); the back end has not imported it (the driver pins imported pages; the substrate refuses them too);
  the kernel was not given it in the last 5 s (`hos_commit`); none of its pages is in the working set (cold pages took them
  out and nobody touched them since: the cold-pages clock is reused, `HL_MEM_COLD=0` leaves the age alone); it is
  plain read-write committed memory; and **it is unchanged**: the file is read again from the RomFS and decoded with the
  host's own copy of its dictionary (the bytes step 2 proved exact), the granule is made read-only, compared byte for byte
  with the decode, and dropped only if equal. A granule written since its decode is never dropped. A pass looks at 64 MB of
  granules and stops after 250 ms.
- **The decision and its writers.** While the granules of a range are read-only for the comparison, a writer faults: a
  handler of this module (first in line, only for the range being decided) makes it wait for the decision, then runs the
  instruction again: on the bytes if the granule was kept, through the substrate's restore if it was dropped. The kernel
  (`hos_commit`, which tells the module first and waits for a decision under way there) and the driver (`hos_noaccess_import`,
  the same) never meet a read-only page.
- **The touch** (the substrate's owner hook): every dropped granule of the touched range is restored from the file in one
  atomic restore a run, under the range's lock (a second thread on the same range waits, then finds it restored). The
  kernel's and the import's paths restore only the granules they are given (an import of a 16 MB chunk would otherwise bring
  back every range it cuts). Restores are counted by path: the game (read/write), the back end's threads, the kernel, an import.
- **What may become of a dropped range.** The game's allocator handing it out again: the new owner's first touch restores
  the old bytes and its write lands on them, which is what the memory held. A new decode over it: the same, then the new
  range replaces the old as a candidate (granules of the old range still dropped keep their source). The per-file heap's
  region unmapped, decommitted or discarded: the substrate forgets the granules and tells the module
  (`hos_gp_set_on_forget`), which forgets them and the candidates there; a record the substrate no longer calls dropped is
  forgotten every 10 s as a second line of defence. The source never goes away: the RomFS file is read by its name (the
  mods' file first, as the game reads it) and the dictionaries' host copies are kept for the session.
- Why not a hook at the NVN `Finalize` of pools, buffers and textures (asked by the owner): the ranges dropped are the
  resource files' decoded bytes in the game's heaps; a pool, buffer or texture made over them ends without the memory
  being given up (it stays the heap's allocation, which the game may read or free later). Forgetting a dropped granule
  there would leave the substrate with a dropped granule nobody can restore; keeping it is right whatever the game does
  next. The signal that the bytes are given up is the heap region's unmap, which the substrate's forget hook carries.
- Switches: `HL_MEM_DROP=1` (default **0**), needs `HOS_GUEST_PAGES=1`; `HL_MEM_DROP_CHECK=1` hashes each granule at its
  drop and again at its restore and counts mismatches; `HL_MEM_DROP_LOG=1` a line every 10 s (always with the check):
  MB dropped now and in all, restores by path and a second, a touch's time (mean, most), refusals by reason (imported,
  kernel, in use, changed, no source, not plain, substrate, given up), sources decoded. Tests:
  `native/tests/drop_test.cpp` (a stand-in substrate: drop and exact restore, every refusal, eight threads on one dropped
  range with a slow restore, a writer during the decision, hold and give-up, a new decode over a dropped range, forget,
  reconcile, the import's partial restore).

### The imports given back once the mirror holds the pool (`RX_VK_NO_HOST_IMPORT`, `gpu/vk/vk_memory.cpp`)

- What the import is read for once a pool has its VRAM mirror (read whole: `import_pool`, `pool_lookup`, the mirror's
  make and syncs, the write-backs and their diff, the readback refresh, the texel views and the GPU unswizzle,
  `hos_noaccess_import`): the mirror's first fill; syncs of pages the CPU wrote that no kept capture holds ("live" pages);
  the write-backs of what the GPU wrote (pools bound for writing); readback pages (written by the GPU too); bindings while
  the pool has no usable mirror (not made yet, given back as idle, a parallel replay's segment). The texture paths read
  none here: the log's "GPU reads of imported guest memory" says 0 texel views and 0 MB of unswizzles in the castle walk.
- So a pool with its mirror on, never bound for writing, whose mirror's fill is done, gives its import back at a
  submission's start (freed once the GPU is past every use; the host is then told, `hos_noaccess_unimport`, that those
  pages are no longer pinned, unless another import holds them: cold pages, the drop policy and the substrate may take
  them again). Every use above imports it again first, counted by why; a pool imported again keeps its import 600
  submissions, and for good after its third time (a pool whose live pages come every frame keeps it, as before). Pools
  bound for writing keep their import: their write-backs go through it (the kind that genuinely needs it).
- Switch `RX_VK_NO_HOST_IMPORT=1` (default **0**); a line `vk: imports given back as their mirror holds the pool: ...`
  gives the counts. `memory_guest_imported` and the `vk: imports:` line count only pools that hold an import.

### (i) Buffer storage restored from its VRAM mirror: not built

What it would take, now that the imports can go: a mirrored pool's pages dropped once synced (`PageX::syn` equal to the
page's generation, no GPU write pending), and a touch served by a GPU copy of the granule from the mirror into a
host-visible buffer, waited for. That wait is the hard part: a touch can come from the replay's own thread (texture
uploads read pool memory) while it holds `pools_mu` and records the very command buffer the copy would need, so the
restore needs a queue and command pool of its own and a rule for touches under the back end's locks. Not started in this
session; the decoded resources came first, as the brief ordered.

### In the game (PC 3050, one run a line, 110 s; development and function, not the main PC's figures)

Every run below ran to its end with no crash line, and its last shots were looked at against `build/s5/refshots-<scene>/`:
the market complete, textures and geometry right, nothing black, missing, flickering or tinted (the camera and the
characters elsewhere at a given frame, as between any two runs). **`HL_MEM_DROP_CHECK`: 0 mismatches in every run** (every
restored granule hashed against its hash at the drop). "Base" is the same build with every switch off; "all" is
`RX_VK_CAP_ONLY_GPU=1 RX_VK_NO_HOST_IMPORT=1 HOS_GUEST_PAGES=1 HL_MEM_DROP=1 HL_MEM_DROP_CHECK=1`. MB; game RAM from
`HOS_MEM_SPLIT=95`, the rest from `RX_VK_RES_LOG=1`'s last lines; "dropped" is the drop's own count at the end.

| Scene | Switches | Game RAM | Process RAM | **Process commit** | VRAM | Live imports | Dropped now | Restores (MB) | Imports given back (MB unpinned) |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| castle-move | base | 4063 | 7727 | 19221 | 6412 | 1895 | - | - | - |
| | `RX_VK_NO_HOST_IMPORT=1` alone | 3639 | 7687 | 19328 | 6380 | 716 | - | - | 29 (1179) |
| | `HL_MEM_DROP=1` alone (an earlier build, `m2-d`) | 4478 | 8441 | **17936** | 6657 | 1862 | 1246 | 59 (323) | - |
| | drop + no host import | 4652 | 7596 | **17727** | 6334 | 684 | 1115 | 95 (384) | 29 (1146) |
| | all | 4193 | 7508 | **17688** | 6658 | 716 | 1274 | 91 (364) | 28 (1146) |
| | all, build `m2-f` (partial restores for imports) | 4424 | 8227 | **17584** | 6300 | 716 | 1212 | 130 (405) | 31 (840) |
| | all, `m2-g` (decodes over 32 MB left out: the final build) | 4024 | 7103 | **17307** | 6480 | 700 | 1029 | 271 (401) | 30 (889) |
| kokiri-idle | base | 2509 | 5728 | 16748 | 5132 | 1589 | - | - | - |
| | all | 2509 | 7020 | **15963** | 5643 | 700 | 1085 | 16 (291) | 28 (921) |
| | all, `m2-f` | 2790 | 6070 | **15467** | 5540 | 684 | 1281 | 27 (324) | 31 (891) |
| | all, `m2-g` | 2829 | 6341 | **15320** | 5602 | 700 | 1438 | 246 (441) | 29 (810) |
| hyrule-walk | base | 3410 | 7432 | 18735 | 6214 | 1798 | - | - | - |
| | all | 2537 | 5559 | **15241** | 5379 | 700 | 1479 | 17 (305) | 31 (905) |
| | all, `m2-f` | 3004 | 5658 | **15836** | 5964 | 700 | 1233 | 43 (312) | 29 (1225) |
| | all, `m2-g` | 2796 | 6018 | **15605** | 5873 | 700 | 1370 | 212 (366) | 30 (937) |

What it says:

- **The process's commit charge, with every switch on (the final build `m2-g`): -1.9 GB in the castle walk (19.2 -> 17.3 GB),
  -1.4 GB in kokiri-idle (16.7 -> 15.3), -3.1 GB in hyrule-walk (18.7 -> 15.6)**; -0.8 to -3.5 GB over all the runs with
  the drop on (one run a line; the hyrule base run's commit was high, part of that is the run). The drop gives back 1.0-1.5 GB of decoded resources by the
  end of a 110 s run, and that figure was still growing at every run's end: a range is looked at 30 s after its decode and only
  once cold pages have taken it out of the working set, and the scenes load at 40-60 s; "in use" (granules still resident)
  is the largest refusal by far (17,000-26,000 looks at a granule in a run, each counted). A longer session drops more.
- **The game's RAM does not move with the drop alone**: what is dropped was out of the working set already (that is the
  rule). The gain in RAM is what no longer comes back: the scene loads' come-backs of cold pages (section 8: ~1 GB in 10-20 s)
  are now restores of exactly the granules touched, from the file. No run here changed scene, so that is **not measured**.
- **`RX_VK_NO_HOST_IMPORT` gives back 28-31 imports (1.2-1.4 GB) a run; 0.9-1.2 GB of game memory is no longer pinned**;
  live imports 1.6-1.9 GB -> 0.7 GB. Imported again: 3-6 a run (3 at a mirror given back as idle, 1-2 at a pool's first
  written binding), 2-5 ms in all, 1.5 ms at most; no live page left out of a sync. With it the drop's "imported" refusals
  fall from 10,468 granules (654 MB) to 0-63. VRAM: within run-to-run differences (the mirrors are unchanged).
- Restores: 16-271 a run, 290-440 MB, nearly all at a scene's load; granules come back through the handler (the game and the
  back end reading a texture or a model again) and through the imports of new pools over dropped ranges (5-184 restores a
  run; in the `m2-g` runs those are most of the granules restored, 5,399-6,555 of 5,853-7,053 by the substrate's count,
  each restore a few granules since it is cut to the run the import asks for). A touch: 1-13 ms on average over 10 s, at most
  642-836 ms (a large file decoded whole on the touching thread at a load): see the size limit below.
- `RX_VK_CAP_ONLY_GPU=1` on the PC 3050 (stream 8d asked): 1,317-1,612 capture pages a frame (5.1-6.3 MB), 0 of them in the
  left-out ranges, 0 reads or bindings into them, in all three scenes.
- No large slowdown: the runs' frame counts are those of the base runs (the PC 3050 is not measured for speed).
- The touch's cost: before the size limit, a restore decodes the whole file on the touching thread, and the castle, kokiri
  and hyrule runs had touches of 836, 675 and 764 ms at their loads (large files decoded whole at ~1.8 ms a MB): hitches. Decodes over 32 MB are
  now not candidates (`HL_MEM_DROP_MAX_MB`, 0 for any): 9 files, 854 MB, left out in the castle walk, and the longest touch
  fell to 57-61 ms in the three scenes (build `m2-g`).

### Not verified

- The main PC: every figure above is the PC 3050's, one run each; the commit and RAM columns move by a few hundred MB
  between runs of one build there. Both switches stay off by default until the main PC's validation.
- Scene changes, long sessions, a PC short of RAM or of page file: the restores at a load, and the RAM that no longer comes
  back then, were not measured (no run changed scene).
- The writer-during-decision path in the game: no writer met a granule being decided in any run ("writers waited 0"); it is
  tested without the game only. The kernel's path likewise (0 restores through `hos_commit`).
- `RX_VK_NO_HOST_IMPORT` with the parallel replay (`RX_VK_PARALLEL`), on AMD and Intel drivers (imports freed while live: the
  device losses noted in `retired_imports_give_back` were Intel's and AMD's), and with `RX_VK_IMPORT_HIGH`.
- A drop pass can still take seconds (one of 10.6 s in the castle at its load: a range is decided whole, its file decoded
  and its pages read back from the page file); it runs on its own thread, but a touch of a range on the same lock stripe
  waits for it.
- (i), the mirrors' readback restore, is not built.

### For a session played by hand (zone changes, the log on)

    HOS_GUEST_PAGES=1 HL_MEM_DROP=1 HL_MEM_DROP_CHECK=1 HL_MEM_DROP_LOG=1 RX_VK_NO_HOST_IMPORT=1 RX_VK_CAP_ONLY_GPU=1 RX_VK_RES_LOG=1

and in the log: `hl drop:` (every 10 s: MB dropped now, restores a second and by path, a touch's time, **MISMATCHES**, which
must stay 0), `mem: guest pages:` (the substrate's counters), `vk: imports given back as their mirror holds the pool`
(imports given back and taken again), `vk: process memory` (the commit). A zone change is where the restores and the
come-backs are: the restores a second and the longest touch of the lines around it are the figures to read.

## 15. A touch restores what it needs: the spill; mirrored pools' storage (`HL_MEM_DROP_POOLS`, 2026-11-13, second session)

### Why a decode cannot restore one granule

Read from the frame and block headers of the RomFS's largest files (headers only): every file is **one
zstd frame with an 8 MB window** (the 12 largest: 18-306 MB of content, 148-2,447 blocks), and 86-100% of its compressed
blocks bring their own entropy tables while the rest reuse the previous block's ("treeless"); repeat offsets and the
window's history are carried from block to block too. A block can therefore only be decoded after every block before it:
there is no point to seek to. The chunked files (`stream-boundary.md` 5) are the same single frame, read in chunks. So a
granule deep in a 300 MB file costs a decode of everything before it, on the touching thread: the 836 ms touches of
section 14.

### The spill (`host/hl_drop.c`, `hl::mem::Drop`)

At the drop, the granule's bytes have just been compared byte for byte with the file decoded again (they are exact), and
are written into a slot of a file of 64 KB slots in the temporary folder (`FILE_FLAG_DELETE_ON_CLOSE`, up to
`HL_MEM_DROP_SPILL_MB`, default 4096). A touch then restores the granule touched and `HL_MEM_DROP_AHEAD` (3) after it from
the spill, without decoding; the kernel's and the import's paths restore the run they ask for. A granule without a slot
(the spill full) restores as before, its range decoded whole. The spill holds no commit charge; Windows may keep its pages
in the file cache (standby memory, which anyone's demand takes). The 32 MB limit of section 14 applies only without the
spill. `HL_MEM_DROP_CHECK` hashes every restored granule against its hash at the drop, whichever way it came back. The
drop thread also decodes a file without holding its range's lock (a touch of the same range waited up to 308 ms for it).

Note what this changes: the restore no longer reads the source; the source (the file, below the mirror) is what proves at
the drop that the bytes are the ones it holds.

### Mirrored pools' storage (`HL_MEM_DROP_POOLS`, default 0)

The simplest exact class of section 9 (i): a pool with its mirror on, never bound for writing, whose import was given back
(`RX_VK_NO_HOST_IMPORT`): only the CPU's syncs write that mirror. When its import goes, the back end tells the host
(`hos_mirror_held`) and the pool's range becomes a candidate whose source is the mirror read back (`mkwvk_mirror_read`,
`gpu/vk/vk_memory.cpp`: a copy into a host-visible staging buffer in a command buffer of its own, submitted after everything
submitted before, waited for on the drop thread; `pools_mu` held only while it is recorded, and the mirror not destroyed
under it). How the bytes are known to be in sync: they are compared with guest memory, read-only, byte for byte; a page
the CPU wrote and that no sync has copied yet differs and is refused. Such granules are dropped only into the spill (never
restored by reading the mirror on the touching thread, which may be the replay's, holding the back end's locks). A pool's
`nvnMemoryPoolFinalize` cancels its candidates (`hos_memsplit_pool`); granules already dropped stay, restorable from the
spill, which is what the memory held (it stays the game's heap's).

The drop thread decides a range in batches of 32 granules (2 MB): the spill's writes are made with no lock held, and the
range's lock and the decision's are held over one batch at a time (a touch waited 240-442 ms for a 64 MB range decided in
one go, in the first runs with the pools).

### In the game (PC 3050, one run a line, 110 s)

"All" as in section 14 (`RX_VK_CAP_ONLY_GPU=1 RX_VK_NO_HOST_IMPORT=1 HOS_GUEST_PAGES=1 HL_MEM_DROP=1 HL_MEM_DROP_CHECK=1`),
the spill on (its defaults); "+ pools" adds `HL_MEM_DROP_POOLS=1`. Base: section 14's base runs (the same tree, switches
off). Every run reached its end with no crash line and its last shots are right; **0 mismatches in every run**, 0 granules
restored by a decode (all from the spill).

| Scene | Build | Commit (base -> this) | Dropped now | Of mirrors (in all) | Longest touch (an import's) | Restores (MB) | Mirror reads: mean / most |
|---|---|---:|---:|---:|---:|---:|---|
| castle-move | all, spill (`m2-i`) | 19221 -> 17817 | 1335 | - | 1.9 (2.7) | 588 (118) | - |
| | all + pools, batches of 32 (`m2-l`) | 19221 -> 17845 | 1434 | 182 | 7.5 (3.7) | 732 (154) | 417 / 1158 ms |
| | all + pools, batches of 8 (`m2-m`, final) | 19221 -> 16945 | 1286 | 70 | 11.8 (2.0) | 600 (123) | 187 / 310 ms |
| kokiri-idle | all, spill (`m2-i`) | 16748 -> 15693 | 1639 | - | 1.6 (2.7) | 454 (92) | - |
| | all + pools, batches of 32 (`m2-l`) | 16748 -> 15567 | 1508 | 268 | **25.8** (2.1) | 436 (90) | 185 / 752 ms |
| | all + pools, batches of 8 (`m2-m`, final) | 16748 -> 15634 | 1415 | 195 | 1.6 (4.2) | 475 (96) | 380 / 1091 ms |
| hyrule-walk | all, spill (`m2-i`) | 18735 -> 17788 | 988 | - | 4.5 (5.9) | 580 (151) | - |
| | all + pools, batches of 32 (`m2-l`) | 18735 -> 15872 | 1397 | 43 | 0.7 (5.4) | 486 (123) | 94 / 135 ms |
| | all + pools, batches of 8 (`m2-m`, final) | 18735 -> 15520 | 1589 | 253 | **32.2 (1573)** | 565 (404) | 239 / 1180 ms |

What it says:

- **The spill met the bar for the decoded resources** (`HL_MEM_DROP_POOLS=0`, `m2-i`): the longest touch 1.6-4.5 ms in
  the three scenes (an import's 2.7-5.9 ms); with the pools on, 1.6 and 11.8 ms in kokiri and castle (`m2-m`), 0 granules decoded at a touch, 0 mismatches, and the 32 MB limit lifted (the 9 files, 854 MB, are candidates
  again). Dropped at the end of the runs: 1.0-1.6 GB; the commit charge 0.9-1.4 GB under the base runs.
- **Mirrored pools' storage adds 43-268 MB** in a 110 s run, **and costs a hitch: it stays off (`HL_MEM_DROP_POOLS=0`)**.
  In the hyrule run of the final build an import of a pool's chunk (a pool imported again: its mirror given back as idle,
  or its first written binding) met 280 MB of its dropped granules and restored them all from the spill on the back end's
  thread: 1.57 s, and a game thread touching the same range waited for it (32 ms). A pool that may be imported again is
  a bad candidate; the rule that would exclude it (pools whose mirror is never given back, never written) is not written. Their granules are mostly already candidates as decoded
  resources, or in use, or still imported early in the run; a mirror read takes 94-417 ms on average and up to 1.2 s (a
  copy of a 16-64 MB range queued behind the frame's work) on the drop thread: no game thread waits for it.
- Restores: 436-732 a run, 90-404 MB, all from the spill, read ahead.

### Not verified, and what is left

- The main PC; scene changes and long sessions; a full spill (4 GB: the decode path then takes over for files under
  32 MB, larger files and mirrors are not dropped); a disk that is slow or busy (a touch reads the spill: a cold read from
  a hard disk would cost milliseconds a granule).
- `HL_MEM_DROP_POOLS` restores only from the spill: reading the mirror back at the touch was not built (a touch can come
  from the replay's thread holding the back end's locks: the readback would need its own queue or a rule for those
  touches). Pools bound for writing, and pools whose import is kept, are not candidates.
- The spill changes the brick's nature: the restore's bytes are a copy this module wrote at the drop (exact: compared with
  the source, and hashed); the source (the file, the mirror) is what proves that the bytes may go. A copy on disk is held
  for every dropped granule (1.0-1.6 GB in these runs), in the temporary folder.

## 16. Where the process's memory is outside the game's (`HOS_HOST_SPLIT`, 2026-11-14)

The play session at 2560x1440 (`build/s5/play8.orch.log`) had the game's memory at 2.9 GB of RAM against a working set of
10-11 GB and a private commit of 25-28 GB. This section is the census of the rest, made with a new instrument.

### The instrument (`host/hos_hostsplit.c`, off by default)

`HOS_HOST_SPLIT=<seconds>[,<every>]` walks the whole address space (`VirtualQuery`) at that time, again every `<every>` seconds,
and at a normal exit; every committed page gets an owner and is counted as committed and as resident (`QueryWorkingSetEx`).
Owners, strongest first: a tag (`hos_mem_tag(ptr, size, name)`, exported by the executable; the back end finds it by name as
it does `hos_noaccess_import`); blocks of 256 KB and more of the C and Windows heaps (`HeapWalk`), then the heaps' regions;
host thread stacks (TEB); images by module, and by section for the executable and the back end; the guest window as one line;
files mapped, by name; the rest, private allocations by the size of their allocation. Tagged today: the back end's mapped
host-visible buffers by kind and memory class (`vk captures, cached`, `vk staging, BAR`, `vk BC cache, cached` ...; new
`MemKind::BcCache`), the job trace's records, the granule marks. Not tagged (their tests cut the functions out of the
sources, and a call to the tag would not link there): the trace's tables, the capture table, the pipeline cache save's
buffer; they show as heap blocks of their size. The log has the table, the heaps against `HeapSummary`, a histogram of the
heaps' big blocks, the 30 largest allocations, the private allocations by size class, two lines of totals and one
`timeline` line a census. The walk takes 0.4-0.6 s of the process's address space (the heaps are locked for 60-160 ms).

What the totals showed, and a trap: **pages of VRAM that the driver maps into the process through the BAR** (the staging
arenas, the streams) are "valid" to `QueryWorkingSetEx` and are not in the process's working set. Counted apart, the
reconciliation is exact: pages in RAM against the working set, **-12 to +38 MB** in every census of the good runs (a census
takes half a second while pages move). The private commit charge exceeds the committed private pages by a constant
**248-267 MB** that the walk cannot see (not the BAR: it does not follow it; probably charge for reservations such as thread stacks).

Not done: which call of the back end made a driver allocation. A `VirtualAlloc` hook around the back end's Vulkan calls would
say, and was not written; the driver's memory is reported by the size of its allocations and by when it appears.

### What it found (main PC, RTX 5070, market save, scripted idle, window 1280x720, game output 2560x1440, `HOS_HOST_SPLIT=14,9` and `35,30`; RAM in MB)

Non-game RAM at 60-98 s, in order of size, and its commit:

| # | What | RAM | Committed | When |
|---|---|---:|---:|---|
| 1 | **The driver's arenas**: two private allocations of 1.0 GB each (a third of 0.3-0.4 GB until the first scene change) | 1.6-2.4 GB | 2.0-2.6 GB | **already 1.9 GB resident at 14 s, on the title**, before any scene |
| 2 | **The pipeline cache's save**: `vkGetPipelineCacheData` into one vector, fully resident (`vk_draw.cpp pipeline_cache_save`) | **1.17 GB**, peaks only | the same | 30-45 s: each time 32 pipelines were built since the last save (a scene load); the heaps go from 0.17 to 1.6-1.7 GB and back to 0.5 |
| 3 | **Commit beside VRAM**: 130 private regions of 32-64 MB (plus 3 of 64-128) | 0.07 GB | **4.2 GB** | grows with the scene's VRAM use: 0.3 GB at 14 s, 4.2-4.5 GB at 73-98 s |
| 4 | The C heaps outside the peak: 0.26 GB of small blocks (the texture shadow, tables) and 0.1-0.2 GB of blocks over 256 KB | 0.4-0.55 GB | 0.45-0.5 GB | |
| 5 | Images: `HeyListen.exe` (.text 436 MB committed, 100-116 resident; .data 180) and the driver's DLLs | 0.15-0.28 GB | 0.85-1.05 GB | |
| 6 | Tagged: capture buffers (cached host memory) | 0.1-0.23 GB | the same | |
| - | VRAM mapped through the BAR (not RAM): staging 76-620 MB, streams 96, other 28 | 0 | 0.2-0.7 GB | |

Sums (market, 720 window, 98 s): the game 3.7 GB (window), **non-game RAM 3.4 GB** (+ 1.17 GB at the pipeline cache's save), working
set 7.2 GB; commit charge 15.7 GB, of which the game's window 6-7 GB and the non-game 9 GB (item 3: 4.2, item 1: 2.6, images 1.0, heaps 0.5, rest).
So in a scripted run **about 3.4 GB of RAM and 9 GB of commit are not the game's**. The play session's 8 GB of RAM is not
reproduced by a 110 s scripted run: the run has what a session starts with; what grows in a session (pipelines built in
play into the arenas, more scene changes with a save each, the driver's allocations beside VRAM) was not seen. Of this
session's 8 GB, items 1 and 2 (about 2.5-3.5 GB) are what was found and are present from the first minute.

### What would remove each

1. **Arenas (1.6-2.4 GB RAM, 2-2.6 GB commit).** Present on the title, so it comes from what the back end makes at start (the
   precompile of the program cache and the pipelines in the driver). The audit of `docs/perf-audit.md` measured 5.1 GB with the
   precompile and 1.8 GB without it, and -0.7 GB for compute programs let go (not re-measured here). Lever: build compute
   pipelines on demand (the Market binds ~110 of the ~7800), and let the shader modules go.
2. **Pipeline cache save (+1.17 GB peak).** One buffer for the whole cache, 2-3 times a session, exactly when a scene loads
   and the game's memory is highest. Lever: save at exit or when nothing loads (a rule on idle time), or not at all while
   the cache is only appended to; a smaller cache (the driver's data for 40,000 pipelines: build fewer).
3. **Commit beside VRAM (4.2 GB commit, no RAM).** The driver charges system commit for the VRAM it hands out in 32-64 MB
   blocks; the lever is VRAM use itself (textures, mirrors, render targets). Takes no RAM; counts only against the commit limit.
4. **The C heaps (0.4-0.55 GB).** Nothing unbounded found in these runs; the big blocks are the capture table (13 MB) and
   buffers of the back end; the transient of item 2 is the only large one.
5. **Images (0.15-0.28 GB RAM).** The executable's code and data: 100 MB of its .text is touched; PGO ordering or splitting cold code
   is the only lever; small.

### Not verified

- **The 2560x1440 window and `hyrule-field`:** not run. The one 2560x1440 run (`h1-m1440`) did not reach the scene (VRAM
  1 GB at the end, textures 132 MB), so its table is of the title and loading and shows nothing about resolution; the owner
  needed the main PC and the last runs were not made. Resolution, as the bench sets it, is `HOS_WINDOW_SIZE=<w>x<h>` for the
  window and `HOS_OUTPUT_RES` (`output_res=` in `hookshot_options.ini`, 1 = 2560x1440) for the game's frame, which the slots already have at 2560x1440.
- The census of a long session; which call made which driver allocation; `RX_VK_PRECOMPILE=0` against these numbers.
- The `at exit` census (the harness kills the game).

## 17. What grows (2026-11-14): the owner's 45-minute session, and eight minutes standing still on the PC 3050

### The play session (`build/s5/play8.orch.log`, 17 zones, `HL_TRACE` on; one `vk: process memory` line per 600 presents)

| Counter | line 16 | 168 | 281 | 394 | 562 (end) | Bound? |
|---|---:|---:|---:|---:|---:|---|
| private commit, MB | 10539 | 23067 | 24406 | 24525 | 27866 | none seen: only rises |
| working set, MB | 6105 | 11525 | 10978 | 11248 | 10309 | follows the zone |
| C heaps in use, MB | 1088 | 1198 | 1323 | 1467 | 1504 | none seen (see below) |
| **imports live** (pinned guest ranges with a VRAM mirror each) | 4 (160 MB) | 66 (2102) | 95 (3167) | 148 (4565) | **229 (6615 MB)** | none seen; 46 retired in all |
| capture buffers made | 49 | 550 | 1108 | 1955 | 3955 | churn: kept 70-120 MB, spares 50-240 MB stay flat |
| capture table, MB | 11 | 25 | 27 | 27 | 27 | grows with the pages seen, flat after the third zone |
| BC cache, MB | 0 | 656 | 1023 | 1022 | 1022 | its 1024 MB cap (that build stored everything) |
| graphics pipelines made / bound | 6396 / 25 | 6405 / 2167 | 6432 / 3263 | 6491 / 4713 | 6675 / 6058 | the game's set |
| cold-page units left in for good (`HL_MEM_COLD_HEAPS`) | 0 | 1259 | 5054 | 7703 | **19508** | only rises: a unit that comes back twice is never taken out again |
| drop spill held, MB | 860 | 2665 | 2115 | 1250 | 964 | 4 GB cap |
| VMA device blocks, MB | 992 | 7448 | 7637 | 6707 | 9163 | follows VRAM |

HL_TRACE's own tables are fixed in size, not growing without end: the freed ring (1 M entries of 136 bytes, 136 MB), the live
table (2 M of 64 bytes, 128 MB), 2 MB a thread that has events, 12 MB for the back end's writes: **up to 0.4-0.6 GB of the C
heaps, reached in the first minutes**. The recorder explains why this session started at 1.09 GB of C heaps where an untraced
run has 0.55 GB (PC 3050 below), but not the further +0.4 GB over the session: that is the game side and the host's own
tables, and the two lines that name them are the imports and the cold-page records.

### Standing still, eight minutes (PC 3050, market, `HOS_HOST_SPLIT=60:60`, `h1-3050-long2`; RAM in MB)

| | 61 s | 121 | 182 | 243 | 304 | 365 | 426 | 486 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| guest window | 1181 | 3205 | 2116 | 2130 | 2114 | 2125 | 2110 | 2128 |
| driver arenas (private 512 MB and up) | 1622 | 1774 | 1780 | 1809 | 1823 | 1827 | 1815 | 1815 |
| tagged (captures, BC cache, BAR left out) | 199 | 829 | 887 | 891 | 955 | 943 | 1010 | 964 |
| of it the BC cache | 0 | 640 | 670 | 698 | 704 | 711 | 711 | 719 |
| C heaps | 269 | 481 | 484 | 507 | 511 | 512 | 512 | 513 |
| images | 166 | 233 | 236 | 249 | 250 | 250 | 250 | 250 |
| private 32-64 MB (driver, usually commit only) | 2 | 34 | 34 | 34 | 1398 | 1081 | 1336 | 1080 |
| commit charge | 8836 | 15866 | 16669 | 16396 | 16389 | 16275 | 16412 | - |

Nothing leaks while the player stands: after the load the window, the arenas, the heaps (+30 MB in six minutes), the images, the
imports (48 to 50) and the capture table are flat, and the commit charge is flat at 16.3-16.7 GB. What moves: the BC cache
fills toward its cap (640 to 719 MB; on this 8 GB card it stores everything, bounded at 1024 MB), and from 5 minutes **1.1-1.4
GB of the driver's 32-64 MB regions become resident**, with VRAM at 80% of the card: the driver keeps system-memory backing for
what it holds in VRAM, and it fills in once VRAM is nearly full (not seen on the 12 GB main PC at 40% VRAM: the lever is VRAM
use). The cold-page units left in for good rose by 4246 in the last three lines without the player moving.

### Who grows, and whether it has a bound

1. **Imports and the commit charge they bring** (6.6 GB of live imports at the end of the session, 229 of them): grows with every zone;
   no limit other than the game's pools ending. The commit charge (10 to 28 GB) follows imports plus VMA blocks. Next: read what
   retires an import (46 given back of 275 made) and give back those of pools the game no longer binds.
2. **The cold-page policy's "left in for good" units** (19508): a rule that cannot be undone, so the working set it was written to
   lower rises over a session. Bound: the heap's size, reached after hours.
3. **C heaps** (+0.4 GB over the session beyond the recorder): not attributed; the heap walk names big blocks only. The
   host's tables for pools, drops and imports are the candidates.
4. Bounded: the BC cache (cap), capture buffers and table, the drop's spill (cap), pipelines (the game's set).

## 18. The pipeline cache's save (`RX_VK_PCACHE_MISSES`, `RX_VK_PCACHE_MAP`, 2026-11-14)

What it does: `pipeline_cache_save` (`vk_draw.cpp`) runs every 300 presents on a detached thread when 32 pipelines have been
counted as built since the last save, fetches the whole `VkPipelineCache` with `vkGetPipelineCacheData` into one `std::vector`
and writes it to `mkw_vk_pipelines.bin` through a temporary file (the file is the driver's pipeline data: 0.3 GB on the PC
3050, 1.2 GB on the main PC). The counter counted **every graphics pipeline made, including those the cache already had**, so the
load of any scene counted 32 at once and the 1.2 GB was fetched and written again, with nothing new in it. The replay does not
wait for it (a thread of its own), the cache is internally locked.

Changes (both default on; `=0` is the old behaviour):
- `RX_VK_PCACHE_MISSES`: a graphics pipeline counts only when `VkPipelineCreationFeedback` says the cache did not have it (compute
  already did this).
- `RX_VK_PCACHE_MAP`: the data is fetched into a mapped view of the temporary file: no private commit, no working set that stays.
  Vulkan cannot hand the data over in pieces, so a true stream is not possible; the file's own pages are what is written.
- A log line `vk: pipeline cache saved (...)` gives the process's private commit before the fetch and with the data in hand.

PC 3050, market, scripted, 235 s, one run each (`h1-3050-pcA2` old, `pcB` new): old, two saves, at the title and at the scene
load: private commit 5383 to 5674 MB and 16124 to 16419 MB (+291 and +295 MB for a 287 MB cache), 151 ms each; new: **no save,
no spike** (the run had no misses: 105 compute pipelines from the cache, 0 compiled; the old run had 86 compiled, so it was not a comparable warm start: its saves are real but its cache state differed)
(run-to-run state differs). The mapped writer's own line was not seen in a run (no save was needed with the warm cache); it is
built and tested only by the suite (the vector path's cases, with a stub for the mapped one).

Not verified: that a killed run keeps what it compiled. A cold-cache run (`pcC`, cache file removed, 290 s) was killed
inside the start-up precompile (1242 of 3675 pipelines) before any save, which is also what the old code does there (the
precompile builds into the driver's cache and counts nothing, and the save at its end never came). With misses in play the
rule is the old one (32 built, checked every 300 presents).

## 19. The ledger: every copy, by class of resource (`RX_VK_LEDGER`, 2026-11-14)

The console holds the game's resources once, in ~9.8 GB that the CPU and the GPU share. This engine holds them in guest memory
(RAM and commit), in VRAM mirrors of the pools, in Vulkan images, in the BC cache, in staging and captures, and pins them through
imports. Section 16 counted what is outside the game's memory; this section is the one table that says, class by class, how
many times the game's own bytes are held.

### The instrument (`gpu/vk/vk_ledger.cpp`, `host/hos_hostsplit.c`; off by default)

`RX_VK_LEDGER=<seconds>`: a table every `<seconds>` and at a normal exit, lines `vk: ledger:`. Nothing else changes while it is off
(not a mark, not a count). While it is on:
- **Marks.** One 16-bit word for every 4 KB guest page below 1 TB, in 16 MB leaves made when first touched. The bindings OR in
  what the GPU used the page for: vertex, index and uniform (marked at the draw, whichever way the block reaches the GPU),
  storage, indirect, GPU-written, other bindings (copies, texel buffers). Texture storage is marked when the image is made,
  rendered storage at every GPU write. A pool's marks are cleared when a pool is made over the range. A per-thread memo means
  a range bound draw after draw is marked once.
- **The snapshot**, taken on the replay's thread at a present: the live textures (storage address and size, the Vulkan image's
  bytes and format, ASTC block size, rendered, finalized, depth), the game's pool flags, the live mirrors and imports, the
  pool-table chunks (import, mirror, last bind, last sync), the spare and render-target-pool images, the BC cache.
- **The count**, on a thread of its own:
  - every page of every live pool (the host's counted list) gets a class. The class is the live texture over it (by NVN
    storage: ASTC by block size, other compressed, uncompressed; or color, depth, or finalized-but-kept render targets), else
    its marks, else its pool's flags (shader code, physical, never used by CPU access);
  - the page's guest commit and residency come from the host (`hos_ledger_pages`: `VirtualQuery` and `QueryWorkingSetEx`,
    nothing touched);
  - the mirrors and imports are classified page by page the same way;
  - the rest comes from the allocations by kind (`vk_vma.cpp`), VMA's block statistics, and a quiet census by the host
    (`hos_hostsplit_totals`: the same walk as section 16, its groups, no report; the tags of `hos_mem_tag` are kept while the
    ledger is on).
- **Reconciliation.** VRAM: the rows (allocations by kind, plus free space in VMA blocks, plus VRAM outside VMA, plus
  allocations not counted by kind) against the device heaps' use, which is the `VRAM breakdown` figure. RAM: the census's
  pages against the working set. Each line prints its remainder.
- **Cost.** The snapshot is 2,000-5,000 small records on the replay's thread. The count took 3.9 s on its thread at 90 s, of
  which 0.86 s was the census, and that holds the C heaps' locks for 60-160 ms. Use 30 s or more between tables.

### The ledger at the end of a long run (main PC, RTX 5070, build `r1-led2`, `hyrule-field` save, scripted walk, 420 s, the table at 360 s; window 1280x720, game output 2560x1440; MB)

The run is `build/s5/r1long`. It ran to its end with no crash line, the game-speed ratio was normal, and nothing else ran on
the machine. The table at 120 s and the one at 360 s are within a few MB of each other: the scripted walk never leaves Hyrule
Field.

| Class | Count | Console | Guest committed | Guest RAM | Mirror | Image | Import | Image / console |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| textures ASTC 4x4 | 31 | 11 | 10 | 10 | 1 | 13 | 1 | x1.18 (BC7) |
| textures ASTC 5x5 | 525 | 353 | 228 | 9 | 0 | 600 | 2 | x1.70 (BC7) |
| textures ASTC 6x6 | 167 | 43 | 24 | 1 | 0 | 57 | 0 | x1.32 (BC1 53, BC7 3) |
| textures ASTC 8x5, 8x6 | 241 | 47 | 32 | 0 | 0 | 80 | 0 | x1.70-1.74 (BC1 59, BC7 21) |
| textures ASTC 8x8 | 18 | 114 | 114 | 0 | 0 | 399 | 0 | **x3.49** (BC7 262, BC1 136) |
| textures ASTC 10x8 to 12x12 | 69 | 3 | 1 | 0 | 0 | 11 | 0 | x3.2-5.8 |
| textures BC and other compressed | 831 | 544 | 476 | 31 | 0 | 615 | 22 | x1.13 |
| textures uncompressed | 149 | 157 | 156 | 109 | 71 | 158 | 110 | x1.01 |
| render targets, color | 145 | 324 | 13 | 1 | 9 | 340 | 9 | x1.05 |
| render targets, depth/stencil | 18 | 95 | 0 | 0 | 0 | 115 | 0 | x1.21 |
| render targets, finalized, kept | 37 | 33 | 1 | 0 | 0 | 34 | 16 | x1.02 |
| buffers: vertex / index / uniform | | 131 / 10 / 21 | the same | the same | the same | | the same | mirror x1.00 |
| buffers: storage | | 279 | 279 | 279 | 279 | | 279 | mirror x1.00 |
| buffers: indirect and GPU-written | | 118 | 118 | 118 | 118 | | 118 | mirror x1.00 |
| pools: shader code | | 324 | 324 | 14 | 4 | | 4 | |
| pools never used: CPU cached | | 3303 | 2668 | 465 | 389 | | 421 | |
| pools never used: CPU no access / uncached | | 602 / 69 | 223 / 69 | 217 / 65 | 8 / 51 | | 8 / 51 | |
| game memory outside pools | | 4522 | 4522 | 845 | 0 | | 0 | |
| outside any pool (the import chunks' reach) | | | | | **493** | | **556** | |
| **all game resources** | | **11115** | **9434** | **2336** | **1592** | **2431** | **1768** | VRAM x0.36, RAM x0.21 |

What belongs to no game resource, and the reconciliation:
- **VRAM, 5324 MB used, reconciled with 0 MB left over.** Texture images 2431. Other images 112 (88 of them the render-target
  pool). Live mirrors 1592. Write-back shadows 261. Scratch (BC encode) 160. Streams 96. Staging 12. Other buffers 32. Free in
  VMA blocks 397 (image blocks 120, render-target blocks 240, buffer blocks 36). Outside VMA 228 (pipelines, shader modules,
  descriptor pools, swapchain, the driver's own).
- **RAM: working set 6474 MB; the census counts 6486 (12 MB the other way).**
  - The game's memory: 2336 (pools 1490, outside pools 845).
  - **The driver's private arenas: 3079 in RAM of 3332 committed.** That is the largest single owner, larger than the game.
  - C heaps 480; images 258; captures 146; the BC cache 0 (it stores lazily and nothing was evicted).
  - Private commit 19275 MB: the window 9434, private memory with no owner 8706 (beside VRAM and the arenas, section 16).
- Against the console: **VRAM 5.3 GB + RAM 6.5 GB = 11.8 GB, for 9.4 GB of the game's committed memory.** The game's own
  memory is only 2.3 GB of the RAM.

The market (`r1l1`, castle-move, 90 s; that run overlapped another game on the machine, so only its memory figures count) has
the same structure. Mirrors 1422 MB (521 bound as buffers, 403 never bound, 494 outside every pool, 3 texture-only). Render
targets 1518 MB of images: a **12288x12288 D16 target of 288 MB**, ten 2560x1440 R11G11B10 (150 MB), nine D32 (135 MB),
twenty-three 1280x720 RGBA16F (172 MB), and 65 finalized but kept (212 MB). BC cache 466 MB. Driver arenas 1486 MB in RAM.

### The structural questions, answered

1. **Texture storage is not mirrored, and the pools are not mixed.**
   - Of the 1592 MB of mirrors, 73 MB are pages that only ever held texture storage. 561 MB are pages bound as buffers. 454 MB
     are pool pages never bound. 502 MB lie outside every pool. Market: 3 / 521 / 403 / 494 of 1422.
   - Of 4933 live pools, 67 hold only texture storage (1216 MB of it) and 659 only buffers (561 MB bound). 6 are "mixed",
     every one under 8 MB, and 0 MB was used both ways over the pools' lives.
   - The mixing the owner feared is one level up: an import chunk is 16 MB around a binding (section 11.3), and it takes in
     the neighbouring pools, texture pools among them, and memory outside every pool.
   - **A mirror per bound range** would hold the buffer pages: 1,447 runs over 9,921 granules of 64 KB, 620 MB. By size:
     404 runs of 4 KB or less, 733 of 64 KB or less (13 MB), 237 up to 1 MB (59 MB), 71 up to 16 MB (248 MB), 2 above
     (236 MB). Per 64 KB granule that is about 620 MB, against 1592 of mirrors and 1768 of imports. The thousands of tiny
     runs are 14 MB in all, so per-pool or per-granule mirrors can round them up.
2. **After its upload, who reads a texture's guest bytes or its mirror.**
   - The mirror: nobody. Texture pages are not in mirrors except where a chunk's reach covers them (73 MB).
   - The guest bytes are read again only by the back end: a re-upload when an evicted texture is made again, the BC cache's
     hash and the ASTC alpha scan (section 7), and `UnswizzleTexture` for non-ASTC uploads. The game itself touched under
     1 MB of `0x024` in 35 s (section 7).
   - In this run the textures' guest RAM is already low because cold pages took it out: 5x5 9 MB of 228 committed, 8x8 0
     of 114. Only their commit remains.
   - The BC cache stored 0 here (lazy, and no eviction on a 12 GB card); in the market run it held 466 MB because the lazy
     trigger fired.
3. **Render targets.**
   - In the field at 2560x1440: 145 color targets of 340 MB and 18 depth of 115 MB. The largest are 2560x1440 RGBA8 x5
     (75 MB), R11G11B10 x4 (60 MB), D32 x3 (45 MB), and a 512x512 RGB10A2 target of 64 MB (an array or volume).
   - The sizes are the game's own: `texture_get` takes width and height from the NVN descriptor (`d.width`), and nothing in
     the back end scales a target.
   - The game sizes its frame from the display the host reports (`output_res`, `nn_applet.c`: 1 = 2560x1440 against the
     console's 2048x1152 docked). That is +56% of pixels on every full-frame target.
   - The 12288x12288 D16 shadow target (288 MB, market; also in the owner's and a player's logs) is the game's request.
     Whether it follows the reported display is **not measured**: one `HOS_OUTPUT_RES=0` run against `=1` settles it.
   - "Finalized, kept" (33 MB here, 212 MB in the market) are targets the game finalized that nothing frees under the
     budget. `keep_finalized` (`RX_VK_KEEP_FINALIZED_MB`, 512) keeps the small drawn-once ones on purpose (issues 107, 386).
     The large ones stay only because the eviction pass runs past 7/8 of the texture budget, or for targets drawn over.
     `RX_VK_FREE_FINALIZED` (below) frees them.
4. **The resource files in the heaps** are console bytes: the game holds them on the console too, in the same 9.8 GB. Here
   they are 4522 MB committed outside pools, of which 845 MB is in RAM (cold pages, section 12). The "pools never used"
   (3303 MB of CPU-cached storage) are, by `HOS_HEAP_SPLIT` in the same run, the pool-backed heaps whose data this scene does
   not draw: `agl::TextureDataMgr` (2443 MB, 105 MB in RAM), `ProbeHeap` (214 MB), `Foliage Scene Heap`, `gsys::SystemTask`,
   `CreateModelScene`, and the UI's `Common...bntx` (217 MB). These are console bytes as well. What is ours is only their
   commit, which the drop (section 14) can give back.
5. **The pipeline and program caches in memory** are the driver's private arenas: 3079 MB in RAM of 3332 committed in the
   field (1486 in the market at 90 s). Add the transient 1.17 GB of `vkGetPipelineCacheData` at a save (section 16; H1's fix
   is on main). The program cache is not held: each program is borrowed from `hookshot_programs.bin` (6.4 GB on disk) and let
   go. 3216 graphics pipelines were made, of which 892 were bound.

### What releases a chunk today

The back end was told when a pool is made (`memory_pool_flags`), never when it ends. A chunk left the pool table only in
three ways: a larger import took it in (`retire_pool`), the import-pressure rule fired (`RX_VK_IMPORT_HIGH`, off on Windows),
or `RX_VK_NO_HOST_IMPORT` gave back its import alone (the entry and the mirror stay). A mirror goes only through the idle
rules: 6000 mirror epochs, or 600 when the mirror budget or the 80% VRAM rule presses. So the chunks and mirrors of a zone the
game has left stay for good unless VRAM presses. That is the play sessions' growth (H1: live imports 0.16 -> 6.6 GB over 17
zones). In this run, with no zone change: 49 chunks of 1768 MB hold 1185 MB of live pools, 102 MB of finalized pools and
480 MB of no pool at all. One chunk of 80 MB has no live pool left; it was written by the GPU and last bound 190,000
submissions ago.

### Two levers built behind switches (default 0)

- `RX_VK_POOL_FINALIZE=1` (`host/nvn_native.c`, `host/rx_backend.c`, `gpu/vk/vk_memory.cpp` `finalize_retire`):
  - the host tells the back end at a live pool's `nvnMemoryPoolFinalize` (the branch for a pool that is still live: not the
    double-finalize case);
  - once the replay has taken a capture made after every submit that came before that finalize, each chunk over its range
    with no live pool left (`hos_gpu_live_pools`: a newer pool at the same address keeps the chunk) retires as `retire_pool`
    does. Its mirror is freed after the GPU, its import is given back (`retired_imports_give_back`: on NVIDIA by default),
    there is no inheritance, and the write-backs it left out are dropped;
  - a chunk bound by the recording under way waits one submission, and that is counted;
  - log line: `vk: chunks retired as their game pools all ended`.
- `RX_VK_FREE_FINALIZED=1` (`gpu/vk/vk_texture.cpp`): a finalized render target that `keep_finalized` does not keep goes 3
  presents after its last use, once the GPU is past it, whatever the budget. Nothing samples it after:
  - the game cannot sample a texture it finalized;
  - the BC cache holds ASTC transcodes only;
  - the write-back before eviction is for live targets;
  - the upscaler and the present read their own images and the frame's live target.

  The one risk is a texture made again over the storage and sampled undrawn, for targets over 4 MB.
- Neither switch has run in the game yet: the main PC was taken by the owner's sessions. Both are built (`r1-led2`), and the
  game-free suite passes (the known `nx2/test_nup_paths.py` failure only).

### The levers, ranked by MB (field 360 s; the zone-changing session will rescale the first)

| # | Lever | VRAM | RAM | Commit | Needs | What could break |
|---:|---|---:|---:|---:|---|---|
| 1 | Chunks retired at their pools' finalize (`RX_VK_POOL_FINALIZE`, built) | the mirrors of zones left (play8: imports grew to 6.6 GB) | the pinned imports of zones left | the same | a hand-played session with zone changes | a binding of a retired range before the replay passed the finalize (ordered by captures; counted) |
| 2 | Mirrors per pool or per bound granule, no import (section 17 lever B) | ~970 of 1592 (keep ~620) | the 1768 MB pinned, as cold pages and the drop can then take them | ~1.1 GB (576 outside every pool, 454 never bound) | fill, live pages and write-backs through staging (the lazy release's path); every bound pool mirrored while bound | write-back latency before an invalidate; small-pool churn |
| 3 | The driver's arenas (precompiled pipelines; section 16) | | 1.5-3.1 GB | 3.3 GB | compute pipelines on demand, fewer kept graphics pipelines | first-use hitches |
| 4 | ASTC 8x8 and larger blocks kept as BC1 where there is no alpha, instead of BC7 | ~260 of 399 (8x8 BC7 262 at x4 of ASTC) | | | the alpha scan already decides BC1 against BC7 per texture; widen BC1's use | quality on textures with alpha gradients |
| 5 | Finalized render targets freed (`RX_VK_FREE_FINALIZED`, built) | 33-212 | | | a session | a target made again over its storage and sampled undrawn, over 4 MB |
| 6 | Output resolution: the game's frame at 2048x1152, not 2560x1440 | ~-36% of full-frame targets (~170-300 MB) | | | an A/B run (`HOS_OUTPUT_RES`), and the shadow map's size against it | picture size, the owner's choice |
| 7 | The texture-storage commit dropped once uploaded (the drop, section 14, on by default) | | 0 (already cold) | ~1.0-1.5 GB | the main PC's validation of `HL_MEM_DROP` | restore hitches |
| 8 | Write-back shadows (261 MB) and BC scratch (160 MB) made only while used | ~0.4 GB | | | shadows' 85/95% rule; scratch freed after a load | a stall at the next transcode |

### The owner's reference session (play10: about 18 minutes, zone changes, levers off; `build/s5/play10.orch.log`)

Ledger tables every 60 s, 60 to 1083 s, MB:

| Time | Console | Game committed | Game RAM | Mirrors | Images | Imports | Chunks: live pools / finalized pools / no pool | VRAM used (free in VMA blocks) | Working set | Commit |
|---:|---:|---:|---:|---:|---:|---:|---|---|---:|---:|
| 60 s | 9618 | 7370 | 2662 | 1100 | 2311 | 1260 | 749 / 80 / 430 | 5906 (668) | 8179 | 19129 |
| 603 s | 12997 | 11419 | 4360 | 2235 | 3113 | 3503 | 2215 / 144 / 1143 | 9139 (1733) | 11310 | 28506 |
| 1083 s | 13100 | 11527 | 6041 | 1754 | 3124 | 3854 | 2622 / 100 / 1131 | 9414 (2704) | 10440 | 28926 |

- **The growth is the game's own live pools plus the chunks' reach, not ended pools.**
  - Chunks with no live pool left: 1 to 11, 80-240 MB of import, and few of them hold a mirror. Retiring at finalize
    (`RX_VK_POOL_FINALIZE`) would have released at most 0.24 GB here.
  - The live pools in the chunks grew 0.75 -> 2.6 GB. Vertex buffers alone went 136 -> 842 MB as the models of the zones
    visited stay loaded: the game's memory, bounded by its 9.8 GB.
  - Memory in chunks that no pool covers grew 0.43 -> 1.13 GB.
- **RAM.**
  - The game: +3.4 GB. Its pools went 1.5 -> 4.8 GB in RAM because the imports pin them (+2.6 GB), and cold pages cannot take
    out pinned pages.
  - The BC cache: +1.0 GB (at its 1 GB cap from 363 s, when VRAM reached 93% and the lazy trigger fired).
  - C heaps 0.52 -> 0.86 GB; captures +0.2 GB.
  - The driver's arenas: 4.6 GB committed throughout; their RAM fell 4.4 -> 1.5 GB at about 780 s (Windows trimming,
    *(inference)*).
- **Commit** +9.8 GB: the window +4.2, the driver's private regions of 32-128 MB beside VRAM 0.1 -> 9.5 GB (census; they track
  VRAM use), the BC cache and captures +1.2.
- **VRAM** +3.5 GB: images +0.8, mirrors +0.65, **free space in VMA blocks 0.67 -> 2.7 GB** (image blocks 1.7, render-target
  blocks 0.9). That free space is fragmentation, and `RX_VK_IMAGE_GENERATIONS` (off) is its lever.

### Lever B: a mirror per pool, the import gone (design)

What exists already:
- `RX_VK_IMPORT_CHUNK_MB=0` imports and mirrors the game's pool itself instead of a 16 MB chunk. The chunk exists only for the
  driver's import cost (5-45 ms each, 11.3) and its limit of about 4,096 memory objects. 1,773 pools were bound in play10.
- `RX_VK_NO_HOST_IMPORT=1` gives the import back once the mirror holds a pool that was never written (section 14).

Together they are most of lever B: mirrors per pool, and imports only for pools the GPU writes and for the time a mirror is
being filled. **Stage B1 is a measurement of those two switches** (the runs below). The remaining stages:

- **B2: fill without an import.** `mirror_make` copies the whole pool from its import (`vkCmdCopyBuffer(p->buffer, ...)`).
  Instead, copy guest memory into staging (CPU memcpy, ~1.6 ms per 16 MB) and from there into the mirror. Pages written since
  then come from the captures, as they do today.
- **B3: no import for pools the GPU writes.** Write-backs go GPU -> host staging -> guest memory. The staged lazy release
  already does this (`LazyStage`, `hos_gpu_write_live`, with its checks). An invalidate then waits for that copy too.
- **B4: no fallback to guest memory.** A pool with no usable mirror (budget refused, given back as idle) is read by the GPU
  through its import today. Without imports such a pool must get a mirror at its binding (made and filled in the upload
  command buffer, as now). The idle give-back then frees only the mirror, and a later binding makes it again.

The check: `RX_VK_POOL_MIRROR_CHECK=1` reads each mirror back at a submission's end, after its syncs, and compares it with
guest memory page by page for every page with no CPU write since its sync (`PageX::syn`) and no pending GPU write. Mismatches
are counted, and the first few are logged with the page, its pool's flags and the last writer. Zero is the bar.

What it is worth, in play10's terms:
- imports 3854 -> 0 MB pinned, so cold pages and the drop can take the pools' pages: the game's RAM would sit closer to the
  field run's 2.3 GB than 6.0;
- mirrors 1754 -> the bound pools' own size: the 528 MB outside every pool and the 433 MB never bound go.

What could break:
- the fill's CPU copy on the replay's thread at a scene load;
- write-back latency at an invalidate (12 functions read pools back);
- pools made and bound every frame (the actor heap's 8 KB pools): mirrors made and freed often. VMA's block suballocation
  keeps that cheap.

### Lever B, first version (`RX_VK_POOL_MIRROR`, default 0; `RX_VK_POOL_MIRROR_CHECK`)

Built in `gpu/vk/vk_memory.cpp` (the "a mirror per pool, never imported" block) and `vk_texture.cpp` (`upload_to_earliest_begin`).
- A pool that is bound as a buffer gets a table entry with no import, cut to the game's pool. A pool larger than a chunk keeps
  16 MB chunks, cut to the pool. Nothing outside a pool is mirrored.
- Its mirror is made at its first binding and filled from guest memory through a host-visible staging buffer: a CPU copy,
  then a GPU copy in the recording's uploads. The staging buffer is freed once the GPU is past it.
- In a parallel segment the mirror is recorded into the earliest open segment's init command buffer. No segment has bound the
  pool before (a binding without a mirror would have imported it), and every segment's init is submitted before its uploads.
- CPU writes reach the mirror from the captures, as before; "live" pages (no kept capture holds them) go through staging.
- Never-imported mirrors are not given back as idle: they are all the GPU has of the pool.
- What still imports is counted by why, and the pool is then marked imported:
  - a binding for writing (its write-backs go through the import; that is the next stage);
  - texel views over the pool;
  - readback refreshes;
  - a binding with no mirror possible: no recording, budget refused, `RX_VK_MIRROR_DEFER`.
- `RX_VK_POOL_MIRROR_CHECK=1`: every 2 s a thread reads a never-imported mirror back (`memory_mirror_read`) and compares it with
  guest memory page by page. Pages that differ are looked at again 500 ms later, once the next submits have collected the write
  marks. A page the CPU wrote after the mirror's last sync is left out; any other difference is a mismatch. The first version
  counted 3 mismatches on CPU-uncached (`0x22`) per-frame pages: writes not yet stamped. They disappeared with the wait.

Run (main PC, `hyrule-field` save, scripted walk, 110 s, build `r1-pm3`):
- **0 mismatches** over 32 pools and 74,937 pages (2,407 written by the CPU since their sync, left out).
- 674 table entries were made with no import; 576 were still never imported at the end. Fills: 668, 707 MB through staging.
  Imported after all: 92 bound for writing, 6 for texel views, 0 bindings in parallel segments.
- At 100 s: mirrors **707 MB** (15 MB outside every pool) against 1,580 with the chunks (500 outside); imports held **402 MB**
  against 828-1,768; VRAM 4,358 MB.
- No crash. The shots at 1500 and 1600 presents are right. The run summary shows no large slowdown (not a speed claim).
- Per-pool imports with the existing switches (`RX_VK_IMPORT_CHUNK_MB=0 RX_VK_NO_HOST_IMPORT=1`) were worse: a binding in a pool
  refused a mirror (over 1 GB: `agl::TextureDataMgr`'s 2.4 GB) imports it whole, and 3.3 GB were held.

What then makes a page of a live pool droppable:
- **A pool never imported** has no pinned page: the substrate's refusal of a granule ever imported (`hos_gp_drop` -2) does not
  apply. A cold page with no CPU write since its mirror's last sync is held, exactly, by the mirror, and by the file when it is
  a decoded resource.
- `HL_MEM_DROP_POOLS`' candidate rule (a mirror on, never bound for writing, compared byte for byte with guest memory at the
  drop, into the spill) applies as it is. The `hos_mirror_held` notice must also be sent for never-imported pools (today it is
  sent when an import is given back): **not done yet**.
- The hitch of section 15 (a re-import restored 280 MB of dropped granules at once on the back end's thread) cannot happen for
  these pools: there is no import to make again.
- What remains are the restores the touches need:
  - the game's own touches (rare; section 7);
  - the back end's staging reads of live pages and fills of a mirror made again. Those are bounded: a fill reads only the
    pool's own pages, and only when its mirror is made, once a pool's life unless it is retired.
- The restore reads the spill. Reading the mirror back at the touch is still not built, for the reason of section 14: a touch
  on the replay's thread holds the back end's locks.

Next stages:
- B3: write-backs through staging, for pools the GPU writes (the staged lazy release's path: GPU -> host staging ->
  `hos_gpu_write_live`), with the wait at an invalidate.
- The `hos_mirror_held` notice for never-imported pools, so `HL_MEM_DROP_POOLS` can drop their cold granules.
- Three zones and a hand-played session with the check on.

## 20. The commit budget of the 30-minute reference session (`play10.orch.log`, 2026-11-14)

Sources: the census (`HOS_HOST_SPLIT=60,60`, 19 tables, a column below is one of them) and R1's ledger (`RX_VK_LEDGER=60`, 18 tables;
its first table is at the census' 181 s: the ledger's clock starts 121 s later). Process commit 4.7 GB on the title, 19.1 GB with
the first zone loaded, 28.0 GB at 547 s, **29.5 GB at 1158 s**. MB, private commit by owner (images are not private commit):

| Owner | 60 s (title) | 181 s (zone loaded) | 547 s (mid) | 1158 s (end) | 181 s to end | Bound? |
|---|---:|---:|---:|---:|---:|---|
| The game's window | 1028 | 7370 | 11398 | 11527 | +4157 | the game's heaps; flat after 609 s |
| **Driver regions of 32-64 MB (beside VRAM)** | 128 | 5491 | 8420 | 9201 | **+3710** | follows the VMA blocks (below) |
| Driver arenas (private 512 MB and up; 128-512 MB before it merged at 486 s) | 2760 | 4550 | 4600 | 4630 | +80 | flat after the start (below) |
| Tagged by us: captures, BC cache | 0 | 153 | 1326 | 1536 | +1383 | BC cache at its 1024 MB cap from 303 s; captures 0.4 GB |
| C and Windows heaps | 468 | 604 | 1027 | 1091 | +487 | flat from 547 s (below) |
| Driver regions 64-128 MB | 0 | 44 | 149 | 293 | +249 | creeps; VMA's larger blocks |
| Other private (8-32, 1-8 MB, stacks, BAR charge) | 331 | 648 | 815 | 868 | +220 | |
| **Not seen by the walk** (a constant charge, see section 16) | 237 | 266 | 268 | 414 | +148 | |
| **Process private commit** | 4738 | 19119 | 27998 | 29523 | **+10404** | |

The columns add up to the process figure within 6 MB. From the title to the end: +24.8 GB, of which the window 10.5, the 32-64 MB
regions 9.1, the arenas 1.9, tagged 1.5, heaps 0.6. There is no unexplained remainder beyond the constant 0.24-0.41 GB.

### (a) Do our Vulkan textures pile up when the game reuses storage? No

The back end's textures (ledger header) go 1292, 3666, 3109, 3992, 4375, 3050, 2369 ... 1679 ... 2807 over the session; the
texture images' VRAM 2311, 3659, 3820, 4197, 3515, 2695 ... 2117 ... 3124 MB; mirrors 1100-2524 MB; render-target images 112-240.
None is monotone: the figures fall when the zone changes, and the live pools they sit in grow only from 3778 (5694 MB) to 5231
(6626 MB). The ledger's "both (a range reused, or a buffer over a texture)" is 0 MB in every table: no range is both texture
storage and a bound buffer. Not measured: whether one range holds two textures of ours at once (the ledger counts images, not
overlaps): what the counts say is that the images alive follow the game's, and do not accumulate.

### (b) The driver: arenas, and the regions beside VRAM

- **Arenas**: 2.76 GB committed at the title (before any scene), 4.07 GB when the first zone has loaded, 4.6 GB from 486 s: a 0.5 GB
  region that was "128-512 MB" until 425 s grew into the 512 MB class (the sum does not change). Pipelines made: 6645 to 6720 over
  the session (+75) while the arenas moved by 0.08 GB after the first zone: the arenas are the start-up's (the 6600 pipelines made
  before the first table, about 0.7 MB each with their shader modules), not the zones'. Their RAM: 2.6, 3.9, 3.9 GB ... then 1.7 GB
  from 853 s with the commit unchanged: Windows took the pages out of the working set (`private by size 512+` RAM 3934 at 792 s,
  1731 at 853 s); the peak is not a leak.
- **The 32-64 MB regions are the driver's system-memory commit for VRAM, and it follows the VMA blocks, not what is allocated in
  them**: over the 16 tables of the zone, commit of the 32-128 MB regions against the heap-0 blocks of `vk: memory heaps` correlates
  0.875 (ratio 1.09-1.11 from 486 s on: 9.2 GB against 8.6 GB of blocks at the end), against allocations in them 0.12 (blocks 7.2-8.6 GB
  while allocations moved 4.3-7.0 GB). **Empty space inside blocks costs commit one for one: 3.4 GB at the end (8572 MB of
  blocks, 5217 MB allocated; the ledger's "free in VMA blocks" 0.67 to 2.7-3.7 GB).** Freeing blocks that hold nothing frees
  that commit (not tried: a measurement of the correlation, not of the effect). Over-budget: VRAM used was 9.1-9.8 GB of 10.7-10.9
  allowed (84-91%) in the second half; the working set did not collapse, and nothing in the log shows demotion beyond the arenas'
  trimming above.
- **The 15.1 GB working-set peak** (reached between 792 s and 853 s; census at 792 s: 13.7 GB): the game's window 6.7 GB (it swings
  3.7-6.8 GB as zones load and cold pages come back), **the driver's arenas 3.9 GB resident**, ours 1.4 GB (BC cache 1.0 GB at its cap,
  captures 0.4 GB), heaps 0.9, regions and images 0.5. Ours and the game's are 56% of it, the driver's 28%.

### (c) The C heaps, 519 to 861 MB in use (ledger) over the session

By size: blocks over 256 KB went from 194 to 277 MB (blocks of 256 KB to 1 MB 25 to 94 MB; 1-4 MB 18 to 45; 4-16 MB 33 to 56;
16-64 MB 64 to 80) and then stopped; blocks under 256 KB, about 320 to 610 MB, make the rest (+290 MB). Named: the capture table 8
to 25 MB (51 chunks, bounded by the pages seen), the texture shadow 0-24 MB (bounded by `kShadowMax`), capture spares in the
back end's own buffers are not in the C heap. **The remaining 250 MB or so of small blocks is not attributed** (no allocation
hook): the candidates are the per-pool, per-texture and per-import records (live pools +1450, imports 43 to 108) and the
descriptor and pipeline tables. It is bounded in practice: the heap rose in the first 7 minutes (zone loads) and stayed at 860-940
MB in use from 423 s to the end.

### Levers this gives, by size

1. Give back empty VMA blocks (up to 3.4 GB of commit; free space in blocks fell between 0.67 and 3.7 GB with the zone).
2. The window's growth (+4.2 GB) is the game's heaps: the memory manager's job (section 4).
3. The arenas (4.6 GB commit, 3.9 GB RAM at the peak): start-up work (section 16 item 1).
4. The BC cache's 1 GB of RAM when VRAM is above 3/4 of its budget (section 11.1): it earns its memory only if textures are made again.
