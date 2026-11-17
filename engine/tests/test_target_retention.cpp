// The eviction pass's own choice of which render-target textures go (gpu/vk/vk_texture.cpp, texture_frame, from `order` to the end of
// its loop) and the rule that a finalized texture whose storage the game wrote over is obsolete (finalized_obsolete), both compiled as they
// stand in the source and run over made-up textures: tests/test_target_retention.py cuts that text into CUT and OBSOLETE and builds this
// file around them. No game, no GPU.
//
// What it runs: laps of a route. Each lap makes a set of render targets the size of the terrain tiles (260 x 260 RG8 with nine
// levels: 176 KB of texels, 325 KB as the allocator gives it), the GPU draws each one once, the game finalizes it and never uses it again, and no
// later target is drawn over its storage. The game then reuses some lap's storage: the CPU writes over it. Which of them does the pass let
// go of, with the textures within the budget (a sweep: nothing is over it) and over it?
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../gpu/vk/keep_rendered.h"
#include "../gpu/vk/target_overlap.h"

namespace mkwvk {
struct Texture {   // (the fields the pass and finalized_obsolete read)
    uint64_t gpu = 0, guest = 0, guest_size = 0, last_frame = 0, last_written = 0, first_written = 0, first_render_frame = 0, last_value = 0;
    uint64_t bytes = 0, pad = 0;
    uint32_t levels = 1, layers = 1, depth = 1, width = 0, height = 0, id = 0, release_gen = 0;
    int nvn_format = 0;
    bool gpu_written = false, released = false;
    bool flush_ok = true;   // (flushable(): its texels can be written back)
};
using Ranges = std::vector<std::pair<uint64_t, uint64_t>>;
uint64_t frame_no = 1, total_bytes = 0;
std::map<uint64_t, std::unique_ptr<Texture>> textures;
std::unordered_multimap<uint64_t, Texture*> by_gpu;
std::map<uint64_t, std::pair<uint64_t, uint32_t>> gpu_owned;
std::unordered_map<uint64_t, uint64_t> written_since_release;   // (guest address -> how many bytes of its storage the CPU has written)
bool flushable(const Texture& t) { return t.flush_ok; }
/// (what the CPU wrote of [guest, guest + size) after generation gen: the first `n` bytes of it, as one run, for the textures the run says)
void memory_written_runs(uint64_t guest, uint64_t size, uint32_t gen, Ranges& out) {
    (void)size, (void)gen;
    const auto it = written_since_release.find(guest);
    if (it != written_since_release.end() && it->second) out.push_back({0, it->second});
}
uint64_t dead_min_unused_presents() { return 1; }
void logf(const char*, ...) {}

#include OBSOLETE   // (finalized_obsolete, as the source has it)

/// The pass's choice at one present (the text cut from texture_frame, with the locals it reads before it)
std::vector<Texture*> choose(uint64_t completed, bool sweep, uint64_t budget) {
    const bool known = true;
    const uint64_t used = 1ull << 30, allowed = 8ull << 30;   // (far from the driver's limit: keep_rendered's 3600 presents)
    const bool dead_targets = false, full_pass = true;   // (the transient-target sweep is off: nothing here is drawn over)
    const uint64_t overwritten_unused = 8;   // (its default)
#include CUT
    std::vector<Texture*> out;
    for (const Evict& e : evict) out.push_back(e.tex);
    return out;
}
}  // namespace mkwvk

using namespace mkwvk;

static int fails;
#define CHECK(name, cond) do { if (!(cond)) { std::printf("FAIL: %s (line %d)\n", name, __LINE__); fails++; } } while (0)

constexpr uint64_t kTexels = 176 * 1024, kPad = 149 * 1024;   // (a tile's texels with their levels; the diagnostic's line has the allocator give each 325 KB)
constexpr int kPerLap = 60, kLapFrames = 300;

static void reset() {
    textures.clear(), by_gpu.clear(), gpu_owned.clear(), written_since_release.clear();
    frame_no = 1, total_bytes = 0;
}
static Texture* make_tile(uint64_t gpu, uint64_t bytes, uint64_t pad) {   // (drawn this present, once)
    static uint32_t ids;
    auto t = std::make_unique<Texture>();
    t->id = ++ids, t->gpu = t->guest = gpu, t->guest_size = t->bytes = bytes, t->pad = pad, t->width = t->height = 260, t->levels = 9;
    t->nvn_format = 0xd, t->gpu_written = true;
    t->first_render_frame = t->first_written = t->last_written = t->last_frame = frame_no;
    Texture* p = t.get();
    textures[t->id] = std::move(t);
    by_gpu.emplace(gpu, p);
    gpu_owned[gpu] = {gpu + bytes, 1};
    total_bytes += bytes + pad;
    return p;
}
static void apply(const std::vector<Texture*>& gone) {   // (destroy_later, forget_gpu and the erase of the three sites)
    for (Texture* t : gone) {
        total_bytes -= std::min(total_bytes, t->bytes + t->pad);
        auto r = by_gpu.equal_range(t->gpu);
        for (auto it = r.first; it != r.second; ++it)
            if (it->second == t) {
                by_gpu.erase(it);
                break;
            }
        written_since_release.erase(t->guest);
        textures.erase(t->id);
    }
}

/// `laps` laps of kPerLap tiles each at storage no lap uses again. `reused_after`: laps after which the game writes over a lap's
/// storage (0: never), `written` of the bytes of each (a quarter, three quarters, all)
static size_t run(int laps, uint64_t budget, int reused_after, double written = 1.0, uint64_t texels = kTexels, uint64_t pad = kPad) {
    reset();
    uint64_t next = 0x10000000;
    std::vector<std::vector<uint64_t>> lap_ids;   // (ids, not pointers: the pass frees the textures)
    for (int lap = 0; lap < laps; lap++) {
        std::vector<uint64_t> mine;
        for (int i = 0; i < kPerLap; i++, next += 0x80000) mine.push_back(make_tile(next, texels, pad)->id);
        lap_ids.push_back(mine);
        for (int f = 0; f < kLapFrames; f++) {
            frame_no++;
            for (uint64_t id : mine) {
                const auto it = textures.find(id);
                if (it != textures.end() && frame_no == it->second->first_written + 3) it->second->released = true, it->second->release_gen = 1;
            }
            if (reused_after && lap >= reused_after && f == 0)   // (the game writes over the storage of a lap long gone)
                for (uint64_t id : lap_ids[lap - reused_after]) {
                    const auto it = textures.find(id);
                    if (it != textures.end()) written_since_release[it->second->guest] = (uint64_t)(written * (double)it->second->guest_size);
                }
            apply(choose(frame_no, total_bytes <= budget, budget));
        }
    }
    size_t alive = 0;
    for (const auto& l : lap_ids)
        for (uint64_t id : l) alive += textures.count(id);
    return alive;
}

int main() {
    const uint64_t big = 64ull << 30;   // (a budget nothing comes near: every present is a sweep)
    const int laps = 30;

    // (1) the game writes over the storage of a lap two laps on: what the image held is gone from that memory, on the console too
    for (const uint64_t budget : std::initializer_list<uint64_t>{big, 1ull << 20}) {
        const size_t alive = run(laps, budget, 2);
        std::printf("storage written over two laps on, %s the budget: %zu of %d tiles alive after %d laps\n", budget == big ? "within" : "over",
                    alive, laps * kPerLap, laps);
        CHECK("tiles whose storage the game has written over go", alive <= 3 * (size_t)kPerLap);
    }
    // (2) not when the game wrote over a part of it only (the memory still holds the rest of what the image does), nor over a small
    // texture's (its pages are shared with what stands beside it)
    CHECK("a quarter of the storage written is not enough", run(10, big, 2, 0.25) == 10 * kPerLap);
    CHECK("half of it is not either", run(10, big, 2, 0.5) == 10 * kPerLap);
    CHECK("three quarters is", run(10, big, 2, 0.75) <= 3 * (size_t)kPerLap);
    CHECK("a texture of 16 KB is never judged by its pages", run(10, big, 2, 1.0, 16 * 1024, 48 * 1024) == 10 * kPerLap);

    if (!fails) std::puts("PASS: a finalized render target goes once the game has written over its storage");
    return fails ? 1 : 0;
}
