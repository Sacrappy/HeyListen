# engine/native: the native engine

Native x64 C++ written for HeyListen, replacing the game's engine one subsystem at a time. The plan, the inventory of what
still runs from the translation and the boundary with the gameplay code are in
[docs/native-engine](../../docs/native-engine/README.md).

Rules of this tree:

- It contains no code, data, key or asset of the game. It includes only its own headers (`hl/...`) and the C++ standard
  library; `tests/test_native_engine.py` refuses anything else.
- A module does not know guest addresses or layouts. Its bridge (`hl/bridge`) does, and is the only place that does.
- A module is written and tested without the game first, then switched on behind its bridge and checked against the
  translated subsystem it replaces.

| Module | Headers | State |
|---|---|---|
| Job system: workers, dependencies, lanes for gameplay jobs | `hl/jobs/JobSystem.h`, `hl/jobs/GuestLanes.h` | first version, tested |
| Main loop: phases, frame step, pacing | `hl/app/MainLoop.h` | first version, tested |
| Job bridge: the game's job entry points and shared layouts | `hl/bridge/JobBridge.h` | table only, not switched on (`hl::bridge::ready()`) |

Test (no game, no GPU):

```
python -I -B engine/tests/test_native_engine.py --cxx <clang++ or g++>
```
