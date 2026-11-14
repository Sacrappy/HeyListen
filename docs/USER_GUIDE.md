# HeyListen: user guide

HeyListen is a fork of [Hookshot](https://github.com/NotRexed/Hookshot) that turns **your own** Switch 2 copy of
*The Legend of Zelda: Ocarina of Time* into a native Windows game. Nothing from the game is downloaded: the HeyListen Builder
builds the game on your PC from the files you give it.

## What you need

- Windows 10 or 11, 64-bit, a CPU with AVX2, 16 GB of RAM or more.
- A graphics card with Vulkan 1.3 and mesh shaders. **12 GB of VRAM recommended**; tested on **NVIDIA RTX** cards only.
- About 60 GB free while building, and an internet connection for the download (about 500 MB).
- Your own copy of the game, dumped from **your** Switch 2 as a `.nup` file: program `0400486024DEC000`, update v131072,
  build `e0f3ccfa` (the version Hookshot supports; other versions do not work). The `romfs` and `exefs` folders work too.
  HeyListen does not help with dumping and never asks for keys or firmware.

## 1. Dump your game

Dump Ocarina of Time from your own console as a single `.nup` file with the homebrew tool you normally use, and copy it to your
PC. Keep it: updates rebuild from the copy kept in the game folder, but it is your only source.

## 2. Run the builder

1. Download `HeyListenBuilder.exe` from the [latest release](https://github.com/Sacrappy/HeyListen/releases/latest) and run it.
   It downloads the builder and its tools (Python, the compiler and the rest, each checked by SHA-256, from their official
   releases) into `%LOCALAPPDATA%\HeyListenBuilder` and opens the HeyListen Builder. Nothing else is installed.
   Windows SmartScreen may warn about an unknown publisher: *More info* > *Run anyway*.
2. **Choose your `.nup`**: the builder lists the ones it finds in Downloads, Desktop and Documents; otherwise press
   *Browse...* or drop the file on the window. It checks that it is the supported version, and you pick the game language.
3. Choose where the game goes (default `%USERPROFILE%\Games\HeyListen`) and press **Build the game**.

## 3. Wait for the build and the shader preparation

The first build reads your game, translates its code to C and compiles it: expect about 10 to 45 minutes depending on your CPU and memory (you can use your PC meanwhile). Then
the game prepares its shaders on first start (a few minutes, with a progress window). Leave both to finish; later starts are
quick. If the preparation is closed early, or the shader cache (`hookshot_programs.bin`) loses what was prepared, the next
start prepares again. The builder keeps its build files in `%LOCALAPPDATA%\HeyListenBuilder` so updates only recompile what changed
(*Free up space* on the last page removes them).

## 4. Play

Press **Play**, or use the *HeyListen* shortcut on the desktop or in the Start menu. The game folder's `HeyListen.exe` starts it too
(the game itself is `bin\HeyListen.exe`, which is what Task Manager shows). **Insert** opens the in-game menu
(graphics, controls, frame rate, cheats). Frame generation is off by default.

## Saves

Saves are kept in the game folder, in `user\save\` (keep that folder when you copy or move the game). The builder backs them
up whenever it copies saves over them.

### Saves from and to an emulator

On the builder's last page (or the launcher page of a built game): **Import saves from an emulator** copies an Ocarina of
Time save from Lichii, Eden and its forks (Frizti, Orchid, PokeDinner, Eglantine), NX2navi, or any folder you pick, into
HeyListen; **Export saves** copies yours the other way. The builder shows what it found on your PC, says exactly what it
would copy, and copies nothing until you say *Yes*. The side that gets overwritten is backed up first to
`%LOCALAPPDATA%\HeyListenBuilder\save-backups`. Close the game and the emulator first. Start HeyListen once before importing,
so its save folder exists. (The copy is done by Hookshot's `scripts/oot_saves.py`; its guide is `engine/docs/saves.md`.)

## Updates

When a new HeyListen release is out, the builder says so when it opens (it asks GitHub for
`Sacrappy/HeyListen`'s latest release; nothing is sent about you). **Update** downloads only the parts that changed and builds
your game folder again from the game files the folder keeps: you do not need the `.nup` again, and your saves stay.
The in-game update offer is off unless you turn it on (`HOOKSHOT_UPDATE_CHECK=1` in the game folder's `hookshot_env.txt`).

## Known issues

Occlusion is off; rare missing or teal textures while walking when VRAM runs short; tested on NVIDIA RTX only; see the
release notes for the full list.

## Problems

The builder's problem page has *Copy for a bug report*. Report issues at
<https://github.com/Sacrappy/HeyListen/issues> with your GPU, driver and the log. Do not attach game files, keys or saves
with personal data.

## Credits and license

HeyListen is GPL-3.0-or-later. It is a fork of Hookshot by NotRexed and its contributors (GPL-3.0), whose recompiler,
builder and renderer do the work; see `THIRD_PARTY_NOTICES.md`. The Legend of Zelda and Ocarina of Time are Nintendo's;
HeyListen is not affiliated with Nintendo and ships none of its assets.
