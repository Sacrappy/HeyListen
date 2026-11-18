# Hookshot (oot-recomp)

**Hookshot** is the port's name (as Skybound is TOTKPort's and BlueShell is mkw-recomp's); the maintainer's repository
folder stays `oot-recomp`. The game builds as `hookshot.exe` + `hookshot_vk.dll` (`hookshot` + `hookshot_vk.so` on Linux).

A static recompilation of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake to native x86-64 Windows and Linux:
the game's code lifted to C (NotRexed's a64-kit, Rexcomp's AArch64 kit), the Horizon OS and NVN2 implemented
natively, the GPU work replayed on Vulkan. It is a fork of mkw-recomp (`..\mkw-recomp`, Mario Kart World), whose
paths it keeps so MKW's fixes apply here.

**No game files are in this repository.** The dump is read where it lies (`work/dump.txt`); everything made from it
(modules, image, lifted C, builds, runs) goes to `work/`, which git ignores.

Status: the game plays; the README's [status table](../readme.md#project-status) says how far. The design, its stages and
the lessons carried over from MKW: [design.md](design.md).

## Use

Requirements: Python 3 with `lz4`, `numpy`, `capstone`, `unicorn` (2.1), `zstandard` (and, for the button
prompts, `nx2/prompt_requirements.txt`'s); a compiler: on Windows llvm-mingw
(`HOOKSHOT_MINGW`, or an `llvm-mingw` folder in the clone), else emsdk's clang at `C:\emsdk` (or `EMSDK`) with Visual
Studio's C++ tools; on Linux clang and lld. For the Vulkan back end, a CPM package cache with the Vulkan and SPIR-V
headers, VMA and FFmpeg: `RX_CPM`, else `cpm` in the clone, else the old caches in mkw-recomp's `work/eg` or
`..\Eglantine\.cache\cpm`. [CONTRIBUTING.md](../CONTRIBUTING.md#development-setup) says where each comes from.

Run everything with `python -I` from this folder.

| Command | What |
|---|---|
| `python -I nx2/nup.py <file.nup> --extract work/dump` | the `.nup` dump package to `work/dump/exefs` + `romfs` (newest title; ZBIC NSOs made plain by `nx2/unpack.py`) |
| `python -I nx2/dump.py --dump <folder with exefs/ and romfs/> --set` | what the dump is (Switch 1 or 2, ids, modules, SDK, NVN); `--set` records it in `work/dump.txt` |
| `python -I nx2/lift.py [--write]` | image + discovery + coverage (`work/image/coverage.txt`); `--write` emits the C to `work/gen` |
| `python -I nx2/census.py --nx2` | instruction census per module, after the Switch 2 forms |
| `python -I nx2/test_insn.py [--per 6 --states 12]` | every instruction form the game uses, Unicorn vs lifted C |
| `python -I nx2/test_leaf.py [--count 3000 --states 4] [--calls]` | random whole functions (or call trees), Unicorn vs recompiled native code on the real image |
| `python -I nx2/lift.py --standalone --write` then `python -I nx2/build.py --standalone --jobs 4` | the standalone game: `work/sa/build/hookshot.exe` + `hookshot_vk.dll` |
| `powershell -File scripts/run-vk.ps1 [-Window] [-Seconds 120] [-Name x]` | a muted test run on Vulkan, with screenshots in `work/runs/<name>/shots` |
| `python -I scripts/prof.py <stacks file>` | a profile sampled with `HOS_PROFILE=<seconds>` + `HOS_SAMPLE=<thread>` + `HOS_SAMPLE_FILE=<stacks file>` (the sampler runs only with `HOS_PROFILE` set), named from the linker maps |

The dump can also come from `OOT_DUMP` (its folder) or `OOT_EXEFS`; `OOT_WORK` moves the work folder.

The built game's own arguments (`image.bin`, `--romfs`, `--user`, `--trace`, `--prepare-shaders`) are in the README's
[Running the game from the command line](../readme.md#running-the-game-from-the-command-line).

Scripted test input (`HOS_INPUT_SCRIPT` and `HOS_INPUT_LIVE`) is applied before the window-focus check. A log saying
the game window is not in front does not mean those presses were discarded. `HOS_HEADLESS` and
`HOS_WINDOW_NOACTIVATE` change window creation, not this input routing. The game must still reach a state that reads
and responds to the supplied controller input.

Physical keyboard and mouse input stays with the foreground window. Physical controllers can also play in the
background when the existing "Controller in background" option or `HOS_INPUT_BACKGROUND=1` enables them;
`HOS_INPUT_BACKGROUND=0` overrides that option. With `HOS_INPUT_EXCLUSIVE` set (to any value, `0` too) the script
alone drives the game until 5 s after its last press; without it the player's devices add to the script's presses. It
is not a focus override.

## Switches for testing and debugging

The running game reads its switches from the environment or from `hookshot_env.txt` beside `image.bin` (one
`NAME=VALUE` a line, `#` for notes; `HOS_ENV_FILE=<path>` reads that file instead). The build reads its own from the
environment. The README has the ones players are asked to try (`RX_VK_ASTC_RGBA8`, `RX_VK_LOW_SPEC`,
`RX_VK_MOVIE_UPSCALE`, `RX_VK_SKIP_CODE`, `HOS_OOT_CHEATS`). There are several hundred more, most of them one-off
experiments: `grep -rn 'getenv("' host gpu/vk` lists them, and the comment where each is read says what it does.
"Set" below means any value, `0` included.

`RX_VK_MESH_NOCULL=<prefix,...>` turns off the culling in the task shaders whose native code key starts with one of
the prefixes. On a Steam Deck, `RX_VK_MESH_NOCULL=2c56dc1006942f74` avoids the same hang without leaving anything out:
that task shader walks a coarse depth grid cell by cell for every meshlet and ends with
`visible = (result & 1) | (c3[2320] & 4)`, bit 4 being the game's own "draw everything". The switch makes that answer
true, and dead code elimination removes the grid walk, so every meshlet is drawn and the rasterizer clips what is off
screen. It costs some GPU time on cards that cull quickly, so it is off unless set. The log says
"culling off, every meshlet drawn" for each task shader it changed.

### Running a test

| Switch | What it does |
|---|---|
| `HOS_HEADLESS` (set) | No window: the renderer draws offscreen, for screenshots and frame dumps |
| `HOS_WINDOW_SIZE=<w>x<h>` | The window's size in pixels (1280x720 by default, wider for an ultrawide picture) |
| `HOS_WINDOW_NOACTIVATE` (set) | The window opens behind the others and does not take the focus (Windows) |
| `HOS_FULLSCREEN=1` | Starts in fullscreen; the Insert menu's Fullscreen row is then left alone |
| `HOS_AUDIO=0` | No sound device is opened (the mixer still runs); `scripts/run-vk.ps1` sets it unless `-Sound` |
| `HOS_AUDIO_NO_EFFECTS=1` | The audio effects (reverbs, delay, biquad filters on mixes and splitter destinations) are left out: the dry sound, to compare |
| `HOS_AUDIO_NO_REVERB=1` | Only the reverbs left out |
| `HOS_LANGUAGE=<code>` | The game's language for this run, a code from `language.txt` |
| `HOS_OUTPUT_RES=<0..4>`, `HOS_HANDHELD=1`, `HOS_ASPECT=<ratio>` | Over the options file: the output resolution (0 2048x1152, 1 2560x1440, 2 3840x2160, 3 1920x1080, 4 5120x1440, or one of those sizes written out; any other value is logged and the game's size kept), the handheld profile, a picture wider than 16:9 (2.3889 for 3440x1440) |
| `HOS_DYNRES=0`, `HOS_REALTIME_STEP=0`, `HOS_TEXT_SPEED=<0..3>` | Over the options file: Resolution 100% (non-zero: Auto), Game speed Per frame (1: Real time), Text speed |
| `RX_VK_FRAMEGEN=0`, `RX_VK_FRAMEGEN_MULT=<2..8>`, `RX_VK_PACE=0`, `RX_VK_UPSCALE=<0..2>` | No frame generation whatever the menu says; over the options file: its multiplier, frame pacing, the upscaling filter |
| `RX_VK_NO_INTEL_SERIAL=1` | On Windows, Intel's compiler (UHD and Arc) is entered by one thread (`IntelDriverGate`, `gpu/vk/vk_internal.h`: the game's modules and pipelines, and every pipeline the renderer makes for itself), and a fault creating a shader module or a pipeline leaves that one out. A draw's pipeline ask never waits behind a compile: while a worker holds the gate it is answered cold (`vk: asked cold: Intel's gate busy`). This builds them side by side, as on every other GPU and on Intel elsewhere, for the run; the Debug tab's Intel shader compiles row (Intel graphics on Windows only; `intel_shader_compiles=` in `hookshot_options.ini`) does the same and is the player's choice. The log says which. Nothing changes on other GPUs: the gate is a no-op there |
| `RX_VK_NATIVE_FADD=0` or `1` | The shaders' float adds to storage buffers: 1 the GPU's own atomic where it has `VK_EXT_shader_atomic_float`'s `shaderBufferFloat32AtomicAdd`, 0 a compare-exchange loop on every GPU, the extension not enabled. By default the GPU's own only with Intel's Windows driver from 101.9154 on the Arc B-series cards (Battlemage, PCI device 0xE2xx): the loop lost the device on entering Kokiri Forest on a B580. NVIDIA's driver keeps the loop here (Longshot's default is the native add there). Changing it, or the first start after an update that changes it, makes the translator's stamp differ, so every program is translated and prepared again. The log's `vk: float atomic add on storage buffers:` line says which. The loop gives up after 1024 attempts, and an add that gives up is lost |
| `RX_VK_SSBO_DEVICE_ALIGN=1` | Storage buffers bound and translated at the device's own `minStorageBufferOffsetAlignment`, as before. By default at 16 bytes at least (`storage_binding_align`, `gpu/vk/vk_internal.h`): the translator's 8- and 16-byte vector reads are exact only from a 16-byte boundary, and AMD and Mesa's ANV report 4 (in OoT: green lighting, cyan sky). Ported from Hookshot 6e11217e1 (#2442). Changes the program cache's stamp on those GPUs, so the shaders are prepared again. For A/B |
| `RX_VK_DEVICE=<n>` | The graphics card listed as `vk: device <n>` in the log, over the menu's GPU device |
| `HOOKSHOT_NO_UPDATE_CHECK=1` | No look for a newer release at the start |
| `HOS_TRACE=1` | Every Horizon call logged, as `--trace` does |
| `RX_BACKEND=<name>` | Unset or `vulkan`: `hookshot_vk.dll`; any other value but `eden`: the WebGPU renderer, when the exe was built with one |
| `HOS_MEMINFO_OLD=1` | The used memory the game is told (`QueryMemoryInfo`, svc `GetInfo` 7 and 22) is the program and the system's share alone, as before. By default it adds the heap and the pages the game mapped through `AllocateMemoryPages`, at most the console's total (`hos_used_memory`, `host/hos_mem.c`): told less, the game kept mapping pages for its caches (7.9 GB in a long session). Ported from Longshot a4c77174 (#324). For A/B |
| `HOS_PAGES_LIMIT=1` | A diagnostic: `AllocateMemoryPages` is refused past the console's total (heap and mapped pages), as on the console. Keep it off: the game retries a refused allocation forever |
| `HOS_GPU_ALLOC_RETRY=0` | A model's material-block allocations from a full heap (the OptimizeChangeBank thread's two CPU buffers and two GPU-memory calls in f_804566e0) not tried in the heap's parents, as shipped (`host/hl_heap_retry.c`, issue 1697) |
| `HOS_GPU_ALLOC_TEST_FAIL=<n>` or `<gpu>,<cpu>` | A test: the first n of those allocations of each kind (or that many of the GPU memory and of the CPU buffers) refused by the heap they name, as a full heap would, so the retry runs |
| `HL_MAINLOOP=0` | The game's main loop is the translated one again. By default the main thread is handed to the native main loop (`hl::bridge::FrameLoop`, `docs/native-engine/main-loop.md`) at the framework's loop `0x809667b0`; it needs the job bridge (`HL_JOBS`, on). `HL_MAINLOOP_CHECK=1` counts its invariants every 10 s in the log. For A/B |

### Input for scripted runs

| Switch | What it does |
|---|---|
| `HOS_INPUT_SCRIPT=<presses>` | Presses timed from the game's first controller read |
| `HOS_INPUT_LIVE=<file>` | A file the test appends lines of presses to, read every 30 ms; each line's times count from when it is read. A line `!cheat <n>` sends a Cheats page action, `!synclog` logs the NVN syncs (`!dump`, `!shots`, `!rdoc` and `!flasharm` are the WebGPU renderer's) |
| `HOS_INPUT_FROM_PLAY=1` | The script's times count from the moment play begins (the `play: began` line of the log: the player's first update) instead of the first controller read; nothing fires before. With `HL_SKIP_TITLE=1` a scene has the same inputs whatever the title and loading took |
| `HL_SKIP_TITLE=0` or `1` | Skip title screen (the Gameplay row, saved as `skip_title_screen`; option id `HL_OPT_SKIP_TITLE`, `gpu/vk/vk_opt_ids.h`) for one run, wins over the row: A is pressed through the title and file list, from 20 s after the first controller read, every 2 s, until the player runs; with no save it does nothing |
| `HOS_INPUT_FRAME_CLOCK=1` | The script's time runs at 60 game frames a second instead of the wall clock |
| `HOS_INPUT_EXCLUSIVE`, `HOS_INPUT_BACKGROUND`, `HOS_INPUT_SCRIPT_BACKGROUND` (set) | As above; the last lets every device count while the window is behind another one |
| `RX_VK_MENU_SCRIPT=<seconds>:<key>,...` | Timed Insert menu keys: `INS`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `ESC`, `ENTER`, `TABL`, `TABR` |
| `RX_VK_UI_PROMPTS=pad` or `keys` | The menu's button prompts for a controller or the keys, whatever was used last (screenshots) |
| `RX_VK_UI_GAME_FONT=0` | The menu, the counter and the start-up screen in the included Libertinus, not the game's own font |

The presses are `<seconds>:<button>[+<button>...][/<ms>]`, separated by commas, for example `2:A,5.5:LUP/1500,9:ZL+R`.
The buttons are `A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`, and the sticks pushed to their edge `LUP
LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT`. A press is held for 200 ms unless `/<ms>` says otherwise. The names are
case-sensitive, and an unknown one is ignored.

### Pictures and frames

| Switch | What it does |
|---|---|
| `RX_VK_SHOTS=<dir>` | A PNG every `RX_VK_SHOT_EVERY` presents (300) as `vk_<present>.png`, at `RX_VK_SHOT_W` x `RX_VK_SHOT_H` (1280x720) |
| `RX_VK_FLASH=1` | The flash detector, with `RX_VK_SHOTS` and `RX_VK_SHOT_EVERY=1`: writes the frames around a flash and dumps the next one |
| `RX_VK_FRAMELOG=<file>` | Every frame shown, at 320x180, while `<file>.go` exists; `python -I scripts/framelog.py <file>` finds the flashes |
| `RX_VK_DUMP=<present>[+<count>][,...]` | Every step of the frame after that present as PNGs, in `dump_<present>` under `RX_VK_DUMP_DIR` (or `RX_VK_SHOTS`, or the current folder); `RX_VK_DUMP_STEPS=<lo>-<hi>` and `RX_VK_DUMP_PROGS=<key>,...` narrow it |
| `RX_VK_READBACK_STRICT=0`, `1` or `<hex size>,...` | Whether reads of GPU results (the light meter) wait for their write-back to land, not only for the recordings' timeline. A list of sizes makes only ranges of exactly those sizes wait: `1040,c0` is the light meter's, which stopped the pulses on RADV as `1` does (0 in 2 runs each against 52 and 23 with `0`) while waiting a few times less often. With RADV the default is `1040,c0` (without it the auto-exposure pulses, the whole picture brighter for ~16 frames, when the CPU is busy; `1` waited mostly on two 0x1b580 ranges that are not the meter: 69 fps against 81 in Kokiri on an RX 9070 XT); off elsewhere (on NVIDIA it cost ~18% and no pulses were seen). Set `=1040,c0` (or `=1`) in `hookshot_env.txt` if the picture pulses on another driver. `RX_VK_FRAMELOG` shows the pulses; `RX_VK_EXPOSURE_LOG`'s factor does not |
| `RX_VK_PASS_TRACE=1` | The passes of the last frames, logged at a flash or a dark frame (`RX_VK_PASS_TRACE_AT=<n>`: every n-th present) |
| `RX_VK_PROBE=1` or `2` | The centre texel after every pass: `probe_<frame>.txt` at a flash (1), or `probe_all_<n>.txt` at speed (2); `python -I scripts/probe.py` reads them |
| `RX_VK_DUMP_FAILED=<dir>` | The native code of every program that does not translate, for `nx2/lower_test.py` (game data: keep it local) |

### Time and profiles

| Switch | What it does |
|---|---|
| `HOS_PROFILE=<seconds>` | Every interval, the busiest threads in the log; it also starts the samplers below |
| `HOS_SAMPLE=<thread>` + `HOS_SAMPLE_FILE=<file>` | One thread's stacks, sampled every millisecond (needs `HOS_PROFILE`); `python -I scripts/prof.py <file>` reads them |
| `HOS_SAMPLE_ALL=<file>` | Every game and renderer thread every 2 ms while `<file>.go` exists (needs `HOS_PROFILE`); `scripts/crit.py`, `scripts/vkprof.py` and `scripts/hotlist.py` read it |
| `HOS_THREAD_TIMES=<seconds>` | Every interval, the threads that used over 5% of a core |
| `HOS_SHORT_SLEEP_TIMER=0` or `1` | The game's sleeps under a millisecond (SleepThread): `0` all short sleeps yield (or PAUSE-spin with `HOS_SHORT_SLEEP_PAUSE`) to their time, as before; `1` all wait on the timer, the host's too. Not set: on Linux the game's wait on the timer (`clock_nanosleep`) and the host's yield, which lowered the frame's CPU 44.8 -> 35.4 ms at the same frame time in Hookshot (a1eb46046, #2637); on Windows all yield. For A/B |
| `HOS_FPS_LOG` (set), `HOS_PRESENT_TRACE=<csv>` | The game's frame limiter, VRR flags and nominal rate every 300 presents; a CSV line per game present |
| `RX_VK_FRAME_LOG=<csv>` | A CSV line per present: frame, seconds, frame time in ms, QueryPerformanceCounter ticks (Windows) |
| `HOS_LOG_QPC=1` | Every log line ends with ` @qpc <ticks>`, the clock of PresentMon's `TimeInQPC` and of the frame log's last column |
| `RX_VK_PASS_TIMING=1` or `2` | GPU time per pass (2: per program too), logged every 600 presents |
| `RX_VK_SLOW_LOG=<ms>` | The replay's times for each frame that took longer than this |
| `HOS_PGO=<file>` | In a run of an `RX_PGO=gen` build: `<file>.reset` clears the counters, `<file>.dump` writes them to `<file>`. It is read before `hookshot_env.txt`, so set it in the environment |

### The GPU

| Switch | What it does |
|---|---|
| `RX_VK_VALIDATION` (set) | The Khronos validation layer |
| `RX_VK_CHECKPOINTS=0` | No NVIDIA checkpoints (on by default; after a lost device they name the draw it stopped at) |
| `RX_VK_CHECKPOINTS=1` | Without the NVIDIA ones (AMD, Intel): VK_AMD_buffer_marker's markers, logged after a lost device and also once the GPU has finished none for `RX_VK_CHECKPOINTS_STALL_S` seconds (2; 0: only after the loss), so a ring that times out with no submission coming back still names the work |
| `RX_VK_HANG_DUMP=<file>` | Every thread's stack to the file once no frame was shown for `RX_VK_HANG_SEC` seconds (20) (Windows) |
| `RX_VK_NOOPT` (set) | Pipelines built without the driver's optimisations |
| `RX_VK_SPLIT_UBFLUSH=0/1` | A submission cut into parts (`RX_VK_CHUNK`): 1 each part writes the uniform updates recorded so far into game memory at its end, as before; 0 only the last part writes them all, as an unsplit submission does. Default 0 on Intel without VRAM mirrors (the iGPUs: white blocks and flashes in Kokiri Forest otherwise), 1 elsewhere |
| `RX_VK_SPLIT_BEFORE_DRAW=0` or `1` | A submission cut into parts (`RX_VK_CHUNK`): 1 a part ends only right before a plain draw (one with a vertex stage), never before a mesh draw or a dispatch; 0 after its 120th draw or dispatch whatever follows. Default 1 on Intel (an Arc B580's native mesh shaders drew white shards after a part that began with mesh draws, Longshot #443), 0 elsewhere |
| `RX_VK_GPU_CLOCK=0`, `1`, `peak` or `standard` | The GPU's clock (Linux, AMD; `gpu/vk/gpu_clock.h`). By default, on the Steam Deck's APU (LCD or OLED, by its name) the renderer holds amdgpu's peak power state while the game runs, when the GPU's power level is `auto` (a level chosen by hand or by SteamOS is left alone): 1300 MHz there, where its automatic clocks held 1040 in play and dropped to 200 between reads (Deck OLED: Kokiri Forest at handheld settings +46%, Hyrule Field at native +19%; Deck LCD +8% to +20%; the APU the same where it was at its 15 W limit, about 4 W more where it was not, so a shorter battery life there). Elsewhere, unset, the clocks are the driver's and nothing is read. `RX_VK_GPU_CLOCK=0` (or `off`, empty): the driver's own clocks on the Deck too. `RX_VK_GPU_CLOCK=1`: the Deck's rule, on another integrated GPU with Steam's `SteamDeck=1` too. That is the peak state's fixed clock, below the 1600 MHz the GPU reaches with a clock set by hand (the GPU clock slider in SteamOS's performance settings), which did as well or better; amdgpu's stable power states, all a program may ask for without root, have none higher. `peak` or `standard` asks for that state on any AMD GPU, whatever its level. The driver puts the level back when the game ends or is killed, and a write to `power_dpm_force_performance_level` goes over it |
| `RX_VK_PCACHE_PLAY_SAVE=0/1/2` | The pipeline cache's saves in play: `1` (default) only when the free physical memory is at least twice the cache plus 4 GB and 5 minutes after the last one, on a background-mode thread (low I/O and memory priority); `0` never in play (the exit, the prepare and Restart now still save); `2` every time one is asked, as before |
| `RX_VK_PCACHE_FETCH_LOCK=0/1` | A pipeline cache's data is fetched (`vkGetPipelineCacheData`) only with no pipeline build under way (Intel's Windows driver crashes otherwise); the background save is put off to the next request when builds run throughout. Default: on for Intel (vendor 0x8086) under Windows, off elsewhere; `1` forces it on any GPU |
| `RX_VK_COMPUTE_QUICK_FIRST=0/1` | On one-at-a-time compute builds (Intel by default, `RX_VK_COMPUTE_SERIAL`): a dispatch's first pipeline is made without the optimizer and the optimized one after it, on the back lane; the prepare and the precompile ask for the first. Default `1`; `0`: one optimized build, as before |
| `RX_VK_SKIP_PROG=<prefix>,...` | Leaves out the draws of these programs (default `53509044401d78d2`; `-` leaves out none) |
| `RX_VK_PIPE_SKIP=0` | A draw whose pipeline is not ready waits for it, instead of being left out when its targets hold an earlier frame |
| `RX_VK_PIPE_BY_PROGRAM=0` | A graphics pipeline is shared by its key (program code + variant) alone, as before. On (default): it belongs to the program object it was built from, and another object with the same key gets a pipeline of its own (two objects can share a key: a pipeline built for one and drawn with the other ran the wrong shaders under the wrong layout, the GPU hung and the device was lost near water). On RADV the same draw ends in `radv: GPUVM fault detected` from the texture unit (`CLIENT_ID: (TCP)`, the address 0 or another one each start), a `slow submit` of ~2 s, the kernel's `ring gfx_0.0.0 timeout` and reset, then the device lost, seconds to minutes after a save loads: v0.4.1 on an RX 7800 XT in Kokiri Forest, 4 of 4 starts, against 0 of 4 with this change put into the same build and 0 of 3 on main (`docs/tested/2026-11-17-pipe-by-program-nyakuoff-run01.md` to `-run03.md`). The log says `vk: pipeline ... was made for another program object` (first 12 times) |
| `RX_VK_PRECOMPILE=0` or `1` | The shader preparation before the title off or on (on in builds made with `RX_RELEASE=1`, as the builders' are) |
| `RX_VK_NO_PROGRAM_CACHE` (set) | Neither reads nor writes `hookshot_programs.bin` and the pipeline notes |
| `RX_VK_NO_F16=1` / `RX_VK_F16=1` | The shaders' 16-bit floats (`float16_shaders` in `gpu/vk/vk_shader.cpp`): kept where the device has `shaderFloat16`, else the game's f16 math lowered to 32 bits and the cooperative-matrix networks left out, the gameplay ones stood in for by the TAAU stand-in's upscale (`upscale_stub` in `gpu/vk/vk_draw.cpp`). A module that declares Float16 without the feature is invalid (GTX 10xx, Polaris RX 400/500). `RX_VK_NO_F16=1` lowers on any card (a test of that path), `RX_VK_F16=1` keeps f16 on a card without it; the log says `vk: shaders: 16-bit floats ...` and why. Part of the program cache's stamp only on a device without `shaderFloat16` (and by name, under `RX_VK_NO_F16`) |
| `RX_VK_HANDLE_TABLES=0` | A texture handle a shader reads from a table at an index of its own (OoT's clustered lights: each light's shadow map, read per pixel from the light table) translated as before instead of as a descriptor array of the table's first 64 entries (`handle_tables_on` in `gpu/vk/vk_shader.cpp`, the texture pass's handle tables): those shadow tests then read no texture (the dark and green blocks of issue 52). Off, the texture pass's other changes that came with the tables are off too (its search skipping texture results and float math, words of 12- and 16-byte loads, the shared handle programs, the pointer words), so the translation is the one before them. On by default where the GPU indexes sampled image arrays per pixel (`shaderSampledImageArrayNonUniformIndexing`) and has at least 256 sampled images, samplers and resources a stage and 512 sampled images and samplers a set; the log says `vk: handle tables as descriptor arrays: on`, or which check turned them off. Part of the program cache's stamp when off; `nx2/bake_ds` bakes with them on unless `--feature no-handle-tables` |
| `RX_VK_MIRROR=0`, `RX_VK_MIRROR_MB=<MB>` | No VRAM mirrors of the game's memory pools (on for discrete cards); their budget |
| `RX_VK_NO_DMABUF=1` | Linux: guest memory goes to the GPU as host-pointer imports, not dma-bufs (udmabuf), and the guest window is anonymous memory for them (a shared memfd mapping cannot be one: no pool would be imported). Slow on AMD: amdgpu checks every page of them at each submission. `HOS_GUEST_MEMFD=1`: the memfd all the same |
| `RX_VK_SPARSE_QUEUE=1` | Linux: a pool larger than udmabuf's limit (several dma-bufs in a sparse buffer) is bound on a queue of another family, not the replay's, so its import does not wait for every frame queued on the GPU (issue 14: ~1 s on a Steam Deck; 540 ms behind 540 ms of queued work against 2-4 ms on an RX 9070 XT). Off by default until run in the game. `vk: pool ...: N dma-bufs bound in ... ms (..., its fence ...)` shows where an import's time went |
| `RX_VK_ASTC_CPU=1` or `0` | ASTC textures decoded on the CPU (as RGBA8), or by the GPU's decoder (and big ones re-encoded as BC3). Any value but `0`, `false`, `off`, `no` or empty turns the CPU decode on. Unset: on the CPU on Windows with the graphics built into AMD's Ryzen 7000 (PCI 0x164E) and 9000 (0x13C0) desktop CPUs (2 CUs; not the APUs' larger iGPUs), where the GPU's decode of Kokiri Forest's terrain arrays ran so long in one submission (~2000 dispatches, 4-6 s) that Windows reset the GPU (Longshot #489); by the GPU elsewhere. The CPU decode takes about 6 s an array on the replay thread (a pause as the area loads). The log says `vk: ASTC decoded on the CPU` when it is on |
| `RX_VK_TARGET_POOL_MB=<MB>`, `RX_VK_DEAD_EVERY=<presents>`, `RX_VK_DEAD_MIN_UNUSED=<presents>` | The render target pool: the most its idle images may hold (by default a twelfth of what the driver allows, 256 to 768 MB: `gpu/vk/target_pool.h`; `0`: no pool), how often the dead transient targets are looked for (every present by default) and how long one must have gone unused (1 present). `RX_VK_TARGET_POOL_MB=256 RX_VK_DEAD_EVERY=10 RX_VK_DEAD_MIN_UNUSED=3` is the earlier behaviour, in which the game's per-frame targets were made and destroyed about twice a frame (Longshot #994: ~20 % of the render thread in `vkDestroyImage` and image creation in Castle Town at night) |
| `RX_VK_MESH_OPT=1` / `=0` | Task and mesh programs: `1` builds them with the driver's optimizer on every GPU, `0` without it on every AMD driver. Default: without it on AMD's own drivers only (their optimizer hung the GPU on these programs: RX 6600, PR 430), with it on Mesa's RADV (without it they ran so long that the GPU was reset: Steam Deck, issue 14, as upstream Hookshot #1965 found) and every other driver |
| `RX_VK_MESH_G3=0` | Task and mesh programs: each storage buffer's range is worked out once, in the first block, instead of at every global access (a mesh program has ~80 inlined, 40% of OoT's biggest one). On by default on every GPU (PC 3050, RTX 3050 at 2560x1440: -0.6 ms GPU in Kokiri Forest, -1.4 ms in Hyrule Field, same pictures); `=0` is the earlier translation (and its own program cache: the name is under `RX_VK_MESH`). Not run on AMD yet: if a Steam Deck or an AMD card draws wrong or hangs in a mesh scene, `RX_VK_MESH_G3=0` in `hookshot_env.txt` is the first thing to try |
| `RX_VK_MESH_NOINLINE_ABOVE=<n>` | A mesh stage of more than n IR instructions keeps its global access helpers out of line and its ranges unhoisted (G3's hoisting made OoT's biggest mesh program, 43ec, twice as long). 30000 by default (PC 3050, RTX 3050 at 2560x1440: -1.1 ms GPU in Kokiri Forest, Hyrule Field unchanged, same pictures); `=0` inlines everything as before. A name under `RX_VK_MESH`: its own program cache |
| `RX_VK_FOLD_PSEUDO=0` | The translator folds a constant bit-field extract or bitwise and/or/xor that carries zero/sign flags (on by default: 79% fewer foldable ops, 22% fewer mesh instructions, a smaller program cache and faster translation; no GPU time change, the drivers already folded them). `=0`: off. Covered by the program cache's stamp |
| `RX_VK_EARLY_GPU=0` / `=1` / `=2` | A big command buffer (one replayed in segments) goes to the GPU as it is recorded: the work before it first, then each segment in order as the thread that recorded it finishes, instead of after its dry pass and the replay thread's own segment. `=2` also cuts the first segment short (`RX_VK_EARLY_GPU_FIRST` draws and dispatches, 48 by default). Default `1` (PC 3050, 2560x1440, interleaved A/B with the same pictures: Kokiri Forest 36.45 -> 38.05 fps, the market 42.05 -> 42.85; the GPU's idle time a frame ~1 ms shorter); `=0` is the old path |
| `RX_VK_RADV_SUBMIT_THREAD=0` or `=1` | Linux, an AMD card: Mesa's `MESA_VK_ENABLE_SUBMIT_THREAD`. Decided once at process start (host/hos_main.c `main()`, after `hookshot_env.txt` is read and before the first thread: a `setenv` is not safe once other threads can `getenv`) and kept set for the whole process. The device is not picked yet there, so the rule is an `amdgpu` card: some `/sys/class/drm/card<N>/device/driver` links to `amdgpu` (connectors such as `card1-DP-1` and render nodes are passed over). With one, the variable unset and 12 or more hardware threads (`sysconf(_SC_NPROCESSORS_ONLN)`), it is set to `1`, so every queue sends its submissions from a Mesa thread (Mesa reads it in each `vkCreateDevice`, `vk_device_init`) and the submit ioctl leaves the replay's thread; below 12 threads it is left alone (Mesa's thread takes a share of a core). `=1` sets it whatever the thread count, `=0` (false, off, no, empty) leaves it alone; a value the user set (`0`, `1`, anything) is kept, and without an `amdgpu` card nothing is touched. On a machine with an AMD card and another Mesa GPU (an Intel iGPU's ANV) it reaches that driver's queues too. The log says which rule decided after its first line, `env: Mesa's submit thread on: 12 threads (...)`, `off: 8 threads, below 12 (...)`, `on: RX_VK_RADV_SUBMIT_THREAD=1`, `off: RX_VK_RADV_SUBMIT_THREAD=0` or `left as the user set it`, and the Vulkan back end, once the device is picked, `vk: RADV: Mesa's submit thread ... (decided at start)` (or that the picked device is not RADV). The menu's restart starts with the environment the process started with (`/proc/self/environ`), so it decides the same way; a process started from the game inherits `HOS_MESA_SUBMIT_THREAD_SET=1` beside the variable and decides again rather than taking the `1` as the user's. Measured on an RX 9070 XT (Mesa 26.2.6, Ryzen 5 5500X3D, Kokiri Forest, frame limit off), with the variable set by hand: 90.2 -> 100.3 fps (+10.1, se 1.8, ahead in 6 of 6 interleaved rounds); the replay thread's `queue submit` 1.16 -> 0.01 ms a frame, its replay span 9.3 -> 7.1 ms, its CPU 42% -> 21%, Mesa's thread 21-27% of a core; no brightness pulses, no device loss. Recipe: skip the title, `HOS_INPUT_SCRIPT=8:A,10:A,12:A,14:A`, `!cheat 1000` (Kokiri Forest) 2 s after play begins, the window from the teleport + 30 s for 70 s, `HOS_FPS_LIMIT=360`, `HOS_AUDIO=0`, saves isolated with `XDG_DATA_HOME`, 6 interleaved rounds. Not run on an APU, a Steam Deck or other RADV cards: the extra share of a core matters on a 4-core handheld. With the thread a submission's errors (a lost device) show at the next submit or wait, not at the `vkQueueSubmit2` that caused them, and the `queue submit` times and `slow submit` lines no longer include the kernel's submit |
| `RX_VK_HALF_RATE_SHADING=1` | Linux, RADV: keeps `RADV_FORCE_VRS_CONFIG_FILE`. The Steam Deck's Game Mode sets it (a file holding Steam's Half Rate Shading rate, `1x1` when off) for everything it starts; while that file exists RADV puts the rate into every graphics pipeline's key, so the pipelines the preparation built are not found and a start from Game Mode compiles them all again (about 20 a second on a Deck, 25-30 minutes). By default the variable is unset at process start (host/hos_main.c `driver_env_choose()`, beside the submit-thread decision, before SDL can load Vulkan) when it names a file that exists, and the log says so (`vk: RADV_FORCE_VRS_CONFIG_FILE=... dropped`); a name with no file, an empty value or no variable is left alone, and `RADV_FORCE_VRS` (a rate set by hand) is never touched. `=1` keeps it: Steam's Half Rate Shading then reaches the game and the start compiles its own pipelines |
| `RX_VK_VRAM_MB=<MB>` | The texture cache's budget (default 70% of what the driver allows, at most 6 GB) |
| `RX_VK_COMMIT_RESERVE_MB=<MB>` | Windows only: the commit the texture cache leaves free before Windows' commit limit (RAM plus the page file; default 2560, `0` off). Below it the shortfall comes off the textures' budget and `vk: commit limit near:` is logged. At the limit itself a refused C++ allocation in the renderer (`new_refused`, `gpu/vk/vk_main.cpp`) waits up to 5 s, then the out-of-memory notice and the game stops, instead of an uncaught `std::bad_alloc` (exception 0x20474343; `MkwVkHost::out_of_memory`, API version 15) |
| `RX_VK_UPLOAD_BUDGET_MS=<ms>`, `RX_VK_UPLOAD_BUDGET_SUM=1` | The upload budget: once a thread has spent this long uploading textures in a present (40 by default; `0`: never), a new ASTC texture a draw samples there is held back for a later present and sampled as the null descriptor (black) meanwhile. The time is the looking-up thread's own, so the parallel replay's segment workers each have it; `RX_VK_UPLOAD_BUDGET_SUM=1` counts every thread's summed, as before (at the Market's arrival the sum reached 40 ms while the busiest thread had spent 11-31 ms, and 300-450 textures were held back). The log's `vk: deferral:` lines say, per present that held any back, how many and what took the time (new ASTC, other new, uploaded again; the busiest thread's share), and `vk: deferral done:` how long they waited (presents and ms, p95 and max) and how many were never looked up again |
| `RX_VK_FIRST_UPLOAD_OWN=0` | A texture's first upload reads every page of its storage the submit-time capture kept a copy of, as before. By default it takes only the copies the submission being recorded took, and guest memory for a page whose copy is an earlier capture's: OoT fills a new texture's storage (another texture's before) after submitting the draws that sample it, and the kept copies were the old texture's bytes for one frame (issue 66: the Market's one-frame flashes, its lightmaps blown out to magenta and white, roofs, timber frames and banners in another material's colours). The `vk: texture storage read as submitted` line counts the pages read from guest memory this way |
| `RX_VK_REUPLOAD_OWN=0` | A texture's re-upload reads every page of its storage the submit-time capture kept a copy of, as before. By default it takes, as a first upload does, only the copies the submission being recorded took, and guest memory for a page whose copy is an earlier capture's: OoT rewrites the Market's lightmaps and materials in place for a while after an arrival, writing a page more than once, and a capture taken between two of those writes was uploaded for one frame (issue 66: bursts of one-frame flashes 20-35 s after arrival, the picture blown out to white and magenta, walls and ground in lighter colours, or both blue banners white, with the first-upload fix on). The same log line counts these pages |
| `RX_VK_NO_DRAWN_PAGE_MAP=1` | A texture in a virtual pool that the GPU writes before anything uploads it has no page map noted at that write, as before: once the game has finalized it, its next check takes all of its storage for remapped and uploads it whole from guest memory over what the GPU drew (Ocarina of Time's HUD labels go blank after a zone change; issue 3's black surfaces flickering in Zora's Domain, which the log shows as `vk: upload over a drawn image` lines with the whole image dirty; `vk: uploads over a drawn image: N so far` counts them all past the log's 400 lines) |
| `RX_VK_NO_DRY_CLEAR_MARKS=1` | The parallel replay's dry pass does not mark the targets a clear writes as the GPU's (it marks a draw's: `RX_VK_NO_DRY_MARKS=1` turns both off), as before: a pass that only clears its targets is marked when its own segment records it, and a later segment recorded first uploads stale guest memory over them, an upload that runs after the clear (issue 3: the world black for one frame in Zora's Domain, a few a minute while moving; the log's `vk: parallel: ... written (a pass's colour target) in segment N, uploaded by the later segment M` lines count them, none with `RX_VK_PARALLEL=0`) |
| `RX_VK_NO_DRY_XFER_MARKS=1` | The same for the destination of a texture-to-texture copy |
| `RX_VK_NO_DRAWN_WRITEBACK=1` | Small mip-chained textures the GPU drew are not written into guest memory a few frames later (where that memory is still zero, or still their own last write-back), as before: Ocarina of Time's ground garbles after some minutes in one area (its copies of drawn terrain tiles are all zero) |
| `RX_VK_NO_WRITEBACK` (set) | GPU writes to mirrored pools are not copied back to guest memory (an experiment, for A/B) |
| `RX_VK_WB_DIFF=0` | Write-backs copy every byte again instead of only what changed against a VRAM shadow (the diff is on by default; v2.0.9 and v2.0.10 had it off, issue 1161) |
| `RX_VK_DLSS=0/1/2`, `RX_VK_DLSS_PRESET=<letter>`, `RX_VK_DLSS_LOG=1`, `RX_VK_REFLEX`, `RX_VK_REFLEX_LOG=1` | NVIDIA DLSS and Reflex for one run: the readme's NVIDIA DLSS section. `RX_VK_DLSS_TRACE=<frames>` logs one frame's large draws every that many frames, `RX_VK_DLSS_DEBUG=motion` shows the motion vectors, `=alternate` the game's own upscale every other frame |
| `RX_VK_FSR4=0/1/2`, `RX_VK_FSR_DIR=<folder>` | AMD FSR 4 for one run (0 never, 1 FSR 4, 2 its Native AA), and another folder for AMD's DLLs (not from `hookshot_env.txt`): the readme's AMD FSR 4 section. `RX_VK_FSR4=1/2` and `RX_VK_DLSS=1/2` go over the Upscaler row, a saved FSR 3.1 too where the engine they ask for runs; `RX_VK_FSR=1` goes over them (one upscaler at a time). The DLSS trace and debug switches above apply to FSR 4 too, on the frame chain they share |
| `RX_VK_ASYNC=1` | The replay on a thread of its own instead of the game's submitting thread |
| `HOS_VCORES=0`, `HOS_CORES=<1..8>`, `HOS_RESERVE_CORE=0` | No virtual cores; the cores the game sees (6); no core kept for the replay (one is, with 8 or more logical processors) |
| `HOS_VCORE_STARVE_US=<microseconds>` | A game thread that has waited this long for a virtual core (default 1000000, 1 s; 0: never) goes before every other waiter at the next release of a core it may use, and asks the holder to give the core up at its next checkpoint whatever the holder's priority, so a wait that better threads keep winning ends in a grant, not in the escape after 2 s (`HOS_VCORE_ESCAPE_US`, `HOS_VCORE_ESCAPE_EQ_US`). The `vcores:` line every 5 s counts these grants (`starved grants`) |

### Building

| Switch | What it does |
|---|---|
| `HOOKSHOT_JOBS=<n>` (or `--jobs`) | Compilers at once (default one per CPU thread, each started when the free memory holds it; `HOOKSHOT_MEM_GATE=0` turns that check off) |
| `HOOKSHOT_LIFT_JOBS=<n>` | Processes that write the lifted C (default the cores, at most 16) |
| `HOOKSHOT_KEEP_BUILD_FILES=1` | Keeps the lifted C and the objects after a build, so that the next one compiles only what changed |
| `HOOKSHOT_MINGW=<dir>`, `HOOKSHOT_MSVC=1` | The llvm-mingw to build with; the Visual Studio route instead |
| `RX_CPM=<dir>`, `WGPU_NATIVE=<dir>` | The CPM cache (Vulkan and SPIR-V headers, VMA, FFmpeg) and wgpu-native |
| `RX_TARGET=windows` | On Linux: the Windows game, with llvm-mingw's Linux build in `HOOKSHOT_MINGW` |
| `RX_RELEASE=1` | A release build (`make_game.py` sets it): no console window, the shaders prepared before the title |
| `RX_PGO=gen` or `RX_PGO=<file>` | The hot code counting its branches for a profile, or built with one (`make_game.py` passes the shipped profile) |
| `RX_VK_PGO=gen`, `RX_VK_PGO=<file>` or `RX_VK_PGO=0` | The Vulkan back end's play profile (`nx2/vk_pgo.py`, after Hookshot's #569 and ProjectReLink). Unset: `<work>/hookshot_vk.profdata` when there is one and its stamp names this checkout's back end sources (`gpu/vk/*.cpp`, `*.h`, suyu's texture decoders) and this compiler, else the back end as without it; the build log says which and why. `gen` (Linux only): the back end's own files count their branches; run with `LLVM_PROFILE_FILE=<dir>/hookshot_vk-%p.%cprofraw` (written as it counts, so the game can end any way), then `python -I scripts/vk_profile.py merge <dir>` from the same checkout writes the profile and its `.stamp`. `<file>`: that profile, checked the same way. `0`: none |
| `RX_MARCH=<level>` | The CPU level to build for (default this PC's: `x86-64-v3`, else `x86-64-v2`, else `x86-64`) |
| `RX_VK_MMA_UNROLL=0` or `1` | How the game's upscale networks multiply where the GPU has no cooperative matrices (the Steam Deck and other AMD GPUs before RDNA 3, GTX 10 and 16, Intel's integrated GPUs; `gpu/vk/eden/shader_recompiler/backend/spirv/mma_software.h`): 0 the loop, as before; 1 the unrolled form, tried on any GPU. By default it is tried on RADV, where Longshot measured it (Steam Deck, the gameplay network at Resolution 100%: 3.82 ms a frame instead of 8.61; not measured in HeyListen). Either way a self-test at start runs both forms on the device and keeps the unrolled one only where every word of it is the loop's; the log's `vk: software matrix multiplies` line says which. It changes the shaders' translation (one preparation of the shaders) |
| `RX_VK_ONLY=1` | Only the Vulkan back end, with no lift |
| `OOT_WORK`, `OOT_DUMP`, `OOT_EXEFS`, `OOT_ROMFS` | The work folder and the dump's folders, over `work/dump.txt` |

### Crash reports and performance statistics

Both are opt-in and off by default; the addresses and the rows are in [performance statistics](../../docs/performance-statistics.md)
and the [user guide](../../docs/USER_GUIDE.md#privacy-crash-reports-and-performance-statistics). Without an address in the
build (`nx2/addresses.ini` `telemetry=` is empty) nothing can be sent and the menu's two rows say "Not available in this build".

| Switch | What it does |
|---|---|
| `HL_TELEMETRY_URL=<https address>` | The telemetry server for this run, over the build's address (a test against a local server). Only from the environment: `hookshot_env.txt` is refused it. A bad address (not https, spaces, a login in it) is no address: nothing is sent |
| `HL_TELEMETRY_TEST_HTTP=1` | With `HL_TELEMETRY_URL`, also takes a plain `http://` address on this PC itself (`127.0.0.1`, `localhost`, `[::1]`), so a test server needs no certificate. For tests only; nothing else is ever sent over http |
| `HOS_CRASH_TEST=start` or `<seconds>` | The game crashes on purpose (a write to address 16), just after the crash handlers are set up or that many seconds later, to see the crash window. Not taken from `hookshot_env.txt` |
| `HOS_CRASH_WATCH=0` | No crash window: the helper (`HeyListen --crash-watch <pid> <run id> <game folder> <log> -`, the game's own executable, started early) is not started, and a crash is only recorded in `bug-reports/pending` |
| `RX_VK_PERF_INTERVAL=<seconds>` | Seconds of play between two performance summaries (60 to 840, 300 by default) |
| `HL_PERF_MAX_SENDS`, `HL_PERF_TIMEOUT`, `HL_PERF_BACKOFF=0` | The sender's limits: summaries a start (12), a request's seconds (10), and whether a busy server is left alone until its `Retry-After` (on) |
| `send_crash_reports=1`, `send_performance_stats=1` | The two menu rows as `hookshot_options.ini` keeps them (`Send crash reports automatically`, `Send performance statistics`); anything but `1` is off |

## The options file

`hookshot_options.ini`, in the game folder, holds the Insert menu's rows as `<key>=<number>`: the row's name in lower
case with `_` for spaces (`frame_generation=2`, `invert_camera_x=1`), except Output resolution, which is `output_res`.
`gpu/vk/vk_menu.cpp` writes the whole file at every change (`save`), with `options_version=3`,
`microphone_device=<name>` when a microphone is chosen by name, and the controls (`pad_<button>` and `key_<button>`:
the README's [Controls](../readme.md#controls)). A value outside a row's range is clamped when it is read. Where the
number is not the obvious one:

| Key | Values |
|---|---|
| `quality_preset` | 0 Custom, 1 Minimum (4GB), 2 Low (6GB), 3 Balanced (8GB), 4 High (12GB), 5 Ultra (16GB+) |
| `gpu_device` | 0 Auto, n the n-th card with Vulkan 1.3 as the row lists them |
| `frame_generation` | 0 Off, 2 to 8 the multiplier |
| `resolution` | 0 Auto, 1 100%, 2 75%, 3 67%, 4 50% |
| `output_res` | 0 2048x1152, 1 2560x1440, 2 3840x2160, 3 1920x1080, 4 5120x1440 (or the size written out, `1920x1080`; no other size) |
| `upscaling` | 0 Off, 1 Catmull-Rom, 2 Lanczos |
| `sharpness`, `rumble_strength`, `motion_sensitivity_x`, `motion_sensitivity_y` | percent |
| `mouse_sensitivity` | hundredths: 100 is 1.00 |
| `robust_buffers` | 0 Auto (on), 1 Off, 2 On |
| `game_speed` | 0 Per frame, 1 Real time |
| `text_speed` | 0 Normal, 1 2x, 2 4x, 3 Instant |
| `microphone` | 0 Off, 1 Default, 2 and up a microphone (the name in `microphone_device` decides which) |
| `mouse_camera` | 0 As the stick, 1 Native |
| `layout_preset` | 0 Nintendo, 1 Xbox / PC, 2 Swap Face, 3 Custom |

The other rows are 0 Off and 1 On. Some are read at the start, before the menu exists, by the code that needs them:
`handheld_profile` and `output_res` (`host/nn_applet.c`), `ultrawide` (`host/hos_main.c`), `robust_buffers`
(`gpu/vk/vk_main.cpp`), the controls (`host/nn_hid.c`) and `skip_title_screen` (`host/oot_autoload.c`).

## Layout

| Path | What |
|---|---|
| `docs/design.md` | why mkw-recomp's pipeline, the architecture, MKW's lessons as rules, stages, testing |
| `nx2/` | the lift pipeline and its tests (mkw-recomp's, made game-neutral; `kit.py` has the paths), and `make_game.py`, which the builders run |
| `host/` | the standalone Horizon runtime (TOTKPort's `hos/` as adapted by mkw-recomp), with the native NVN2 layer `nvn_native.c`; `host/posix` has the Win32 calls on Linux |
| `gpu/vk/` | the Vulkan back end (`hookshot_vk.dll`, `hookshot_vk.so`), with Eden's shader recompiler and sirit |
| `gpu/render/`, `gpu/third_party/` | the WebGPU renderer the Windows exe falls back to when it was built with wgpu-native, its op stream (`rx_nvn.h`), suyu's decoders |
| `builder/` | Hookshot Builder: the Windows app (.NET 8 WPF), its setup and MSI, and the Linux builder and launcher scripts |
| `scripts/` | test runs, the profile and log readers, the save copier (`oot_saves.py`), release tooling, `scripts/linux` (dependencies, AppImage) |
| `tests/` | the game-free checks, all run by `tests/run_all.py` |
| `third_party/` | a64-kit (Hookshot's few changes marked `(hookshot)`), libopus, zbic (Atmosphere's zstd with ZBIC, for unpacking only), SDL2's licence, Dear ImGui and its Vulkan backend, zstd's decoder, the Libertinus fonts |

License: GPL-3.0-or-later, as mkw-recomp: a64-kit's translator is suyu's (GPL-2.0-or-later) and Eden's code is
GPL-3.0.

## Native replacements of hot guest code (TICK-026)

A hot guest block (or function) can be handed to a host function written from its observed behaviour. The lifted C is
never edited by hand: `nx2/native_replace.py` (run by `nx2/oot_patches.py` after every lift) puts a gate at the
block's entry and a differential check at its exit label(s); the native code lives in `host/hos_native.c`.

- **Switch per replacement**: `HOS_NR_<NAME>` = `0` (guest code; the default until a replacement is proven, set in
  `nr_info[].default_mode`), `1` (native) or `diff`.
- **Differential mode** (`HOS_NR_<NAME>=diff`, debug only): 1 call in `HOS_NR_DIFF_EVERY` (default 64) per thread runs
  the native code first on a copy of the registers, with its stores held in a log (`nr_st`); the guest code then runs
  as usual and, at the block's exit, x0-x31, NZCV and the logged stores are compared with the guest's. The log
  (`native: <name> differential: N checks, D differences`) prints at every 2^k checks and every 100,000; the first 20
  differences are logged in full. Stores the guest makes that the native code does not make are not seen: each
  replacement documents the memory its guest block writes.
- **Safety**: an entry records SHA-1 hashes of the block's lifted text regions the native code was written against. If
  a game update or lifter change alters them the gate is not added (the guest code runs) and the lift prints
  `native_replace: ... NOT applied` with the new hashes.
- **Adding one**: in `host/hos_native.c` add an enum value, an `nr_info` row, the native function (guest memory through
  `nr_ld*` / `nr_st`, the guest address and the observed semantics in its comment; no game code or data copied) and
  `int hos_nr_<name>(Ctx *c) { return nr_gate(c, ID, fn); }`; in `nx2/native_replace.py` add an entry (same id: head,
  entry text, exit labels, goto label, regions and hashes). Lift (`dev-build.ps1 -Lift`), run a full bench run with
  `HOS_NR_<NAME>=diff` (0 differences), then measure A/B with `=1` against `=0`.
- **Finding candidates**: `python -I scripts/hotmap.py <HOS_SAMPLE_ALL file>` (with `RX_MAPS` at the build's maps):
  the critical chain's guest functions by inclusive and self time, with threads, callers and callees
  (`docs/hotmap.md`).
