# HeyListen: user guide

HeyListen is a fork of [Hookshot](https://github.com/NotRexed/Hookshot) that turns **your own** Switch 2 copy of
*The Legend of Zelda: Ocarina of Time* into a native Windows game (Linux: see [the Linux section](#linux) below, a
first and little tested release). Nothing from the game is downloaded: the HeyListen Builder builds the game on your PC
from the files you give it.

## What you need

- Windows 10 or 11, 64-bit, or Linux (see [the Linux section](#linux)); a CPU with AVX2, 16 GB of RAM or more.
- A graphics card with Vulkan 1.3 and mesh shaders. **12 GB of VRAM recommended**; tested on **NVIDIA RTX** cards (Windows);
  on Linux a contributor plays on an **AMD Radeon RX 9070 XT** (RADV).
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

## Linux

**New and little tested.** The Linux AppImage comes from v0.3.2. We have no Linux PC of our own: it was checked on Windows
Subsystem for Linux, which has no graphics card for Vulkan (so the game itself was not played there; see the release
notes for exactly what ran), and a contributor reported the Linux build of the `main` branch (not this release) working
on CachyOS with a Radeon RX 9070 XT (Mesa's RADV driver). Expect problems and report them (see Problems below).

- **Download** `HeyListen-<version>-x86_64.AppImage` from the [latest release](https://github.com/Sacrappy/HeyListen/releases/latest).
  It is the builder with a compiler, Python, FFmpeg, SDL2 and the Vulkan back end inside; nothing else is installed.
  The release's `heylisten-linux.txt` is the list of parts the update downloads (you do not need it).
- **Needs**: a 64-bit Linux with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora, Arch, SteamOS...), a CPU with AVX2,
  16 GB of RAM or more, about 60 GB free while building, FUSE 2 for AppImages (or run it with
  `--appimage-extract-and-run`), an X11 or Wayland desktop, and a GPU whose driver gives Vulkan 1.3 with mesh shaders
  (`vulkaninfo | grep mesh_shader` lists `VK_EXT_mesh_shader`; Mesa for AMD and Intel, NVIDIA's driver). 12 GB of VRAM
  recommended. Your game is the same `.nup` as above.
- **Run**: allow the file to run (`chmod +x HeyListen-*-x86_64.AppImage`, or the file manager's "Allow executing file as
  program") and start it. Choose your `.nup` (or the `romfs` and `exefs` folders) and the language, keep the game
  folder `~/Games/HeyListen` or choose another, press **Build**, wait (10 to 45 minutes, then the shader preparation at
  the first start), then **Play**. Keep the AppImage where it is: the launcher uses it to update.
- **Without the window**: `./HeyListen-<version>-x86_64.AppImage --cli --nup ~/your-game.nup --out ~/Games/HeyListen`
  builds, `--play [folder]` starts the game, `--update <folder>` updates a folder.
- **Where things are kept**
  - The game folder (`~/Games/HeyListen`): `HeyListen` is the launcher (Play, and Update when a release is newer; the
    applications menu entry "HeyListen" opens it), the game itself is `bin/hookshot`, and the folder keeps the game's
    settings (`hookshot_options.ini`, `hookshot_env.txt`, `language.txt`), the shader lists and the game files an update
    builds from.
  - **Saves**: `$XDG_DATA_HOME/heylisten/save/` (`~/.local/share/heylisten/save/`). Keep it when you move the game.
  - The builder, after an update: `~/.local/share/heylisten/builder`. Its settings: `~/.config/heylisten/builder.ini`. Its
    build files (the translated code and objects, which an update reuses): `~/.cache/heylisten-builder`. The menu entry:
    `~/.local/share/applications/heylisten.desktop`.
  - To uninstall, delete the game folder, these four, and the AppImage.
- **If you used Hookshot on Linux before**: HeyListen shares nothing with it. Hookshot's saves in `~/.local/share/hookshot`,
  its builder and its settings stay where they are, untouched, and HeyListen starts with no save. To carry on with a
  Hookshot save, close both games and copy `~/.local/share/hookshot/save` to `~/.local/share/heylisten/save` (copy, do not
  move).
- **If the game runs at a few frames a second**: it cannot use `/dev/udmabuf`. The game's log then says
  `guest memory as dma-bufs: no (/dev/udmabuf cannot be opened)`, and every block of the game's memory reaches the GPU by a
  slower way (3 to 8 fps in Kokiri Forest on the Radeon above). Some distributions give that device to the logged-in user,
  others only to the `kvm` group: `sudo usermod -aG kvm $USER`, then log out and back in. If the device does not exist,
  load its kernel module: `sudo modprobe udmabuf`.
- **Stutter**: programs that use a lot of CPU or GPU while you play (a local AI model, a big compile) make the game
  stutter much more than on a quiet PC.
- **Building it yourself** instead of using the AppImage: `engine/CONTRIBUTING.md` (Linux) has the tools to install
  (`scripts/linux/deps.sh` fetches the rest, about 30 minutes the first time) and `engine/readme.md` the `make_game.py`
  command, which takes your `.nup` and makes the game folder as the builder does. A game built this way from the `main`
  branch is started with `./hookshot` in its folder and keeps its saves in `~/.local/share/hookshot/save`.
- **Updates**: the builder asks GitHub for `Sacrappy/HeyListen`'s latest release (nothing is sent about you) and downloads
  only the parts of the AppImage that changed from this repository's `linux-parts` release, checked by SHA-256, into
  `~/.local/share/heylisten/builder`; then it builds your game folder again from the game files the folder keeps. A
  release that has no Linux files yet cannot be installed on Linux: wait for its AppImage to appear on the release page.

## Mods

File mods work as in Hookshot: put them in a `mods` folder in the game folder, beside `HeyListen.exe`, in the layout
emulators use: `mods\<name>\romfs\...` holds files that replace or add to the game's own. A folder whose name starts with
`-` or `_` is off; when two mods have the same file, the folder whose name sorts last wins (name them `01-...`, `02-...`).
Nothing is rebuilt: the game reads them at its next start, and `hookshot_log.txt` names each mod and the files read from it.
Code patches (`exefs`, `.pchtxt`, `.ips`) are not loaded: the game's code is compiled ahead of time. A mod that makes
files bigger must ship the game's resource size table with its entries grown; the details are in `engine/readme.md`
(Mods).

## Updates

When a new HeyListen release is out, the builder says so when it opens (it asks GitHub for
`Sacrappy/HeyListen`'s latest release; nothing is sent about you). **Update** downloads only the parts that changed and builds
your game folder again from the game files the folder keeps: you do not need the `.nup` again, and your saves stay.
The in-game update offer is off unless you turn it on (`HOOKSHOT_UPDATE_CHECK=1` in the game folder's `hookshot_env.txt`).

## Known issues

Occlusion is off; rare missing or teal textures while walking when VRAM runs short; tested on NVIDIA RTX (Windows; Linux: see its
section); see the release notes for the full list.

## Problems

The builder's problem page has *Copy for a bug report*. Report issues at
<https://github.com/Sacrappy/HeyListen/issues> with your GPU, driver and the log. Do not attach game files, keys or saves
with personal data.

## Credits and license

HeyListen is GPL-3.0-or-later. It is a fork of Hookshot by NotRexed and its contributors (GPL-3.0), whose recompiler,
builder and renderer do the work; see `THIRD_PARTY_NOTICES.md`. The Legend of Zelda and Ocarina of Time are Nintendo's;
HeyListen is not affiliated with Nintendo and ships none of its assets.
