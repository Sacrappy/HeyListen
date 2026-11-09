# TICK-015 — Cut the engine's own GPU copies  · model: Opus · after 014
**Why**: TICK-010: the frame at a 120 limit is GPU-bound (~12.9 ms GPU a frame); of that ~3.7 ms are the engine's own copies: mirror write-backs to guest memory 1.6 ms / 69 MB a frame, CPU-page copies into mirrors 2.1 ms / 11 MB. The main thread also waits 4-5.5 ms a frame for the GPU in the exposure-meter capture wait (RX_VK_CAPTURE_WAIT).
**Goal**: write back only what the CPU will read (track the guest pages the CPU actually reads back; read traps or the game's readback set), copy only dirty ranges into mirrors, move copies to a transfer queue overlapping graphics if the GPU has one, and shorten/remove the capture wait once GPU headroom exists.
**Acceptance**: GPU time a frame (RX_VK_PASS_TIMING, nvidia-smi) down; Hyrule fps and p99 up at limit 120; no exposure flashes (flash detector) and picture correct.
**Branch**: `feature/TICK-015-gpu-copies`
