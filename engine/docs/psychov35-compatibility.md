# PsychoV35 and the upscalers

(HeyListen, after Longshot b80a68f1, #608.) Native HDR and PsychoV35 stay the player's choices; Vanilla is still the default
tone mapper. With PsychoV35 on, its RGBA16F scene goes to the upscalers that can take it.

## Settings

`RX_VK_PSYCHOV35_DLSS` is on when unset or empty; `0` turns the route off, for troubleshooting. Put `RX_VK_PSYCHOV35_DLSS=0` in
`hookshot_env.txt` and restart to turn it off; remove the line (or set `1`) to turn it on again. It covers the temporal chain
and FSR 3.1 frame generation's own inputs. It does not turn on FSR frame generation, lift its 2x limit, or give HDR on an SDR
display. Upscaler Off and the TAAU replacement take the RGBA16F scene whatever it says. `RX_VK_DLSS_HDR=0` and
`RX_VK_FSR4_HDR=0` still keep those engines from taking the HDR scene.

The chain engines are DLSS/DLAA, FSR 4 and its Native AA, and FSR 3.1, which goes on the chain while PsychoV35 is on
(`RX_VK_FSR3_CHAIN=0` keeps it on its own inputs, where PsychoV35's scene is not taken). The Upscaler row showing an engine
does not prove it ran: check the log for the RGBA16F route and for the frames drawn or generated. Ghosting on characters and
the HUD, and the fallback on a reset frame, depend on the engine and are not changed by this.

## Testing

Compare the route on and off in the same scene, and check a cold launch, saved settings, and changes of render resolution,
tone mapper, HDR and frame generation. Note the commit, GPU, driver and OS tested.

The tone mapper row shows only with HDR set to Native: set that first. For DLAA, choose DLSS and then the Native render
resolution.
