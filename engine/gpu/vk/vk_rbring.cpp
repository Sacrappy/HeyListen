// HL_RB_LAG (STEP1, docs/native-engine/readback-ring.md): one deterministic mechanism for the three GPU-result classes the game's CPU reads
// (R1 the exposure meter, R2 the 0x40 slots, R3 the 0x2000 pair). The ranges the CPU reads are learned from its reads (invalidate); a
// program's binding written to such a range (gpu_buffer_bind, in the mirror) is not written back to guest memory any more: the end of
// the submission copies the range's bytes into an entry of an engine-owned ring, tagged with the frame count, and the game's next read
// of it applies to guest memory the newest unapplied entry that is `lag` frames old, without a wait for the GPU (rb_ring.h has the rules).
// HL_RB_LAG_CHECK=1: the old path runs unchanged and what the ring would have applied is compared with what the game read.
#include <algorithm>
#include <thread>   // (FLICK1: the meter pace's sleep)
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include "vk_internal.h"
#include "rb_ring.h"

namespace mkwvk {
extern int64_t presents_now;   // (vk_replay.cpp)
bool rbring_wait_value(uint64_t v, double ms);   // (vk_replay.cpp)
namespace {

// HL_RB_LAG=<L> (0 or unset: off): the ranges the CPU reads back (the exposure meter's block, the 0x40 slots, the 0x2000 blocks) are staged into an engine-owned ring when the GPU has written them, and a read is answered with the newest result L frames old (1..6; 2 is the default lag to use) with no wait for the GPU and no write-back of those ranges; the same values run to run. HL_RB_LAG_CHECK=1: the old path runs and the ring is compared with what it read (per-class counts in the log). HL_RB_LAG_WAIT_MS=<ms> (200): how long a read waits for an entry the GPU has not finished. HL_RB_LAG_LOG=1: one log row a frame with the hash of the values served.
const rb_ring::Config& config() {
    static const rb_ring::Config c = [] {
        rb_ring::Config k = rb_ring::parse(getenv("HL_RB_LAG"), getenv("HL_RB_LAG_CHECK"), getenv("HL_RB_LAG_WAIT_MS"));
        if (!k.lag && k.check) k.lag = 2;   // (a check alone compares the lag 2 ring)
        // (FIF1) with the CPU k-1 frames ahead of the replay (RX_VK_FRAMES_IN_FLIGHT=k) an entry is staged k-1 frames later: lag 2 + (k-1)
        // keeps every read free of a wait for the replay; HL_RB_LAG says otherwise
        const char* fk = getenv("RX_VK_FRAMES_IN_FLIGHT");
        if (k.lag && !(getenv("HL_RB_LAG") && *getenv("HL_RB_LAG")) && fk && atoi(fk) > 1)
            k.lag = std::min<unsigned>(rb_ring::kMaxLag, 2u + (unsigned)std::min(atoi(fk), 3) - 1u);
        // (PIPE2) RX_VK_ORDER_THREAD=1 (on by default where the host keeps a core for the replay, hos_main.c; vk_replay.cpp): the replay on an order thread, the submitting threads return
        // after the capture, a frame behind the game's CPU as frames in flight are: the ring serves at lag 3
        const char* ot = getenv("RX_VK_ORDER_THREAD");
        if (k.lag && !(getenv("HL_RB_LAG") && *getenv("HL_RB_LAG")) && ot && atoi(ot) != 0) k.lag = std::max(k.lag, std::min<unsigned>(rb_ring::kMaxLag, 3u));
        return k;
    }();
    return c;
}
bool frame_log_on() {
    static const bool on = getenv("HL_RB_LAG_LOG") && atoi(getenv("HL_RB_LAG_LOG")) != 0;
    return on;
}

// (the standing, rb_ring.h: stageable (a binding written in the mirror was staged), tainted (a written binding that is not exactly this
// range overlaps it: its write-back would land in it, the old path), writes (written bindings of exactly this range) and staged (those of
// them staged; the rest went to guest memory with a write-back), first_stage, the CPU's reads, settle)
struct Addr : rb_ring::Standing {
    int cls = 0;
    rb_ring::Entry ents[rb_ring::kMaxEntries];
    unsigned n = 0;
    uint64_t applied = 0;   // the newest seq handed to the game
    // (EXPO1, HL_METER_LOG: what the last staged binding was, and how many were staged in the last whole frame)
    VkBuffer last_buffer = VK_NULL_HANDLE;
    VkDeviceSize last_offset = 0;
    int64_t stage_frame = -1;
    unsigned in_frame = 0, last_frame_stages = 0;
};
struct Req {
    uint64_t guest, size;
    VkBuffer buffer;
    VkDeviceSize offset;
};

std::mutex mu;
std::map<std::pair<uint64_t, uint64_t>, Addr> table;   // (guest, size)
VkBuffer ring_buffer = VK_NULL_HANDLE;
uint8_t* ring_map = nullptr;
bool ring_failed = false;
uint64_t arena_at = 0, arena_total = 0, next_seq = 0;

struct Stats {
    std::atomic<int64_t> reads{0}, applied{0}, none{0}, waited{0}, late{0}, again{0};   // (again: PIPE2, the meter's block handed the last value back)
    std::atomic<int64_t> chk_new{0}, chk_equal{0}, chk_differ{0}, chk_zero{0}, chk_nonzero{0}, chk_notdone{0}, chk_old{0};
    std::atomic<int64_t> age[4] = {};
    std::atomic<int64_t> got[5] = {};   // (serve mode) the age in frames of the entries handed out: 1, 2, 3, 4, more
} stats[5];
std::atomic<int64_t> st_staged{0}, st_staged_bytes{0}, st_direct{0};
std::atomic<int64_t> st_kept_wb{0}, st_retaken{0};   // (EXPO1: written bindings of a tainted range given their write-back; ranges taken over again)
std::atomic<uint64_t> learned_lo{~0ull}, learned_hi{0};   // (the span of the learned ranges: a written binding outside it is not looked up)
constexpr unsigned kQuietReads = rb_ring::kQuietReads;
// A range is served by the ring when its writes are exactly its staged ones, nothing else's write-back lands in it, and either a staged entry
// exists or no program has written it at all (rb_ring::served). Without `done`: the settle value is not looked at (logs, the check's quiet ranges)
bool served(const Addr& a, uint64_t done = ~0ull) { return rb_ring::served(a, done); }

bool ring_init() {   // (mu held)
    if (ring_map) return true;
    if (ring_failed) return false;
    ring_failed = true;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = rb_ring::kArena;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)   // (system memory first: the host reads it every frame)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits >> i & 1) && (f & want) == want && (pass || !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) type = i;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory{};
    void* words = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(dev.device, &ai, nullptr, &memory) != VK_SUCCESS ||
        vkBindBufferMemory(dev.device, buffer, memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev.device, memory, 0, rb_ring::kArena, 0, &words) != VK_SUCCESS) {
        logf("vk: HL_RB_LAG: no host memory for the readback ring: the old path stays");
        vkDestroyBuffer(dev.device, buffer, nullptr);
        return false;
    }
    std::memset(words, 0, rb_ring::kArena);
    ring_buffer = buffer;
    ring_map = (uint8_t*)words;
    ring_failed = false;
    logf("vk: HL_RB_LAG=%u%s: the exposure meter, the 0x40 slots and the 0x2000 blocks the CPU reads come from a ring of %u KB, %u frames old, "
         "with no wait for the GPU (docs/native-engine/readback-ring.md)", config().lag, config().check ? " (check: the old path runs)" : "",
         (unsigned)(rb_ring::kArena >> 10), config().lag);
    return true;
}

// ---- the served-values hash, one row a frame (HL_RB_LAG_LOG=1) ----
std::mutex hmu;
struct FrameHash { uint64_t sum = 0, by[5] = {}; uint32_t reads = 0; };
std::map<int64_t, FrameHash> frame_hash;   // frame -> sum of the reads' hashes (and by class), reads
uint64_t run_hash = 0;
void hash_add(int64_t frame, int cls, uint64_t h) {
    if (!frame_log_on()) return;
    std::lock_guard lk(hmu);
    auto& e = frame_hash[frame];
    e.sum += h, e.by[cls] += h, e.reads++;
    for (auto it = frame_hash.begin(); it != frame_hash.end() && it->first < frame - 3;) {
        run_hash = (run_hash ^ it->second.sum) * 1099511628211ull + (uint64_t)it->first;
        logf("vk: rbring frame %lld hash %016llx reads %u run %016llx r1 %016llx r2 %016llx r3 %016llx", (long long)it->first,
             (unsigned long long)it->second.sum, it->second.reads, (unsigned long long)run_hash, (unsigned long long)it->second.by[1],
             (unsigned long long)it->second.by[2], (unsigned long long)it->second.by[3]);
        it = frame_hash.erase(it);
    }
}

// (FLASH2) RX_VK_RB_R4=<0|1> (0): the 0x3980 culling results the CPU reads (R4) are served by the ring too, at its fixed lag (3 frames old made the plants pop: off); 0: read as the replay left them, after the order thread's precise wait (RX_VK_ORDER_RB_JOIN=5)
bool r4_on() {
    static const bool on = getenv("RX_VK_RB_R4") && atoi(getenv("RX_VK_RB_R4")) != 0;
    return on;
}
const char* cname(int cls) { return cls == 1 ? "R1" : cls == 2 ? "R2" : cls == 3 ? "R3" : "R4"; }

}   // namespace

bool rbring_on() { return config().lag != 0; }
bool rbring_check_on() { return config().lag != 0 && config().check; }

/// The CPU reads (guest, size): the range is one to stage from now on
void rbring_note_read(uint64_t guest, uint64_t size) {
    if (!config().lag || !guest) return;
    if (!rb_ring::class_of(size)) return;
    const int cls = rb_ring::class_of(size);
    if (!cls) return;
    if (cls == 4 && !r4_on()) return;
    // (EXPO3, HL_RB_CLASSES=<mask> (15: all): bit 0 the meter's blocks R1, bit 1 the 0x40 slots R2, bit 2 the 0x2000 blocks R3; a class left out is
    // never learned, so its reads take the old path (the waits, the write-backs): an A/B to find which class steps the picture at the 60 limit)
    static const unsigned classes = getenv("HL_RB_CLASSES") ? (unsigned)strtoul(getenv("HL_RB_CLASSES"), nullptr, 0) : 15u;
    if (!(classes >> (cls - 1) & 1)) return;
    std::lock_guard lk(mu);
    if (table.size() > 4096) {
        if (meter_log_rows() > 0) logf("vk: rbring: the table of learned ranges cleared (4096 ranges) at frame %lld", (long long)rb_frame_now());
        table.clear(), learned_lo = ~0ull, learned_hi = 0;
    }
    Addr& a = table[{guest, size}];
    a.cls = cls, a.reads++;
    {   // (HL_RB_LAG_LOG=2: the learned ranges that are not served, every 6000 reads)
        static const bool dbg = getenv("HL_RB_LAG_LOG") && atoi(getenv("HL_RB_LAG_LOG")) >= 2;
        static uint64_t n = 0;
        if (dbg && ++n % 6000 == 0) {
            int rows = 0;
            for (const auto& [k, v] : table)
                if (v.reads >= kQuietReads && !served(v) && rows++ < 12)
                    logf("vk: rbring not served: %#llx +%#llx R%d reads %llu stageable %d tainted %d writes %llu staged %llu", (unsigned long long)k.first,
                         (unsigned long long)k.second, v.cls, (unsigned long long)v.reads, v.stageable, v.tainted, (unsigned long long)v.writes,
                         (unsigned long long)v.staged);
        }
    }
    if (learned_lo > guest) learned_lo = guest;
    if (learned_hi < guest + size) learned_hi = guest + size;
    if (cls == 1 && meter_log_rows() > 0) {   // (EXPO1, HL_METER_LOG: every R1 range the ring knows, every 3000 R1 reads)
        static uint64_t r1 = 0;
        if (++r1 % 3000 == 0)
            for (const auto& [k, v] : table)
                if (v.cls == 1) {
                    const int nw = rb_ring::newest(v.ents, v.n);
                    uint32_t w[4] = {};
                    if (nw >= 0 && ring_map && rb_ring::intact(v.ents[nw], arena_total, 16)) std::memcpy(w, ring_map + v.ents[nw].at, 16);
                    logf("vk: rbring R1 range %#llx +%#llx: served %d writes %llu staged %llu reads %llu entries %u, newest #%lld t%lld %08x %08x %08x %08x",
                         (unsigned long long)k.first, (unsigned long long)k.second, served(v), (unsigned long long)v.writes,
                         (unsigned long long)v.staged, (unsigned long long)v.reads, v.n, nw >= 0 ? (long long)v.ents[nw].seq : -1ll,
                         nw >= 0 ? (long long)v.ents[nw].tag : -1ll, w[0], w[1], w[2], w[3]);
                }
    }
}

/// Every binding for writing (a mirror's or guest memory's, gpu_buffer_bind): the learned ranges it overlaps are marked, an exact one counted
void rbring_note_written(uint64_t guest, uint64_t size) {
    if (!config().lag || guest >= learned_hi.load(std::memory_order_relaxed) || guest + size <= learned_lo.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(mu);
    for (auto it = table.lower_bound({guest > 0x2000 ? guest - 0x2000 : 0, 0}); it != table.end() && it->first.first < guest + size; ++it) {
        const uint64_t lo = it->first.first, hi = lo + it->first.second;
        if (hi <= guest) continue;
        if (lo == guest && hi == guest + size) it->second.writes++;
        else it->second.tainted = true;
    }
}

/// A program's binding for writing in a mirror (pools_mu held): true when the range is staged, so no write-back is made for it
bool rbring_stage_bind(uint64_t guest, uint64_t size, VkBuffer buffer, VkDeviceSize offset, RbPending& pend) {
    if (!config().lag || !rb_ring::class_of(size) || !buffer) return false;
    std::lock_guard lk(mu);
    const auto it = table.find({guest, size});
    if (it == table.end()) return false;
    if (!ring_init() || pend.bytes + rb_ring::copy_bytes(it->second.cls, size) > rb_ring::kMaxPending) return false;
    // (this binding was counted by rbring_note_written just before) (EXPO1) a tainted range keeps its write-backs: its reads take the old
    // path; one whose writes before this went to guest memory is taken over again from this one, served once the GPU is past it (rb_ring.h)
    const bool was = it->second.stageable;
    const uint64_t settle0 = it->second.settle;
    // HL_RB_LAG_KEEP=0: as before 2026-11-23 (A/B): every learned range's binding is staged, served or not, with no settle (a range moved to the old path by one direct write never gets its write-backs again: the exposure froze); 1 (the default): a tainted range keeps its write-backs, a range taken over again is answered once the GPU is past it
    static const bool keep = !getenv("HL_RB_LAG_KEEP") || atoi(getenv("HL_RB_LAG_KEEP")) != 0;
    if (!keep) {
        if (!it->second.stageable) it->second.first_stage = it->second.writes ? it->second.writes - 1 : 0;
        it->second.staged++, it->second.stageable = true;
    } else if (!rb_ring::stage(it->second)) {
        st_kept_wb++;
        return false;
    }
    if (it->second.settle == rb_ring::kUnsettled && settle0 != rb_ring::kUnsettled) {
        // (its entries from before went to the game's reads or are older than what the old path gave it meanwhile: none is handed out)
        if (const int nw = rb_ring::newest(it->second.ents, it->second.n); nw >= 0)
            it->second.applied = std::max(it->second.applied, it->second.ents[nw].seq);
        st_retaken++;
        if (meter_log_rows() > 0 || st_retaken.load() % 200 == 1)
            logf("vk: rbring: %#llx +%#llx taken over again by the ring (%lld so far): %llu of its %llu written bindings went to guest memory (%s)",
                 (unsigned long long)guest, (unsigned long long)size, (long long)st_retaken.load(),
                 (unsigned long long)(it->second.writes - it->second.staged), (unsigned long long)it->second.writes,
                 was ? "since the last staged one" : "before the first staged one");
    }
    {   // (HL_METER_LOG's description)
        Addr& a = it->second;
        const int64_t f = rb_frame_now();
        if (f != a.stage_frame) a.last_frame_stages = a.in_frame, a.in_frame = 0, a.stage_frame = f;
        a.in_frame++, a.last_buffer = buffer, a.last_offset = offset;
    }
    for (RbPending::Req& r : pend.reqs)
        if (r.guest == guest && r.size == size) {   // (written twice in a submission: the last binding's state is what the end copies)
            r.buffer = buffer, r.offset = offset;
            return !config().check;
        }
    pend.reqs.push_back({guest, size, buffer, offset});
    pend.bytes += rb_ring::copy_bytes(it->second.cls, size);
    return !config().check;
}

/// The end of a submission (pools_mu held): what the submission's programs wrote to the staged ranges, copied into the ring
void rbring_flush(VkCommandBuffer cmd, uint64_t value, RbPending& pend) {
    std::vector<RbPending::Req>& pending = pend.reqs;
    if (pending.empty()) return;   // (read without mu: written under pools_mu, as this call is)
    std::lock_guard lk(mu);
    if (pending.empty() || !ring_map) {
        pending.clear(), pend.bytes = 0;
        return;
    }
    const int64_t tag = rb_tag_for(value);
    auto barrier = [&](VkPipelineStageFlags2 s, VkAccessFlags2 sa, VkPipelineStageFlags2 d, VkAccessFlags2 da) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = s, b.srcAccessMask = sa, b.dstStageMask = d, b.dstAccessMask = da;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    std::stable_sort(pending.begin(), pending.end(), [](const RbPending::Req& a, const RbPending::Req& b) { return a.buffer < b.buffer; });
    std::vector<VkBufferCopy> regions;
    for (size_t i = 0; i < pending.size();) {
        const VkBuffer buffer = pending[i].buffer;
        regions.clear();
        for (; i < pending.size() && pending[i].buffer == buffer; i++) {
            const RbPending::Req& r = pending[i];
            Addr& a = table[{r.guest, r.size}];
            const uint64_t n = rb_ring::copy_bytes(a.cls, r.size);
            rb_ring::Entry e;
            e.seq = ++next_seq, e.tag = tag, e.value = value;
            e.cum = arena_total;
            e.at = rb_ring::allocate(arena_at, arena_total, n);
            rb_ring::push(a.ents, a.n, e);
            rb_ring::settle_at(a, value);   // (EXPO1: a range taken over again is answered once the GPU is past this submission)
            regions.push_back({r.offset, e.at, n});
            st_staged++, st_staged_bytes += (int64_t)n;
        }
        vkCmdCopyBuffer(cmd, buffer, ring_buffer, (uint32_t)regions.size(), regions.data());
    }
    barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    pending.clear(), pend.bytes = 0;
}

/// HL_RB_LAG_CHECK=1: a read of a range no program writes (answered with no wait in lag mode) before the check's entry: would the waits have changed it?
bool rbring_quiet(uint64_t guest, uint64_t size) {
    if (!config().lag || !config().check || !rb_ring::class_of(size)) return false;
    std::lock_guard lk(mu);
    const auto it = table.find({guest, size});
    return it != table.end() && !it->second.stageable && served(it->second);
}
void rbring_quiet_result(uint64_t size, bool changed) {
    static std::atomic<int64_t> reads{0}, changes{0};
    if (changed) changes++;
    if (++reads % 3000 == 0)
        logf("vk: rbring check quiet ranges (read 0x%llx bytes; no program writes them): 3000 reads, %lld of them changed by the old path's waits",
             (unsigned long long)size, (long long)changes.exchange(0));
}

/// True: this read is answered by rbring_apply and waits for nothing (lag mode, not the check, a range whose bindings are staged)
bool rbring_serves(uint64_t guest, uint64_t size) {
    if (!config().lag || config().check) return false;
    if (!rb_ring::class_of(size)) return false;
    uint64_t settle = 0;
    {
        std::lock_guard lk(mu);
        const auto it = table.find({guest, size});
        if (it == table.end() || !served(it->second)) return false;
        if (!(settle = it->second.settle)) return true;
    }
    // (EXPO1: a range taken over again: served once the GPU is past the submission that staged it, so no write-back of the bindings before
    // lands after an answer. The GPU's progress read outside the lock: completed() applies staged write-backs)
    if (settle == rb_ring::kUnsettled || settle > vk_timeline_completed()) return false;
    std::lock_guard lk(mu);
    const auto it = table.find({guest, size});
    if (it == table.end()) return false;
    if (it->second.settle == settle) it->second.settle = 0;
    return served(it->second, settle);   // (a settle set again meanwhile is above the value reached)
}

namespace {
thread_local RbApplyNote tl_apply_note;   // (FLASH1, HL_METER_TRACE: what this thread's last rbring_apply did)
}
RbApplyNote rbring_take_note() {
    const RbApplyNote n = tl_apply_note;
    tl_apply_note = RbApplyNote{};
    return n;
}

// ---- (FLICK1) the exposure meter's reads before the frame's present ----
// The game's module job reads the meter's three blocks (0x1040) once a frame and turns them into the frame's exposure and light
// parameters. With the order thread the game's present is not held by the replay, and when that job runs late (a hitch, a wait of
// its own) the present is handed over while the job is still reading: the next frame is shown with the whole 3D picture brighter
// (the HUD is not), the one-frame flash. Proven with HL_METER_DELAY_EVERY=60 HL_METER_DELAY_MS=12 (the frame's first meter read held
// 12 ms every 60th frame, Kakariko standing still, 180 s): 131 delays, 132 strict flashes, each one frame after a delay; with this
// wait: 132 delays, 1 flash (before it had learned the frame's reads). Here the hand-over waits, 20 ms at most, for as many meter
// reads in the frame as the frames before had made by their own hand-over (the least of the last 16): a frame whose job is on time
// waits for nothing (one wait in about 3600 presents, about 2 ms, in Kakariko). With the order thread only (without it the replay
// holds the present). RX_VK_ORDER_METER_PACE_MS=<ms> (20; 0 off).
std::atomic<int64_t> mp_c{-1}, mp_n{0};
void meter_read_note(int64_t c) {
    int64_t cur = mp_c.load(std::memory_order_acquire);
    if (c > cur) {
        if (mp_c.compare_exchange_strong(cur, c, std::memory_order_acq_rel)) {
            mp_n.store(1, std::memory_order_release);
            return;
        }
    }
    if (mp_c.load(std::memory_order_acquire) == c) mp_n.fetch_add(1, std::memory_order_acq_rel);
}
void rbring_meter_pace(int64_t clock) {
    static const int ms = getenv("RX_VK_ORDER_METER_PACE_MS") ? atoi(getenv("RX_VK_ORDER_METER_PACE_MS")) : 20;
    if (ms <= 0) return;
    static int64_t hist[16], hi = 0, waits = 0, timeouts = 0, wait_ns = 0, ticks = 0;
    auto seen = [&] { return mp_c.load(std::memory_order_acquire) == clock ? mp_n.load(std::memory_order_acquire) : 0; };
    int64_t expect = INT64_MAX;
    for (int i = 0; i < 16; i++) expect = std::min(expect, hist[i]);
    if (hi < 16) expect = 0;   // (learning)
    int64_t n = seen();
    if (expect > 0 && n < expect) {
        const auto t0 = std::chrono::steady_clock::now();
        waits++;
        while ((n = seen()) < expect) {
            if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(ms)) {
                timeouts++;
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        wait_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    }
    hist[hi++ % 16] = n;
    if (++ticks % 3600 == 0) {
        logf("vk: meter pace (FLICK1, 3600 presents): %lld waited for the frame's meter reads (%.3f ms each), %lld gave up after %d ms; reads by a "
             "present now %lld", (long long)waits, waits ? wait_ns / 1e6 / waits : 0.0, (long long)timeouts, ms, (long long)expect);
        waits = timeouts = wait_ns = 0;
    }
}
/// The read of a served range: the newest unapplied entry `lag` frames old into guest memory
void rbring_apply(uint64_t gpu, uint64_t size) {
    const uint64_t guest = host->gpu_guest(gpu);
    const int64_t c = rb_frame_now();
    if (size == 0x1040) {   // (FLICK1, a test: HL_METER_DELAY_EVERY=<frames> HL_METER_DELAY_MS=<ms> (10): the frame's first meter read held that long, every n-th frame)
        static const int64_t every = getenv("HL_METER_DELAY_EVERY") ? atoll(getenv("HL_METER_DELAY_EVERY")) : 0;
        static const int dms = getenv("HL_METER_DELAY_MS") ? atoi(getenv("HL_METER_DELAY_MS")) : 10;
        static std::atomic<int64_t> last{-1};
        if (every > 0 && c % every == 0 && last.exchange(c) != c) {
            logf("vk: meter delay at f %lld (present %lld): %d ms", (long long)c, (long long)presents_now, dms);
            std::this_thread::sleep_for(std::chrono::milliseconds(dms));
        }
    }
    if (size == 0x1040) meter_read_note(c);   // (FLICK1)
    tl_apply_note = RbApplyNote{};
    tl_apply_note.c = c;
    rb_wait_staged(c, config().lag);   // (frames in flight: the replay stages a frame's entries before it shows it)
    rb_ring::Entry e;
    int cls;
    uint64_t total;
    {
        std::lock_guard lk(mu);
        const auto it = table.find({guest, size});
        if (it == table.end()) return;
        Addr& a = it->second;
        cls = a.cls;
        total = arena_total;
        // (EXPO3, HL_RB_PICK=0: the newest eligible entry as before) the steady pick: no sample is jumped over by a read one frame late
        static const bool steady = !getenv("HL_RB_PICK") || atoi(getenv("HL_RB_PICK")) != 0;
        const int k = steady ? rb_ring::pick_steady(a.ents, a.n, c, config().lag, a.applied) : rb_ring::pick(a.ents, a.n, c, config().lag, a.applied);
        stats[cls].reads++;
        if (k < 0) {
            // (PIPE2) the exposure meter's block: the game reads and zeroes it, so a read with no new entry finds the game's own
            // zero and the meter hold (vk_replay.cpp) hands it the last result again. Here the ring hands that last value back
            // itself, which is the same value by the same rule (the frame count and the entries), so the read is answered without
            // a hold and the held-read count stays at 0. (Only the meter's block: an R2/R3 range with no new entry reads what the
            // old path read.)
            int again = -1;
            if (size == 0x1040 && a.applied && memory_is_meter(guest))
                for (unsigned i = 0; i < a.n; i++)
                    if (a.ents[i].seq == a.applied) { again = (int)i; break; }
            if (again < 0) {
                stats[cls].none++;
                tl_apply_note.kind = 3;
                hash_add(c, cls, rb_ring::hash_read(cls, guest, nullptr, 0));
                return;
            }
            stats[cls].again++;
            e = a.ents[again];
            tl_apply_note.kind = 2;
        } else {
            e = a.ents[k];
        }
    }
    const uint64_t n = rb_ring::copy_bytes(cls, size);
    if (!rb_ring::intact(e, total, n)) return;   // (the ring went round since: far too old to hand out)
    if (e.value > vk_timeline_completed()) {
        stats[cls].waited++;
        if (!rbring_wait_value(e.value, config().wait_ms)) {
            stats[cls].late++;
            tl_apply_note.kind = 4, tl_apply_note.tag = e.tag, tl_apply_note.value = e.value;
            return;
        }
    }
    uint8_t buf[rb_ring::kMaxCopy];
    std::memcpy(buf, ring_map + e.at, n);
    host->gpu_write(gpu, buf, n);
    stats[cls].applied++;
    if (!tl_apply_note.kind) tl_apply_note.kind = 1;
    tl_apply_note.tag = e.tag, tl_apply_note.value = e.value;
    stats[cls].got[std::min<int64_t>(4, std::max<int64_t>(1, c - e.tag)) - 1]++;
    {
        std::lock_guard lk(mu);
        const auto it = table.find({guest, size});
        if (it != table.end()) it->second.applied = std::max(it->second.applied, e.seq);
    }
    hash_add(c, cls, rb_ring::hash_read(cls, guest, buf, n));
    if (stats[cls].reads % 3000 == 0)
        logf("vk: rbring %s: 3000 reads, %lld applied (%lld waited for the GPU, %lld not done in time), %lld nothing new, %lld handed the last value "
             "back (the meter's block); age in frames of the entries "
             "applied 1/2/3/4+: %lld/%lld/%lld/%lld (a fixed lag L hands out entries L frames old or older); staged so far %lld ranges, %.1f MB; "
             "ranges taken over again %lld, written bindings of tainted ranges given their write-back %lld",
             cname(cls), (long long)stats[cls].applied.exchange(0), (long long)stats[cls].waited.exchange(0), (long long)stats[cls].late.exchange(0),
             (long long)stats[cls].none.exchange(0), (long long)stats[cls].again.exchange(0),
             (long long)stats[cls].got[0].exchange(0), (long long)(stats[cls].got[1].exchange(0) + 0),
             (long long)stats[cls].got[2].exchange(0), (long long)(stats[cls].got[3].exchange(0) + stats[cls].got[4].exchange(0)),
             (long long)st_staged.load(), st_staged_bytes.load() / 1048576.0, (long long)st_retaken.load(), (long long)st_kept_wb.load());
}

/// HL_RB_LAG_CHECK=1, after the old path's waits: the ring's candidate against the value the game is about to read
void rbring_check(uint64_t gpu, uint64_t size) {
    if (!config().lag || !config().check) return;
    const uint64_t guest = host->gpu_guest(gpu);
    const int64_t c = rb_frame_now();
    rb_ring::Entry e;
    int cls = 0;
    uint64_t total = 0;
    bool have = false;
    int age = -1;
    {
        std::lock_guard lk(mu);
        const auto it = table.find({guest, size});
        if (it == table.end() || !it->second.stageable) return;
        Addr& a = it->second;
        if (a.tainted || a.writes - a.first_stage != a.staged) st_direct++;
        cls = a.cls, total = arena_total;
        if (const int nw = rb_ring::newest(a.ents, a.n); nw >= 0) age = (int)std::min<int64_t>(3, std::max<int64_t>(0, c - a.ents[nw].tag));
        const int k = rb_ring::pick(a.ents, a.n, c, config().lag, a.applied);
        if (k >= 0) e = a.ents[k], have = true, a.applied = e.seq;
    }
    Stats& st = stats[cls];
    st.reads++;
    if (age >= 0) st.age[age]++;
    const uint64_t n = rb_ring::copy_bytes(cls, size);
    uint8_t waited[rb_ring::kMaxCopy];
    host->gpu_read(gpu, waited, n);
    hash_add(c, cls, rb_ring::hash_read(cls, guest, waited, n));   // (HL_RB_LAG_LOG=1: what the old path's waits gave the game, as the control of the ring's hashes)
    if (have) {
        st.chk_new++;
        if (e.value > vk_timeline_completed() && !rbring_wait_value(e.value, config().wait_ms)) {
            st.chk_notdone++;
        } else if (!rb_ring::intact(e, total, n)) {
            st.chk_old++;
        } else if (!std::memcmp(waited, ring_map + e.at, n)) {
            st.chk_equal++;
        } else {
            st.chk_differ++;
            static std::atomic<int> logged{0};
            if (logged++ < 24) {
                uint64_t w = 0;
                while (w + 4 <= n && !std::memcmp(waited + w, ring_map + e.at + w, 4)) w += 4;
                uint32_t a = 0, b = 0;
                std::memcpy(&a, waited + w, 4), std::memcpy(&b, ring_map + e.at + w, 4);
                logf("vk: rbring check %s differs at frame %lld %#llx: first word %llu waited %08x ring %08x (entry tag %lld)", cname(cls),
                     (long long)c, (unsigned long long)guest, (unsigned long long)(w / 4), a, b, (long long)e.tag);
            }
        }
    } else {
        bool zero = true;
        for (uint64_t i = 0; i < n && zero; i++) zero = waited[i] == 0;
        (zero ? st.chk_zero : st.chk_nonzero)++;
    }
    if (st.reads % 3000 == 0)
        logf("vk: rbring check %s (lag %u): 3000 reads; a new entry in %lld (equal to the waited value %lld, differ %lld, ring entry not done %lld, "
             "ring overwritten %lld); no new entry in %lld reads (the waited value is zero in %lld, not zero in %lld); newest entry's age "
             "in frames 0/1/2/3+: %lld/%lld/%lld/%lld",
             cname(cls), config().lag, (long long)st.chk_new.exchange(0), (long long)st.chk_equal.exchange(0), (long long)st.chk_differ.exchange(0),
             (long long)st.chk_notdone.exchange(0), (long long)st.chk_old.exchange(0), (long long)(st.chk_zero.load() + st.chk_nonzero.load()),
             (long long)st.chk_zero.exchange(0), (long long)st.chk_nonzero.exchange(0), (long long)st.age[0].exchange(0),
             (long long)st.age[1].exchange(0), (long long)st.age[2].exchange(0), (long long)st.age[3].exchange(0));
}

/// (EXPO1, a test) HL_RB_TEST_DIRECT=<frame>[:<every>] (default off): from that frame on (and again every <every> frames), the next written binding of each exposure-meter block (0x1040) the ring learned goes to guest memory directly, as a binding of a pool with no mirror does after a zone change: the freeze of 2026-11-23 on demand (with HL_RB_LAG_KEEP=0) and the ring's take-over (without)
bool rbring_test_direct(uint64_t guest, uint64_t size) {
    static const int64_t from = getenv("HL_RB_TEST_DIRECT") ? atoll(getenv("HL_RB_TEST_DIRECT")) : 0;
    static const int64_t every = [] {
        const char* e = getenv("HL_RB_TEST_DIRECT");
        const char* c = e ? strchr(e, ':') : nullptr;
        return c ? atoll(c + 1) : 0ll;
    }();
    if (from <= 0 || size != 0x1040 || !config().lag || !memory_is_meter(guest)) return false;
    const int64_t f = rb_frame_now();
    if (f < from) return false;
    const int64_t round = every > 0 ? (f - from) / every : 0;   // (each block once a round)
    std::lock_guard lk(mu);
    const auto it = table.find({guest, size});
    if (it == table.end()) return false;
    static std::map<uint64_t, int64_t> done;   // (block -> the round it was sent direct in)
    const auto d = done.find(guest);
    if (d != done.end() && d->second >= round) return false;
    done[guest] = round;
    logf("vk: HL_RB_TEST_DIRECT: frame %lld, the binding of meter block %#llx sent to guest memory directly (writes %llu, staged %llu)",
         (long long)f, (unsigned long long)guest, (unsigned long long)it->second.writes, (unsigned long long)it->second.staged);
    return true;
}

/// (EXPO1, HL_METER_LOG) the ring's state of a range, for a log row: how it is served, its counters, the last staged binding, and its
/// entries newest first (seq, tag, done or not, the first four words; "gone" when the ring went round over it)
void rbring_describe(uint64_t guest, uint64_t size, char* out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    if (!config().lag) {
        snprintf(out, cap, "ring off");
        return;
    }
    std::lock_guard lk(mu);
    const auto it = table.find({guest, size});
    if (it == table.end()) {
        snprintf(out, cap, "not learned (table %zu)", table.size());
        return;
    }
    const Addr& a = it->second;
    size_t n = (size_t)snprintf(out, cap, "served %d stageable %d tainted %d writes %llu first %llu staged %llu reads %llu settle %lld applied %llu, "
                                "last bind %#llx+%#llx (%u staged in its last frame) table %zu:", served(a), a.stageable, a.tainted,
                                (unsigned long long)a.writes, (unsigned long long)a.first_stage, (unsigned long long)a.staged,
                                (unsigned long long)a.reads, (long long)a.settle, (unsigned long long)a.applied, (unsigned long long)(uintptr_t)a.last_buffer,
                                (unsigned long long)a.last_offset, a.last_frame_stages, table.size());
    unsigned order[rb_ring::kMaxEntries];
    for (unsigned i = 0; i < a.n; i++) order[i] = i;
    std::sort(order, order + a.n, [&](unsigned x, unsigned y) { return a.ents[x].seq > a.ents[y].seq; });
    const uint64_t done = vk_timeline_completed();
    for (unsigned k = 0; k < a.n && n < cap; k++) {
        const rb_ring::Entry& e = a.ents[order[k]];
        uint32_t w[4] = {};
        const bool ok = ring_map && rb_ring::intact(e, arena_total, 16);
        if (ok) std::memcpy(w, ring_map + e.at, 16);
        n += (size_t)snprintf(out + n, cap - n, ok ? " #%llu t%lld%s %08x %08x %08x %08x" : " #%llu t%lld%s gone", (unsigned long long)e.seq,
                              (long long)e.tag, e.value > done ? " (not done)" : "", w[0], w[1], w[2], w[3]);
    }
}

}   // namespace mkwvk
