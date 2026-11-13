// HeyListen native engine: the recording pass's check (hl/gfx/PassCheck.h).
#include "hl/gfx/PassCheck.h"

namespace hl::gfx {

namespace {
constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull, kFnvPrime = 0x100000001b3ull;

uint64_t mix(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) h = (h ^ ((v >> (8 * i)) & 0xff)) * kFnvPrime;
    return h;
}
}   // namespace

uint64_t PassRecording::shape() const {
    uint64_t h = mix(kFnvBasis, calls_.size());
    for (const Command& c : calls_) h = mix(h, c.op);
    return h;
}

PassPath PassCheck::next_path() {
    std::lock_guard<std::mutex> g(lock_);
    // (splitmix64: a fixed sequence, so two runs choose alike, with no pattern a frame's order of passes could follow)
    uint64_t z = (draw_ += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return ((z ^ (z >> 31)) & 1) ? PassPath::Native : PassPath::Translated;
}

void PassCheck::add(PassPath path, uint32_t mode, const PassRecording& recording) {
    const uint64_t key = mix(recording.shape(), mode);
    const uint64_t n = recording.calls().size();
    std::lock_guard<std::mutex> g(lock_);
    PathStats& s = stats_[(int)path];
    s.passes++;
    s.calls += n;
    if (n > s.most_calls) s.most_calls = n;
    by_mode_[mode < 2 ? mode : 2]++;
    seen_[(int)path][key]++;
    std::vector<uint64_t>& ops = op_calls_[(int)path];
    for (const Command& c : recording.calls()) {
        if (c.op >= ops.size()) ops.resize(c.op + 1);
        ops[c.op]++;
    }
}

PassCheck::Report PassCheck::report() const {
    std::lock_guard<std::mutex> g(lock_);
    Report r;
    for (int p = 0; p < 2; p++) {
        r.path[p] = stats_[p];
        r.path[p].shapes = seen_[p].size();
        for (const auto& [key, passes] : seen_[p]) {
            if (seen_[1 - p].count(key)) continue;
            r.path[p].unmatched += passes;
            r.path[p].unmatched_shapes++;
        }
    }
    for (int m = 0; m < 3; m++) r.by_mode[m] = by_mode_[m];
    if (stats_[0].passes && stats_[1].passes) {
        const std::vector<uint64_t>&t = op_calls_[0], &n = op_calls_[1];
        for (size_t op = 0; op < t.size(); op++) {
            const double rt = (double)t[op] / (double)stats_[0].passes;
            if (rt < 1.0) continue;
            const double rn = (double)(op < n.size() ? n[op] : 0) / (double)stats_[1].passes;
            const double d = rn > rt ? (rn - rt) / rt : (rt - rn) / rt;
            r.ops_compared++;
            if (d >= r.most_op_difference) r.most_op_difference = d, r.most_op = (uint32_t)op;
        }
    }
    return r;
}

}   // namespace hl::gfx
