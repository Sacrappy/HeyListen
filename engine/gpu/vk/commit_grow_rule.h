// Copyright (C) 2026 Sacrappy
#pragma once
// (COMMIT1, 28 Nov 2026) The commit grow rule, apart from the renderer so that tests/test_commit_grow_rule.py runs it alone.
// A system-managed page file grows when a commit request does not fit; the commit reserve (vk_texture.cpp, RX_VK_COMMIT_RESERVE_MB)
// keeps the charge under the limit, so no request ever fails and Windows never grows it. Near the limit the renderer therefore asks
// once in a while for a block of commit (a helper thread commits it and gives it back at once): when the page file can grow, the
// limit rises and the textures stop being cut; on a fixed page file, or one at its maximum, the tries do not raise the limit,
// stop after a few, and the reserve rule works as before.
#include <cstdint>

namespace commit_grow_rule {

constexpr int64_t kSpacingNs = 10'000'000'000;   // at most one try every 10 s
constexpr int kMaxFailed = 6;                    // tries in a row that did not raise the limit, then none for the session
constexpr uint64_t kRound = 64ull << 10;         // the size asked is rounded up to 64 KB

struct State {
    int64_t last_ns = 0;   // (the last try's start, ns; 0: none yet)
    int failed = 0;        // (tries in a row that did not raise the limit)
};

/// the size to ask for: what the reserve lacks, and the reserve again, rounded up to 64 KB (0 when the commit left covers the reserve)
inline uint64_t ask_bytes(uint64_t commit_left, uint64_t reserve) {
    if (!reserve || commit_left >= reserve) return 0;
    const uint64_t n = (reserve - commit_left) + reserve;
    return (n + kRound - 1) / kRound * kRound;
}

/// whether to try now (`now_ns` > 0): only below the reserve, 10 s after the last try, and not after kMaxFailed failures in a row
inline bool should_try(const State& s, uint64_t commit_left, uint64_t reserve, int64_t now_ns) {
    if (!reserve || commit_left >= reserve) return false;
    if (s.failed >= kMaxFailed) return false;
    return !s.last_ns || now_ns - s.last_ns >= kSpacingNs;
}

/// a try starts at `now_ns`
inline void started(State& s, int64_t now_ns) {
    s.last_ns = now_ns;
}

/// a try ended: the limit before and after; returns the try's number in the failed run (0 when the limit rose: the count starts again)
inline int finished(State& s, uint64_t limit_before, uint64_t limit_after) {
    if (limit_after > limit_before) return s.failed = 0;
    return ++s.failed;
}

}   // namespace commit_grow_rule
