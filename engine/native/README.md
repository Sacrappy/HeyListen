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
| C surface for the layer that runs guest code | `hl/c/hl_bridge.h` | used by `engine/host/hl_host.c` and one `hl_<module>.c` a module (`hl_jobs.c`) |

Test (no game, no GPU):

```
python -I -B engine/tests/test_native_engine.py --cxx <clang++ or g++>
```
