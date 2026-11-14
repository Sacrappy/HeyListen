# Black screen then exit on a GPU without mesh shaders (GTX 10xx, RX 5000 and older)

For a player whose log says `vk: no mesh shaders: task and mesh programs run as compute shaders (issue 640 fallback)`
and ends with `submit ... failed (-4)` and `device lost` (GitHub issue 1: a GTX 1060 6GB, driver 581.57, v0.3.1).
Our own GPUs have mesh shaders, so we cannot reproduce the loss. Three short runs tell us where it happens.

## How

Close the game. Open `hookshot_env.txt` (beside `HeyListen.exe`; create it if it is missing) in Notepad and put the
lines of ONE run below in it, one per line, nothing else changed. Play until the black screen (or two minutes if
nothing happens), then close the game, keep `game_log.txt` (beside the exe) under another name, and take the lines
away again before the next run.

Run 1, the most useful: the fallback left out. The grass and foliage the fallback draws will be missing.

    RX_VK_NO_MESH_FALLBACK=1

The first start with this line rebuilds the game's shader cache for a GPU without the fallback (several minutes at
start, once; it rebuilds again when the line is taken out). If the game then runs past the place where it closed, the fault is in the fallback's work. If it still closes, the
fallback is not the cause and the log of run 2 matters more.

Run 2: the driver's own report of the faulting address.

    RX_VK_FAULT_INFO=1

Run 3: pipelines built without the driver's optimiser (it was the cause of hangs on some AMD cards).

    RX_VK_NOOPT=1

Only if the Vulkan SDK is installed already (do not install it for this): Run 4, which prints every Vulkan usage
error the back end makes.

    RX_VK_VALIDATION=1

## What to send

Per run, the whole `game_log.txt` if it is under a few MB, else these lines (the first 90 lines of the file are
needed too: they say the GPU, its limits and the switches the run had):

- every line containing `device lost`, `checkpoint`, `stage 0x`, `before:`, `after:`, `device fault`, `fallback:`
  or `float controls`;
- the 60 lines before the first `device lost`;
- every `mesh draw` line;
- which run it was (1, 2 or 3), and whether the game closed, and how many seconds after the first cutscene.

Please also say the GPU, the driver version and whether the other fork that runs on the same PC shows the grass.
