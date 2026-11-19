# HeyListen: user guide

HeyListen is a fork of [Hookshot](https://github.com/NotRexed/Hookshot) that turns **your own** Switch 2 copy of
*The Legend of Zelda: Ocarina of Time* into a native Windows game (Linux: see [the Linux section](#linux) below, a
first and little tested release). Nothing from the game is downloaded: the HeyListen Builder builds the game on your PC
from the files you give it.

Questions and help: the [HeyListen Discord](https://discord.gg/34GV5hdC3Q). Bugs: [GitHub issues](https://github.com/Sacrappy/HeyListen/issues).

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
(graphics, controls, frame rate, cheats); on a controller, press Home, the touchpad click (DualSense, DualShock 4) or
Select and Start together. The same press closes it. If none of those suits your pad, bind your own on the Controls page:
the **Open menu** row takes one gamepad button, or two pressed together (L3 + R3, Share + Options). It is not set by default, and
the ways above keep working. A combination held does not reach the game. When a controller is opened the game's log
(`hookshot_log.txt`) says what it is and which buttons open the menu. Frame generation is off by default.

### Upscaler

The menu's Graphics tab has an **Upscaler** row: what scales the 3D picture up to the output. **Game's own** (the default) is
the game's own upscaling network. **NVIDIA DLSS**, **NVIDIA DLAA**, **AMD FSR 3.1**, **AMD FSR 4** and **AMD FSR 4 Native AA** are
offered where they run. **Off** is on every GPU: the game's upscaling network is not run at all, and the 3D is drawn at the
**Render resolution** and stretched to the output, with no anti-aliasing. Try it if the game's own network shows glitches on your
GPU (some older AMD cards, handhelds), or to save its GPU time. Render resolution works as with the game's own upscaler (at 100% the
picture is the 3D as drawn), **Sharpness** still applies (it works on the picture after the upscaler), and frame generation is
unaffected. In `hookshot_options.ini` it is `dlss=6`; the launcher's Upscaler setting has it as **Off**.

## Install with RecompLauncher

[RecompLauncher](https://recomplauncher.com) installs HeyListen for you, like a PC game, and keeps it up to date. It does
not replace the builder: it runs it for you, with no window.

1. In RecompLauncher, find **HeyListen** and install it.
2. On its setup screen, give it your copy of the game: the `.nup` file (program `0400486024DEC000`, update v131072, build
   `e0f3ccfa`), or the folder with its `romfs` and `exefs`. It checks that it is the right version.
3. Tick **NVIDIA DLSS** or **AMD FSR 4** if you want them (each asks you to accept its maker's license; the files are
   downloaded from NVIDIA or AMD to your PC, checked, put in place for you, and never shared; nothing to copy by hand). On an NVIDIA RTX card the launcher also fetches NVIDIA's own `nvngx_dlss.dll`
   (the optional **NVIDIA DLSS runtime** item of Easy Setup, which you can switch off) into the game's `ngx` folder.
   Two more ticks are **off** unless you turn them on: **Send anonymous performance statistics** and **Send crash reports
   automatically**. They only write `send_performance_stats=1` and `send_crash_reports=1` into the game's
   `hookshot_options.ini`, the two rows of the in-game menu (Gameplay tab); what they send is in
   [performance-statistics.md](performance-statistics.md). Change them any time in RecompLauncher's **Settings**, group
   **Privacy** (Off or On), or in the menu. Then let it build. This takes the same 30 to 60 minutes as the builder and needs
   the same space; the screen shows the progress.
4. Press **Play**. Video and gameplay settings (fullscreen, resolution, upscaler, frame generation, frame rate, text speed and
   more) are on the game's page in RecompLauncher; they change `hookshot_options.ini` in the game folder, the same file the
   in-game menu (Insert or F1) saves. Your saves are in the game folder's `user\save` (Windows) or
   `~/.local/share/heylisten/save` (Linux), and the launcher backs them up.

The game is made in the `HeyListen` folder inside the folder RecompLauncher installed HeyListen in (the install folder is
RecompLauncher's own setting: HeyListen's profile cannot choose it; ask for it in the launcher). To build it again (after
an update that says so, or with the other options ticked), change your copy on the setup screen or reinstall; the builder's
own window works on the same folder if you start it yourself. The first start prepares the game's shaders, as it does after a
build made with the builder.

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

- **Download** `HeyListen-<version>-Linux-x64.AppImage` from the [latest release](https://github.com/Sacrappy/HeyListen/releases/latest).
  It is the builder with a compiler, Python, FFmpeg, SDL2 and the Vulkan back end inside; nothing else is installed.
  The release's `heylisten-linux.txt` is the list of parts the update downloads (you do not need it).
- **Needs**: a 64-bit Linux with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora, Arch, SteamOS...), a CPU with AVX2,
  16 GB of RAM or more, about 60 GB free while building, FUSE 2 for AppImages (or run it with
  `--appimage-extract-and-run`), an X11 or Wayland desktop, and a GPU whose driver gives Vulkan 1.3 with mesh shaders
  (`vulkaninfo | grep mesh_shader` lists `VK_EXT_mesh_shader`; Mesa for AMD and Intel, NVIDIA's driver). 12 GB of VRAM
  recommended. Your game is the same `.nup` as above.
- **Run**: allow the file to run (`chmod +x HeyListen-*-Linux-x64.AppImage`, or the file manager's "Allow executing file as
  program") and start it. Choose your `.nup` (or the `romfs` and `exefs` folders) and the language, keep the game
  folder `~/Games/HeyListen` or choose another, press **Build**, wait (10 to 45 minutes, then the shader preparation at
  the first start), then **Play**. Keep the AppImage where it is: the launcher uses it to update.
- **Without the window**: `./HeyListen-<version>-Linux-x64.AppImage --cli --nup ~/your-game.nup --out ~/Games/HeyListen`
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
- **A long hitch (up to a second) the first time an area is used**, with `vk: hint: udmabuf's limit is 64 MB` in the log:
  the kernel lets `/dev/udmabuf` share at most 64 MB at once by default, so the game builds its larger memory blocks from
  several pieces, and that can hold up a frame. Raise the limit until the next reboot with
  `echo 1024 | sudo tee /sys/module/udmabuf/parameters/size_limit_mb` before you start the game, or for good with a file
  `/etc/modprobe.d/udmabuf.conf` holding the line `options udmabuf size_limit_mb=1024` (then reboot). The log then says
  `dma-bufs of at most 1024 MB`.
- **Stutter**: programs that use a lot of CPU or GPU while you play (a local AI model, a big compile) make the game
  stutter much more than on a quiet PC.
- **Building it yourself** instead of using the AppImage: `engine/CONTRIBUTING.md` (Linux) has the tools to install
  (`scripts/linux/deps.sh` fetches the rest, about 30 minutes the first time) and `engine/readme.md` the `make_game.py`
  command, which takes your `.nup` and makes the game folder as the builder does. A game built this way is
  started with `./HeyListen` in its folder (the launcher; the game itself is `bin/hookshot`) and keeps its saves where the
  AppImage's game does, `~/.local/share/heylisten/save`.
- **Updates**: the builder asks GitHub for `Sacrappy/HeyListen`'s latest release (nothing is sent about you) and downloads
  only the parts of the AppImage that changed from this repository's `linux-parts` release, checked by SHA-256, into
  `~/.local/share/heylisten/builder`; then it builds your game folder again from the game files the folder keeps. A
  release that has no Linux files yet cannot be installed on Linux: wait for its AppImage to appear on the release page.

## Mods

File mods work as in Hookshot: put them in a `mods` folder in the game folder, beside `HeyListen.exe`, in the layout
emulators use: `mods\<name>\romfs\...` holds files that replace or add to the game's own. A folder whose name starts with
`-` or `_` is off; when two mods have the same file, the folder whose name sorts last wins (name them `01-...`, `02-...`).
Nothing is rebuilt: the game reads them at its next start, and `hookshot_log.txt` names each mod and the files read from it.
A mod that makes files bigger must ship the game's resource size table with its entries grown; the details are in
`engine/readme.md` (Mods).

### Code mods (exeFS)

Code patches (`.ips`, `.pchtxt`) change the game's own instructions. The game's code is translated to PC code when it
is built, so the **builder** applies them, before the translation, and the game has to be built again for them to count.
Nothing is read from them when the game starts.

1. Put the mod in `mods\<name>\exefs\` of the game folder: an `.ips` file named after the module's build id
   (`<build id>.ips`, as Atmosphere reads it) or a `.pchtxt` file (any name; it names its build id on its `@nsobid-` line).
   Only mods made for this game's build are accepted: a patch for another version stops the build with the mod's name.
2. Open the builder. On the game folder's start page choose **Code mods (exeFS)**: it lists the mods it found with their
   patches and what each applies to, tells the risks and how long the rebuild takes, and rebuilds the game when you confirm.
   (When you build the game the first time, tick **Code mods (exeFS)** on the options page instead.) The choice stays with
   the game folder: an update of the game applies the mods again. To take a code mod out, put `-` before its folder's name
   (or delete it) and rebuild with the option on, or rebuild with it off.
3. Start the game. `hookshot_log.txt` says which functions a code mod left to the translated code.

A game that is already built shows only **Play** (and **Update** when a new version exists) as buttons; the way to build it again with code mods is the 
link **Mods: rebuild the game with code mods (exeFS)...** under them. The build log says, mod by mod, which files were applied and which were not used.

What to expect:

- **Only `.ips` and `.pchtxt` patches are applied.** A mod that comes with a `subsdk` file (`subsdk4`, `subsdk7`, ...; made with exlaunch or Skyline) 
  is a second program that hooks the game while it runs, by rewriting the game's instructions in memory. A translated game has no such instructions 
  to rewrite, so these mods cannot work, and the builder lists the file as "NOT USED" with the reason. The small `.ips` patch such a mod also ships 
  is applied, but it is usually only the hook into the subsdk, so it does nothing useful. `main.npdm` is not read either.

- **A rebuild costs a lot of time**: the whole game is translated again from the patched code, then only the compiled parts
  that changed are compiled again. On the main PC (Ryzen 7 7800X3D, 8 build workers) the translation took 9 minutes and the compile and link 6 more: about 16 minutes in all; slower PCs take longer, and the builder shows its own estimate. Taking the mod out later costs the same. Without code mods nothing changes: a game
  built without the option is built exactly as before.
- **Code patches can break the game or the save**, or stop working after a game update. Keep a copy of your saves first.
- Offsets: both formats count from the start of the module's file, whose header is 0x100 bytes long (a module's own address is
  the offset minus 0x100); a `.pchtxt` offset gets its `@flag offset_shift` added first. A patch may change code, constants
  and data, but only inside the module (a write outside it, into its header, or between its parts is refused). Two mods that
  write the same byte differently are refused, the same value is fine.
- Where the native engine answers a function itself (graphics recording, job system, physics boxes and so on), a patch inside
  that function wins: the engine leaves that function to the translated code, which carries the patch, and logs it
  (`hl: ... left to the lifted code: an exeFS mod patches it`). The build log says `NOT gated` for each entry gate left out.
  That part of the engine's speed-up is off for that function.
- The OS functions the game folder answers itself (the console's system software) cannot be patched: the build says so.

## Updates

When a new HeyListen release is out, the builder says so when it opens (it asks GitHub for
`Sacrappy/HeyListen`'s latest release; nothing is sent about you). **Update** downloads only the parts that changed and builds
your game folder again from the game files the folder keeps: you do not need the `.nup` again, and your saves stay.

**When you start the game yourself** (`bin\HeyListen.exe` on Windows, `bin/hookshot` on Linux), it looks for a newer release
for a few seconds before it opens. Nothing is asked when you are offline, and the start is never held back. When a newer release
is out, a window offers:

- **update now**: HeyListen Builder updates the game folder as above (the game closes);
- **keep my version this time**: the game starts, and you are asked again next time;
- **skip this version**: the game starts, and you are asked again only when a later release is out;
- **never ask again**: the game starts and stops looking. Turn it back on in the game menu: **Settings**, **Gameplay**,
  **Check for updates**. The choice is `update_check=0` in the game folder's `hookshot_options.ini`.

When RecompLauncher starts the game on Windows, its Play runs `HeyListenBuilder-Setup.exe --play`, which looks for a newer
release first (the game itself is then told not to ask again) and, when there is one, asks in the same way: play my version, open
the download page, or stop asking (this release only, or for good). It installs nothing: RecompLauncher is what installs a newer
release. On Linux the AppImage's `--play` starts the game, and the game asks as above.

## Button prompts

The **Button prompts** row (Insert menu, Controls page) chooses which controller's icons the game shows in its texts and menus:
Auto follows the controller you plug in, or pick Xbox, PlayStation 4, PlayStation 5, Steam Deck or Switch. A new choice shows the
next time you start the game. The icons are drawn when the builder makes your game folder, from your own game files and
[Kenney's](https://kenney.nl/assets/input-prompts) CC0 Input Prompts, into the `prompts` folder there; they are not downloaded or
copied from anywhere, so a folder from another build is not needed. No `prompts` folder in your game folder (a game built by a release up to 0.4.2,
whose builder could not draw them): update to a newer release and build again to make it; the game's log says `prompts: no prompts folder` while it is missing.

## Known issues

Occlusion is off; rare missing or teal textures while walking when VRAM runs short; tested on NVIDIA RTX (Windows; Linux: see its
section); see the release notes for the full list.

## Privacy: crash reports and performance statistics

HeyListen sends nothing unless you turn it on, and nothing is turned on by default. Two rows on the Gameplay tab of the
in-game menu (Insert) control it:

- **Send performance statistics**: off by default. On, the game counts how its frames went (frame times, stutters and what
  they waited on, per map) and sends a summary of the counts at the next start. A summary holds the PC's GPU, driver, CPU,
  RAM, operating system, the upscaler, preset and frame rate cap, the game's version and the map names. It has no name, no
  ID and no path, and what the team keeps is totals for all players, nothing per player. `perf_sent` in the game folder keeps
  a copy of everything that was sent, to read.
- **Send crash reports automatically**: off by default. After a crash a window says what happened and asks. **Report
  manually** copies a report to your clipboard and opens a new issue on GitHub for you to paste it into; **Report
  automatically** shows you the exact report and sends it only when you press Send. With this row on, the report is sent at
  once and the window shows only if that fails. The report has the crash's error line, the game version, your GPU, CPU,
  RAM and operating system, the map, the last 150 lines of the game's log and its settings. Your home folder, user name and
  the names of your audio devices are taken out first. It never has your saves, the game's files or keys.

Turn either row off and what was waiting to be sent (`perf_pending`, `perf_sent`) is deleted. A build with no address
to send to shows both rows as *Not available in this build* and sends nothing. Details and the exact fields:
[performance statistics](performance-statistics.md).

## Problems

The builder's problem page has *Copy for a bug report*. Report issues at
<https://github.com/Sacrappy/HeyListen/issues> with your GPU, driver and the log. Do not attach game files, keys or saves
with personal data.

## Credits and license

HeyListen is GPL-3.0-or-later. It is a fork of Hookshot by NotRexed and its contributors (GPL-3.0), whose recompiler,
builder and renderer do the work; see `THIRD_PARTY_NOTICES.md`. The Legend of Zelda and Ocarina of Time are Nintendo's;
HeyListen is not affiliated with Nintendo and ships none of its assets.
