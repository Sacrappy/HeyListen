// Copyright (C) 2026 Sacrappy
// RX_VK_RB_EARLY (ORDER2, docs/research/order-readback-decouple.md): with the order thread, the culling / camera results (R4, 0x3980) reach
// the game's CPU from a copy recorded right behind each program that writes them, not from the write-back at the end of their submission.
// The eight programs that write the block run at the head of the frame's last submission, whose replay takes ~4 ms; its write-backs land
// after the whole of it and of its GPU work, and the culling job of the next frame read it first and waited (flash2f: a quarter of the
// reads, 1.6 ms on the main PC, 4.4-5.5 ms on the PC 3050). The rules are in rb_early.h; RX_VK_RB_EARLY=2 runs the old path and compares.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "vk_internal.h"
#include "rb_early.h"

namespace mkwvk {
extern int64_t presents_now;   // (vk_replay.cpp; FLICK1)
}
namespace mkwvk {
namespace {

const rb_early::Config& config() {
    // (ORDER2) RX_VK_RB_EARLY=<0|1|2> (1 with the order thread, else 0): the 0x3980 culling results the CPU reads come from a copy recorded right behind each program that writes them, the read waiting only for its own frame's copy (1); 0: as before (the order thread's wait for the frame's last submission, RX_VK_ORDER_RB_JOIN=5); 2: the old path runs and each read is compared with the copy the rule would have handed out. RX_VK_RB_EARLY_WAIT_MS=<ms> (30): how long a read waits for its copy before it takes the old path
    static const rb_early::Config c = [] {
        const rb_early::Config k = rb_early::parse(getenv("RX_VK_RB_EARLY"), getenv("RX_VK_RB_EARLY_WAIT_MS"), ot_on());
        if (k.mode) logf("vk: RX_VK_RB_EARLY=%d: the 0x3980 culling results are %s from a copy right behind each program that writes them (wait %.0f ms at most)",
                         k.mode, k.mode == 1 ? "served" : "checked", k.wait_ms);
        return k;
    }();
    return c;
}

std::mutex mu;
rb_early::Range ranges[rb_early::kRanges];
unsigned nranges = 0;
std::atomic<unsigned> nranges_a{0};   // (nranges, read without mu by the written-binding check)
std::atomic<uint64_t> span_lo{~0ull}, span_hi{0};   // (the served ranges' span: a written binding outside it is not looked at)
uint64_t last_final_job = 0;   // (mu: the newest job that finished writing some served range)
VkBuffer ring_buffer = VK_NULL_HANDLE;
uint8_t* ring_map = nullptr;
bool ring_failed = false;
constexpr VkDeviceSize kBytes = (VkDeviceSize)rb_early::kRanges * rb_early::kSlots * rb_early::kSlotBytes;

bool ring_init() {   // (mu held)
    if (ring_map) return true;
    if (ring_failed) return false;
    ring_failed = true;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kBytes;
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
        vkBindBufferMemory(dev.device, buffer, memory, 0) != VK_SUCCESS || vkMapMemory(dev.device, memory, 0, kBytes, 0, &words) != VK_SUCCESS) {
        logf("vk: RX_VK_RB_EARLY: no host memory for the copies: the old path stays");
        vkDestroyBuffer(dev.device, buffer, nullptr);
        return false;
    }
    std::memset(words, 0, kBytes);
    ring_buffer = buffer;
    ring_map = (uint8_t*)words;
    ring_failed = false;
    return true;
}
VkDeviceSize slot_at(unsigned range, unsigned slot) { return ((VkDeviceSize)range * rb_early::kSlots + slot) * rb_early::kSlotBytes; }
uint32_t stamp_of(unsigned range, unsigned slot) {
    return *(const volatile uint32_t*)(ring_map + slot_at(range, slot) + rb_early::kStampOffset);
}
int find_locked(uint64_t guest, uint64_t size) {
    for (unsigned i = 0; i < nranges; i++)
        if (ranges[i].guest == guest && ranges[i].size == size) return (int)i;
    return -1;
}

// (the copies a thread's bindings asked for, recorded behind its next dispatch)
struct Pending {
    VkBuffer src;
    VkDeviceSize offset, size, dst;
    uint32_t seq;
};
thread_local std::vector<Pending> tl_pending;
thread_local bool tl_split = false;   // (a final copy was recorded: the replay ends its part right after, rbearly_split_take)

struct Stats {
    std::atomic<int64_t> reads{0}, final_ok{0}, newest{0}, rec_waits{0}, rec_ns{0}, rec_worst{0}, gpu_waits{0}, gpu_ns{0}, gpu_worst{0},
        fb_rec{0}, fb_gpu{0}, fb_none{0}, binds{0}, finals{0}, left{0}, checks{0}, mismatches{0}, splits{0}, repeats{0}, late_ok{0}, stale{0}, prev_same{0}, prev_diff{0}, stall_prev{0}, old_nowhere{0}, old_same_range{0}, old_other_range{0};
    std::atomic<int> old_logged{0};
    std::atomic<int> tainted_logged{0};
} st;
void note_max(std::atomic<int64_t>& m, int64_t v) {
    int64_t w = m.load();
    while (v > w && !m.compare_exchange_weak(w, v)) {}
}
int64_t now_ns() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

/// The slot a read of range `ri` is answered with (waits for the order thread's recording and the GPU's copy, wait_ms at most):
/// false = none in time. out gets the bytes, *seq_out the slot's sequence number. cap_ms: how long each of the two waits may last
bool prev_on() {
    // (ORDER2) RX_VK_RB_EARLY_PREV=<0|1> (0): with RX_VK_RB_EARLY=1, a read is answered with its range as it stood before the job it would wait for (the newest copy of an earlier job: never a wait), the value flash2f's wait handed out; 0: the copy of that job
    static const bool on = getenv("RX_VK_RB_EARLY_PREV") && atoi(getenv("RX_VK_RB_EARLY_PREV")) != 0;
    return on;
}
thread_local uint64_t tl_read_target = 0;   // (the target when this thread's read began: rbearly_note_read; the check compares with what the read
                                            // would have been answered with then, not after the old path's waits, by which the next frames may be in)
bool fetch(unsigned ri, uint64_t size, uint8_t* out, bool count, uint32_t* seq_out, double cap_ms, bool prev = false) {
    const uint64_t target = tl_read_target ? tl_read_target : ot_rb_target();
    const int64_t t0 = now_ns(), cap = (int64_t)(cap_ms * 1e6);
    rb_early::Pick p;
    bool rec_waited = false, logged_long = false;
    for (;;) {
        {
            std::lock_guard lk(mu);
            p = prev ? rb_early::pick_prev(ranges[ri], target) : rb_early::pick(ranges[ri], target, ot_rb_done(), last_final_job);
        }
        if (p.kind != rb_early::kWait) break;
        rec_waited = true;
        if (count && !logged_long && now_ns() - t0 > 20000000) {   // (a read 20 ms behind the recording: what the order thread is on, 60 lines)
            logged_long = true;
            static std::atomic<int> rows{0};
            if (rows++ < 60) {
                char b[300];
                ot_describe_now(b, sizeof b);
                logf("vk: rbearly: a read of %#llx waits 20 ms for job %llu's copy: %s", (unsigned long long)ranges[ri].guest, (unsigned long long)target, b);
            }
        }
        if (now_ns() - t0 > cap) {
            if (count) st.fb_rec++;
            return false;
        }
        std::this_thread::yield();
    }
    const int64_t t1 = now_ns();
    if (count && rec_waited) st.rec_waits++, st.rec_ns += t1 - t0, note_max(st.rec_worst, t1 - t0);
    if (p.kind == rb_early::kNone) {
        if (count) st.fb_none++;
        return false;
    }
    bool gpu_waited = false;
    while (stamp_of(ri, p.slot) != p.seq) {
        gpu_waited = true;
        if (stamp_of(ri, p.slot) > p.seq || now_ns() - t1 > cap) {   // (overwritten already, or not done in time)
            if (count) st.fb_gpu++;
            return false;
        }
        std::this_thread::yield();
    }
    const int64_t t2 = now_ns();
    if (count && gpu_waited) st.gpu_waits++, st.gpu_ns += t2 - t1, note_max(st.gpu_worst, t2 - t1);
    std::memcpy(out, ring_map + slot_at(ri, p.slot), (size_t)size);
    if (stamp_of(ri, p.slot) != p.seq) {   // (the slot went round while it was copied)
        if (count) st.fb_gpu++;
        return false;
    }
    if (count) (p.kind == rb_early::kSlotFinal ? st.final_ok : st.newest)++;
    *seq_out = p.seq;
    return true;
}
void log_stats() {
    const int64_t r = ++st.reads;
    if (r % 1200) return;
    auto ms = [](int64_t ns, int64_t n) { return n ? ns / (double)n / 1e6 : 0.0; };
    const int64_t rw = st.rec_waits.exchange(0), gw = st.gpu_waits.exchange(0);
    logf("vk: rbearly: 1200 reads of the 0x3980 culling results: %lld from their job's last copy, %lld from the newest copy (the job wrote "
         "the other range); %lld waited for the order thread's recording (%.2f ms each, worst %.2f), %lld for the GPU's copy (%.2f ms each, "
         "worst %.2f); fell back to the old path: %lld recording not in time, %lld GPU not in time, %lld no copy; bindings copied %lld (%lld "
         "final), %lld left with no dispatch behind them, %lld parts ended behind a final copy; %lld reads with nothing newer (guest memory left "
         "alone), %lld answered after the longer wait, %lld left to guest memory as it was (STALE); check: %lld compared, %lld mismatches; the "
         "previous copy (RX_VK_RB_EARLY_PREV) equal %lld, different %lld; answered with the previous copy in a stall (RX_VK_RB_EARLY_STALL_MS) %lld; the old path's value found among the copies: of the "
         "range %lld, of another range %lld, none %lld",
         (long long)st.final_ok.exchange(0), (long long)st.newest.exchange(0), (long long)rw, ms(st.rec_ns.exchange(0), rw),
         st.rec_worst.exchange(0) / 1e6, (long long)gw, ms(st.gpu_ns.exchange(0), gw), st.gpu_worst.exchange(0) / 1e6,
         (long long)st.fb_rec.exchange(0), (long long)st.fb_gpu.exchange(0), (long long)st.fb_none.exchange(0), (long long)st.binds.exchange(0),
         (long long)st.finals.exchange(0), (long long)st.left.exchange(0), (long long)st.splits.exchange(0), (long long)st.repeats.exchange(0),
         (long long)st.late_ok.exchange(0), (long long)st.stale.exchange(0), (long long)st.checks.exchange(0), (long long)st.mismatches.exchange(0),
         (long long)st.prev_same.exchange(0), (long long)st.prev_diff.exchange(0), (long long)st.stall_prev.exchange(0),
         (long long)st.old_same_range.exchange(0), (long long)st.old_other_range.exchange(0), (long long)st.old_nowhere.exchange(0));
}

}   // namespace

int rbearly_mode() { return config().mode; }

/// The CPU reads (guest, size): a served size becomes a range the copies are made for
void rbearly_note_read(uint64_t guest, uint64_t size) {
    if (!config().mode || !rb_early::served_size(size) || size > rb_early::kMaxBytes) return;
    tl_read_target = ot_rb_target();
    std::lock_guard lk(mu);
    if (find_locked(guest, size) >= 0 || nranges >= rb_early::kRanges) return;
    rb_early::Range& r = ranges[nranges];
    r = rb_early::Range{};
    r.guest = guest, r.size = size;
    nranges++;
    nranges_a.store(nranges, std::memory_order_relaxed);
    if (guest < span_lo.load()) span_lo.store(guest);
    if (guest + size > span_hi.load()) span_hi.store(guest + size);
    logf("vk: rbearly: %#llx +%#llx is served from copies behind its writers (%u ranges)", (unsigned long long)guest, (unsigned long long)size, nranges);
}

/// A program's binding for writing in a mirror (vk_memory.cpp gpu_buffer_bind, pools_mu held), on the thread recording it: a served
/// range takes a slot, and the copy is recorded behind the thread's next dispatch (rbearly_after_dispatch)
/// true: the range is served (RX_VK_RB_EARLY=1, not tainted): the caller leaves its write-back out, the reads write it into guest memory
bool rbearly_bind(uint64_t guest, uint64_t size, VkBuffer buffer, VkDeviceSize offset) {
    if (!config().mode || !buffer || !rb_early::served_size(size)) return false;
    std::lock_guard lk(mu);
    const int ri = find_locked(guest, size);
    if (ri < 0 || ranges[ri].tainted || !ring_init()) return false;
    const bool serve = config().mode == 1;
    const rb_early::Issue is = rb_early::issue(ranges[ri], ot_current_job());
    if (is.final) last_final_job = std::max(last_final_job, ranges[ri].cur_job), st.finals++;
    st.binds++;
    if (is.final) tl_split = true;
    for (Pending& p : tl_pending)   // (bound twice before one dispatch: the copy goes to the newer slot)
        if (p.src == buffer && p.offset == offset && p.size == size) {
            p.dst = slot_at((unsigned)ri, is.slot), p.seq = is.seq;
            return serve;
        }
    if (tl_pending.size() >= 16) {
        st.left++;
        return serve;
    }
    tl_pending.push_back({buffer, offset, size, slot_at((unsigned)ri, is.slot), is.seq});
    return serve;
}

/// Every written binding (vk_memory.cpp gpu_buffer_bind): one that overlaps a served range without being it taints the range (its
/// write-backs would land in it: it goes back to the old path for good)
void rbearly_note_written(uint64_t guest, uint64_t size) {
    if (!config().mode || !nranges_a.load(std::memory_order_relaxed)) return;
    if (guest >= span_hi.load(std::memory_order_relaxed) || guest + size <= span_lo.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(mu);
    for (unsigned i = 0; i < nranges; i++) {
        rb_early::Range& r = ranges[i];
        const bool was = r.tainted;
        rb_early::note_written(r, guest, size);
        if (r.tainted && !was && st.tainted_logged++ < 8)
            logf("vk: rbearly: %#llx +%#llx overlapped by a written binding %#llx +%#llx: back to the old path", (unsigned long long)r.guest,
                 (unsigned long long)r.size, (unsigned long long)guest, (unsigned long long)size);
    }
}

/// Right after a compute dispatch is recorded (vk_draw.cpp), on its command buffer: the served ranges it wrote, copied into their slots
/// and stamped. A slot left behind by a binding with no dispatch after it (a draw's) is never stamped, and its reads fall back
void rbearly_after_dispatch(VkCommandBuffer cmd) {
    if (tl_pending.empty()) return;
    if (!ring_buffer) {
        tl_pending.clear();
        return;
    }
    auto barrier = [&](VkPipelineStageFlags2 s, VkAccessFlags2 sa, VkPipelineStageFlags2 d, VkAccessFlags2 da) {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = s, b.srcAccessMask = sa, b.dstStageMask = d, b.dstAccessMask = da;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
    };
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (const Pending& p : tl_pending) {
        const VkBufferCopy c{p.offset, p.dst, p.size};
        vkCmdCopyBuffer(cmd, p.src, ring_buffer, 1, &c);
    }
    barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (const Pending& p : tl_pending) vkCmdFillBuffer(cmd, ring_buffer, p.dst + rb_early::kStampOffset, 4, p.seq);
    barrier(VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
    tl_pending.clear();
}

/// (RX_VK_RB_EARLY_SPLIT=0 off) after the dispatch that wrote a served range's last copy of its job, the replay ends the recording's part
/// there (split_part), so the copy goes to the GPU at once and not with the rest of the part (up to 120 draws and dispatches later)
bool rbearly_split_take() {
    // (ORDER2) RX_VK_RB_EARLY_SPLIT=<0|1> (1): with RX_VK_RB_EARLY, the recording's part ends right after the dispatch that wrote the job's last copy of the culling results, so that copy reaches the GPU at once
    static const bool on = !getenv("RX_VK_RB_EARLY_SPLIT") || atoi(getenv("RX_VK_RB_EARLY_SPLIT")) != 0;
    if (!tl_split) return false;
    tl_split = false;
    if (on) st.splits++;
    return on;
}

/// RX_VK_RB_EARLY=1: the read of a served range answered from its copy (into guest memory). false: not served (the old path runs)
bool rbearly_answer(uint64_t gpu, uint64_t size) {
    if (config().mode != 1 || !rb_early::served_size(size)) return false;
    int ri;
    {
        std::lock_guard lk(mu);
        ri = find_locked(host->gpu_guest(gpu), size);
        if (ri < 0 || !ring_map || !ranges[ri].next_seq || ranges[ri].tainted) return false;
    }
    // (its write-backs are left out while it is served: a read must find its copy. Past the usual wait, a longer one (200 ms) for the
    // order thread and the GPU; then guest memory as it is, counted as STALE)
    uint8_t buf[rb_early::kMaxBytes];
    uint32_t seq = 0;
    // (ORDER2) RX_VK_RB_EARLY_STALL_MS=<ms> (0 off): a read whose own copy is not there within that time (the replay stalled behind uploads or a long job, a zone's load) takes the range's previous copy at once instead of waiting: one frame of last frame's culling results against a hitch of tens of ms
    static const double stall_ms = getenv("RX_VK_RB_EARLY_STALL_MS") ? atof(getenv("RX_VK_RB_EARLY_STALL_MS")) : 0.0;
    const double first_ms = stall_ms > 0 ? std::min(stall_ms, config().wait_ms) : config().wait_ms;
    bool ok = fetch((unsigned)ri, size, buf, true, &seq, first_ms, prev_on());
    if (!ok && stall_ms > 0 && fetch((unsigned)ri, size, buf, false, &seq, 0.0, true)) ok = true, st.stall_prev++;
    if (!ok && fetch((unsigned)ri, size, buf, false, &seq, 200.0)) ok = true, st.late_ok++;
    if (!ok) {
        st.stale++;
        if (getenv("HL_RB_EVENT_LOG")) logf("vk: rbevent early present %lld %#llx stale", (long long)presents_now, (unsigned long long)host->gpu_guest(gpu));
        log_stats();
        return true;
    }
    bool write = false;
    {
        std::lock_guard lk(mu);
        if (rb_early::fresh(ranges[ri], seq)) ranges[ri].applied = seq, write = true;
    }
    if (write) host->gpu_write(gpu, buf, (size_t)size);
    else st.repeats++;   // (the game's own writes since the last copy are kept)
    if (getenv("HL_RB_EVENT_LOG") && !write)
        logf("vk: rbevent early present %lld %#llx repeat seq %u", (long long)presents_now, (unsigned long long)host->gpu_guest(gpu), seq);
    log_stats();
    return true;
}

/// RX_VK_RB_EARLY=2, after the old path's waits: the copy the rule picks against what guest memory holds now
void rbearly_check(uint64_t gpu, uint64_t size) {
    if (config().mode != 2 || !rb_early::served_size(size)) return;
    int ri;
    {
        std::lock_guard lk(mu);
        ri = find_locked(host->gpu_guest(gpu), size);
        if (ri < 0 || !ring_map || !ranges[ri].next_seq || ranges[ri].tainted) return;
    }
    uint8_t mine[rb_early::kMaxBytes], old[rb_early::kMaxBytes];
    uint32_t seq = 0;
    if (fetch((unsigned)ri, size, mine, true, &seq, config().wait_ms)) {
        {   // (only a read that would write a newer copy is compared: the game's writes between its reads are its own)
            std::lock_guard lk(mu);
            if (!rb_early::fresh(ranges[ri], seq)) {
                st.repeats++;
                log_stats();
                return;
            }
            ranges[ri].applied = seq;
        }
        host->gpu_read(gpu, old, (size_t)size);
        st.checks++;
        {   // (the previous copy, RX_VK_RB_EARLY_PREV's answer, against the same value)
            uint8_t prev[rb_early::kMaxBytes];
            uint32_t pseq = 0;
            if (fetch((unsigned)ri, size, prev, false, &pseq, 0.0, true)) (std::memcmp(prev, old, (size_t)size) ? st.prev_diff : st.prev_same)++;
        }
        {   // (what the old path's value is, when it is no copy of this range: any stamped slot of any served range equal to it, by its age)
            int found = -1;
            uint32_t newest_seq = 0;
            std::lock_guard lk(mu);
            for (unsigned i = 0; i < nranges && found < 0; i++)
                for (unsigned k = 0; k < rb_early::kSlots && found < 0; k++) {
                    const rb_early::Slot& sl = ranges[i].slots[k];
                    if (!sl.seq || stamp_of(i, k) != sl.seq) continue;
                    if (!std::memcmp(ring_map + slot_at(i, k), old, (size_t)size)) found = (int)(i * 1000 + (ranges[i].next_seq - sl.seq)), newest_seq = sl.seq;
                }
            (found < 0 ? st.old_nowhere : (found / 1000 == ri ? st.old_same_range : st.old_other_range))++;
            if (found >= 0 && st.old_logged++ < 12)
                logf("vk: rbearly check: the old path's value is range %d's copy seq %u, %d bindings before its newest (the read is of range %d)", found / 1000,
                     newest_seq, found % 1000, ri);
        }
        if (std::memcmp(mine, old, (size_t)size)) {
            const int64_t m = ++st.mismatches;
            if (m <= 24 || m % 100 == 0) {   // (the words that differ: how many, and the first eight with both values; then one in 100)
                char b[600];
                int n = 0, words = 0;
                for (size_t w = 0; w + 4 <= size; w += 4) {
                    uint32_t x, y;
                    std::memcpy(&x, mine + w, 4), std::memcpy(&y, old + w, 4);
                    if (x == y) continue;
                    if (words++ < 8 && n < (int)sizeof b - 40) n += snprintf(b + n, sizeof b - n, " +%#zx %08x/%08x", w, x, y);
                }
                uint64_t job = 0, pjob = 0;
                uint32_t pseq = 0;
                unsigned learned_n = 0;
                {
                    std::lock_guard lk(mu);
                    const rb_early::Range& r = ranges[ri];
                    for (const rb_early::Slot& sl : r.slots)
                        if (sl.seq == seq) job = sl.job;
                    const rb_early::Pick pp = rb_early::pick_prev(r, tl_read_target);
                    if (pp.kind == rb_early::kSlotFinal) pseq = pp.seq, pjob = r.slots[pp.slot].job;
                    learned_n = rb_early::learned(r);
                }
                logf("vk: rbearly check MISMATCH %#llx +%#llx (clk %lld, seq %u job %llu; target %llu done %llu cur %llu; previous final seq %u job %llu; "
                     "learned %u): %d words differ, copy/old path:%s", (unsigned long long)host->gpu_guest(gpu), (unsigned long long)size,
                     (long long)rb_frame_now(), seq, (unsigned long long)job, (unsigned long long)tl_read_target, (unsigned long long)ot_rb_done(),
                     (unsigned long long)ot_current_job(), pseq, (unsigned long long)pjob, learned_n, words, n ? b : "");
            }
        }
    }
    log_stats();
}

}   // namespace mkwvk
