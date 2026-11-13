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
| +104..+120 | children list; +120 their count (the parent's count is raised under its lock) |
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
- How the 7.75 GB of resident guest memory splits by heap and pool (a measurement, not a reading).
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
