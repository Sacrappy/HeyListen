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

**Shaders are prepared again once** with this release: the first start takes a few minutes longer, and the first session
builds the driver's pipelines as you play, so it stutters more than usual. The second start is smooth again.

{TAG}, new since {SINCE}
-----

## New options

* **V-Sync On/Off and a V-Sync mode row**: On waits for the screen's refresh (no tearing); the mode row picks FIFO with
  two images (least delay), FIFO with three (the default), or Mailbox. Off is the game's own way. A **VRR** row (G-Sync /
  FreeSync) shows the game without tearing and without V-Sync's wait.
* **NVIDIA DLSS Frame Generation** (RTX 40 and newer) as a *Frame generation by* choice, next to the built-in and AMD
  FSR 3.1 makers: 2x up to the most your GPU's runtime allows (6x on an RTX 50). It needs the DLSS or FSR 4 upscaler for
  depth and motion; until then the built-in one makes the frames. Off by default.
* **DLSS model row names DLSS 4 and DLSS 4.5**, plus **DLSS 3** (CNN, preset E); the first-run default follows your GPU's
  generation and VRAM (DLSS 3 under 7 GB).
* **Upscaler and Render resolution** now sit above the frame rows; **DLAA** is a Render resolution choice; the **Frame
  rate limit** is the rate you see; the multiplier stays within its mode. The Upscaler also has an **Off** choice, for GPUs
  where the game's own network shows glitches.
* **PsychoV35 HDR tone mapper** (Duck912, #101, after RenoDX's tone mapper for this game): with HDR set to *The game's
  own*, the *HDR tone mapper* row picks the game's curve or PsychoV35, whose highlights roll off into your screen's peak
  and keep their colour, with its own grading rows (saturation, contrast, exposure...). It works with every upscaler and
  gives frame generation 10-bit frames (DLSS Frame Generation keeps 8-bit frames for now). A saved setting from the
  pre-release's own tone mapper loads as PsychoV35.
* **DLAA before the bloom** (Graphics, after Longshot): DLAA smooths the 3D before the game's bloom and tone map. It needs
  the Render resolution at Native (DLAA); the row is greyed out otherwise.
* **Keyboard button prompts**: *Button prompts = Keyboard* shows the keys and mouse buttons you bound, in the game's
  texts, hints and menus (movement keys as they are bound). Switching to it rebuilds the program cache once.
* **HUD layout row**: the HUD at the screen's edges (every screen shape) or in the middle 16:9.
* **A tidier menu**: rows that only matter with another setting on (V-Sync mode, the frame generation rows, the DLSS
  model, the HDR and PsychoV35 rows) are hidden while that setting is off; the HDR row says when your screen has no HDR
  (Windows' *Use HDR* off); *Layout preset* and *Button prompts* are on the Controls tab. Button prompts apply after a
  restart.
* **Open menu** is a bindable control, and a touchpad click opens the in-game menu.
* **Check for updates**: the update offer also works when the game is started without the launcher, with *Never ask
  again* and a Settings row. The Builder's **Play** button starts the game through the builder (`--play`).
* `--fullscreen`: a one-run start in the display mode a shortcut asks for.
* **Performance statistics** (still opt-in and off by default) also say whether frame generation was on, its multiplier,
  the DLSS model, the render scale, the frame's parts and what caused each hitch. The pipeline states and, per map, the
  shader programs used can be shared too, with the same row. With the row on, a **detailed profile** is sent as well:
  where the CPU spends its time (by module and thread kind, the costliest game functions by address) and the GPU's time
  per render pass, with what was running during stutters; it costs about 3% of fps while the row is on
  (`HL_TELEMETRY_PROFILE=0` keeps the rest and turns the profile off). Nothing is sent unless you turn the row on. Crash reports now
  name every frame of the backtrace on your PC, the crash window says that a sent report is published (GitHub issue #55
  and Discord) and how to have one removed, and every folder path is redacted before it is shown or sent.

## Fixes

* **Cutting grass (#50)**: the leaves now fly and vanish at every cut, also on a busy CPU (before, debris piled up at the
  first cut's place, or none showed). Small game memory pools the GPU writes are now always copied back to the game, and
  a pool the CPU starts writing is copied back at once. This also ends the vegetation that vanished for a few frames when
  you cut a plant right after loading. No measurable fps cost. (`RX_VK_WB_SMALL_EAGER_KB=0` / `RX_VK_WB_POOL_EAGER=0`
  restore the old behaviour.)
* **Above 60 fps** (CraftyCorvid, cuecuemoar, Jeff Aigner): Link pulls himself up a ledge at normal speed, frozen Zoras
  stand still, water ripples step every frame, and the ocarina's notes show one at a time with the music.
* **Exposure**: the light meter's sums are cleared on the GPU and a read that finds zeros gets the last result (pixl27);
  its reads also wait for their write-back while the game runs slow, so no white flash under load.
* **AMD integrated GPUs** (ROG Ally and similar): each present waits for its frame's GPU work by default, so HUD icons no
  longer flash and the minimap keeps its texture (bubblesnout, #81, #34).
* **Crash and robustness** (pixl27, jcabarca, Chad-, NotRexed): a driver fault while a shader is read becomes one failed
  shader and a message instead of a silent close; an AMD pipeline the compiler rejects is built again without
  optimization (the Market's and Kakariko's dogs); each submission ends with a barrier so the game reads the GPU's
  results; vertex divisors and host memory imports follow what the device allows; the first-start preparation gives up
  cleanly when it stalls; texture formats keep their own alpha; sparse BC images respect the device's limits.
* **Long game folder paths** (NotRexed, monsterlag, cuecuemoar): a game folder whose deepest file passes 260 characters
  plays, paths keep UTF-8, and the cutscenes' voices play in a deep folder.
* **Linux** memory: the renderer gives memory back under memory pressure (dukenheimer) and the driver's compiles run as
  many at once as the memory holds (Tomiss); Linux tries sound device 0 first when `HOS_AUDIO_DEVICE` is unset (Mooseca,
  #99); Windows keeps the default output first.
* **Voices**: a Voice Language saved in `option.sav` (for example Japanese voices under English text) is applied after a
  restart, not only after you change it again (HumanBean, #94). A 5.1 mix's centre channel also goes to the front pair, so
  voices survive a sound device that drops the centre (#82).
* **Above 60 fps**: the Scarecrow's Song no longer hangs on its first note (mooki, #87); Epona jumps fences and the Shadow
  Temple's barge carries Link (CraftyCorvid); the battle music stops after a fight (CraftyCorvid); Gohma reaches her
  ceiling position (PirateKingIsh).
* **No more white flashes on slower cards**: the captures' wait for late pages follows the GPU (pixl27, #81, #34).
* **Water**: two volume-box draws left out (a GPU hang in water on RTX 50, Link white in water); a draw is skipped past
  its pipeline only where the next frame redraws that target; compressed textures upload where render targets used their
  storage (pixl27).
* **Castle Town and Hyrule Castle direct sunlight**: the textures and samplers the game registers before the renderer
  starts are read from the host's copies (mooki, #80).
* **Shaders** (the stamp group): sized queries, texture handles from constant banks, conversions, subgroup barriers,
  8/16-bit stores, TLD.MS, ISBERD.O, R2UR and more (jarynclouatre, klimovich008, hckrman101, Joey Magnus, Miguel Luna,
  Kolkka, yassine milal, mooki).
* **Foliage noise on wave64 GPUs** (#67) and AMD/RDNA fixes: 16-bit floats in shaders only where the device has them
  (Mooseca); the mesh index map in whole 32-primitive blocks (Kolkka).
* **Textures**: a re-upload or first upload takes only its own submission's copies (Mooseca, #66).
* **Crashes**: the native main loop is off again by default (`HL_MAINLOOP=1` turns it on); it was the first suspect in
  v0.4.2's `ModuleSystemWorker` crashes. Cold-page trimming backs off under memory pressure. The game never runs on a
  software Vulkan device beside a real GPU, and stops with a message when it is the only one.
* **New areas** wait for their translations again, as in v0.4.2.
* **Menu**: the menu's clip rects stay upright and its help box stays inside a windowed window (NotRexed).
* **Video**: the movie is decoded ahead on its own thread, and on the GPU through DXVA when your PC has an AV1 decoder.
* **Frame generation** says its runtime's version at the start; its multiplier stops at what the runtime offers (#1486,
  pmwork-personal); changing it while playing no longer crashes.
* **Free camera (Cheats)** is found again on block lifts; the gameplay view's hook is set at lift time (Codex).

## Performance

* The game's yield gives up its virtual core only, not the processor.
* Lifted stores mark only the pages the GPU can read (the bitmap form is off by default).
* The shaders' preparation uses all of the PC's threads and tells the builders how long it will take; a driver compile
  that outlasts the wait is given up and remembered.
* NVIDIA's NGX starts on its own thread at the first present with DLSS chosen.
* Intel and integrated GPUs without VRAM mirrors copy uniform buffers as submitted by default (Dukenheimer, #1795).
* The dry pass's appends no longer wait behind the early segments' reads (Mooseca).
* Stutters (pixl27): emptied video memory blocks go back to the driver on a thread of their own instead of the render
  thread; the sync-object table's rehash no longer reads every object under its lock; retired memory pools nothing needs
  are set aside instead of walked at every submission (a slowdown that grew with the session).

## Linux

* The AppImage builds again (CreateHardLinkA no longer hides link(2)), and the exe exports every function the Vulkan back
  end looks up by name (Mooseca).
* RDNA 3.5 APUs take the compute path on RADV (#81); RADV's mesh shaders stay off by default (#51, Dewey Harry D. bartolo,
  hckrman101); `RX_VK_NO_DMABUF=1` gives the guest window anonymous memory (Mooseca).
* The crash record's names link on Linux; the logs folder's `.log` links work for the launcher's crash reports.

## Known issues

* **The first visit to a new area can stall for about 30 seconds on slower PCs** while its shaders are translated (as in
  v0.4.2); the next visits are smooth.
* **The one-time shader preparation** after updating takes a few minutes, and the first session stutters while the
  driver's pipelines are built.
* **AMD on Windows**: if you lose the GPU (device lost) after area changes, set `RX_VK_KEEP_RETIRED=1`; terrain missing
  after area changes is the opposite case, `RX_VK_AMD_GIVE_BACK=1` gives retired imports back as NVIDIA does (off by
  default after an RX 6750 XT lost its device without KEEP_RETIRED). Add either to `hookshot_env.txt`.
* **Some plants' wind animation** (the blue-flowered plants in Hyrule Field) looks wrong; it was there before 0.4.3 and is
  being looked at.
* **DLAA costs fps**: it draws the 3D at your screen's full resolution, so expect 15-20 fps fewer than DLSS's scaled
  modes.
* **Integrated GPUs with no mesh shaders** (for example the Radeon in desktop Ryzen 7000 CPUs) take a long first start
  and stay very slow; they are not a target yet.
* **V-Sync at 144 Hz**: with V-Sync On, a frame that misses a refresh waits for the next one (FIFO with two images shows
  72 fps on a 144 Hz screen while the game cannot reach 144). V-Sync Off is the game's own way.
* **Native HDR needs an HDR screen with Windows' HDR switched on**; otherwise the game stays in SDR.
* **Keyboard prompts need the `prompts` folder** the builder makes from 0.4.3 on; a game built by 0.4.2 or earlier keeps the
  game's own icons until you build again.

## Credits

Every port and its author is in the [README](https://github.com/Sacrappy/HeyListen#credits). Thanks in this release to
Mooseca, Duck912 (PsychoV35), jarynclouatre, J and llun (Keyboard prompts and the HUD layout), Jeff Aigner, pixl27,
NotRexed, Dukenheimer, Tomiss, HumanBean, mooki, CraftyCorvid, cuecuemoar, monsterlag, bubblesnout, jcabarca, Chad-,
PirateKingIsh, Joey Magnus, hckrman101, klimovich008, Kolkka, Miguel Luna, yassine milal, Zacharias Xenakis,
pmwork-personal, futondestroyer, LRF17, unixman69, MustardTiger, Codex, Dewey Harry D. bartolo, Robin and Mesquita, the
RenoDX authors (ShortFuse, Carlos Lopez Jr.), and the testers who sent reports and logs. Upstream: the Hookshot (NotRexed) and Longshot (MagicBeanDev) forks.
