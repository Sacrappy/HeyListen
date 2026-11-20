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

### Graphics: Upscaler, Render resolution and frame generation

The Graphics tab has these rows together, in this order (the Quality preset, Fullscreen and GPU device first, the picture's output
and window settings after):

1. **Upscaler**: what scales the 3D picture up to the output. **Game's own** (the default) is the game's own upscaling network.
   **NVIDIA DLSS**, **AMD FSR 3.1** and **AMD FSR 4** are offered where they run. **Off** is on every GPU: the game's upscaling
   network is not run at all, and the 3D is drawn at the **Render resolution** and stretched to the output, with no anti-aliasing. Try
   it if the game's own network shows glitches on your GPU (some older AMD cards, handhelds), or to save its GPU time. Render
   resolution works as with the game's own upscaler (at 100% the picture is the 3D as drawn), **Sharpness** still applies (it works on
   the picture after the upscaler), and frame generation is unaffected. In `hookshot_options.ini` it is `dlss=6`; the launcher's
   Upscaler setting has it as **Off**.
2. **Render resolution**: the share of the output the 3D is drawn at. With **NVIDIA DLSS** its last choice is **DLAA (native)**: the 3D
   at full size (100%) with DLSS's anti-aliasing, the sharpest and the slowest. With **AMD FSR 4** the same choice is **Native AA (FSR
   4)**. Other upscalers have no such choice. (They were Upscaler choices before; an options file saved with NVIDIA DLAA or FSR 4
   Native AA as the Upscaler is read as DLSS or FSR 4 with this Render resolution, and runs as before. `resolution=7` in the options
   file.)
3. **DLSS model** (with NVIDIA DLSS).
4. **Frame generation by**: who makes the made frames: **Built-in / AMD FSR 3.1** (the game's own interpolation, and AMD FSR 3.1's at 2x
   where it runs), **NVIDIA DLSS FG** (RTX 40 and newer, with a temporal upscaler, DLSS or FSR 4, and NVIDIA's runtime files) or
   **AMD FSR 3.1 FG**. In `hookshot_options.ini` it is `frame_generation_by` (0, 1, 2).
5. **Frame generation**, directly under it: Off, Auto and 2x to 8x, but only what the mode above makes. **NVIDIA DLSS FG** goes up to
   what NVIDIA's runtime makes on your GPU (**6x on an RTX 50, 2x on an RTX 40**) and has no Auto. **AMD FSR 3.1 FG** makes 2x only.
   **Built-in** has Auto and 2x to 8x. Changing the mode keeps the multiplier within the new mode's, never raising it (8x becomes 6x
   when you switch to DLSS FG on an RTX 50); a higher value saved before shows, and runs, as the top of the mode.
6. **Frame pacing**.

### Frame rate limit

**Frame rate limit** is the rate shown: the game's own frames and, with frame generation on, the frames made between them. Frame
generation Off or Auto: the game runs at the limit (with Auto, made frames fill what the game falls short of, up to the limit and your
display's refresh). With a fixed multiplier the game runs at the limit divided by it: at 120 with 3x the game runs at 40 and 120 frames a
second are shown; at 240 with 4x the game runs at 60. The game never runs under 20 frames a second for the limit: where the multiplier
would leave it less (60 with 4x or more) the multiplier is lowered to the highest that does not (3x at 60, 4x at 90, 6x at 120). The
FPS counter's line **Frame limit** says it, for example `120 fps (game 40 x 3)`. **Unlimited** (after 240; `frame_rate_limit=6`)
holds nothing back: the game runs as fast as the PC can, and every one of its frames gets its made frames (your display, and a
V-Sync setting of the driver, may still hold the shown rate). Above 60 the limit needs **Game speed** on Real time (the default); with
Per frame the game runs at 60 as before and the limit does not apply.

### HDR

The Graphics tab's **HDR** row sends HDR10 to an HDR screen (Windows: turn on **Use HDR** in the display settings). **Off** is the
SDR picture, **Expanded** is the game's SDR picture with its whites at the **HDR paper white** and its highlights up to the **HDR peak
brightness** (set those two rows to your screen's), and **Native (the game's)** is the game's own HDR picture, told your screen's peak
and paper white. With Native two more rows appear:

- **HDR tone mapper**: **Vanilla** (the default) keeps the game's own curve. **PsychoV35** is [RenoDX](https://github.com/clshortfuse/renodx)'s
  tone mapper for this game (its zelda-oot-oz mod, MIT, ported here by Duck912): the same mid grey, with highlights that roll off into
  the peak and keep their colour, and no banding in the sky and in the dark (the game's scene is drawn in 16-bit floats with it). It
  comes with rows of its own, each under 0 to 100 with 50 as the game's: **PsychoV35 UI brightness**, **color space**, **midgray**,
  **match tone slope**, **tone slope**, **hue clip emulation**, **exposure**, **highlights**, **shadows**, **contrast**,
  **saturation**, **bloom**, and **PsychoV35 reset all**, which puts them back to RenoDX's defaults. They apply at once.
- **HDR contrast** (while HDR is on; hidden with PsychoV35, which has its own contrast).

PsychoV35 works with every **Upscaler** (the game's own, NVIDIA DLSS, AMD FSR 3.1 and FSR 4, Off) and with frame generation, which keeps
the HDR picture at 10 bits. **DLAA before the bloom** (with NVIDIA DLSS, under **DLSS model**; off by default) smooths the scene before
the game's bloom and tone map, so the glow comes from a smooth picture. In `hookshot_options.ini` the rows are `hdr`, `hdr_tone_mapper`
(0 Vanilla, 1 PsychoV35) and `dlss_pre_bloom`. An options file from the earlier build that had **Native + our tone mapper** (`hdr=3`)
is read as Native with PsychoV35 (that tone mapper is gone: there is one). `RX_VK_PSYCHOV35_DLSS=0` turns PsychoV35's upscaler routes
off for troubleshooting.

## Install with RecompLauncher

[RecompLauncher](https://recomplauncher.com) installs HeyListen for you, like a PC game, and keeps it up to date. It does
not replace the builder: it runs it for you, with no window.

1. In RecompLauncher, find **HeyListen** and install it.
2. On its setup screen, give it your copy of the game: the `.nup` file (program `0400486024DEC000`, update v131072, build
   `e0f3ccfa`), or the folder with its `romfs` and `exefs`. It checks that it is the right version.
3. Tick **NVIDIA DLSS** or **AMD FSR 4** if you want them (each asks you to accept its maker's license; the files are
   downloaded from NVIDIA or AMD to your PC, checked, put in place for you, and never shared; nothing to copy by hand). On an NVIDIA RTX card the launcher also fetches NVIDIA's own `nvngx_dlss.dll`
   (the optional **NVIDIA DLSS runtime** item of Easy Setup, which you can switch off) into the game's `ngx` folder.
   In the game's menu (Graphics, **DLSS model**) you choose **DLSS 4** or **DLSS 4.5** (preset M or L) for every mode, DLAA included, on any RTX card. **DLSS 3** (the older CNN model) is lighter on video memory. The first time, the game picks DLSS 3 on a card with less than 7 GB of video memory, else DLSS 4 on RTX 20 and 30 (DLSS 4.5 costs them more GPU time, but you can use it) and DLSS 4.5 on RTX 40 and 50, and saves it; after that your choice is kept.
   Two more ticks are **off** unless you turn them on: **Send anonymous performance statistics** and **Send crash reports
   automatically**. They only write `send_performance_stats=1` and `send_crash_reports=1` into the game's
   `hookshot_options.ini`, the two rows of the in-game menu (Gameplay tab); what they send is in
   [performance-statistics.md](performance-statistics.md). Change them any time in RecompLauncher's **Settings**, group
   **Privacy** (Off or On), or in the menu. Then let it build. This takes the same 30 to 60 minutes as the builder and needs
   the same space; the screen shows the progress.
4. Press **Play**. Video and gameplay settings (fullscreen, resolution, upscaler, frame generation, frame rate, text speed and
   more) are on the game's page in RecompLauncher; they change `hookshot_options.ini` in the game folder, the same file the
   in-game menu (Insert or F1) saves. On a screen that is not 16:9 (an ultrawide, 4:3, 16:10), set **Ultrawide** to the
   screen's shape: the HUD keeps its 16:9 size, its corners at the screen's edges, and the in-game menu's **HUD layout** row
   (Graphics) puts it back in the middle 16:9, either in RecompLauncher or in the game. Your saves are in the game folder's
   `user\save` (Windows) or `~/.local/share/heylisten/save` (Linux), and the launcher backs them up.

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
- **"Only a software renderer (llvmpipe) was found"**: a window with this message, and the game closes (its log has
  `vk: only a software renderer was found`). The game needs your graphics card's own Vulkan driver, and Vulkan listed
  only Mesa's `llvmpipe`, a renderer that runs on the CPU and cannot run the game (`vulkaninfo --summary` lists the same
  device). Install your distribution's Mesa Vulkan driver for the GPU: `mesa-vulkan-drivers` (Debian, Ubuntu, Fedora;
  it has AMD's and Intel's) or `vulkan-radeon` for AMD and `vulkan-intel` for Intel (Arch), or NVIDIA's own driver
  (your distribution's NVIDIA driver package) for NVIDIA. Then check that `vulkaninfo --summary` lists your card and
  start the game again. A game started inside a container, a Flatpak or a
  sandbox that does not give it `/dev/dri` sees no card either: allow that device, or start the game outside it. The same
  window can appear on Windows: install or update the graphics driver from AMD, NVIDIA or Intel there.
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

## V-Sync and G-Sync / FreeSync (VRR)

Three rows on the Insert menu's Graphics tab (Insert or F1), all applied at once, no restart.

**V-Sync** says whether frames wait for the screen's refresh.

- **Off** (the default, how the game always was): the latest frame is shown at once. The least delay; the picture can tear
  where the game's frame rate and the screen's do not match.
- **On**: frames wait for the refresh: no tearing, a little more delay, and the frame rate cannot pass the refresh rate. With
  frame generation, a multiplier the refresh has no room for shows fewer made frames per game frame (the game keeps its own rate).
  **V-Sync mode**, just below, chooses how many images the swapchain keeps.

**V-Sync mode** is greyed while V-Sync is Off; it says so in its hint there.

- **FIFO (double buffering)**: two images. The least delay with V-Sync, but a frame that misses its refresh waits a whole one:
  below 60 fps at 60 Hz the game falls to 30. A display or driver that asks for at least three images cannot do it (the row's
  hint and the log say so).
- **FIFO (triple buffering)** (the default, and what V-Sync On always was): three images, one kept free, so a slow frame costs
  a refresh rather than half the frame rate.
- **Mailbox (triple buffering)**: the Mailbox present mode with three images (four with frame generation). No tearing, the game
  can render past the refresh instead of waiting for it, and the newest frame is shown. Mailbox needs a spare image, so it is
  triple buffered by nature: there is no "Mailbox double". It is offered only where the display lists Mailbox; AMD's Windows
  driver does not, so the row there says "not offered" and a saved Mailbox falls back to FIFO (triple buffering), said in the log.

With frame generation on, the game keeps the images frame generation needs whatever the row says: FIFO (double buffering) uses
three (the row's hint says so), and Mailbox uses four.

The game's log (`logs/`) has one `vk: swapchain` line with V-Sync on or off, the mode, the present mode and the image count
actually used; the F10 overlay's Presentation line shows the present mode. Both are useful in a report.

**VRR (G-Sync / FreeSync)** is **new**, for a screen with a variable refresh rate. **Off** by default: nothing changes until
you turn it on. **On** gives the screen what it needs to follow the game's frame rate:

1. V-Sync On's present mode (no tearing); VRR uses FIFO whatever the V-Sync rows say while it is On.
2. In fullscreen, the game's window is made as a borderless popup window covering the screen, which Windows can show directly
   (independent flip) instead of through its desktop compositor. G-Sync and FreeSync only work on that path.
3. The Frame rate limit, when above 60, is held 3 fps under the screen's refresh rate (NVIDIA's advice), a lower limit of
   yours is kept: 141 fps on a 144 Hz screen, 237 on 240. With frame generation, the made frames are spaced the same way.
4. NVIDIA Reflex is left as you set it: turn it **On** for the lowest delay (it is not used with frame generation).

**Auto** is On in fullscreen when the display lists a variable refresh range in its EDID (a hint, not proof), Off otherwise.
The game's log (`logs/`) says what was chosen and why on a line starting `vk: VRR:`; the F10 overlay's Presentation line says
`fifo (VRR)` when it is on.

**To use G-Sync (NVIDIA)**: in the NVIDIA Control Panel, *Set up G-Sync*, tick *Enable G-Sync, G-SYNC Compatible*, and choose
*Enable for full screen mode* (or *windowed and full screen mode*). Then in the game: Fullscreen On, VRR On (the G-Sync
indicator, *Display > G-SYNC Compatible Indicator* in the control panel, or your monitor's own refresh rate display, shows
the screen following the game). **FreeSync (AMD)**: turn FreeSync on in the AMD software's *Display* page and in the monitor's
menu, then the same rows. Windowed play also works with NVIDIA's *windowed and full screen* choice, with no popup window.

VRR is new and has been tested only for what a PC without a variable refresh screen can show (the present mode, the window,
the frame rate limit). If you have a G-Sync or FreeSync screen, please tell us whether it works for you, and what screen, GPU
and driver you have, on the [issue tracker](https://github.com/Sacrappy/HeyListen/issues). On Linux the rows set the present
mode in the same way, but variable refresh is the compositor's job (enable it in your desktop's display settings; the game
has no window style to change there).

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
Auto follows the controller you plug in, and is the **Keyboard**'s when none is connected (through SDL or XInput); or pick Xbox,
PlayStation 4, PlayStation 5, Steam Deck, Switch or Keyboard. Keyboard shows the key or mouse button your controls put each action
on (the first of its two keys), WASD for the left stick's ways (or the movement keys you bound) and the mouse for the camera; the
ocarina's notes and the pause menu's controller picture keep the Switch's buttons. A new choice shows the
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
  RAM, operating system, the upscaler (with its DLSS model and the render resolution), frame generation and its multiplier,
  the preset and frame rate cap, the game's version and the map names. It has no name, no
  ID and no path. Performance statistics are never published per player: what the team keeps is totals for all players,
  nothing per player. `perf_sent` in the game folder keeps
  a copy of everything that was sent, to read.
  The same row also lets the game send the **graphics pipeline states** it met that HeyListen's shipped list does not have
  yet, at the next start, so the list can grow from what players actually run: for each, hashes of the game's shaders,
  texture and vertex formats and a few render-state numbers, **nothing of the game's code or data**, plus the game version,
  the GPU's vendor and driver and two build stamps. No name, no ID, no path. At most 1,800 a session, each one once;
  `pipelines_sent` keeps a copy of what was sent. Set `HL_TELEMETRY_PIPELINES=0` to turn only this part off.
  The same row also notes, **per map**, which shader programs and pipelines the game's draws used, as 64-bit hashes with a
  use count and their order of first use — **hashes of shader programs and pipeline states only, no game code** — and sends
  them at the next start, so a release can have the next zone's pipelines ready before you enter it; `zones_sent` keeps a
  copy. Set `HL_TELEMETRY_ZONES=0` to turn only this part off.
  The same row also sends, per map, a **detailed profile** of where the time goes, every 5 minutes and at exit: how many of
  the CPU's samples (one for each 1/250 s of CPU time a thread uses) fell in the game's translated code, our native
  engine, the renderer, the graphics driver and the operating system, the addresses of the functions with the most samples
  (an address in the game's program or an offset in HeyListen's own files, **numbers only, no code, no names, no data**) and
  how long the GPU spent in each render pass, with the GPU's vendor and driver, the PC's GPU, CPU and operating system names
  and the build's id. `profile_sent` keeps a copy. Set `HL_TELEMETRY_PROFILE=0` to turn only this part off.
- **Send crash reports automatically**: off by default. After a crash a window says what happened and asks. **Report
  manually** copies a report to your clipboard and opens a new issue on GitHub for you to paste it into; **Report
  automatically** shows you the exact report and sends it only when you press Send. With this row on, the report is sent at
  once and the window shows only if that fails. The report has the crash's error line with the code of each step named
  (`f_80580b38+0x1c4`, a function name, or a system file and offset), the game version and which lifted code it ran,
  your GPU, CPU, RAM and operating system, the map, the last 150 lines of the game's log and its settings. Your home
  folder, user name, every folder path and the names of your audio devices are taken out first. It never has your saves,
  the game's files or keys. See [crash reports](crash-reports.md).
- **Crash reports are published, not kept private**: a report becomes a comment on the project's GitHub issue #55 and a
  message in the project's Discord server, with the detailed report as a .txt file. The server keeps a crash report until
  it is removed on request. To get one deleted, open an issue or ask on Discord with the report's time and we remove it.

Turn either row off and what was waiting to be sent (`perf_pending`, `perf_sent`, `pipelines_pending`, `pipelines_sent`,
`zones_pending`, `zones_sent`, `profile_pending`, `profile_sent`) is deleted. A build with no address
to send to shows both rows as *Not available in this build* and sends nothing. Details and the exact fields:
[performance statistics](performance-statistics.md).

## Problems

The builder's problem page has *Copy for a bug report*. Report issues at
<https://github.com/Sacrappy/HeyListen/issues> with your GPU, driver and the log. Do not attach game files, keys or saves
with personal data.

A maintainer may ask you to try a switch, a line such as `RX_VK_ASTC_RGBA8=1` in `hookshot_env.txt` beside `image.bin`
(in the game folder; on Linux `~/Games/HeyListen`, not `bin/`). [docs/SWITCHES.md](SWITCHES.md) lists every switch, what
it does and which symptom it fixes, and says how to set one (also from Steam's launch options).

## Credits and license

HeyListen is GPL-3.0-or-later. It is a fork of Hookshot by NotRexed and its contributors (GPL-3.0), whose recompiler,
builder and renderer do the work; see `THIRD_PARTY_NOTICES.md`. The Legend of Zelda and Ocarina of Time are Nintendo's;
HeyListen is not affiliated with Nintendo and ships none of its assets.
