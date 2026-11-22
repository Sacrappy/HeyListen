# RecompLauncher (for developers)

[RecompLauncher](https://recomplauncher.com) installs a recomp from a GitHub release and runs the `recomplauncher.json` it finds
in it. HeyListen reaches level 3 of its [SDK](https://recomplauncher.com/sdk): it plays (level 1), shows its settings (level 2)
and builds the game from the player's copy on the launcher's setup screen, with no window (level 3). The guide for players is
in [USER_GUIDE.md](USER_GUIDE.md#install-with-recomplauncher).

## The pieces

| What | Where |
|---|---|
| The file | [`recomplauncher.json`](../recomplauncher.json), at the repository root (`$schema`, format 1) |
| The game it checks | `game.check.nup_build_id` is `e0f3ccfa`, the one build `engine/nx2/make_game.py` supports (`BUILD_ID`; program `0400486024DEC000`, update v131072) |
| The headless build | `make_game.py --prepare --game <.nup or romfs+exefs folder> --out <folder>` |
| On Windows | `HeyListenBuilder.exe --cli <make_game.py's arguments>` (`engine/builder/HookshotBuilder/HeadlessCli.cs`): no window, the package's own Python, the same environment as the builder window, exit code 0 on success; the build is stopped with it |
| On Linux | `HeyListen.AppImage --cli <the same arguments>` (`AppRun`) |
| The Windows asset | `HeyListen-<tag>-Windows-x64.zip`: the whole builder (its Python, the compiler, the sources), `recomplauncher.json` at the top (`tools/release/make_launcher_zip.py`, called by `make_release.py`) |
| The Linux asset | `HeyListen-<tag>-Linux-x64.AppImage`, with `recomplauncher.json` inside it, at `/recomplauncher.json` (`engine/scripts/linux/appimage.sh`) |
| The tests | `engine/tests/test_recomplauncher_json.py`, `engine/tests/test_progress_lines.py`, `engine/tests/test_update_offer.py`, `engine/tests/test_update_order.py`. The first also validates the file against a copy of the launcher's schema when `RECOMPLAUNCHER_SCHEMA` names one: the launcher's repository is GPL v3, so its schema is not copied into ours |

## What the file says

- `start`: the builder's own `--play <folder>`, like Longshot's (`HeyListenBuilder-Setup.exe` on Windows, `HeyListen.AppImage` on
  Linux, args `--play {install}/HeyListen`), so a newer release is looked for before each launch. No `env` and no `requires`
  (the list added in 0.4.2 was not the cause of the developer page's 0 of 6 features, and was taken out).
  - **Windows**: the setup program (`engine/builder/setup/setup.c`, `play_folder`; the zip already holds it, in `HeyListenBuilder-Setup.exe`
    at its top, because it is part of the builder). It is the program to start, rather than the .NET `HeyListenBuilder.exe`, because it
    is native (no window and no .NET start for a Play), it is the program the builder's own Repair and the game folder's launcher run, and
    `--play` needs nothing the .NET app has. The `--play` command is ported from Longshot's setup (Zacharias Xenakis, "--cli and --play for
    RecompLauncher on Windows"); Longshot's only starts the game, so the look for updates is HeyListen's own.
    - The game folder is checked first: not there, no `bin\HeyListen.exe` (or `bin\hookshot.exe` of a folder built before the rename), or no
      `image.bin` (the build did not finish): a clear message (a window, and `error: ...` on stderr), exit code 1, nothing started.
    - Then the latest release is asked for (a thread, 2 s per step and 4.5 s in all). **No network**, a slow one or GitHub's API limit:
      nothing is said and the game starts. **A newer release** than the folder's `hookshot_version.txt` (the same order as the game's
      offer, `host/hos_update_ver.h`): a window asks whether to play the folder's version now, open the download page (the game does
      not start), or keep the version and stop asking (this release only, or for good). It never installs: the launcher unpacked this
      builder, and the launcher is what installs a newer release (it asks again for the setup when `prepare.version` changes).
      Skipped releases and "never ask again" are the files the game's own offer uses (`user\hookshot_update_skip.txt`,
      `update_check=0` in `hookshot_options.ini`), so the two agree, and the in-game Settings row **Check for updates** turns it back on.
    - The game is started with `HOOKSHOT_NO_UPDATE_CHECK=1`: the question was asked here (or turned off), so the game does not ask a
      second time. The launcher's own `HOOKSHOT_NO_UPDATE_CHECK=1` also turns the look off. `HOOKSHOT_SETUP_QUIET=1` is for a test
      (no window; the question counts as "play").
    - The setup waits for the game and returns its exit code, so the launcher knows when the session ends.
  - **Linux**: `HeyListen.AppImage --play <folder>` (`AppRun`, `hookshot-launch`) starts `bin/hookshot` there. It no longer sets
    `HOOKSHOT_NO_UPDATE_CHECK`: the AppImage has no window of its own to ask, so the game's own offer (`host/hos_update.c`: curl,
    3 s at most, then zenity) does it, with the same four answers.
  - The game's own offer is on by default for a game started without the launcher, off when the builder or the launcher started
    it. See `docs/USER_GUIDE.md` (Updates).
- `prepare`: `--cli --prepare --game {game} --out {install}/HeyListen`. The game folder is inside the install folder, so the
  program paths of `start` are inside the release as the launcher wants. `done` lists `HeyListen/image.bin`, so a folder
  made by the builder window in the same place counts as prepared. `timeout` is 4 hours.
  - `--prepare` is `--progress-lines` (lines `progress <0..1> <text>`, one per step of the bar; the builder window's `@@` lines
    are not printed), the shaders left to the game's first start (the step must not open a window), the package's `emsdk`,
    `cpm` and `wgpu-native` found beside the tools, the builder's own work folder (`%LOCALAPPDATA%\HeyListenBuilder`, or the
    folder chosen in the builder) and the PC's language for the game (`language.txt` in the game folder changes it).
  - A failure ends with a line `error: <reason>` on the output and a nonzero exit code.
  - `--send-perf-stats` / `--send-crash-reports` (make_game.py, so the Windows `--cli` and the AppImage's take them too) write
    `send_performance_stats=1` / `send_crash_reports=1` into the game folder's `hookshot_options.ini`, changing no other key
    (`set_option_keys`); left out, they change nothing. The two ticks that pass them start off and have no license; the
    **Privacy** settings write the same keys through the `config` entry. The telemetry address (`nx2/addresses.ini`) is set at
    the release, and without it the game's menu rows say "not available in this build".
  - The `inputs` entry `dlss` (`shared: nvidia.dlss`, optional) has the launcher put NVIDIA's own `nvngx_dlss.dll` at
    `HeyListen/ngx/nvngx_dlss.dll`, the first folder `gpu/vk/vk_dlss.cpp` looks in. Nothing of NVIDIA's or AMD's is in our
    repository or release.
  - The two **options** are ticks on the setup screen. Each adds its arguments when on and starts off, because it has a
    license to accept: **NVIDIA DLSS** (`--fetch-ngx-sdk --accept-nvidia-license`, NVIDIA RTX cards) and **AMD FSR 4**
    (`--fsr --fetch-fsr-runtime --accept-amd-license`, Windows, AMD cards). The license links are the ones the builder shows
    (`scripts/ngx_sdk.py`, `scripts/ffx_sdk.py`; a test keeps them equal). Without a tick nothing is downloaded.
- `config`: `HeyListen/hookshot_options.ini`, the file the in-game menu (`gpu/vk/vk_menu.cpp`) keeps. The launcher writes only the
  keys of the choices; the menu writes the whole file again when the player changes a row, so the two agree.
- **Fullscreen** is one of the settings rows (the menu's `fullscreen` key), and the game also takes a `--fullscreen` command-line
  switch (PR 91's first ask), for a shortcut or another launcher: one run, written nowhere. `hos_main.c`'s parse sets
  `HOS_FULLSCREEN=1` (on Windows before the window is made, on Linux in `main`, before the display), `host/display.c` and
  `host/posix/display_sdl.c` apply it when the window is made, and `vk_menu.cpp`'s Fullscreen row leaves the window alone while it
  is set (`fullscreen_tick`). RecompLauncher itself uses the settings row, which saves the player's choice.
- `settings`: one row per menu row, with the menu's own values. A test (`test_recomplauncher_json.py`) checks that every key is a
  row of the menu and every value is one the row takes. The keys are the rows' names with `_` (`frame_rate_limit`,
  `frame_generation`, `skip_title_screen`); `dlss` is the Upscaler (0 the game's own, 1 DLSS, 3 FSR 3.1, 4 FSR 4, 6 Off: no upscaling network; 2 DLAA and 5 FSR 4 Native AA are not choices any more, and a file that saves them is read as 1 or 4 with the native Render resolution, `migrate` in `gpu/vk/upscaler_rows.h`), `resolution` the Render resolution (7: native, DLAA with DLSS and Native AA with FSR 4), `frame_rate_limit` the Frame rate limit (0 to 5 the rates from 60 to 240, 6 Unlimited), `frame_generation_by` Frame generation by (0 built-in, 1 NVIDIA DLSS FG, 2 AMD FSR 3.1 FG), `upscaling` the Window
  scaling filter, `output_res` the Output resolution (see `ini_key` in `vk_menu.cpp`).
  `dlss_model` has no `defaults` on purpose: the schema calls them the game's values for keys the file does not have yet, which a
  launcher may write before the first run, and the game picks that one for the GPU (`menu_first_run_model`).
- `saves`, `folders`, `actions`: saves are `HeyListen/user/save` on Windows and `~/.local/share/heylisten/save` on Linux. The
  launcher attaches the newest `.log` of the `logs` folder to a crash report (PR 91's second ask): on both systems the game
  (`host/hos_run_logs.h`, called from `hos_main.c` once it opens `hookshot_log.txt`, which it does whenever it is started with no
  terminal, as the launcher starts it) keeps `HeyListen/logs/hookshot.log` (this run) and `hookshot.1.log` to `hookshot.4.log`
  (the runs before), hard links to the same files as `hookshot_log.txt` and `hookshot_log.1.txt` to `.4.txt`, which stay for
  players. Started from a terminal the game logs to the terminal, as before. On Linux `host/posix/windows.h` gives the header
  the same file calls (`CreateHardLinkA` in `win32_posix.c`).

## The launcher's game page and its six ticks

recomplauncher.com/game/heylisten showed "Launcher features 2 of 6" (Easy Setup, Settings) "set up by RecompLauncher for now"
because its catalog snapshot still used the launcher's own profile, made before our file reached the releases. The page does not
read the repo on each visit: the launcher reads the file from the AppImage and the release zip first (their DEVELOPERS.md, "Where
the file goes"; a GitHub repo's root is read only for Gitea and Forgejo servers, not GitHub), and the page's ticks come from their
catalog. Their own check (the "Check my repo" button of the submit form) says of ours today: found in the repo, reads fine, Easy
Setup and Settings. What makes the page ours is claiming the recomp — the free developer account at recomplauncher.com/dev PR 91
points at ("where the file comes from doesn't decide it"): then the ticks the file supports show, four of the six:

| Tick | How |
|---|---|
| Easy Setup | `game` (the player's own copy, checked by build ID), `inputs` (NVIDIA's own DLSS runtime), `prepare.options` (the two licenses and the two reports) |
| Settings | `config` and `settings`, the menu's rows in `hookshot_options.ini` |
| Save backups | `saves`: `HeyListen/user/save`, `~/.local/share/heylisten/save` and the options file |
| Setup step | `prepare`: the builder's `--cli --prepare`, `done` the built `image.bin` |

The two the file does not claim:

- **Checks your copy**: the page's tick is `game.dumps`, a CRC32 of a whole known copy from Redump, for a "verified" badge. That is
  a hash of a game file, and we publish none (Switch 2 `.nup` packages are not on Redump either). The launcher checks the
  player's copy without it: `game.check.nup_build_id`, the one build `make_game.py` supports, read from the player's own
  `exefs/main` with no keys, refuses a wrong build on the setup screen.
- **Mods**: the launcher's Mods tab needs a Thunderstore community or a GameBanana game read by an `nrm`, `otr` or `hmm` loader.
  Ours is a folder of file mods (`folders.mods`, the action opens it; code mods are the builder's `--code-mods`, and exefs mods
  have issue #36), so there is nothing to point the tab at.

PR 91's third ask, achievements through Recomp Achievements, is for later; its own two asks are done: the `.log` crash log above,
and the `--fullscreen` switch above.

## When you add or rename a menu row

Add it to `settings` (or leave it out on purpose) with the row's ini key and values. `test_recomplauncher_json.py` fails when a
key is not a row, so a rename in `vk_menu.cpp` cannot leave the file behind. The V-Sync (`vsync`) and VRR (`vrr`) rows are in
the file, both Off by default.

## When players must build again: `prepare.version`

Players set up once: a new release never asks them to build again. The launcher asks again only when
`prepare.version` is different from the one their setup ran with. Change it (any text; it is `"1"` now) in the same commit as a
change that makes an old game folder wrong for the new release, for example:

- a release the game folder must be rebuilt for (the lifter, the profile, the pipeline cache or `image.bin` changed in a way an
  old folder cannot take, which the builder window's Update does too),
- a change of the game folder's layout (where the game, `image.bin` or the options file is),
- a new `--prepare` default that changes what is built.

Do not change it for a release that only changes the builder window, the docs or the settings rows. The old folder keeps working
until the player sets up again, and `make_game.py --update` (the builder window's Update) is still the cheaper way for a player
who also uses the builder. Bump `version`, keep `done`, say so in the release notes.

## Making a release

`python -I tools/release/make_release.py --tag v0.4.1` (or `make_release.ps1`) now also writes
`build/release/<tag>/HeyListen-<tag>-Windows-x64.zip`, from the parts the release lists (the parts an earlier release made are
downloaded and checked, as for the setup). It is about 460 MB. Upload it with the other assets (the command it prints). The Linux
workflow names the AppImage `HeyListen-<tag>-Linux-x64.AppImage` and checks that `recomplauncher.json` inside it is the
repository's (`tools/release/check_release_linux.py`).

The release workflow (`.github/workflows/release.yml`, docs/release.md) runs the same script and attaches the zip to the draft; `collect_release.py` checks the zip's `recomplauncher.json` against the repository's.

## Checking the file with the launcher

In RecompLauncher: Settings, Developers, turn on Developer tools, then **Add a build** and pick the unpacked zip's folder. Its
Developer tab reads the file and shows the level reached, the setup inputs, the settings and any problem. Try the setup with a
copy of the game (the build takes a while), change a setting and press Play. Level 3 is reached when `prepare` shows as the
setup's "Getting the extras" step.

To get listed: [submit](https://recomplauncher.com/submit?platform=pc) `Sacrappy/HeyListen` and say it supports levels 1 to 3.

## The install folder

A profile cannot let the player choose where the game is installed: the guide (recomplauncher.com/sdk-files/guide.md) defines
`{install}` as "your game's folder", the launcher's own choice, and the schema (recomplauncher.com/schema/recomplauncher.json,
`additionalProperties: false` at the top) has no field for an install folder; the only path fields are `prepare.done`, `config`,
`saves`, `folders` and the `{...}` placeholders. Where HeyListen goes is the launcher's setting, so a request for choosing it belongs
to the launcher's developer. (HeyListen's own builder window and the AppImage let the player choose the game folder.)
