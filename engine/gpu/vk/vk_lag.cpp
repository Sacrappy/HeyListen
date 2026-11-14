// (HeyListen, 2026-11-14) RX_VK_LAG_LOG=1: replay lag, the truth of the game's waits, and what the capture protects. See vk_lag.h.
//
// What is measured, with the host clock (the same as the host's hos_now_ns) and the game's present count (the presents it has
// asked for, nvnQueuePresentTexture):
// - per submit (QueueSubmitCommands), "S" rows: when the game made it (t_sub, f_sub, from the host: hos_lag_sub), when the back end
//   took it (t_bq: after the host's hold, before the capture), when the replay started recording it (t_rs: after the capture) and
//   finished (t_re), and when the GPU finished it (t_gpu: the timeline semaphore, watched by a thread of its own that spins on it);
//   each with the present count then;
// - per game wait or poll, "W" rows: what was asked, what was answered, when, the fence's recording number; "F" rows for the
//   queue finishes;
// - per submit, "C" rows: of the pages the submit's capture copied (a sample, one in RX_VK_LAG_STRIDE, default 4), how many hold
//   other bytes in guest memory when the replay has recorded the submit (the pages the capture was needed for), by what the pages
//   were bound for (the ledger's marks: RX_VK_LEDGER=3600 turns them on); "G" rows: the same once the GPU had finished it.
// The file is lag_log.csv in the working directory; tools read it afterwards. Off: nothing is marked, kept or written.
#include "vk_lag.h"
#include "vk_internal.h"
#include "vk_ledger.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace mkwvk {

bool lag_on() {
    static const bool v = getenv("RX_VK_LAG_LOG") && atoi(getenv("RX_VK_LAG_LOG")) != 0;
    return v;
}

namespace {
constexpr uint64_t kRing = 8192;
struct Rec {
    uint64_t value = 0;
    int64_t t_sub = 0, f_sub = 0, t_bq = 0, f_bq = 0, t_rs = 0, f_rs = 0, t_re = 0, f_re = 0, t_gpu = 0, f_gpu = 0, t_grec = 0, f_grec = 0, t_gend = 0, f_gend = 0;
    uint64_t seq_end = 0;   // (the recording the last piece of its work went into)
    int flags = 0;   // (1: stopped at an event wait, 2: queued behind a stopped submission of its queue)
    bool row = false;   // (the S row written)
};
Rec ring[kRing];
int64_t rec_t[kRing];   // (the recordings' timeline: when each number was reached)
std::mutex mu;
FILE* out = nullptr;
thread_local int64_t tl_bq_t = 0, tl_bq_f = 0;
using SubFn = void (*)(int64_t*, int64_t*);
using FramesFn = int64_t (*)();
using CountsFn = void (*)(int64_t*);
SubFn sub_fn;
FramesFn frames_fn;
CountsFn counts_fn;
uint64_t (*completed_fn)() = nullptr;
uint64_t (*rec_completed_fn)() = nullptr;
std::once_flag started;

int64_t now() { return host->now_ns(); }
int64_t frames() { return frames_fn ? frames_fn() : 0; }

void write_row(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void write_row(const char* fmt, ...) {
    if (!out) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
}

// (S: t_sub f_sub t_bq f_bq t_rs f_rs t_re f_re, then t_gpu f_gpu: the timeline semaphore reached the value; t_grec f_grec: the GPU
// finished the recording the back end maps the value to (value_rec, what a game fence waits for: the recording where the work began
// or ended); t_gend f_gend: the GPU finished the recording the last piece of the work went into; flags)
void flush_row(Rec& r) {   // (mu held)
    if (r.row || !r.t_gpu || !r.t_grec || !r.t_gend) return;
    r.row = true;
    write_row("S,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%d\n", (unsigned long long)r.value,
              (long long)r.t_sub, (long long)r.f_sub, (long long)r.t_bq, (long long)r.f_bq, (long long)r.t_rs, (long long)r.f_rs,
              (long long)r.t_re, (long long)r.f_re, (long long)r.t_gpu, (long long)r.f_gpu, (long long)r.t_grec, (long long)r.f_grec,
              (long long)r.t_gend, (long long)r.f_gend, r.flags);
}
std::vector<uint64_t> pend;   // (the values whose recording the GPU has not finished yet; mu)
std::vector<uint64_t> waiting_end;   // (whole in a burst: the recording that ends it is the next one submitted; mu)
uint64_t (*rec_of_fn)(uint64_t) = nullptr;
void (*wait_fn)(uint64_t, uint64_t, uint64_t) = nullptr;

// (the thread that stamps the GPU's completions: it sleeps on the two timelines, so a value is stamped within microseconds of its
// signal)
void poller_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk lag poller");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    uint64_t last = 0, last_rec = 0;
    int64_t last_flush = 0;
    for (;;) {
        if (wait_fn) wait_fn(last + 1, last_rec + 1, 2000000);   // (until either timeline moves, 2 ms at most)
        else Sleep(1);
        const uint64_t c = completed_fn ? completed_fn() : 0;
        const uint64_t rc = rec_completed_fn ? rec_completed_fn() : 0;
        {
            const int64_t t = now(), f = frames();
            std::lock_guard lk(mu);
            if (rec_of_fn)
                for (size_t i = 0; i < pend.size();) {
                    Rec& r = ring[pend[i] % kRing];
                    if (r.value != pend[i]) { pend[i] = pend.back(), pend.pop_back(); continue; }
                    if (!r.t_grec) {
                        const uint64_t seq = rec_of_fn(pend[i]);
                        if (seq && seq <= rc) r.t_grec = t, r.f_grec = f;
                    }
                    if (!r.t_gend && r.seq_end && r.seq_end <= rc) r.t_gend = t, r.f_gend = f;
                    flush_row(r);
                    if (r.t_grec && r.t_gend) pend[i] = pend.back(), pend.pop_back();
                    else i++;
                }
            for (uint64_t v = last + 1; v <= c && c - last < kRing; v++) {
                Rec& r = ring[v % kRing];
                if (r.value == v) r.t_gpu = t, r.f_gpu = f, flush_row(r);
            }
            last = std::max(last, c), last_rec = std::max(last_rec, rc);
        }
        const int64_t t = now();
        if (t - last_flush > 2000000000LL) {
            last_flush = t;
            std::lock_guard lk(mu);
            if (out) fflush(out);
        }
    }
}
void start() {
    std::call_once(started, [] {
        out = fopen("lag_log.csv", "w");
        HMODULE exe = GetModuleHandleA(nullptr);
        sub_fn = (SubFn)(void*)GetProcAddress(exe, "hos_lag_sub");
        frames_fn = (FramesFn)(void*)GetProcAddress(exe, "hos_lag_frames");
        counts_fn = (CountsFn)(void*)GetProcAddress(exe, "hos_lag_counts");
        logf("vk: lag log on (RX_VK_LAG_LOG): lag_log.csv, host hooks %s", sub_fn && frames_fn ? "found" : "NOT found");
        if (out) fprintf(out, "H,%lld\n", (long long)now());
        std::thread(poller_main).detach();
    });
}

// ---- the captures' pages, kept until the submit's recording is whole (and then until the GPU has finished it) ----
struct CapKeep {
    uint64_t value = 0;
    uint32_t gen = 0;
    std::vector<uint32_t> pages, slots;
    bool r_done = false;
    uint64_t age = 0;
};
std::deque<CapKeep> keeps;   // (mu)
int stride() {
    static const int v = getenv("RX_VK_LAG_STRIDE") ? std::max(1, atoi(getenv("RX_VK_LAG_STRIDE"))) : 4;
    return v;
}
bool gpu_done(uint64_t value) {   // (mu held)
    const Rec& r = ring[value % kRing];
    return r.value == value && r.t_gend != 0;
}
}  // namespace

void lag_set_timelines(uint64_t (*completed)(), uint64_t (*rec_completed)(), uint64_t (*rec_of)(uint64_t),
                       void (*wait_any)(uint64_t, uint64_t, uint64_t)) {
    if (!lag_on()) return;
    completed_fn = completed, rec_completed_fn = rec_completed, rec_of_fn = rec_of, wait_fn = wait_any;
    start();
}

void lag_bq_enter() {
    if (!lag_on()) return;
    tl_bq_t = now(), tl_bq_f = frames();
}

void lag_submit(uint64_t value) {
    if (!lag_on()) return;
    start();
    int64_t ts = 0, fs = 0;
    if (sub_fn) sub_fn(&ts, &fs);
    std::lock_guard lk(mu);
    Rec& r = ring[value % kRing];
    r = Rec{};
    r.value = value, r.t_sub = ts, r.f_sub = fs, r.t_bq = tl_bq_t, r.f_bq = tl_bq_f;
    pend.push_back(value);
}

void lag_note_stop(uint64_t value, int bit) {
    if (!lag_on()) return;
    std::lock_guard lk(mu);
    Rec& r = ring[value % kRing];
    if (r.value == value) r.flags |= bit;
}

void lag_recording_submitted(uint64_t seq) {
    if (!lag_on()) return;
    std::lock_guard lk(mu);
    for (uint64_t v : waiting_end) {
        Rec& r = ring[v % kRing];
        if (r.value == v) r.seq_end = seq;
    }
    waiting_end.clear();
}

void lag_run_start(uint64_t value) {
    if (!lag_on()) return;
    const int64_t t = now(), f = frames();
    std::lock_guard lk(mu);
    Rec& r = ring[value % kRing];
    if (r.value == value && !r.t_rs) r.t_rs = t, r.f_rs = f;
}

void lag_run_end(uint64_t value, uint64_t seq_now, bool burst) {
    if (!lag_on()) return;
    const int64_t t = now(), f = frames();
    // (the pages' bytes against guest memory as the recording ends: the writes the capture protected the replay from)
    CapKeep copy;
    {
        std::lock_guard lk(mu);
        Rec& r = ring[value % kRing];
        if (r.value == value) {
            r.t_re = t, r.f_re = f;
            if (burst) waiting_end.push_back(value);
            else r.seq_end = seq_now;
        }
        // (the last capture of the submission: a submission that stopped at an event wait has another taken where it goes on, for what
        // the game wrote up to its signal; the window of the check starts there)
        CapKeep* k = nullptr;
        for (CapKeep& c : keeps)
            if (c.value == value && !c.r_done) k = &c;
        if (!k) return;
        k->r_done = true;
        copy.value = k->value, copy.gen = k->gen, copy.pages = k->pages, copy.slots = k->slots;
        for (auto it = keeps.begin(); it != keeps.end();)
            if (it->value == value && it->gen != copy.gen) it = keeps.erase(it);
            else ++it;
    }
    LagCmp c;
    memory_lag_compare(copy.gen, copy.pages.data(), copy.slots.data(), copy.pages.size(), c);
    std::lock_guard lk(mu);
    write_row("C,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", (unsigned long long)value, (long long)c.sampled,
              (long long)c.differ[0], (long long)c.differ[1], (long long)c.differ[2], (long long)c.differ[3], (long long)c.skipped,
              (long long)c.ns, (long long)t, (long long)f);
}

void lag_cap_taken(uint64_t value, uint32_t gen, const uint32_t* pages, const uint32_t* slots, size_t n, const uint8_t* late_mark) {
    if (!lag_on()) return;
    CapKeep k;
    k.value = value, k.gen = gen;
    const size_t st = (size_t)stride();
    for (size_t i = 0; i < n; i += st) {
        if (late_mark && late_mark[slots[i]]) continue;
        k.pages.push_back(pages[i]), k.slots.push_back(slots[i]);
    }
    std::lock_guard lk(mu);
    keeps.push_back(std::move(k));
    while (keeps.size() > 96) keeps.pop_front();
}

void lag_poll_late() {
    if (!lag_on()) return;
    for (;;) {
        CapKeep k;
        {
            std::lock_guard lk(mu);
            bool found = false;
            for (auto it = keeps.begin(); it != keeps.end();) {
                it->age++;
                if (it->r_done && gpu_done(it->value) && !found) {
                    k = std::move(*it);
                    it = keeps.erase(it);
                    found = true;
                    continue;
                }
                ++it;
            }
            if (!found) return;
        }
        LagCmp c;
        memory_lag_compare(k.gen, k.pages.data(), k.slots.data(), k.pages.size(), c);
        const int64_t t = now(), f = frames();
        std::lock_guard lk(mu);
        write_row("G,%llu,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", (unsigned long long)k.value, (long long)c.sampled,
                  (long long)c.differ[0], (long long)c.differ[1], (long long)c.differ[2], (long long)c.differ[3], (long long)c.skipped,
                  (long long)c.ns, (long long)t, (long long)f);
    }
}

void lag_wait(uint64_t fence, uint64_t timeout_ns, int result, int64_t t0, int64_t t1, uint64_t rec_seq, int path, uint64_t done_at_exit) {
    if (!lag_on()) return;
    const uint64_t rc = rec_completed_fn ? rec_completed_fn() : 0;
    std::lock_guard lk(mu);
    write_row("W,%llu,%llu,%d,%lld,%lld,%llu,%d,%llu,%llu\n", (unsigned long long)fence, (unsigned long long)std::min<uint64_t>(timeout_ns, 1ull << 62),
              result, (long long)t0, (long long)t1, (unsigned long long)rec_seq, path, (unsigned long long)done_at_exit, (unsigned long long)rc);
}

void lag_finish(int64_t t0, int64_t t1, uint64_t q_values, uint64_t completed, uint64_t pushed, int blocked) {
    if (!lag_on()) return;
    std::lock_guard lk(mu);
    write_row("F,%lld,%lld,%llu,%llu,%llu,%d\n", (long long)t0, (long long)t1, (unsigned long long)q_values, (unsigned long long)completed,
              (unsigned long long)pushed, blocked);
}

void lag_present() {
    if (!lag_on()) return;
    static int n;
    if (++n % 600 != 0) return;
    if (counts_fn) {
        int64_t c[24] = {};
        counts_fn(c);
        logf("vk: lag host counts: sync waits %lld (never fenced %lld, fenced %lld, acquire syncs %lld; satisfied %lld, already %lld, timed out %lld; "
             "polls %lld), event reads %lld, queue finishes %lld, fence syncs %lld, held flushes by queue flush %lld finish %lld present %lld sync wait "
             "%lld event poll %lld presenting submit %lld thread %lld other %lld",
             (long long)c[0], (long long)c[1], (long long)c[2], (long long)c[3], (long long)c[4], (long long)c[5], (long long)c[6],
             (long long)c[7], (long long)c[8], (long long)c[9], (long long)c[10], (long long)c[11], (long long)c[12], (long long)c[13],
             (long long)c[14], (long long)c[15], (long long)c[16], (long long)c[17], (long long)c[18]);
        std::lock_guard lk(mu);
        write_row("X");
        for (int i = 0; i < 19; i++) write_row(",%lld", (long long)c[i]);
        write_row("\n");
    }
}

}  // namespace mkwvk
