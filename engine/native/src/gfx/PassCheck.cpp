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
    return r;
}

}   // namespace hl::gfx
