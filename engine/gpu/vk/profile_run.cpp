// (heylisten) The detailed performance profile's runtime: see profile_run.h and profile_report.h.
#include "profile_run.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace mkwvk {
namespace {

namespace pf = profile_report;

// ---- the per-map counts (taken by perf_report.cpp) ----

std::mutex mu;   // the maps, their names, the frames' verdicts. Taken by the presenting thread, the sampler (outside a suspend) and the GPU's collect
std::vector<std::unique_ptr<pf::MapProfile>> maps;
std::map<std::string, uint16_t> map_ids;
constexpr uint16_t kNoMap = 0xFFFF;
std::atomic<uint16_t> cur_map{kNoMap};
std::atomic<uint64_t> frames_closed{0};   // (the index of the frame in progress)
pf::FrameVerdicts verdicts;
std::atomic<bool> running{false};
int run_hz = pf::kHzDefault;
bool gpu_ok = false;   // (the timestamp pool exists: maps say so in their gpu flag)

void fresh_map(pf::MapProfile& m, std::string name) {   // (by value: the name may be the map's own, which the reset clears)
    m = pf::MapProfile();
    m.name = std::move(name), m.hz = run_hz, m.gpu = gpu_ok;
}

// ---- the GPU's pass timestamps ----

constexpr int kSlots = 48;                 // (vk_replay.cpp's kRecordings)
constexpr uint32_t kQ = 512;               // timestamps a recording's command buffer can take
constexpr uint32_t kSpans = kQ / 2;
VkQueryPool pool = VK_NULL_HANDLE;
uint64_t ts_mask = ~0ull;
struct TSpan {
    uint32_t qb = 0, qe = 0;
    uint8_t kind = 0;
    uint16_t map = kNoMap;
    uint64_t key = 0, frame = 0;
};
struct TSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t nq = 0, ns = 0, open_qb = 0;
    bool open = false;
    uint16_t open_map = kNoMap;
    uint64_t open_frame = 0;
    TSpan spans[kSpans];
};
TSlot* slots = nullptr;
std::atomic<int64_t> passes_timed{0}, passes_dropped{0};
constexpr uint64_t kGpuLagFrames = 1;   // (a pass recorded in frame f runs while f + 1 is shown: its time counts there)

int slot_of(VkCommandBuffer cmd) {
    for (int i = 0; i < kSlots; i++)
        if (slots[i].cmd == cmd) return i;
    return -1;
}

bool make_pool() {
    if (pool) return true;
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> q(n);
    vkGetPhysicalDeviceQueueFamilyProperties(dev.physical, &n, q.data());
    if (!dev.vk12.hostQueryReset) return false;   // (the pool is reset from the host after a read-back)
    if (dev.queue_family >= n || !q[dev.queue_family].timestampValidBits || dev.props.limits.timestampPeriod <= 0) return false;   // (no timestamps on the queue: no gpu block)
    const uint32_t bits = q[dev.queue_family].timestampValidBits;
    ts_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = kSlots * kQ;
    if (vkCreateQueryPool(dev.device, &qi, nullptr, &pool) != VK_SUCCESS) {
        pool = VK_NULL_HANDLE;
        return false;
    }
    vkResetQueryPool(dev.device, pool, 0, kSlots * kQ);
    slots = new TSlot[kSlots];
    return true;
}

std::atomic<bool> gpu_on{false};

// ---- the CPU sampler (Windows) ----

#ifdef _WIN32
struct ThreadRec {
    DWORD tid = 0;
    HANDLE h = nullptr;
    int role = pf::kRoleOther;
    ULONG64 cycles = 0;
};
struct SampledPc {
    uintptr_t pc = 0;
    uint32_t tid = 0;
    uint8_t role = 0;
};
constexpr int kMaxThreads = 256, kMaxPerTick = 128;

std::atomic<DWORD> registered_tid[16];
std::atomic<uint8_t> registered_role[16];
std::atomic<int> registered_n{0};

std::thread sampler;
std::atomic<bool> stop_sampler{false};
std::atomic<int64_t> n_samples{0}, n_ticks{0}, n_busy_ns{0}, n_unknown{0}, n_dropped{0};
std::vector<pf::Range> ranges;   // (sampler thread only)
HMODULE exe_module = nullptr, vk_module = nullptr;
int64_t modules_at_ns = 0;

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string thread_name(HANDLE h) {
    PWSTR d = nullptr;
    char name[128] = "";
    if (SUCCEEDED(GetThreadDescription(h, &d)) && d) WideCharToMultiByte(CP_UTF8, 0, d, -1, name, sizeof name, nullptr, nullptr);
    if (d) LocalFree(d);
    return name;
}

std::vector<uint32_t> guest_tids;   // (the guest's own threads, from the host once a refresh: their names are the game's)

int role_for(DWORD tid, HANDLE h) {
    if (std::find(guest_tids.begin(), guest_tids.end(), (uint32_t)tid) != guest_tids.end()) return pf::kRoleGame;
    const int n = registered_n.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < 16; i++)
        if (registered_tid[i].load(std::memory_order_relaxed) == tid) return registered_role[i].load(std::memory_order_relaxed);
    return pf::role_of_name(thread_name(h));
}

void refresh_modules() {
    modules_at_ns = now_ns();
    HMODULE mods[1024];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    std::vector<pf::Range> r;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; i++) {
        MODULEINFO mi{};
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        pf::Range range;
        range.lo = (uintptr_t)mi.lpBaseOfDll, range.hi = range.lo + mi.SizeOfImage;
        range.exe = mods[i] == exe_module, range.vk = mods[i] == vk_module;
        if (range.exe) range.bucket = pf::kHost;
        else if (range.vk) range.bucket = pf::kRenderer;
        else {
            char base[MAX_PATH] = "";
            K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], base, sizeof base);
            range.bucket = pf::module_bucket(base, GetProcAddress(mods[i], "vk_icdGetInstanceProcAddr") != nullptr);
        }
        r.push_back(range);
    }
    std::sort(r.begin(), r.end(), [](const pf::Range& a, const pf::Range& b) { return a.lo < b.lo; });
    ranges.swap(r);
}

struct Where {
    uint8_t module = pf::kOther, key_kind = 0;
    uint64_t key = 0;
};
Where classify(uintptr_t pc) {
    Where w;
    const pf::Range* r = pf::range_of(ranges, pc);
    if (!r && now_ns() - modules_at_ns > 1000000000LL) {   // (a module loaded since: looked for at most once a second)
        refresh_modules();
        r = pf::range_of(ranges, pc);
    }
    if (!r) return w;
    w.module = (uint8_t)r->bucket;
    if (r->exe) {
        uint64_t key = 0;
        const int b = host && host->profile_pc ? host->profile_pc(pc, &key) : pf::kHost;
        w.module = (uint8_t)b;
        if (b == pf::kGameLifted) w.key_kind = 1, w.key = key;
        else w.key_kind = 2, w.key = pf::engine_key(key, false);
    } else if (r->vk) {
        DWORD64 image = 0;
        const PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)pc, &image, nullptr);
        const uintptr_t start = f ? (uintptr_t)(image + f->BeginAddress) : (pc & ~(uintptr_t)15);
        w.key_kind = 2, w.key = pf::engine_key(start - r->lo, true);
    }
    return w;
}

void refresh_threads(std::vector<ThreadRec>& threads) {
    const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    guest_tids.assign(256, 0);
    guest_tids.resize(host && host->profile_threads ? (size_t)std::max(0, host->profile_threads(guest_tids.data(), 256)) : 0);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    std::vector<DWORD> alive;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) alive.push_back(te.th32ThreadID);
    CloseHandle(snap);
    for (size_t i = 0; i < threads.size();) {   // (gone: closed)
        if (std::find(alive.begin(), alive.end(), threads[i].tid) == alive.end()) {
            CloseHandle(threads[i].h);
            threads[i] = threads.back(), threads.pop_back();
        } else i++;
    }
    for (DWORD tid : alive) {
        auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRec& t) { return t.tid == tid; });
        if (it == threads.end()) {
            if (threads.size() >= (size_t)kMaxThreads) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            if (!h) continue;
            ThreadRec t;
            t.tid = tid, t.h = h;
            threads.push_back(t);
            it = threads.end() - 1;
        }
        it->role = role_for(tid, it->h);   // (a name set after the thread started is seen at the next refresh)
    }
}

/// One sample: the thread stopped, its program counter read, the thread running again. Nothing else happens in between: no
/// allocation, no log, no lock (the thread may hold any of them)
void sample_thread(ThreadRec& t, SampledPc& out) {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    out.pc = 0, out.tid = t.tid, out.role = (uint8_t)t.role;
    if (SuspendThread(t.h) == (DWORD)-1) return;
    const BOOL got = GetThreadContext(t.h, &ctx);
    ResumeThread(t.h);
    if (got) out.pc = (uintptr_t)ctx.Rip;
}

void sampler_main() {
    SetThreadDescription(GetCurrentThread(), L"mkw_vk profile sampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);   // (a thread it stopped must not wait for a busy core to be let go)
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    const int64_t period_ns = 1000000000LL / std::max(1, run_hz);
    pf::SampleRing ring;
    std::vector<ThreadRec> threads;
    int64_t next_refresh = 0, next = now_ns();
    SampledPc buf[kMaxPerTick];
    refresh_modules();
    // (the thread's cycles for one sample's share of CPU time: this thread's own cycles over a short busy spell, once)
    ULONG64 every = 4000000;
    {
        ULONG64 c0 = 0, c1 = 0;
        const int64_t s0 = now_ns();
        if (QueryThreadCycleTime(GetCurrentThread(), &c0)) {
            volatile double sink = 0;
            while (now_ns() - s0 < 4000000) sink = sink + 1.0;
            const int64_t s1 = now_ns();
            if (QueryThreadCycleTime(GetCurrentThread(), &c1) && c1 > c0 && s1 > s0)
                every = (ULONG64)((double)(c1 - c0) / (double)(s1 - s0) * (double)period_ns * 0.9);
        }
    }
    while (!stop_sampler.load(std::memory_order_acquire)) {
        next += period_ns;
        const int64_t wait = next - now_ns();
        if (wait > 0 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(wait / 100);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, (DWORD)(wait / 1000000 + 20));
        } else if (wait > 0) Sleep((DWORD)std::max<int64_t>(1, wait / 1000000));
        else next = now_ns();   // (late: no catching up)
        if (stop_sampler.load(std::memory_order_acquire)) break;
        const int64_t t0 = now_ns();
        if (t0 >= next_refresh) next_refresh = t0 + 1000000000LL, refresh_threads(threads);
        const uint64_t frame = frames_closed.load(std::memory_order_acquire);
        const uint16_t map = cur_map.load(std::memory_order_relaxed);
        int n = 0;
        for (ThreadRec& t : threads) {
            // (one sample for each 1/Hz of the thread's own CPU time: a thread that works half the time gets half the samples, and
            // a waiting one none, so the samples are proportional to CPU time and the sampler's own cost follows the cores that are busy)
            ULONG64 c = 0;
            if (!QueryThreadCycleTime(t.h, &c) || c - t.cycles < every) continue;
            t.cycles = c - t.cycles > 2 * every ? c - every : t.cycles + every;   // (what is over carries to the next tick)
            if (n >= kMaxPerTick) break;
            sample_thread(t, buf[n]);
            if (buf[n].pc) n++;
        }
        for (int i = 0; i < n; i++) {   // (the threads run again: now it may allocate and look things up)
            const Where w = classify(buf[i].pc);
            pf::Sample s;
            s.frame = frame, s.map = map, s.role = buf[i].role, s.module = w.module, s.key_kind = w.key_kind, s.key = w.key, s.tid = buf[i].tid;
            if (w.module == pf::kOther) n_unknown++;
            ring.push(s);
        }
        n_samples += n;
        {   // (the samples of the frames that closed settle, with their hitch verdicts)
            std::lock_guard lk(mu);
            const uint64_t closed = verdicts.closed;
            ring.take_closed(closed, [&](const pf::Sample& s) {
                if (s.map >= maps.size()) return;
                const bool hitch = verdicts.is_hitch(s.frame);
                pf::MapProfile& m = *maps[s.map];
                m.add_cpu(s.role, s.module, s.tid, hitch);
                if (s.key_kind == 1) m.guest.add(s.key, hitch);
                else if (s.key_kind == 2) m.engine.add(s.key, hitch);
            });
        }
        n_ticks++;
        n_busy_ns += now_ns() - t0;
    }
    n_dropped += (int64_t)ring.dropped;
    for (ThreadRec& t : threads) CloseHandle(t.h);
    if (timer) CloseHandle(timer);
}
#endif   // _WIN32

}   // namespace

bool profile_running() { return running.load(std::memory_order_relaxed); }

bool profile_start(int hz) {
    if (running.load()) return true;
    run_hz = hz;
    {
        std::lock_guard lk(mu);
        for (auto& m : maps) fresh_map(*m, m->name);
    }
    gpu_ok = make_pool();
    gpu_on.store(gpu_ok, std::memory_order_release);
    bool cpu = false;
#ifdef _WIN32
    exe_module = GetModuleHandleA(nullptr);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&profile_running, &vk_module);
    stop_sampler.store(false);
    sampler = std::thread(sampler_main);
    cpu = true;
#endif
    running.store(cpu || gpu_ok, std::memory_order_release);
    logf("vk: performance profile on: CPU sampler %s (%d Hz), GPU pass timestamps %s", cpu ? "running" : "not built here", hz,
         gpu_ok ? "running" : "not available on this queue");
    return cpu || gpu_ok;
}

void profile_stop() {
    if (!running.exchange(false)) return;
    gpu_on.store(false, std::memory_order_release);
#ifdef _WIN32
    stop_sampler.store(true, std::memory_order_release);
    if (sampler.joinable()) sampler.join();
#endif
    logf("vk: performance profile off: %s", profile_stats().c_str());
}

std::string profile_stats() {
    char b[256];
#ifdef _WIN32
    snprintf(b, sizeof b, "%lld samples in %lld ticks (%.0f us of the sampler's time a tick), %lld in no known module, %lld dropped; %lld passes timed, %lld not",
             (long long)n_samples.load(), (long long)n_ticks.load(), n_ticks.load() ? n_busy_ns.load() / 1e3 / (double)n_ticks.load() : 0.0,
             (long long)n_unknown.load(), (long long)n_dropped.load(), (long long)passes_timed.load(), (long long)passes_dropped.load());
#else
    snprintf(b, sizeof b, "%lld passes timed, %lld not", (long long)passes_timed.load(), (long long)passes_dropped.load());
#endif
    return b;
}

void profile_map(const std::string& name) {
    std::lock_guard lk(mu);
    const auto it = map_ids.find(name);
    if (it != map_ids.end()) return cur_map.store(it->second, std::memory_order_relaxed);
    if (maps.size() >= pf::kMaxMaps) return cur_map.store(kNoMap, std::memory_order_relaxed);
    maps.push_back(std::make_unique<pf::MapProfile>());
    fresh_map(*maps.back(), name);
    map_ids.emplace(name, (uint16_t)(maps.size() - 1));
    cur_map.store((uint16_t)(maps.size() - 1), std::memory_order_relaxed);
}

void profile_frame(double ms, bool hitch) {
    std::lock_guard lk(mu);
    const uint16_t c = cur_map.load(std::memory_order_relaxed);
    if (c < maps.size()) {
        pf::MapProfile& m = *maps[c];
        m.seconds += ms / 1000.0, m.frames++, m.frame_ms_sum += ms;
        if (hitch) m.hitch_frames++;
    }
    verdicts.close(hitch);
    frames_closed.store(verdicts.closed, std::memory_order_release);
}

std::vector<pf::MapProfile> profile_take(bool final) {
    std::vector<pf::MapProfile> out;
    std::unique_lock lk(mu, std::defer_lock);
    if (final) {
        if (!lk.try_lock()) return out;
    } else lk.lock();
    for (auto& m : maps)
        if (m->any()) {
            out.push_back(*m);
            fresh_map(*m, m->name);
        }
    return out;
}

void profile_register_thread(int role) {
#ifdef _WIN32
    const DWORD tid = GetCurrentThreadId();
    const int n = registered_n.load(std::memory_order_acquire);
    for (int i = 0; i < n && i < 16; i++)
        if (registered_tid[i].load(std::memory_order_relaxed) == tid) return;
    if (n >= 16) return;
    registered_tid[n].store(tid, std::memory_order_relaxed), registered_role[n].store((uint8_t)role, std::memory_order_relaxed);
    registered_n.store(n + 1, std::memory_order_release);
#else
    (void)role;
#endif
}

uint64_t profile_build_id() { return host && host->build_id ? host->build_id() : 0; }

// ---- the GPU's pass timestamps ----

bool tprof_on() { return gpu_on.load(std::memory_order_relaxed); }

uint64_t tprof_output_pixels() { return (uint64_t)dev.swap_extent.width * dev.swap_extent.height; }

void tprof_bind(int slot, VkCommandBuffer cmd) {
    if (!tprof_on() || slot < 0 || slot >= kSlots) return;
    TSlot& s = slots[slot];
    s.cmd = cmd, s.nq = s.ns = 0, s.open = false;
}

bool tprof_begin(VkCommandBuffer cmd) {
    if (!tprof_on()) return false;
    const int i = slot_of(cmd);
    if (i < 0) return false;
    TSlot& s = slots[i];
    if (s.open || s.nq + 2 > kQ || s.ns >= kSpans) {
        if (!s.open) passes_dropped++;
        return false;
    }
    s.open_qb = s.nq++;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, (uint32_t)i * kQ + s.open_qb);
    s.open = true;
    s.open_map = cur_map.load(std::memory_order_relaxed);
    s.open_frame = frames_closed.load(std::memory_order_relaxed);
    return true;
}

void tprof_end(VkCommandBuffer cmd, int kind, uint64_t key) {
    if (!tprof_on()) return;
    const int i = slot_of(cmd);
    if (i < 0 || !slots[i].open) return;
    TSlot& s = slots[i];
    const uint32_t qe = s.nq++;
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, (uint32_t)i * kQ + qe);
    TSpan& sp = s.spans[s.ns++];
    sp.qb = s.open_qb, sp.qe = qe, sp.kind = (uint8_t)kind, sp.key = key, sp.map = s.open_map, sp.frame = s.open_frame;
    s.open = false;
}

void tprof_end_label(VkCommandBuffer cmd, const std::string& label) {
    if (!tprof_on()) return;
    const bool fi = label.compare(0, 7, "fsr3fi ") == 0, up = !fi && label.compare(0, 5, "fsr3 ") == 0;
    tprof_end(cmd, fi ? pf::kFramegen : up ? pf::kUpscale : pf::kOtherPass, pf::name_key(label));
}

void tprof_collect(int slot) {
    if (!pool || slot < 0 || slot >= kSlots) return;
    TSlot& s = slots[slot];
    if (!s.nq) return;
    const uint32_t nq = s.nq, ns = s.ns;
    s.nq = s.ns = 0, s.open = false;
    std::vector<uint64_t> t(nq);
    const bool got = ns && vkGetQueryPoolResults(dev.device, pool, (uint32_t)slot * kQ, nq, nq * 8, t.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    vkResetQueryPool(dev.device, pool, (uint32_t)slot * kQ, nq);
    if (!got) return;
    const double period = dev.props.limits.timestampPeriod;
    std::lock_guard lk(mu);
    for (uint32_t i = 0; i < ns; i++) {
        const TSpan& sp = s.spans[i];
        if (sp.qb >= nq || sp.qe >= nq || sp.map >= maps.size()) continue;
        const uint64_t d = (t[sp.qe] - t[sp.qb]) & ts_mask;
        const double ms = (double)d * period / 1e6;
        if (t[sp.qe] == t[sp.qb] || ms > 1000.0) continue;   // (a timestamp that never ran, or a wrap of the clock)
        maps[sp.map]->add_pass(sp.kind, sp.key, ms, verdicts.is_hitch(sp.frame + kGpuLagFrames));
        passes_timed++;
    }
}

}   // namespace mkwvk
