// (HeyListen) What the shaders' preparation (vk_draw.cpp prepare_run) is made of: its progress bar and its workers.
//
// The bar went by programs. A compute program is up to 64 translations and as many driver builds of 0.3 s to tens of seconds, a
// graphics program some milliseconds, so the bar sat at 7% for 8 of the 9 minutes of a preparation on an RTX 3050 and the
// builders' time left, which extrapolates from it, was hours out. The numbers are that PC's profile (docs/prepare-speed.md): a
// compute program with its variants ~2.9 s of thread time, a graphics program ~0.03 s.
//
// The workers were half the CPUs: with 80% of their time inside the driver's compiler (which is the CPU's, not ours) the other
// half of the PC sat idle for the whole of a first start's wait.
//
// Pure arithmetic: tests/test_prepare_speed.py compiles this file.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace prepare_work {
constexpr uint32_t kCompute = 100;        // (a compute program the driver builds: its translations and its builds)
constexpr uint32_t kComputeLeft = 6;      // (one it leaves to play: its translations only)
constexpr uint32_t kGraphics = 1;         // (a graphics or mesh program)

constexpr uint32_t cost(bool compute, bool left_to_play) {
    return compute ? (left_to_play ? kComputeLeft : kCompute) : kGraphics;
}

/// The workers of a preparation on a PC with `cpus` threads. background: in play, behind the game (a quarter of the CPUs, two to
/// four). Before the game all but an eighth of them (a 16 thread PC: 14, a 12 thread one: 11, 8: 7, 4: 4), one to 48; at normal
/// priority one below, so the window and the PC stay usable. env: RX_VK_PREPARE_WORKERS, "old" (the half it was) or a number of 1 to 64
inline unsigned workers(bool background, unsigned cpus, const char* env = nullptr) {
    cpus = std::max(1u, cpus);
    if (env && !std::strcmp(env, "old")) return background ? std::clamp(cpus / 4, 2u, 4u) : std::clamp(cpus / 2, 1u, 32u);
    if (env && std::atoi(env) > 0) return std::clamp((unsigned)std::atoi(env), 1u, 64u);
    return background ? std::clamp(cpus / 4, 2u, 4u) : std::clamp(cpus - cpus / 8, 1u, 48u);
}
}  // namespace prepare_work
