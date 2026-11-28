# HeyListen {TAG}

HeyListen turns **your own** Switch 2 copy of *The Legend of Zelda: Ocarina of Time* into a native Windows and Linux
game. The release holds only the **HeyListen Builder**: no game code, data, keys or shaders. You dump your own game as a
`.nup`, the builder reads it, translates and compiles the game on your PC, prepares its shaders, and starts it. See the
[user guide](https://github.com/Sacrappy/HeyListen/blob/main/docs/USER_GUIDE.md).

🎮 To play, download the HeyListen Builder
=====
### [🪟  Download the Builder for Windows](https://github.com/Sacrappy/HeyListen/releases/download/{TAG}/HeyListenBuilder.exe)
### [🐧  Download the Builder for Linux (AppImage)](https://github.com/Sacrappy/HeyListen/releases/download/{TAG}/HeyListen-{TAG}-Linux-x64.AppImage)
Run it, pick your own `.nup` (Switch 2, version 131072): it builds the game on your PC and starts it. Nothing of the game is in
these files.

Also on [RecompLauncher](https://recomplauncher.com), if you use it: HeyListen is in its library, with the same build done for you
(`HeyListen-{TAG}-Windows-x64.zip` below is what the launcher installs; you don't need to download it).

**Shaders are prepared again once, and the game's code is translated again once** with this release: the first start
after updating takes a few minutes longer, and the first session builds the driver's pipelines as you play, so it
stutters more than usual. The second start is smooth again.

{TAG}, new since {SINCE}
-----

## New options

* **First person** (ToxicToucan; Gameplay tab, off by default): the camera sits at Link's eyes, with his head hidden so his
  body and what he holds come into view. It turns with the mouse one to one, keeps Navi out of the picture and eases the
  eye out while Link is in the air.
* **Volume** (Chad-; Gameplay tab): the game's whole sound from 0% to 100%, heard at once.
* **Sprint with L3** (Controls tab): Off, Hold or Toggle.
* **Texture resolution: High / Low** (Graphics tab): Low leaves out the top level of the biggest textures, for cards with
  little video memory. It applies after a restart. A card with 3 to 4 GB should use Low.
* **Quality preset Lowest (integrated GPU)**: one choice that sets what an integrated GPU needs (a lower render
  resolution, Handheld profile on, Volumetric fog off). It replaces the earlier low-end settings row.
* **Output device** row: choose the sound's device in the menu; it applies at once.
* **Handheld profile, Volumetric fog and Button prompts apply at once**, with no restart. Every quality preset also sets
  the Volumetric fog.
* **Render resolution 33%** with the game's own upscaler, and **Off** for the game's own resolution (shown as Auto).
* **Base frame rate limit before frame generation**, as a row of its own.
* **F10 overlay**: the full overlay shows the 1% and 0.1% lows, read as the usual tools read them (the frame rate of the 99th and
  99.9th percentile frame over the last 30 seconds), so one long frame no longer drags the line down for seconds.
* **HDR10 static metadata** (McNartin): on an HDR screen the game now tells the screen the picture's peak and black level.
* **Button prompts follow a change while you play**: with *Auto*, the prompts follow the device that gave input last.
  When the `prompts` folder is missing, the game's log, the row's help and the Builder now say so. The ocarina's hint
  bar keeps its icons when the prompts change while it is on screen (it shows the new ones the next time it comes up),
  and the keyboard prompts show their key letters around the action button.
* **A touchpad click opens the menu.**
* **AMD FSR 4 frame generation** (Windows, Radeon RX 9000 only) is still **not tested on real hardware**. If you have the
  card and try it, a report is welcome.
* **Crash reports** (still opt-in, as before): the report you may send now carries a diagnosis in words, the area and play
  time, memory figures, the last slow frames, the last Vulkan error and the switches set in `hookshot_env.txt`. Paths, the
  user name and text-like values are masked. Saves, game files and memory are never sent.

## Fixes

* **One-frame brightness flashes** (the lit ground, the lights and the ground fog getting brighter or darker for a single
  frame, seen in the late-November builds; #93): found and fixed. The **order thread**, the renderer's work on a thread of
  its own that gave those builds their frame-rate gain, is **on by default again** where the game keeps a processor core
  for it (`RX_VK_ORDER_THREAD=0` in `hookshot_env.txt` turns it off).
* **Leaves and grass that vanished and came back** while you moved: fixed.
* **Frame rate falling during a long session, and memory filling up, on PCs with 32 GB of RAM or more** (#121): there
  the game was never told how much memory it used, so it never gave any back; its memory grew by several GB in the first
  minutes, until the system ran short and the frame rate collapsed, or the game closed. It is told on every PC now
  (`HOS_MEMINFO_OLD=1` in `hookshot_env.txt` gives the old behaviour back).
* **Freezes of several seconds from file calls**: one cause fixed (no file or log call is made while a thread holds its
  core).
* **Lock-on ring** (#89): all four triangles show around the target.
* **Crashes from GPU writes into memory the game had freed**: a write whose buffer the game freed is left unbound, so it
  cannot land on the game's new objects. Crashes at an area's load and at the title screen are fixed, and the log says more
  when memory runs out.
* **Crash at address 0x150** (#114; NotRexed, after Rekarina's finding): an invalid resource handle now takes the game's own
  empty path.
* **Integrated AMD GPUs**: a driver crash at an area's load when memory is full is fixed (the game skips some drawing for
  a moment instead of closing). The big terrain textures are read from the disk cache instead of decoded again at the
  start of play, so a freeze of 8 to 20 seconds, or a lost GPU, at the start of play from the **second** start on is gone.
  The **first** start after installing can still freeze about 20 seconds.
* **No controller plugged in**: the hitch every second is gone (HumanBean, after Longshot #2124).
* **Foliage cut-outs** are back on Intel Arc B-series and Lunar Lake (tiktiki, twisted89), and Death Mountain Trail's khaki
  veil on Intel is gone (Guep).
* **Pink and green flash** on some mesh shaders: fixed (nachochee3e).
* **Microphone ocarina above 60 fps** (timetohitthedopeho).
* **Gyro and sticks**: a pad's gyro stops with its buttons when the game is behind another window (Dylan Martin); a frozen
  gyroscope is switched off and on again, as after Alt+Tab under Steam (Tony Malins); a stick steers once it has been
  seen at rest, so the camera no longer spins on its own (NotRexed). Steam Input's doubled buttons can be cured with
  `HL_XINPUT_SDL_FIRST=1` (oscarlosu-lab).
* **Insert menu** (jarynclouatre): the pad no longer moves Link while the menu is open in the background, and an input
  still held after the menu no longer blocks the pad's other buttons.
* **Sound back after the device goes away** (Robin, #2190): the output reopens when its device disappears and returns, and
  follows Windows' default when it changes.
* **NPC speech bubbles** stay over their speaker on wide screens (CraftyCorvid).
* **Render targets** (mooki, #30): a finished render target is let go when the game reuses its memory.
* **Small cards**: past the driver's video memory allowance the textures' floor gives way, so a small card's memory comes
  back under it (HumanBean). The storms of texture re-uploads after the pause menu are gone (Veix).
* **Light-meter and capture waits**: a capture's wait for the GPU follows the recording's own timeline (Erfboom, #2458).
* **Shaders and drawing**: dead-code removal keeps the warp-wide operations (black leaves, #97); a mesh draw's indirect
  argument ranges no longer overlap (juliotec, #2719); a texture's storage size is computed in 64 bits (hckrman101);
  overlapping inline buffer updates are ordered (klimovich008, #329); texture blits are checked before they run, a sparse
  texture's blocks are bound when its upload runs out of staging, and clears of ASTC textures are skipped (juliotec).
* **Other crashes and device losses**: the AMD cloth pipeline's optimizer hang (Mesquita); big GPU texture uploads in
  bounded steps against a lost device on integrated AMD GPUs (Jer9); texture staging published before the GPU reads it
  (Issai); capture reads invalidated before the CPU reads them (Arkex-ai); Media Foundation buffers balanced on rejected
  frames (Ghost, #2859); a directory's listing read under one lock (NotRexed, #2539); a driver fault caught by the
  Windows guards is logged (jarynclouatre) and the device's fault information is read on a lost device (Saul Ojeda).
* **Linux saves and window** (jarynclouatre, Zelda Ootr, Itallo Ramon): moving the saves leaves a linked folder alone, a
  move that fails part way brings back what it moved, and the window opens on COSMIC's Wayland (#992).
* **RecompLauncher** (Xarishark): the manifest's build uses the player's own copy as the input game for every release, and
  no update question is shown under the launcher.
* **Frame generation** (HumanBean, SadTransGirl, futondestroyer, Jordan): the picture from before the HUD can feed FSR 3.1
  and the built-in frame generation, with the HUD laid over each made frame (off by default); FSR 3.1's images are let go
  when another maker makes the frames.
* **Below 60 fps at the default 60 limit**, the game's whole-frame counters keep their fractions, so the time of day, the
  water's ripples and the Scarecrow's Song no longer fall behind (SadTransGirl).
* **Castle Town's market**: the neighbouring map is no longer reloaded each time Link turns (SadTransGirl).

## Performance

* **Even frame pacing at a frame rate limit** (after the pacing of MacNativeGames and Rekarina): each frame is shown on an
  even line and the game's time follows the frames as they are shown, so motion no longer jumps by uneven steps at 60,
  90 or 120 fps. On by default with a frame rate limit and V-Sync off; a frame waits a few milliseconds for its place
  (`HL_PRESENT_PACE=0` in `hookshot_env.txt` gives the old pacing back).
* **More of the game's own code runs on the processor's native floating point** (the build takes longer for it: several
  minutes more on a slow processor).
* **Texture disk cache**: on integrated GPUs and cards under 10 GB, transcoded textures are kept on disk and read ahead,
  and the entered map's textures are read first.
* **Memory**: cold pages are trimmed only while RAM is short, and freed address regions stay resident for a while.
* **Smaller gains** (HumanBean, SadTransGirl, Mooseca, pixl27): events and yields without locks when nobody waits, no wake
  call when nobody sleeps, retired capture buffers destroyed on a thread of their own, the vegetation's mesh draws keep
  their passes, cached programs read by the background workers, and a buffer binding checks the left-out ranges without
  the shared lock.
* **Shader preparation**: a comment edit in the translator, or a line about the GPU switches in `hookshot_env.txt`, no
  longer makes every player prepare the shaders again (Jeff Aigner, pixl27).

## Linux

* The game links again on Linux (Mooseca, issue 133), and the sound driver's name and error are logged when the device
  cannot be opened (issue 99).
* The BC-250 runs the compute mesh path by default.

## Known issues

* **On an integrated GPU, the first start after installing can freeze about 20 seconds** when play begins. Later starts do
  not.
* **A freeze of 1 to 2 seconds at the start of play**, and shorter ones when an area loads.
* **A rare one-frame white floor in Gerudo Fortress**, and **a rare single black frame of the 3D picture** with the HUD
  intact.
* **GTX 1060 3 GB class cards can lose the device entering Kokiri Forest**: try Texture resolution Low and
  `RX_VK_SHADOW_SHIFT=2`.
* **The first visit to a new area can stall for about 30 seconds on slower PCs** while its shaders are translated; the next
  visits are smooth.
* **The one-time preparation** after updating takes a few minutes, and the first session stutters while the driver's
  pipelines are built.
* **AMD on Windows**: if you lose the GPU (device lost) after area changes, set `RX_VK_KEEP_RETIRED=1`; terrain missing
  after area changes is the opposite case, `RX_VK_AMD_GIVE_BACK=1` gives retired imports back as NVIDIA does. Add either
  to `hookshot_env.txt`.
* **Some plants' wind animation** (the blue-flowered plants in Hyrule Field) looks wrong.
* **DLAA costs fps**: it draws the 3D at your screen's full resolution, so expect 15-20 fps fewer than DLSS's scaled
  modes.
* **Integrated GPUs with no mesh shaders** (for example the Radeon in desktop Ryzen 7000 CPUs) are slow even with the
  quality preset Lowest; they are not a target yet.
* **V-Sync at 144 Hz**: with V-Sync On, a frame that misses a refresh waits for the next one (FIFO with two images shows
  72 fps on a 144 Hz screen while the game cannot reach 144). V-Sync Off is the game's own way.
* **Native HDR needs an HDR screen with Windows' HDR switched on**; otherwise the game stays in SDR.
* **Keyboard prompts need the `prompts` folder** the builder makes from 0.4.3 on; a game built by 0.4.2 or earlier keeps the
  game's own icons until you build again.

## Credits

Every port and its author is in the [README](https://github.com/Sacrappy/HeyListen#credits). Thanks in this release to
ToxicToucan (First person), McNartin (HDR10 metadata), Xarishark (RecompLauncher), Chad- (Volume), jarynclouatre,
timetohitthedopeho, oscarlosu-lab, Robin, mooki, Erfboom, juliotec, Jeff Aigner, pixl27,
CraftyCorvid, Mooseca, HumanBean, SadTransGirl, NotRexed, hckrman101, klimovich008, Mesquita, Jer9, GrimVisage,
futondestroyer, Jordan, Guep, Veix, tiktiki, twisted89, nachochee3e, d4nkn3ss, Dylan Martin, Tony Malins, Saul Ojeda,
Itallo Ramon, Zelda Ootr, Issai, Arkex-ai, Ghost, Mihawk, i-love-computers-dude, Rekarina, azy2, Dewey Harry D. bartolo,
and the testers who sent reports and logs. Upstream: the Hookshot (NotRexed) and Longshot (MagicBeanDev) forks.
