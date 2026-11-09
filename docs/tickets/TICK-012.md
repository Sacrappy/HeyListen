# TICK-012 — Remaining hitches: textures, pools, recording locks  · model: Sonnet
**Why**: after TICK-008, 13-20 frames > 50 ms per run remain (title, load, area entry) with no pipeline created in play; causes per TICK-008's report: texture and memory pool creation, lock waits between recording threads, guest scheduler frames in the title.
**Goal**: measure each hitch's cause (RX_VK slow-frame logs / SlowWatch, HOS_SAMPLE_ALL around the hitch) and remove the ones on the renderer side: texture/pool creation off the recording threads or pre-allocated, finer locks.
**Acceptance**: frames > 50 ms after the first gameplay frame: 0 in a Hyrule Field run from a warm cache; report the title/load ones separately; picture correct.
**Branch**: `feature/TICK-012-hitches`
