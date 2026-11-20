# Vendor frame generation for HeyListen (FG1, phase 1 research, 2026-11-18)

Read-only research from the vendors' public repositories and docs, plus Hookshot and Longshot's git history. Nothing of NVIDIA's
or AMD's is in this repo or ever will be: the SDK files are fetched by the user, under the vendors' licenses, as for DLSS and
FSR 4 today.

## Summary

| | NVIDIA DLSS Frame Generation | AMD FSR 4 Frame Generation (ML, 4.0.x) |
|---|---|---|
| On Vulkan? | Yes, two ways: Streamline (`sl.dlss_g`) and NGX directly (`NVSDK_NGX_Feature_FrameGeneration`, `nvsdk_ngx_helpers_dlssg_vk.h`) | Yes since FidelityFX/FSR SDK **v2.4.0** (the one `ffx_sdk.py` already pins): `amd_fidelityfx_framegeneration_vk.dll`, FG 4.0.2 and FG swapchain 4.0.0 |
| Hardware | RTX 40: 2x. RTX 50: multi-frame (up to 6x, NGX reports `MultiFrameCountMax`) | RX 9000 series (RDNA 4) only, Windows 11, Vulkan 1.2+ |
| Who presents | We do (NGX writes the made frames into our images) | Either the vendor swapchain replacement or, for the FG context alone, our own display thread (the context only computes images) |
| Feasible on our renderer | Yes, already done by Longshot and Hookshot with NGX, no Streamline | Yes in principle (context-only use); untestable here (no AMD card) |
| SDK files, license | NGX headers + `nvngx_dlssg.dll` from github.com/NVIDIA/DLSS (same pinned release as DLSS SR), NVIDIA RTX SDKs license | `amd_fidelityfx_framegeneration_vk.dll` (36.8 MB) beside the loader/upscaler DLLs of v2.4.0, AMD FSR SDK license |

## 1. NVIDIA DLSS Frame Generation

### 1a. Streamline (`sl.dlss_g`), what the docs say (NVIDIAGameWorks/Streamline 2.14.x, ProgrammingGuideDLSS_G.md)

- Vulkan is supported: `slSetVulkanInfo` replaces `slSetD3DDevice`; the app loads `sl.interposer.dll` dynamically and
  uses its exported `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`. It intercepts `Present`, `CreateSwapchainKHR`,
  `DestroySwapchainKHR`, `GetSwapchainImagesKHR`, `AcquireNextImageKHR`, `DeviceWaitIdle`, `CreateWin32SurfaceKHR`,
  `DestroySurfaceKHR`. The interposer owns the swapchain: it presents the extra frames itself (with the app's present
  and acquire semaphores handed over). The app must also run `slGetFeatureRequirements` before the instance and device
  exist and add the extensions, features and an optical-flow queue it names (native optical flow: driver 527.64, Vulkan 1.1+).
- Tags: depth, motion vectors (dense: camera and object motion), HUD-less colour (`kBufferTypeHUDLessColor`), UI colour and
  alpha (or UI alpha alone, preferred), optional backbuffer for subrects. Vulkan needs each tag's layout, size, format,
  usage flags from the app (the image cannot report them). Camera data (`slSetConstants`) per frame, and **Reflex through
  Streamline is mandatory** (PCL markers, present start/end carrying the frame index); a standalone Reflex integration cannot
  be mixed with DLSS-G.
- Multi-frame: `numFramesToGenerate` (1 to `numFramesToGenerateMax`, from `slDLSSGGetState`): 3 gives 4x, at most 6x.
  Dynamic MFG is D3D12 only, V-Sync with FG is D3D12 only (per the guide), needs driver 595.41+ for dynamic mode.
- Windows 10 20H1+, hardware-accelerated GPU scheduling on, NVIDIA adapter only.
- Files: the SDK's binaries are no longer in the repo (since SL 2.7.32) but in GitHub release zips: `sl.interposer.dll`,
  `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll`, ... The DLSS-G plugin is prebuilt and
  signed only. Streamline is under NVIDIA's own license (license.txt in the repo, third-party notices).
- Minimum Streamline version for Vulkan DLSS-G: NVIDIA does not state it in the pages I could read; Vulkan DLSS-G is
  documented in 2.4.x (the download page lists Vulkan fixes in 2.4.10) and works in 2.14.x. nvpro-samples/vk_streamline is
  NVIDIA's Vulkan sample for DLSS SR + FG.

### 1b. NGX directly (what is already proven for this game)

The NGX SDK we already fetch (`engine/scripts/ngx_sdk.py`, NVIDIA/DLSS v310.9.16) contains
`nvsdk_ngx_helpers_dlssg_vk.h`, `nvsdk_ngx_defs_dlssg.h`, `nvsdk_ngx_params_dlssg.h` (all three are already in the SHA list)
and `lib/Windows_x86_64/rel/nvngx_dlssg.dll` (and `libnvidia-ngx-dlssg.so.310.9.16` on Linux). With them DLSS-G is a feature of
the same NGX session as DLSS SR: `NGX_VK_CREATE_DLSSG`, `NGX_VK_EVALUATE_DLSSG`, parameters
`NVSDK_NGX_Parameter_FrameGeneration_Available`, `..._NeedsUpdatedDriver`, `NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax`.
No Streamline, no interposer, no swapchain hooking: the app records NGX's evaluation into its own command buffer and gets the
made frames in its own images; the application presents. Streamline is the supported path for shipping games; NGX is the lighter
one for an emulator renderer that already paces its own presents. It needs the driver's NGX core (`_nvngx.dll`) and either
`nvngx_dlssg.dll` beside DLSS's runtime or the driver's own copy.

### 1c. Existing work in Hookshot and Longshot

- **Longshot** (git.magicbean.dev/MagicBeanDev/Longshot, author pmwork-personal): `c51df0b9 feat(dlss): DLSS Frame Generation
  through NGX, evenly paced at 3x and up (#2118)` (2026-11-11) and follow-ups (`55b976c0` own runtime in the game folder,
  `ec76f696` multiplier stops at the runtime's most + 1: 6x on RTX 50, 2x on RTX 40, `c7db6c02` compute-queue presents with
  driver 621.24+, `02a2e4f0` V-Sync On, `a6c8ed57` hint text, `9af4808f` overlay lifetimes). It uses NGX, no Streamline: inputs are the
  chain's depth and motion (actor motion included) plus camera (view, projection), output into the renderer's own set of made frames
  which the display thread shows; DLSS FG is the default "Frame generation by" where it runs, FSR 3.1 FG makes frames DLSS FG does
  not (Auto); the game's menus, movies and loading show only real frames; fallbacks say why. Reflex goes through VK_NV_low_latency2.
- **Hookshot** (NotRexed): the same code is in its history (hookshot-nr/main, hs-web/main): `vk_dlss.cpp` includes
  `nvsdk_ngx_helpers_dlssg_vk.h` and creates `NVSDK_NGX_Feature_FrameGeneration`. (The `https://git.notrexed.dev/Hookshot/Hookshot`
  remote and github.com/NotRexed/Hookshot were not fetchable from here today; the already-fetched remote-tracking refs were used.)
- Neither does Streamline. Neither has FSR 4 frame generation (Longshot has FSR 4 upscaling and FSR 3.1 FG).
- **HeyListen main does not have DLSS FG** (no `dlssg` in `engine/`, only the three headers in `ngx_sdk.py`'s list). It is on the
  unmerged sync branches (`sync/longshot-13`, `sync/hookshot-12`). So phase 2 is a port of c51df0b9's design onto main, not new research.

### 1d. Feasibility on our renderer (DLSS FG)

- Swapchain ownership: our display thread presents. NGX does not touch the swapchain, so there is nothing to hook. Good fit.
- Inputs: `vk_fsr3_inputs.cpp` already produces depth and motion vectors for FSR 3.1 FI; the DLSS upscaler already has its own copy.
  DLSS-G needs depth, motion, the camera matrices (view, projection), and the picture; HUD: our `fsr3` path already marks what the game draws
  after the upscale (#1464 in Longshot: "what the game drew over its picture after the upscale is marked"); the same marked picture
  without the HUD is the HUD-less input, the HUD is laid over the made frames by us (NGX has no UI tag in the NGX interface of v310).
- Reflex markers: already there (VK_NV_low_latency2).
- Effort: large but known: about 570 lines in `vk_dlss.cpp`, glue in `vk_main.cpp`, `vk_framegen.cpp` (made frames set), `vk_menu.cpp`,
  `vk_reflex.cpp`, `vk_replay.cpp`, tests. Risks: pacing (3x/4x need present-ahead and extra swapchain images), V-Sync interplay,
  menus/loading screens (no upscaled picture to interpolate), device-lost on drivers that refuse sets, `nvngx_dlssg.dll` version
  skew with the driver, not testable on RTX 40 here (only the RTX 5070: multi-frame yes, 2x only on an RTX 40 is Longshot's report).
- Choice: **NGX, not Streamline.** Streamline would need the interposer to own our swapchain (conflicts with the display thread, the present
  queue logic and our overlays), a second Reflex path, an optical-flow queue and ~10 more DLLs to download; the NGX route reuses the
  SDK fetch we already have and adds one runtime file.

## 2. AMD FSR 4 Frame Generation

- **Vulkan: yes.** FidelityFX/FSR SDK v2.4.0 ("Redstone", the commit `ffx_sdk.py` pins) states Vulkan feature parity with DX12 for FSR
  Frame Generation 4.0.2, FSR Frame Generation (Swapchain) 4.0.0 and the 3.1 FG parts. Earlier SDKs (2.0 to 2.3) were DX12 only for it.
  Files in `Kits/FidelityFX/signedbin/` at that commit: `amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll`
  (already fetched) and **`amd_fidelityfx_framegeneration_vk.dll`** (36 806 928 bytes; hash to be recorded at fetch time).
- Hardware (docs/techniques/frame-interpolation-ml.md, "Limitations"): Windows 11, **Radeon RX 9000 series or later (RDNA 4)**,
  Vulkan 1.2+. The 3.1.x frame interpolation (FSR 3.1 FG) is the analytic one for any GPU (vk_fsr3fi.cpp, already ours).
- API: the "ffx" API (`ffxCreateContext/ffxConfigure/ffxDispatch/ffxQuery` through the loader). Context `ffxCreateContextDescFrameGeneration`;
  per frame the call order is mandatory: `ffxConfigure(ffxConfigureDescFrameGeneration)` (callback/present settings, HUD-less), then
  `ffxDispatch(ffxDispatchDescFrameGenerationPrepareV2)` (depth, motion vectors, camera position/up/right/forward, jitter,
  near/far/FOV, frame time, reset flag), then `ffxDispatch(ffxDispatchDescFrameGeneration)` (input colour, `outputs[4]`, `numGeneratedFrames`,
  HUD-less colour, UI). `numGeneratedFrames` and `outputs[4]` suggest up to 4 made frames per call in the API; whether the ML
  provider accepts more than 1 is not stated in the docs: treat 2x as certain, more as to be tested. Presentation: the doc says the context
  only computes the interpolated image and sends to the swapchain document (`FrameGenerationSwapChainVK`: a replacement for
  `vkCreateSwapchainKHR`, acquire, present with own queues: game, async compute, present, image acquire; UI resource registration;
  frame pacing, driver-assisted pacing needs RX 9000 and driver 26.10.1+). That swapchain is optional: we can dispatch the context
  alone and present ourselves, as we do for FSR 3.1 FI (to be tested; AMD's docs describe the swapchain as the way to get pacing).
- Inputs we already have (depth, motion vectors, camera: `vk_fsr3_inputs.cpp`) fit; HUD-less colour as for 3.1 FI.
- License: AMD's FSR SDK license (`docs/license.md`, already saved by `ffx_sdk.py` as LICENSE-AMD-FSR-SDK.md): binary redistribution
  allowed with notices, no reverse engineering; we still do not redistribute, as for FSR 4 upscaling.
- Feasibility: moderate; the FSR 4 upscaler wrapper (`vk_fsr4.cpp`, 492 lines) already loads the loader and creates contexts; FG would be a
  second context of the same kind next to `vk_fsr3fi.cpp`. Risk: cannot be tested here (no AMD card). Mooseca (RX 9070 XT) must test.
- Nearest option if it fails: the current "FSR 3.1 FG" at 2x (multi-frame is not part of AMD's FSR 3.1 FG either; our own interpolation does 3x-8x).

## 3. Decision for phase 2

1. **DLSS FG via NGX**: port Longshot's design onto main, behind a new off-by-default `frame_generation` choice ("NVIDIA DLSS Frame
   Generation 2x/3x/4x", limited to what the runtime reports); extend `ngx_sdk.py` with `nvngx_dlssg.dll` (+ the Linux .so) pinned at SHA-256.
   Testable here (RTX 5070: up to 4x; the runtime tells the maximum).
2. **FSR 4 FG**: implement only to the extent it can be written safely without a card (fetch of the FG DLL added to `ffx_sdk.py`, the
   setting, graceful "needs RX 9000" fallback), or leave it as a documented test plan for Mooseca. See the final report for what was done.

## 4. The multiplier per mode, the limit, and what was left of Hookshot's follow-ups (branch feat/fg-menu-limits)

- **ec76f696** (pmwork-personal, "DLSS FG's multiplier stops at its runtime's most + 1") is ported with its author: the multiplier
  that runs is ramegen_now() (gpu/vk/vk_framegen.cpp), at most the runtime's most + 1 while DLSS FG makes the frames (6x on an
  RTX 50, 2x on an RTX 40). Here the rule is ramegen_rows.h: the same for DLSS FG, AMD FSR 3.1 FG makes 2x only, the built-in
  generator Auto and 2x to 8x. The V-Sync, swapchain and translation parts of that commit have no counterpart here.
- **Frame rate limit**: the rate presented. Fixed multiplier m: the game runs at the limit over m (host/nn_vi.c ps_plan, with
  HL_OPT_FRAMEGEN_MULT), the multiplier lowered so the game keeps 20 frames a second (60: 3x at most). Off and Auto: the game at the
  limit. Unlimited (rame_rate_limit=6): nothing held.
- **02a2e4f0** ("with V-Sync On, frame generation leaves out the made frames the refresh has no room for") and **6438c2ef** ("fixed
  multipliers on the game's timeline under V-Sync On (FIFO)") are not ported: both belong to Hookshot's V-Sync row and its FIFO
  swapchain (eat/vsync-vrr here, not on this branch) and to the earlier timeline work (#1362: ixed_on_timeline, ifo_slot)
  that this tree's k_framegen.cpp does not have; a cherry-pick of 6438c2ef conflicts in k_framegen.cpp and k_internal.h on those
  functions. They shape how made frames are spread under FIFO, not how many are made or at what rate the game runs, so they do not
  help the limit. They are the ones to take with the V-Sync row.