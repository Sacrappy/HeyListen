// SPDX-License-Identifier: GPL-3.0-or-later
// (Longshot f1b17d59, dukenheimer, PR #223: "give memory back under Linux memory pressure and after the precompile") Linux
// memory pressure: MemAvailable and SwapFree in /proc/meminfo. The renderer's device heap can report room while the PC has
// none left, because every VMA block, staging arena, spare capture buffer and imported guest pool is kernel-pinned in
// physical RAM: freeing them raises MemAvailable, and the kernel's OOM killer (or earlyoom) acts long before the device
// heap says anything. Nothing of Vulkan here: tests/test_linux_pressure.py and tests/linux_pressure_test.cpp.
//
// Issue 88's run (Fedora 45, Intel Arc B580, Mesa 26.2.6, 31 GB) shows the other half of the same picture: the driver's
// HOST-side compiles (NIR, register allocation) take system RAM, gigabytes of it, and nothing in the renderer saw it coming.
// The compiles' share is capped by compile_gate.h; this header is the rest of the give-back: the textures' budget, idle
// imports and the spare capture buffers.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace linux_pressure {

/// Default reserves on Linux:
/// - kDefaultAvailReserve (1536 MB, RX_VK_AVAIL_RESERVE_MB): physical RAM available without swapping (MemAvailable). On a 16 GB
///   Steam Deck OLED (~14.8 GB MemTotal), earlyoom sends SIGTERM at ~5% available (~750 MB) when swap is low, and direct reclaim
///   stalls texture uploads once MemAvailable dips below ~1-1.5 GB. Every VMA block, staging arena, spare capture buffer and
///   imported guest pool is kernel-pinned in physical RAM and cannot be swapped to zram or disk, so freeing them raises
///   MemAvailable directly.
/// - kDefaultCommitReserve (2560 MB, RX_VK_COMMIT_RESERVE_MB): MemAvailable + SwapFree, the Linux counterpart to Windows'
///   ullAvailPageFile commit reserve (vk_texture.cpp texture_frame's rule reads it on both now).
/// - kClearMargin (256 MB) and kBudgetFloor (512 MB): hysteresis and floor for texture_frame()'s budget hold.
constexpr uint64_t kDefaultAvailReserve = 1536ull << 20;
constexpr uint64_t kDefaultCommitReserve = 2560ull << 20;
constexpr uint64_t kClearMargin = 256ull << 20;
constexpr uint64_t kBudgetFloor = 512ull << 20;
/// How often texture_frame reads /proc/meminfo (it is a file read, and a present happens 30-240 times a second)
constexpr uint64_t kSampleEveryPresents = 30;

inline uint64_t reserve_from_env(const char* env, uint64_t def_bytes) {
    if (!env || !*env) return def_bytes;
    return (uint64_t)std::max(0, std::atoi(env)) << 20;
}

inline uint64_t avail_reserve(const char* env) { return reserve_from_env(env, kDefaultAvailReserve); }
inline uint64_t commit_reserve(const char* env) { return reserve_from_env(env, kDefaultCommitReserve); }

/// Parses MemAvailable and SwapFree (in bytes) from /proc/meminfo text, and optionally MemTotal and whether swap is configured
/// (SwapTotal > 0 or SwapFree > 0). Returns false when MemAvailable is not present; SwapFree defaults to 0 when absent.
inline bool parse_meminfo(const char* text, uint64_t* avail_bytes, uint64_t* swap_free_bytes, uint64_t* total_bytes = nullptr,
                          bool* has_swap = nullptr) {
    if (!text || !avail_bytes || !swap_free_bytes) return false;
    *avail_bytes = 0;
    *swap_free_bytes = 0;
    if (total_bytes) *total_bytes = 0;
    bool have_avail = false, seen_swap_total = false;
    uint64_t swap_total = 0;
    for (const char* p = text; *p;) {
        const char* end = std::strchr(p, '\n');
        const size_t len = end ? (size_t)(end - p) : std::strlen(p);
        unsigned long long kb = 0;
        if (len >= 13 && !std::memcmp(p, "MemAvailable:", 13) && std::sscanf(p + 13, "%llu", &kb) == 1) {
            *avail_bytes = (uint64_t)kb << 10;
            have_avail = true;
        } else if (len >= 9 && !std::memcmp(p, "SwapFree:", 9) && std::sscanf(p + 9, "%llu", &kb) == 1) {
            *swap_free_bytes = (uint64_t)kb << 10;
        } else if (len >= 10 && !std::memcmp(p, "SwapTotal:", 10) && std::sscanf(p + 10, "%llu", &kb) == 1) {
            swap_total = (uint64_t)kb << 10;
            seen_swap_total = true;
        } else if (total_bytes && len >= 9 && !std::memcmp(p, "MemTotal:", 9) && std::sscanf(p + 9, "%llu", &kb) == 1) {
            *total_bytes = (uint64_t)kb << 10;
        }
        if (!end) break;
        p = end + 1;
    }
    if (has_swap) *has_swap = !seen_swap_total || swap_total > 0 || *swap_free_bytes > 0;
    return have_avail;
}

/// Reads MemAvailable and SwapFree (and optionally MemTotal and has_swap) from path (or /proc/meminfo on Linux). Off Linux, only
/// an explicit path is read (for tests).
inline bool read_meminfo(uint64_t* avail_bytes, uint64_t* swap_free_bytes, const char* path = nullptr,
                         uint64_t* total_bytes = nullptr, bool* has_swap = nullptr) {
#if !defined(__linux__)
    if (!path || !*path) return false;
#endif
    const char* target = (path && *path) ? path : "/proc/meminfo";
    FILE* f = std::fopen(target, "rb");
    if (!f) return false;
    char buf[4096];
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return parse_meminfo(buf, avail_bytes, swap_free_bytes, total_bytes, has_swap);
}

/// How many bytes short of the reserves the system is: the larger of the physical-RAM shortfall (below avail_reserve_bytes) and,
/// when swap is enabled (has_swap), the total-memory-plus-swap shortfall (below commit_reserve_bytes). 0 when both reserves are
/// met or turned off (0).
constexpr uint64_t shortfall(uint64_t avail_bytes, uint64_t swap_free_bytes,
                             uint64_t avail_reserve_bytes = kDefaultAvailReserve,
                             uint64_t commit_reserve_bytes = kDefaultCommitReserve,
                             bool has_swap = true) {
    const uint64_t avail_over = avail_reserve_bytes > avail_bytes ? avail_reserve_bytes - avail_bytes : 0;
    const uint64_t total_free = avail_bytes <= UINT64_MAX - swap_free_bytes ? avail_bytes + swap_free_bytes : UINT64_MAX;
    const uint64_t commit_over = has_swap && commit_reserve_bytes > total_free ? commit_reserve_bytes - total_free : 0;
    return avail_over > commit_over ? avail_over : commit_over;
}

/// Hysteresis state for texture_frame()'s Linux memory-pressure budget hold:
/// - Enters pressure when shortfall(avail, swap_free, avail_res, commit_res, has_swap) > 0, capturing the texture total at entry
///   (baseline_total = total_bytes) and holding held_budget = max(floor_bytes, baseline_total - over).
/// - While active, if shortfall grows (MemAvailable drops further), held_budget steps down to max(floor_bytes, baseline_total - over),
///   never ratcheting off the already-shrunk total_bytes on each frame while VMA compaction is still freeing blocks.
/// - Once both reserves are cleared by kClearMargin (256 MB), steps held_budget back up by margin_bytes per sample and leaves
///   active once it reaches baseline_total, preventing flapping or an immediate full-cut regrowth spike.
struct PressureHold {
    bool active = false;
    uint64_t baseline_total = 0;
    uint64_t held_budget = UINT64_MAX;
    uint64_t last_over = 0;

    constexpr uint64_t step(uint64_t total_bytes, uint64_t avail_bytes, uint64_t swap_free_bytes,
                            uint64_t avail_res = kDefaultAvailReserve, uint64_t commit_res = kDefaultCommitReserve,
                            uint64_t margin_bytes = kClearMargin, uint64_t floor_bytes = kBudgetFloor,
                            bool has_swap = true) {
        const uint64_t over = shortfall(avail_bytes, swap_free_bytes, avail_res, commit_res, has_swap);
        if (!active) {
            if (!over) {
                last_over = 0;
                return UINT64_MAX;
            }
            active = true;
            baseline_total = total_bytes;
            last_over = over;
            const uint64_t cut = baseline_total > over ? baseline_total - over : 0;
            held_budget = std::max(cut, floor_bytes);
            return held_budget;
        }
        const uint64_t clear_avail = avail_res ? avail_res + margin_bytes : 0;
        const uint64_t clear_commit = commit_res ? commit_res + margin_bytes : 0;
        if (shortfall(avail_bytes, swap_free_bytes, clear_avail, clear_commit, has_swap) == 0) {
            if (margin_bytes && held_budget <= UINT64_MAX - margin_bytes && held_budget + margin_bytes < baseline_total) {
                held_budget += margin_bytes;
                last_over = baseline_total > held_budget ? baseline_total - held_budget : 0;
                return held_budget;
            }
            active = false;
            baseline_total = 0;
            held_budget = UINT64_MAX;
            last_over = 0;
            return UINT64_MAX;
        }
        if (over > last_over) {
            last_over = over;
            const uint64_t cut = baseline_total > over ? baseline_total - over : 0;
            held_budget = std::min(held_budget, std::max(cut, floor_bytes));
        }
        return held_budget;
    }
};

/// How many bytes of idle unmirrored guest-pool imports imports_give_back() should unpin under a Linux memory shortfall,
/// counting imports already queued for destruction (pending_dead_bytes) as given back.
constexpr uint64_t import_want(uint64_t shortfall_bytes, uint64_t pending_dead_bytes) {
    return shortfall_bytes > pending_dead_bytes ? shortfall_bytes - pending_dead_bytes : 0;
}

/// What imports_give_back() may still unpin in this pressure episode: want, at most the deepest shortfall of the episode less what
/// it has unpinned for it already. The texture cut, the staging arenas and malloc_trim cover the same bytes, and asking for the
/// whole shortfall again at every sample unpinned the idle imports over and over while the episode lasted
constexpr uint64_t episode_want(uint64_t want, uint64_t deepest_shortfall, uint64_t given_in_episode) {
    const uint64_t room = deepest_shortfall > given_in_episode ? deepest_shortfall - given_in_episode : 0;
    return want < room ? want : room;
}

} // namespace linux_pressure
