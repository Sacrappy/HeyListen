// (HeyListen, from Longshot 83b161e3) NVIDIA DLAA before the game's bloom and tone map (vk_temporal.cpp temporal_pre_bloom): the pass of a frame it runs
// at. Plain code with no Vulkan calls: tests/test_dlss_pre_bloom.py compiles it alone.
//
// OoT's gameplay frame, as a frame dump shows it (RX_VK_DUMP; DLAA at 5120x2160 in the Market): the scene's colour target
// (B10G11R11, linear light before the exposure) takes the opaque draws; then the game copies it at a quarter and at half the
// size (what the refraction and the soft particles read) and draws more into it with the scene's depth (transparent objects,
// particles, the volumetric fog); then its bloom starts, a draw that reads the scene into targets an eighth of its size, the
// blur chain from there, and the scene mapping draw: the scene and the bloom, times the exposure, through the tone curve, into
// a target of its own. Passes after that put the picture, square-root encoded, back into the scene's image, where the gameplay
// upscale network takes it (vk_temporal.cpp temporal_upscale, where the engines ran until now: after the bloom and the tone map).
//
// So DLAA belongs after the frame's last draw into the scene and before the first pass that reads the scene after that draw:
// the bloom's first draw. Not before the first pass that reads the scene: the copies come before draws into the scene, which
// would then be neither anti-aliased nor rid of the camera's jitter. Which pass it is shows only at the frame's end, so it is
// learned: a Frame lists the frame's samplers (draws that read the scene's colour and do not write it) in order, by program,
// and how many came before the frame's last draw into the scene. At the frame's end (its gameplay upscale) learn() makes the
// next frame's Trigger: the first sampler after that draw, its place in the list and its program. The next frame runs DLAA at
// the sampler with that place and program. A frame whose passes differ (the first one, another area, a menu over the scene)
// meets no such sampler: its DLAA runs at the gameplay upscale as before, and it teaches the next frame. A draw into the scene
// after DLAA ran (late) teaches nothing: the next frame runs DLAA at the gameplay upscale and teaches the one after.
//
// (HeyListen) A learned place is kept (keep()): matched by its program wherever it comes in the list, and left only for an
// earlier one a frame teaches. HeyListen's frames draw into the scene after its bloom starts reading it (40 to 75 draws, and
// 15 to 25 after the tone map's read; a sword swing's trail among them), so most of them teach nothing, and a reader now and
// then falls on the other side of the frame's end, moving the bloom's place. Re-learned every frame as above, DLAA ran before
// the bloom in 1 to 3% of the frames and at the gameplay upscale in the rest, its history restarting at each change: the
// picture flickered (on every sword swing). Those late draws are drawn after DLAA, as in Longshot's frames that have them.
#pragma once

#include <cstdint>

namespace mkwvk::pre_bloom {

constexpr uint32_t kSamplers = 32;   // (a frame's samplers listed: the Market's frame has 6, its bloom's the fifth)

/// Where the next frame's DLAA runs: before its sampler number place (from 0) when that sampler is program key
struct Trigger {
    uint64_t key = 0;
    uint32_t place = 0;
    bool valid = false;
    bool operator==(const Trigger& o) const { return key == o.key && place == o.place && valid == o.valid; }
};

/// One frame's passes, as they are drawn
struct Frame {
    uint64_t keys[kSamplers]{};   // (the samplers' programs, in order: the first kSamplers of them)
    uint32_t samplers = 0;        // (how many so far)
    uint32_t before_last = 0;     // (the samplers that came before the last draw into the scene so far)
    bool ran = false;             // (DLAA ran in this frame, before sampler ran_at)
    uint32_t ran_at = 0;
    bool late = false;            // (a draw into the scene came after it ran)

    /// A draw into the scene's colour (reading it too or not)
    void scene_draw() {
        before_last = samplers;
        if (ran) late = true;
    }
    /// A draw that reads the scene's colour and does not write it: true when DLAA is to run before it (t's place and program)
    bool sampler(uint64_t key, const Trigger& t) {
        const uint32_t at = samplers;
        if (at < kSamplers) keys[at] = key;
        samplers = at + 1;
        if (ran || !t.valid || key != t.key) return false;   // ((HeyListen) its program, at whatever place: keep())
        ran = true, ran_at = at;
        return true;
    }
};

/// At the frame's end: the next frame's trigger. None after a late draw, with no sampler after the last draw into the scene,
/// or when that sampler is past the list
inline Trigger learn(const Frame& f) {
    Trigger t;
    if (f.late || f.before_last >= f.samplers || f.before_last >= kSamplers) return t;
    t.key = f.keys[f.before_last], t.place = f.before_last, t.valid = true;
    return t;
}

/// (HeyListen) At the frame's end: the next frame's trigger, the current one kept. A frame that teaches nothing (a late draw,
/// no reader after the last draw into the scene: a menu, a sword swing) keeps it; one where it did not fire (another area's
/// passes) takes what it teaches; one where it fired changes it only for an earlier reader that frame teaches (closer to the
/// last draw into the scene: the bloom's first draw rather than the tone map's)
inline Trigger keep(const Frame& f, const Trigger& current) {
    const Trigger t = learn(f);
    if (!t.valid) return current;
    if (current.valid && f.ran && t.place >= f.ran_at) return current;
    return t;
}

} // namespace mkwvk::pre_bloom
