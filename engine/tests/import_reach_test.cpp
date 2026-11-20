// Game-free call recorder for vk_memory.cpp's scans down the import table (tests/test_import_reach.py). No Vulkan library or
// device, no host mapping, no game data: imports are recorded ranges, never payload bytes, so imports of several GiB cost
// nothing. The types and bodies between the markers are cut out of the source under test.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#if HAVE_REACH
#include "gpu/vk/import_reach.h"
#endif

namespace mkwvk {
static unsigned checks;
static void check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
using VmaAllocation = void*;
constexpr uint64_t GiB = 1ull << 30, MiB = 1ull << 20, base = 0x1000000000ull, chunk_bytes = 16 * MiB;
struct { VkDevice device{}; bool external_memory_host = true; } dev;
struct Host {
    uint8_t* (*gpu_ptr)(uint64_t, uint64_t*);
    uint64_t (*gpu_guest)(uint64_t);
};
static uint8_t* gpu_ptr(uint64_t, uint64_t*) { return nullptr; }
static uint64_t gpu_guest(uint64_t gpu) { return gpu; }
static Host host_value{gpu_ptr, gpu_guest}, *host = &host_value;
static void logf(const char*, ...) {}
static bool prefetch_on;   // (RX_VK_PREFETCH_CHUNKS=1: read once, at prefetch_chunks' first call)
static const char* getenv(const char* name) { return prefetch_on && !std::strcmp(name, "RX_VK_PREFETCH_CHUNKS") ? "1" : nullptr; }
static bool env_on(const char*) { return false; }
static int64_t import_now_ms() { return 1000; }

/* TYPES */

// ---- what the extracted bodies use, recorded ----
struct Allocation { uint64_t lo, hi; VkDeviceMemory memory; bool live; };
static std::vector<Allocation> allocations;
static unsigned overlapping_imports;   // (imports made while a live one already held some of the same bytes)
static uint64_t next_handle = 1;
static bool import_pool(uint64_t lo, uint64_t hi, Pool& p) {
    for (const Allocation& a : allocations)
        if (a.live && a.lo < hi && lo < a.hi) ++overlapping_imports;
    p.lo = lo, p.hi = hi;
    p.memory = reinterpret_cast<VkDeviceMemory>(next_handle);
    p.buffer = reinterpret_cast<VkBuffer>(next_handle);
    p.address = next_handle++ << 40;
    allocations.push_back({lo, hi, p.memory, true});
    return true;
}
static void vkFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    for (Allocation& a : allocations)
        if (a.live && a.memory == memory) a.live = false;
}
static void destroy_import_buffer(VkBuffer) {}
static std::vector<std::array<uint64_t, 3>> table_sets;   // (guest_pages_set calls: lo, hi, address)
static void guest_pages_set(uint64_t lo, uint64_t hi, uint64_t address) { table_sets.push_back({lo, hi, address}); }
static void shadow_free(Pool&) {}
static uint64_t import_chunk() { return chunk_bytes; }
static uint64_t import_span() { return 0; }   // (RX_VK_IMPORT_SPAN_MB=0: the planned range is the chunks of the binding alone)
static unsigned bg_starts;
static void bg_start() { ++bg_starts; }
static void rb_hazard_locked(uint64_t, uint64_t) {}
// (HeyListen's own: the host's no-access ranges, the pool mirror, the trace and the inherit watch, all off or recorded)
enum { MKWVK_TRACE_IMPORT_MADE = 1 };
constexpr unsigned kPageShift = 12;
static void trace_gpu(int, uint64_t, uint64_t, uint64_t, uint64_t) {}
static void clip_noaccess(uint64_t&, uint64_t&, uint64_t, uint64_t) {}
static void noaccess_import_note(uint64_t, uint64_t, uint64_t, uint64_t) {}
static bool pool_mirror_on() { return false; }
static uint64_t inherit_watch_page() { return ~0ull; }
static uint64_t gen_now = 0;
static bool retiring_finalized = false;
static int64_t st_pm_made = 0;
static void vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}

static std::vector<GuestPool> guest_pools;
static std::map<uint64_t, Pool> imported;
static std::deque<std::map<uint64_t, Pool>::node_type> retired_pools, evicted_pools;
static std::atomic<int64_t> st_retired_walk_ns{0};   // (vk_memory.cpp: the time in the walks over retired_pools, for the imports line)
static Pool* last_pool;
static Pool* pool_cache[4096];
static std::atomic<uint64_t> pools_gen{1};
static std::mutex pools_mu, bg_mu;
static std::condition_variable bg_cv, bg_done_cv;
static std::deque<std::pair<uint64_t, uint64_t>> bg_queue;
static std::map<uint64_t, uint64_t> bg_inflight;
static std::vector<std::pair<uint64_t, Pool>> bg_ready;
struct DeadImport { uint64_t lo, bytes; };
static std::deque<DeadImport> dead_imports;
static int64_t st_bg_waits, st_prefetch_sent, st_prefetch_used, st_prefetch_dropped, st_async_sent, st_async_copies;
static std::vector<Pool*> mirror_wanted, sparse_wanted;
static uint64_t mem_rec_value = 7, mirror_epoch = 10;
static int failures;
static thread_local bool import_cooling;
static thread_local VkResult import_result = VK_SUCCESS;
static Pool* pool_lookup(uint64_t, uint64_t);
static void retire_pool(std::map<uint64_t, Pool>::iterator);

/* ACTUAL */

static void reset() {
    imported.clear(), retired_pools.clear(), evicted_pools.clear(), bg_ready.clear(), bg_queue.clear(), bg_inflight.clear();
    allocations.clear(), table_sets.clear();
    overlapping_imports = bg_starts = 0;
    last_pool = nullptr;
    std::fill(std::begin(pool_cache), std::end(pool_cache), nullptr);
    guest_pools = {{base, base + 8 * GiB}};
#if HAVE_REACH
    live_reach = {};
#endif
}
static unsigned live_allocations() {
    unsigned n = 0;
    for (const Allocation& a : allocations) n += a.live;
    return n;
}
#if HAVE_REACH
static void reach_counts_the_table() {   // (live_reach is exactly the spans in `imported`)
    import_reach::Reach r;
    for (const auto& [lo, p] : imported) r.add(p.hi - p.lo);
    check(std::equal(std::begin(r.count), std::end(r.count), std::begin(live_reach.count)) && r.top == live_reach.top,
          "live_reach does not count exactly the imports in the table");
}
#else
static void reach_counts_the_table() {}
#endif

// The merge escape leaves a 4 GiB import A over the 16 MiB chunk C inside it: a state the source allows today
// (pool_lookup: "would take in ... pools: left overlapping"); and a game pool above 1 GiB, imported whole, is another
static Pool* large_over_small() {
    reset();
    Pool* c = pool_of(base + GiB, 4096);
    check(c && c->lo == base + GiB && c->hi == base + GiB + chunk_bytes, "the first binding's chunk");
    Pool* a = pool_of(base, 4 * GiB);
    check(a && a != c && a->lo == base && a->hi == base + 4 * GiB, "a 4 GiB binding imported whole");
    check(imported.size() == 2 && overlapping_imports == 1, "the source's merge escape left the large import over the small one");
    reach_counts_the_table();
    return a;
}
// The same table, put in directly (no lookup, so no prefetch_chunks call reads RX_VK_PREFETCH_CHUNKS before the case sets it)
static void put(uint64_t lo, uint64_t hi) {
    Pool p;
    check(import_pool(lo, hi, p), "fixture import");
#if HAVE_REACH
    imported_add(p);
#else
    imported.emplace(p.lo, p);
#endif
}

static void run(const char* only) {
    const auto want = [only](const char* name) { return !only || !std::strcmp(only, name); };
    if (want("lookup")) {   // A lookup in A's range 2 GiB past C: the scan must not stop at C and import the same bytes again. (pool_lookup itself:
        // pool_of's caches hold A from the binding that made it and would answer before the table is looked at)
        Pool* a = large_over_small();
        const size_t before = allocations.size();
        Pool* hit = pool_lookup(base + 3 * GiB, 4096);
        check(hit == a, "a lookup behind a small import missed the 4 GiB import covering it");
        check(allocations.size() == before && overlapping_imports == 1, "the bytes of a live import above 1 GiB were imported again");
        reach_counts_the_table();
    }
    if (want("query")) {   // The informational query reaches A past C
        large_over_small();
        check(memory_guest_imported(base + 3 * GiB, base + 3 * GiB + 4096) == 1, "memory_guest_imported missed a live import above 1 GiB");
    }
    if (want("drain")) {   // An import the background thread finished, inside A: dropped, not published over A
        large_over_small();
        Pool b;
        check(import_pool(base + 3 * GiB, base + 3 * GiB + chunk_bytes, b), "fixture background import");
        b.prefetched = true;
        bg_ready.push_back({b.lo, b});
        const size_t tables = imported.size();
        bg_drain();
        check(imported.size() == tables && live_allocations() == 2, "a background import inside a live import above 1 GiB was published");
        reach_counts_the_table();
    }
    if (want("background")) {   // A read binding inside A is covered: nothing goes to the background importer
        large_over_small();
        check(!import_in_background(base + 3 * GiB, 4096) && bg_queue.empty() && !bg_starts,
              "import_in_background sent an import for bytes a live import above 1 GiB holds");
    }
    if (want("prefetch")) {   // RX_VK_PREFETCH_CHUNKS=1: the chunk just above A is imported, and its neighbour below, inside A, is not sent
        reset();
        prefetch_on = true;
        put(base + GiB, base + GiB + chunk_bytes);
        put(base, base + 4 * GiB);
        reach_counts_the_table();
        Pool* d = pool_lookup(base + 4 * GiB + 64, 64);
        check(d && d->lo == base + 4 * GiB && d->hi == base + 4 * GiB + chunk_bytes && overlapping_imports == 1,
              "the chunk above the 4 GiB import, imported on its own");
        check(bg_queue.size() == 1 && bg_queue[0].first == base + 4 * GiB + chunk_bytes && st_prefetch_sent == 1,
              "prefetch_chunks sent a chunk a live import above 1 GiB holds");
        reach_counts_the_table();
    }
    if (want("retire")) {   // Retirement takes the span out: the bound comes back down once the large import leaves
        large_over_small();
        retire_pool(imported.find(base));
        reach_counts_the_table();
        check(imported.size() == 1, "fixture: A retired");
#if HAVE_REACH
        check(live_reach.bound() == 32 * MiB, "the reach of the table did not shrink to the 16 MiB chunk left");
#endif
        // Disjoint chunks again: a lookup two chunks above C stops at C (its own reach) and imports its chunk
        Pool* far = pool_lookup(base + GiB + 2 * chunk_bytes, 4096);
        check(far && far->lo == base + GiB + 2 * chunk_bytes && imported.size() == 2, "a disjoint chunk after the large import left");
        reach_counts_the_table();
    }
#if HAVE_REACH
    if (want("header")) {   // The header alone: classes, bound, the stop rule, removal of what was never added
        import_reach::Reach r;
        check(r.bound() == 1 && import_reach::beyond(r, 0, 1), "an empty table reaches nothing");
        r.add(4096); r.add(16 * MiB); r.add(41747087360ull);   // (a 38.9 GiB import: one device's max_import_size)
        check(r.bound() == 64 * GiB, "the bound of a 41,747,087,360-byte import is 64 GiB");
        check(!import_reach::beyond(r, base, base + 41747087360ull) && import_reach::beyond(r, base, base + 64 * GiB),
              "the stop rule at the largest import's end");
        check(!import_reach::beyond(r, base + 8, base), "an address below the start is never beyond");
        r.remove(41747087360ull);
        check(r.bound() == 32 * MiB, "the bound after the largest import left");
        r.remove(3 * GiB);   // (never added: ignored)
        check(r.bound() == 32 * MiB, "removing a span never added lowered the bound");
        r.add(UINT64_MAX);
        check(r.bound() == UINT64_MAX && !import_reach::beyond(r, 0, UINT64_MAX - 1), "a span of class 64: the scan never stops early");
    }
#endif
    std::printf("PASS %s: %u import-reach checks\n", only ? only : "all", checks);
}
} // namespace mkwvk
int main(int argc, char** argv) { mkwvk::run(argc > 1 ? argv[1] : nullptr); }
