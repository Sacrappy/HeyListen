// (HeyListen) HL_UBO_CHECK=1: whether a uniform block's bytes are final when it is bound, and what of the capture an
// inline of the blocks that are would leave out (docs/native-engine/direct-emission.md section 9). A measurement: the
// replay draws as without it.
//
// The bind side: host/nvn_native.c writes, after each BindUniformBuffer's record, a NOP_UBO_CHECK record holding the
// hash of the block's bytes at the bind, at the end of its recording and at the submit. The replay thread's pass over
// the records (the serial replay or the parallel replay's dry pass: each record once) hands it here with whether an
// UpdateUniformBuffer of the stream overlaps the range; the block is read as submitted (memory_read_as_submitted, what
// the mirror sync and the descriptor's copy give the GPU) and compared.
//
// The capture side: the pages each capture takes in a frame (capture_make) against the ranges the frame's records read
// (uniform binds by their result here, vertex and index streams, storage buffers, indirect arguments, copies'
// sources), classified at each present.
#include "vk_internal.h"
#include "vk_state.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_set>

namespace mkwvk {

bool ubo_check_on() {
    static const bool on = [] {
        const char* e = getenv("HL_UBO_CHECK");
        const bool v = e && atoi(e) != 0;
        if (v) logf("vk: uniform blocks checked against what the replay reads as submitted, and the capture's pages by reader (HL_UBO_CHECK=1)");
        return v;
    }();
    return on;
}

namespace {

struct Count {
    int64_t binds = 0, bytes = 0, equal = 0, diff = 0, updated = 0, unread = 0;
    int64_t bind_end = 0, end_sub = 0, sub_replay = 0, unknown = 0;   // (the different: where the bytes changed)
    int64_t again_equal = 0, again_diff = 0;   // (a range bound before in the same command buffer)
};
struct Range { uint64_t lo, hi; uint32_t kind; };

std::mutex mu;
Count total;
Count by_slot[kNvnStages][16];
Count by_size[7];
std::map<uint64_t, Count> by_pool;   // (flags << 8 | pool size class)
int64_t updates_into_bound = 0;
std::unordered_set<uint64_t> bound_in_cmd;   // (addresses bound in the command buffer the pass is in)
std::vector<uint32_t> captured;              // (the frame's: every capture's pages, a page as often as it was taken)
std::vector<Range> refs;                     // (the frame's)
// pages a frame, summed over the frames since the last line and since the start: captured, then by class
enum { kCaptured, kOnlyEqual, kUboDiff, kUboAndOther, kOtherOnly, kTexture, kNothing, kWithVertex, kWithIndex, kWithStorage,
       kWithIndirect, kWithOther, kClasses };
int64_t win[kClasses], all[kClasses];
int64_t win_frames = 0, all_frames = 0;
std::map<uint64_t, int64_t> nothing_by_pool, nothing_by_region;   // (the last 10 s' pages read by no record: pool flags:size, 16 MB region)
std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();

int size_class(uint64_t n) {
    return n <= 256 ? 0 : n <= 1024 ? 1 : n <= 4096 ? 2 : n <= 16384 ? 3 : n <= 65536 ? 4 : n <= 1048576 ? 5 : 6;
}
const char* const size_names[7] = {"<=256", "<=1K", "<=4K", "<=16K", "<=64K", "<=1M", ">1M"};
int pool_class(uint64_t n) {
    return n <= (1ull << 20) ? 0 : n <= (16ull << 20) ? 1 : n <= (64ull << 20) ? 2 : n <= (256ull << 20) ? 3 : 4;
}
const char* const pool_names[5] = {"<=1M", "<=16M", "<=64M", "<=256M", ">256M"};

uint64_t guest_of(uint64_t gpu) {
    return gpu < (1ull << 40) ? gpu : host->gpu_guest(gpu);
}

void add(Count& c, const nvn_p_ubo_check& k, bool updated, bool unread, bool equal, bool again) {
    c.binds++, c.bytes += (int64_t)k.size;
    if (updated) c.updated++;
    else if (unread) c.unread++;
    else if (equal) c.equal++, c.again_equal += again;
    else {
        c.diff++, c.again_diff += again;
        if ((k.flags & NVN_UBO_CHECK_END) && k.h_end != k.h_bind) c.bind_end++;
        else if ((k.flags & NVN_UBO_CHECK_SUB) && (k.flags & NVN_UBO_CHECK_END) && k.h_sub != k.h_end) c.end_sub++;
        else if (k.flags & NVN_UBO_CHECK_SUB) c.sub_replay++;
        else c.unknown++;
    }
}

std::string line_of(const char* name, const Count& c) {
    char t[320];
    snprintf(t, sizeof t, " %s %lld/%lld/%lld", name, (long long)c.binds, (long long)c.equal, (long long)c.diff);
    return t;
}

void log_tables() {
    const Count& c = total;
    logf("hl ubo check: binds %lld (%.1f MB), equal %lld (%.2f%%), different %lld (changed bind->end of recording %lld, "
         "end->submit %lld, after the submit %lld, not known %lld), updated in the stream %lld, unreadable %lld; ranges bound "
         "before in the same command buffer: %lld equal, %lld different; UpdateUniformBuffers into a bound range %lld",
         (long long)c.binds, c.bytes / 1048576.0, (long long)c.equal, c.binds ? 100.0 * c.equal / c.binds : 0.0, (long long)c.diff,
         (long long)c.bind_end, (long long)c.end_sub, (long long)c.sub_replay, (long long)c.unknown, (long long)c.updated,
         (long long)c.unread, (long long)c.again_equal, (long long)c.again_diff, (long long)updates_into_bound);
    std::string s;
    for (int st = 0; st < kNvnStages; st++) {   // (a line a stage: the log's lines have a length limit)
        s.clear();
        for (int i = 0; i < 16; i++)
            if (by_slot[st][i].binds) {
                char n[16];
                snprintf(n, sizeof n, "%d", i);
                s += line_of(n, by_slot[st][i]);
            }
        if (!s.empty()) logf("hl ubo check stage %d by slot (binds/equal/different):%s", st, s.c_str());
    }
    s.clear();
    for (int k = 0; k < 7; k++)
        if (by_size[k].binds) s += line_of(size_names[k], by_size[k]);
    logf("hl ubo check by size (binds/equal/different):%s", s.c_str());
    s.clear();
    for (const auto& [key, v] : by_pool) {
        char n[48];
        if (key == ~0ull) snprintf(n, sizeof n, "none");
        else snprintf(n, sizeof n, "%#llx:%s", (unsigned long long)(key >> 8), pool_names[key & 0xff]);
        s += line_of(n, v);
    }
    logf("hl ubo check by pool flags:size (binds/equal/different):%s", s.c_str());
    const auto pages = [](const char* what, const int64_t* v, int64_t frames) {
        const double f = frames ? (double)frames : 1.0;
        logf("hl ubo check capture %s (%lld frames), pages a frame: captured %.0f (%.1f MB): read only by equal uniform binds %.0f, "
             "by uniform binds with a different one %.0f, by uniform binds and other records %.0f (of them with vertex %.0f, index "
             "%.0f, storage %.0f, indirect %.0f, other %.0f), only by other records %.0f, texture storage not read by a record %.0f, "
             "read by no record %.0f", what, (long long)frames, v[kCaptured] / f, v[kCaptured] * 4096.0 / 1048576.0 / f,
             v[kOnlyEqual] / f, v[kUboDiff] / f, v[kUboAndOther] / f, v[kWithVertex] / f, v[kWithIndex] / f, v[kWithStorage] / f,
             v[kWithIndirect] / f, v[kWithOther] / f, v[kOtherOnly] / f, v[kTexture] / f, v[kNothing] / f);
    };
    pages("the last 10 s", win, win_frames);
    pages("since the start", all, all_frames);
    const double f = win_frames ? (double)win_frames : 1.0;
    s.clear();
    for (const auto& [key, n] : nothing_by_pool) {
        char t[64];
        if (key == ~0ull) snprintf(t, sizeof t, " none:%.0f", n / f);
        else snprintf(t, sizeof t, " %#llx:%s:%.0f", (unsigned long long)(key >> 8), pool_names[key & 0xff], n / f);
        s += t;
    }
    std::vector<std::pair<int64_t, uint64_t>> top;
    for (const auto& [r, n] : nothing_by_region) top.push_back({n, r});
    std::sort(top.rbegin(), top.rend());
    s += "; by 16 MB region:";
    for (size_t k = 0; k < top.size() && k < 12; k++) {
        char t[48];
        snprintf(t, sizeof t, " %#llx:%.0f", (unsigned long long)(top[k].second << 24), top[k].first / f);
        s += t;
    }
    logf("hl ubo check capture the last 10 s, pages a frame read by no record, by pool flags:size:%s", s.c_str());
    nothing_by_pool.clear(), nothing_by_region.clear();
}

}  // namespace

void ubo_check_begin_cmd() {
    std::lock_guard lk(mu);
    bound_in_cmd.clear();
}

void ubo_check_bind(const nvn_p_ubo_check& k, bool updated) {
    const uint64_t n = std::min<uint64_t>(k.size, NVN_UBO_CHECK_MAX);
    static thread_local std::vector<uint8_t> buf;
    buf.resize(NVN_UBO_CHECK_MAX);
    bool unread = (k.flags & NVN_UBO_CHECK_UNREAD) || !n || !memory_read_as_submitted(k.addr, buf.data(), n);
    const bool equal = !unread && nvn_ubo_hash(buf.data(), n) == k.h_bind;
    const uint64_t g = guest_of(k.addr);
    uint64_t lo = 0, hi = 0, key = ~0ull;
    uint32_t flags = 0;
    if (g && memory_nvn_pool_of(g, &lo, &hi, &flags)) key = (uint64_t)flags << 8 | (uint64_t)pool_class(hi - lo);
    std::lock_guard lk(mu);
    const bool again = !bound_in_cmd.insert(k.addr).second;
    add(total, k, updated, unread, equal, again);
    if (k.stage >= 0 && k.stage < kNvnStages && k.index >= 0 && k.index < 16) add(by_slot[k.stage][k.index], k, updated, unread, equal, again);
    add(by_size[size_class(k.size)], k, updated, unread, equal, again);
    add(by_pool[key], k, updated, unread, equal, again);
    if (g && k.size) refs.push_back({g, g + k.size, equal && !updated ? (uint32_t)kRefUboEqual : (uint32_t)kRefUboOther});
}

void ubo_check_update_into_bound() {
    std::lock_guard lk(mu);
    updates_into_bound++;
}

void ubo_check_ref(uint64_t gpu, uint64_t size, uint32_t kind) {
    if (!gpu || !size) return;
    const uint64_t g = guest_of(gpu);
    if (!g) return;
    std::lock_guard lk(mu);
    refs.push_back({g, g + size, kind});
}

void ubo_check_captured(const uint32_t* pages, int64_t n) {
    std::lock_guard lk(mu);
    captured.insert(captured.end(), pages, pages + n);
}

void ubo_check_frame() {
    const std::vector<std::pair<uint64_t, uint64_t>> tex = memory_tex_capture_ranges();
    std::lock_guard lk(mu);
    std::sort(captured.begin(), captured.end());
    // (each distinct page once, with how many captures took it, and what reads it)
    static std::vector<uint32_t> page;
    static std::vector<int64_t> times;
    static std::vector<uint32_t> bits;
    page.clear(), times.clear();
    for (uint32_t p : captured) {
        if (!page.empty() && page.back() == p) times.back()++;
        else page.push_back(p), times.push_back(1);
    }
    bits.assign(page.size(), 0);
    for (const Range& r : refs) {
        const uint64_t p0 = r.lo >> 12, p1 = (r.hi - 1) >> 12;
        for (auto it = std::lower_bound(page.begin(), page.end(), (uint32_t)std::min<uint64_t>(p0, UINT32_MAX)); it != page.end() && *it <= p1; ++it)
            bits[(size_t)(it - page.begin())] |= r.kind;
    }
    int64_t v[kClasses] = {};
    const uint32_t ubo = kRefUboEqual | kRefUboOther;
    for (size_t i = 0; i < page.size(); i++) {
        const uint32_t b = bits[i];
        const int64_t t = times[i];
        v[kCaptured] += t;
        if (b == kRefUboEqual) v[kOnlyEqual] += t;
        else if ((b & ubo) && !(b & ~ubo)) v[kUboDiff] += t;
        else if (b & ubo) {
            v[kUboAndOther] += t;
            if (b & kRefVertex) v[kWithVertex] += t;
            if (b & kRefIndex) v[kWithIndex] += t;
            if (b & kRefStorage) v[kWithStorage] += t;
            if (b & kRefIndirect) v[kWithIndirect] += t;
            if (b & kRefOther) v[kWithOther] += t;
        } else if (b) v[kOtherOnly] += t;
        else {
            const uint64_t ga = (uint64_t)page[i] << 12;
            auto it = std::upper_bound(tex.begin(), tex.end(), std::make_pair(ga, ~uint64_t(0)));
            if (it != tex.begin() && ga < std::prev(it)->second) {
                v[kTexture] += t;
                continue;
            }
            v[kNothing] += t;
            static uint64_t plo = 1, phi = 0, pkey = ~0ull;   // (the last pool found: pages come in address order)
            if (ga < plo || ga >= phi) {
                uint32_t flags = 0;
                if (memory_nvn_pool_of(ga, &plo, &phi, &flags)) pkey = (uint64_t)flags << 8 | (uint64_t)pool_class(phi - plo);
                else plo = ga, phi = ga + 4096, pkey = ~0ull;
            }
            nothing_by_pool[pkey] += t;
            nothing_by_region[ga >> 24] += t;
        }
    }
    for (int k = 0; k < kClasses; k++) win[k] += v[k], all[k] += v[k];
    win_frames++, all_frames++;
    captured.clear(), refs.clear();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds(10)) {
        last_log = now;
        log_tables();
        std::fill(std::begin(win), std::end(win), 0);
        win_frames = 0;
    }
}

}  // namespace mkwvk
