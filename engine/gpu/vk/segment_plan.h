/* The parallel replay's segments (vk_replay.cpp replay_handle): how many segments a large command buffer is cut into.
   TICK-050: with more segments than recording threads the last ones ran in a second round, one thread recording while
   the others idled; the cap can be the number of recorders instead, so that every segment is recorded at once. */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace mkwvk::segment_plan {

/// The recorders of a parallel replay: the workers, and the replay thread itself when it records a segment
inline int recorders(int workers, bool self) {
    return std::max(0, workers) + (self ? 1 : 0);
}

/// The segments a command buffer of `total` draws and dispatches is cut into: one per `per` of them, at least one, at most
/// `cap` (the recording slots' bound, kMaxBatch). With `one_round` it is also at most `recorders`, so that no segment
/// waits for a thread to come free (a round of its own). recorders <= 0 sets no such bound.
inline int segment_count(int64_t total, int64_t per, int recorders, int cap, bool one_round) {
    if (per < 1) per = 1;
    if (cap < 1) cap = 1;
    int bound = cap;
    if (one_round && recorders > 0) bound = std::min(bound, recorders);
    return (int)std::clamp<int64_t>(total / per, 1, bound);
}

/// The rounds n segments take on r recorders (a diagnosis: more than one is a round where some recorders idle)
inline int rounds(int n, int r) {
    if (n <= 0) return 0;
    if (r <= 0) return n;
    return (n + r - 1) / r;
}

/// A command buffer's recording time over its length (RX_VK_PARALLEL_BALANCE): bins of equal draw-and-dispatch counts, each the
/// time recorded in it (an average over frames)
constexpr int kBins = 32;
struct Profile {
    double ms[kBins] = {};
    bool valid = false;
};

/// What one command buffer's segments took: segment k covered [from[k], to[k]) of the command buffer (fractions, in order) and
/// took ms[k]; its time is spread evenly over the bins it covers, then blended into the profile (alpha: the new one's weight;
/// the first sample is taken whole)
inline void profile_add(Profile& p, const double* from, const double* to, const double* ms, size_t n, double alpha) {
    double sample[kBins] = {};
    bool any = false;
    for (size_t k = 0; k < n; k++) {
        const double a = std::clamp(from[k], 0.0, 1.0), b = std::clamp(to[k], 0.0, 1.0);
        if (b <= a || !(ms[k] >= 0)) continue;
        for (int i = 0; i < kBins; i++) {
            const double lo = (double)i / kBins, hi = (double)(i + 1) / kBins;
            const double o = std::min(b, hi) - std::max(a, lo);
            if (o > 0) sample[i] += ms[k] * o / (b - a), any = true;
        }
    }
    if (!any) return;
    for (int i = 0; i < kBins; i++) p.ms[i] = p.valid ? p.ms[i] * (1 - alpha) + sample[i] * alpha : sample[i];
    p.valid = true;
}

/// The n-1 boundaries (in draws and dispatches, out[0..n-2]) that cut the profile's time into n equal parts; even counts when
/// the profile has nothing yet
inline void balanced_targets(const Profile& p, int n, int64_t total, int64_t* out) {
    if (n < 2) return;
    double sum = 0;
    for (int i = 0; i < kBins; i++) sum += p.valid && p.ms[i] > 0 ? p.ms[i] : 0;
    for (int k = 1; k < n; k++) {
        if (sum <= 0) {
            out[k - 1] = total * k / n;
            continue;
        }
        const double want = sum * k / n;
        double cum = 0, pos = 1;
        for (int i = 0; i < kBins; i++) {
            const double d = p.ms[i] > 0 ? p.ms[i] : 0;
            if (cum + d >= want && d > 0) {
                pos = (i + (want - cum) / d) / kBins;
                break;
            }
            cum += d;
        }
        out[k - 1] = (int64_t)(pos * (double)total + 0.5);
    }
}

} // namespace mkwvk::segment_plan
