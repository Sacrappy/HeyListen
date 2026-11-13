# engine/native: the native engine

Native x64 C++ written for HeyListen, replacing the game's engine one subsystem at a time. The plan, the inventory of what
still runs from the translation and the boundary with the gameplay code are in
[docs/native-engine](../../docs/native-engine/README.md).

Rules of this tree:

- It contains no code, data, key or asset of the game. It includes only its own headers (`hl/...`) and the C++ standard
  library; `tests/test_native_engine.py` refuses anything else.
- A module does not know guest addresses or layouts. Its bridge (`hl/bridge`) does, and is the only place that does. The
  game's structures stay where the game put them; the bridge reads and writes them in place through structs whose
  offsets are checked at compile time (`static_assert(offsetof(...))`).
- The tree is built into the game's executable by `nx2/build.py`; the OS layer reaches it only through `hl/c/`.
- A module is written and tested without the game first, then switched on behind its bridge and checked against the
  translated subsystem it replaces.

| Module | Headers | State |
|---|---|---|
| Job system: workers, dependencies, lanes for gameplay jobs | `hl/jobs/JobSystem.h`, `hl/jobs/GuestLanes.h` | first version, tested; runs the game's stages behind `HL_JOBS` |
| Main loop: phases, frame step, pacing | `hl/app/MainLoop.h` | first version, tested; not in the game yet |
| Job bridge: the game's job entry points and the structures read in place | `hl/bridge/JobBridge.h` | in the game behind `HL_JOBS`: 1 entry point of 18 answered (`hl::bridge::ready()` lists what is left) |
| Memory: which cold pages of static GPU data and of the game's heaps leave the working set | `hl/mem/ColdPages.h` | in the game behind `HL_MEM_COLD` (on) and `HL_MEM_COLD_HEAPS`, through `engine/host/hl_mem.c` (`docs/native-engine/memory-boundary.md`, sections 8 and 11) |
| Graphics front end: the recording pass, its check, the output seam | `hl/gfx/RecordPass.h`, `hl/gfx/PassCheck.h`, `hl/gfx/Commands.h`, `hl/bridge/GfxBridge.h` | in the game behind `HL_GFX` (the listener `0x806602e0` native, its phases translated) and `HL_GFX_CHECK` |
| Streaming: which resource decodes are native, the provenance of decoded ranges | `hl/stream/Decode.h`, `hl/stream/Provenance.h` | in the game behind `HL_STREAM` and `HL_STREAM_CHECK`, through `engine/host/hl_stream.c` (the zstd calls are there; `docs/native-engine/stream-boundary.md`) |
| C surface for the layer that runs guest code | `hl/c/hl_bridge.h`, `hl/c/hl_mem.h`, `hl/c/hl_gfx.h`, `hl/c/hl_stream.h` | used by `engine/host/hl_host.c` and one `hl_<module>.c` a module (`hl_jobs.c`, `hl_mem.c`, `hl_gfx.c`, `hl_stream.c`) |

Test (no game, no GPU):

```
python -I -B engine/tests/test_native_engine.py --cxx <clang++ or g++>
```
