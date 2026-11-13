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
| Main loop: phases, frame step, pacing | `hl/app/MainLoop.h`, `hl/bridge/FrameLoop.h` | in the game behind `HL_MAINLOOP` (off by default): the game's frame steps are its phases |
| Job bridge: the game's job entry points and the structures read in place | `hl/bridge/JobBridge.h` | in the game behind `HL_JOBS`: 1 entry point of 18 answered (`hl::bridge::ready()` lists what is left) |
| Memory: which cold pages of static GPU data and of the game's heaps leave the working set | `hl/mem/ColdPages.h` | in the game behind `HL_MEM_COLD` (on) and `HL_MEM_COLD_HEAPS`, through `engine/host/hl_mem.c` (`docs/native-engine/memory-boundary.md`, sections 8 and 11) |
| Graphics front end: the recording pass, its check, the output seam | `hl/gfx/RecordPass.h`, `hl/gfx/PassCheck.h`, `hl/gfx/Commands.h`, `hl/bridge/GfxBridge.h` | in the game behind `HL_GFX` (the listener `0x806602e0` native, its phases translated) and `HL_GFX_CHECK` |
| Streaming: which resource decodes are native, the provenance of decoded ranges | `hl/stream/Decode.h`, `hl/stream/Provenance.h` | in the game behind `HL_STREAM` and `HL_STREAM_CHECK`, through `engine/host/hl_stream.c` (the zstd calls are there; `docs/native-engine/stream-boundary.md`) |
| Graphics front end: the render-state function, the NVN entry points and their sink, the call-by-call check | `hl/gfx/RenderState.h`, `hl/gfx/Nvn.h`, `hl/gfx/StateCheck.h`, `hl/bridge/GfxBridge.h` | in the game behind `HL_GFX_STATE` (`0x80077d60` through an entry gate) and `HL_GFX_CHECK` |
| Graphics front end: the back end's command records written directly (direct emission, bricks A and B) | `hl/gfx/Records.h` (`RecordWriter`, `RecordSink`), `hl/c/hl_gfx_rec.h` (the shared layouts, the OS layer's surface) | in the game behind `HL_GFX_REC` (the render state as records, no NVN call) and `HL_GFX_REC_CHECK` (the records compared with the NVN procedures'), through `engine/host/hl_gfx_rec.c` (`docs/native-engine/direct-emission.md` section 8) |
| Math: bit-exact kernels of the physics and actor loops, the exactness rules they are written with | `hl/math/Exact.h`, `hl/math/Bounds.h`, `hl/bridge/MathBridge.h` | in the game behind `HL_MATH` (`engine/host/hl_math.c`, its differential check `HL_MATH_CHECK`; `docs/native-engine/physics-loops.md` section 8) |
| C surface for the layer that runs guest code | `hl/c/hl_bridge.h`, `hl/c/hl_mem.h`, `hl/c/hl_gfx.h`, `hl/c/hl_stream.h`, `hl/c/hl_math.h` | used by `engine/host/hl_host.c` and one `hl_<module>.c` a module (`hl_jobs.c`, `hl_mem.c`, `hl_gfx.c`, `hl_stream.c`, `hl_math.c`); the shared adapter over the OS layer's callbacks is `src/c/host_runtime.h` |

Test (no game, no GPU):

```
python -I -B engine/tests/test_native_engine.py --cxx <clang++ or g++>
```
