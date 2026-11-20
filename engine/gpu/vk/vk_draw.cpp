// Draws and dispatches: a program's pipeline layout (its stages' descriptors in Eden's order, as the SPIR-V back end
// numbered them, one push-descriptor set when they fit), graphics pipelines (whatever Vulkan 1.3 and
// VK_EXT_extended_dynamic_state3 take as dynamic state is dynamic; the rest is the key), each draw's descriptors
// (driver banks and c1 from the recording's stream, user uniform and storage buffers in the imported pools, textures
// and storage images as views of vk_texture.cpp's images, texel buffers), passes kept open while the targets stay,
// and the draws themselves.
//
// Pipelines persist in a VkPipelineCache next to the executable (mkw_vk_pipelines.bin).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <exception>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <thread>
#include <setjmp.h>
#include <signal.h>
#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>   // (compile_gate.h trims: a long compile's freed memory handed back)
#endif

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir_opt/psychov35.h"   // (PsychoV35's grading bank: uniform_range)

#include "compat/hookshot_env.h"   // (env_value_off: the switches' one rule)
#include "vk_paths.h"
#include "vk_draw.h"
#include "flat_map.h"
#include "perf_report.h"   // (zone_note_program / zone_note_state: the per-zone key recorder, once a key per map)
#include "dlss_actor.h"
#include "vk_temporal.h"
#include "vk_fsr3.h"          // (AMD FSR 3.1 in the gameplay upscale's place)
#include "vk_fsr3_inputs.h"
#include "vk_dump.h"
#include "vk_gprof.h"
#include "profile_run.h"   // (the performance profile's pass timestamps: tprof_end)
#include "vk_jobs.h"
#include "vk_shader.h"
#include "vk_menu.h"     // (psychov35_bank: PsychoV35's grading for its bank, uniform_range)
#include "vk_texture.h"
#include "vk_ledger.h"   // (RX_VK_LEDGER)
#include "ubo_memo.h"    // (HL_UBO_MEMO)
#include "predict_limits.h"   // (what the prediction of pipelines holds itself to)
#include "compile_gate.h"     // (Longshot c392df3a: how many of the driver's compiles run at once)
#include "movie_upscale_policy.h"
#include "skip_code.h"

#include "shaders/clear_f_frag_spv.h"
#include "shaders/clear_i_frag_spv.h"
#include "shaders/clear_u_frag_spv.h"
#include "shaders/clear_vert_spv.h"
#include "shaders/taau_upscale_comp_spv.h"
#include "shaders/taau_upscale_rgba_comp_spv.h"
#include "shaders/movie_upscale_comp_spv.h"
#include "shader_recompiler/backend/spirv/mma_layout.h"
#include "precompile_save.h"   // (the pipeline cache written before the title after a long precompile)
#include "prepare_work.h"   // (what a program of the seed costs a preparation: its progress bar)
#include "vk_work.h"   // (after the others: counts the commands that do GPU work, for full_barrier)

namespace mkwvk {
// (EvilOOT, RX_VK_DRAW_STATS: the pointer table's regions, vk_bank.cpp)
struct PointerEntry { uint64_t addr, size; };
const PointerEntry* pointer_table(const NvnState& s, int stage);
uint32_t stage_cbuf_word(const NvnState& s, const Stage& st, uint32_t index, uint32_t offset);   // (vk_shader.cpp)
uint64_t sampler_registrations_generation();   // (vk_bank.cpp: sampler_for's cache)
namespace {
// ---- (hookshot) this file's per-thread objects that need a constructor or a destructor ----
// With llvm-mingw every use of such a thread_local calls the file's TLS init function, which saves registers, finds the
// thread's TLS block and checks a guard byte however long ago the thread set them up: ~2.4% of the replay thread on a
// Ryzen 9 3900X in Kokiri Forest (HOS_SAMPLE). Each one is reached through a plain thread_local pointer instead, which needs
// no init function: TLS_OBJECT(name, type) makes name(), the calling thread's object, made on its first use and deleted when
// the thread ends. Only the list that owns them has a destructor, and only a thread's first use of each object touches it.
// A new per-thread object of a type with a constructor or destructor belongs here too (tests/test_tls_objects.py checks).
// (They go when the thread ends, with the list: no other thread_local's destructor may use them.)
struct TlsOwned {
    void* object;
    void** slot;
    void (*del)(void*);
};
std::vector<TlsOwned>& tls_owned() {
    struct List {
        std::vector<TlsOwned> v;
        ~List() {
            for (auto it = v.rbegin(); it != v.rend(); ++it) it->del(it->object), *it->slot = nullptr;
        }
    };
    static thread_local List list;
    return list.v;
}
template <class T>
T& tls_make(T*& slot) {
    slot = new T();   // (value-initialized: zeroed as a thread_local's static storage was)
    tls_owned().push_back({slot, reinterpret_cast<void**>(&slot), [](void* p) { delete static_cast<T*>(p); }});
    return *slot;
}
#define TLS_OBJECT(name, ...)                                                         \
    thread_local __VA_ARGS__* name##_tls = nullptr;                                   \
    inline __VA_ARGS__& name() {                                                      \
        __VA_ARGS__* p = name##_tls;                                                  \
        return p ? *p : tls_make(name##_tls);                                         \
    }

// ---- (hookshot) RX_VK_PROF_FINE=1: where a draw's time goes, finer than the frame line's phases, and what each draw does ----
// Off, each site is a test of one constant. On, a timed site reads the clock twice (~15 ns; the frame line's phases grow by the same, so
// compare a run with it on to another with it on) and the sums are logged with the frame statistics, "a frame (fine)": the times in ms a
// frame, the counts a draw. FT_*: the sections of the descriptor build (banks, c1, user uniform buffers, storage buffers, textures) and
// of the commands (hazards and the pass, pipeline bind, descriptor push, push constants, dynamic state, vertex and index binds, the draw)
enum FineT { FT_BANK, FT_C1, FT_UBO, FT_STORAGE, FT_TEX, FT_HAZARD, FT_BINDPL, FT_BINDDESC, FT_PUSHCALL, FT_PUSHC, FT_DYN, FT_VERTEX,
             FT_DRAWCALL, FT_PROGRAM, FT_PIPELINE, FT_TEXMISS, FT_COUNT };
const char* const kFineT[] = {"banks", "c1", "user uniform buffers", "storage buffers", "textures", "hazards+pass", "bind pipeline",
                                      "bind descriptors", "of it the push call", "push constants", "dynamic state", "vertex+index binds",
                                      "draw call", "program lookup", "pipeline lookup", "of textures: slot misses (view and sampler lookups)"};
enum FineC { FC_DRAWS, FC_BUILDS, FC_DESCS, FC_WRITES, FC_BANK_CALLS, FC_BANK_GEN, FC_BANK_FILLS, FC_BANK_FILL_BYTES, FC_BANK_SAME,
             FC_BANK_NEW, FC_BANK_NEW_BYTES, FC_C1_CALLS, FC_UBO_CALLS, FC_UBO_OVERLAY, FC_UBO_STREAM, FC_PUSH_FULL, FC_PUSH_DELTA,
             FC_PUSH_NONE, FC_PUSH_WRITES, FC_PUSH_DESCS, FC_BIND_PIPE, FC_DYN_FAST, FC_DYN_FULL, FC_DYN_CALLS, FC_VBIND, FC_IBIND,
             FC_PUSHC, FC_PIPE_FAST, FC_PIPE_MAP, FC_POOL_SETS, FC_POOL_DESCS, FC_SSBO_DESCS, FC_TEX_HIT, FC_TEX_MISS, FC_COUNT };
const char* const kFineC[] = {"draws", "descriptor builds", "descriptors", "descriptor writes", "bank calls",
                                      "bank kept by its stage's generation", "bank fills", "bank bytes filled", "bank refills equal",
                                      "bank copies into the stream", "bank bytes copied", "c1 calls", "user uniform buffer calls",
                                      "uniform ranges copied with an update's overlay", "uniform ranges copied into the stream",
                                      "full pushes", "pushes of changed writes only", "pushes of nothing (all as pushed)",
                                      "writes pushed", "descriptors pushed", "pipeline binds", "dynamic state from the draw before",
                                      "dynamic state worked out", "dynamic state commands", "vertex stream binds", "index buffer binds",
                                      "push constants", "pipeline keys from the fast pair", "pipeline keys from the thread's map",
                                      "sets allocated (over the push limit)", "descriptors in them", "storage buffer descriptors built",
                                      "texture slots kept (a handle's view, tex and sampler)", "texture slots resolved again"};
static_assert(std::size(kFineT) == FT_COUNT && std::size(kFineC) == FC_COUNT, "every section and count is named");
const bool g_fine = getenv("RX_VK_PROF_FINE") != nullptr;
int64_t fine_ns[FT_COUNT], fine_n[FC_COUNT];
struct FineScope {
    int ph;
    int64_t t0;
    explicit FineScope(int p) : ph(p), t0(g_fine ? prof_now() : 0) {}
    ~FineScope() { if (g_fine) fine_ns[ph] += prof_now() - t0; }
    FineScope(const FineScope&) = delete;
    FineScope& operator=(const FineScope&) = delete;
};
#define FINE_SCOPE(ph) FineScope fine_scope_##ph(ph)
#define FINE_N(c, n) do { if (g_fine) fine_n[c] += (n); } while (0)
#define FINE_X(c, n) (g_fine ? (void)(fine_n[c] += (n)) : (void)0)   // (the same, as an expression)
/// The sums since the last report, as a frame's times and a draw's counts (the caller: every 120 frames)
void fine_report(double frames) {
    if (!g_fine) return;
    static int64_t last_ns[FT_COUNT], last_n[FC_COUNT];
    char line[1400];
    int k = snprintf(line, sizeof line, "vk: a frame (fine): ms:");
    for (int i = 0; i < FT_COUNT; i++) {
        k += snprintf(line + k, sizeof line - k, " %s %.2f%s", kFineT[i], (fine_ns[i] - last_ns[i]) / frames / 1e6, i + 1 < FT_COUNT ? "," : "");
        last_ns[i] = fine_ns[i];
    }
    logf("%s", line);
    const double draws = std::max<double>(1.0, (double)(fine_n[FC_DRAWS] - last_n[FC_DRAWS]));
    k = snprintf(line, sizeof line, "vk: a frame (fine): %.0f draws; a draw:", draws / frames);
    for (int i = 1; i < FC_COUNT; i++) {
        k += snprintf(line + k, sizeof line - k, " %s %.2f%s", kFineC[i], (fine_n[i] - last_n[i]) / draws, i + 1 < FC_COUNT ? "," : "");
        if (k > 700) {
            logf("%s", line);
            k = snprintf(line, sizeof line, "vk: a frame (fine): a draw:");
        }
    }
    logf("%s", line);
    for (int i = 0; i < FC_COUNT; i++) last_n[i] = fine_n[i];
}

bool no_a2c() {   // (RX_VK_NO_A2C, read once: it was a getenv on every alpha-to-coverage draw, the foliage's)
    static const bool v = getenv("RX_VK_NO_A2C") != nullptr;
    return v;
}

/// 32 lanes, where the stage can be held to it (workgroup_x: a compute stage's width, which full subgroups must divide)
void pin_subgroup(VkPipelineShaderStageCreateInfo& st, uint32_t workgroup_x = 0) {
    static const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo size = [] {
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo r{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
        r.requiredSubgroupSize = 32;
        return r;
    }();
    if (!(dev.sg32_stages & st.stage)) return;
    st.pNext = &size;
    if (workgroup_x && !(workgroup_x % 32) && dev.vk13.computeFullSubgroups)
        st.flags |= VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
}

inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

// ---- environment switches, read once ----
struct Switches {
    bool no_depth = getenv("RX_VK_NODEPTH") != nullptr;
    bool no_cull = getenv("RX_VK_NOCULL") != nullptr;
    bool ignore_discard = getenv("RX_VK_IGNORE_DISCARD") != nullptr;
    bool log_draws = getenv("RX_VK_LOG_DRAWS") != nullptr;
};
const Switches& sw() {
    static const Switches s;
    return s;
}

// (EvilOOT, RX_VK_DRAW_STATS) the storage buffers bound in the measured frame, with their programs: who writes what
struct StatsBind {
    uint64_t prog, addr, size;
    int stage;
    bool written, table;
    uint32_t cbuf = 0, off = 0;
    uint64_t raw = 0;
    uint64_t vkbuf = 0, vkoff = 0;   // (the Vulkan buffer and offset the binding resolved to: two pools over one range show here)
};
std::mutex stats_bind_mu;
std::vector<StatsBind> stats_binds;
// (and the GPU addresses the small storage buffers of each dispatch hold: pointers its program may follow)
struct StatsPtr { uint64_t prog, in_buffer, value; };
std::vector<StatsPtr> stats_ptrs;
std::vector<StatsPtr> stats_ptrs2;   // (second level: in the pointer table's regions, covered by nothing the dispatch has)
// (RX_VK_DRAW_STATS_PROGS: those programs' uniform and storage buffers as the dispatch saw them, hex, once each)
std::vector<std::string> stats_dumps;
// (and as the GPU held them after the wanted draws: 32 slots of up to 1 MB each; StatsQuery::gpu_dump)
constexpr uint32_t kGpuDumpSlot = 1u << 20, kGpuDumpSlots = 32;
VkBuffer stats_gpu = VK_NULL_HANDLE;
VmaAllocation stats_gpu_mem = nullptr;
uint8_t* stats_gpu_map = nullptr;
struct GpuDumpRec { uint64_t prog, addr; uint32_t slot, bytes; int stage, index; };
std::vector<GpuDumpRec> stats_gpu_recs;
// (RX_VK_MISS_LOG=1: each program's slice of one host-visible buffer, its c0+0xff0 descriptor: the global memory
// accesses no binding held, counted, the first 63 addresses kept; translate_program.cpp, spirv_emit_context.cpp)
// (RX_VK_BLOCK_PROBE: 16 KB slices, the per-block entry counters at word 128 + block, emit_spirv.cpp Traverse)
const uint32_t kMissSlice = getenv("RX_VK_BLOCK_PROBE") ? 16384u : 512u, kMissSlices = getenv("RX_VK_BLOCK_PROBE") ? 1024u : 4096u;
VkBuffer miss_buf = VK_NULL_HANDLE;
VmaAllocation miss_mem = nullptr;
uint32_t* miss_map = nullptr;
std::mutex miss_mu;
std::unordered_map<uint64_t, uint32_t> miss_slot;   // program key -> slice
VkDescriptorBufferInfo miss_binding(uint64_t prog) {
    std::lock_guard lk(miss_mu);
    if (!miss_buf) {
        void* m = nullptr;
        if (!create_buffer((uint64_t)kMissSlice * kMissSlices, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, miss_buf, miss_mem, &m))
            return {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        miss_map = (uint32_t*)m;
        std::memset(miss_map, 0, (size_t)kMissSlice * kMissSlices);
    }
    auto it = miss_slot.find(prog);
    if (it == miss_slot.end()) {
        if (miss_slot.size() >= kMissSlices) return {miss_buf, 0, kMissSlice};   // (full: all share the first)
        it = miss_slot.emplace(prog, (uint32_t)miss_slot.size()).first;
    }
    return {miss_buf, (VkDeviceSize)it->second * kMissSlice, kMissSlice};
}
/// (RX_VK_DRAW_STATS_PROGS) a stage's uniform and storage buffers as recorded, hex (first 1 KB each), once per program
void stats_dump_stage(uint64_t prog, const NvnState& s, int stage) {
    static std::mutex dm;
    static std::unordered_set<uint64_t> done;
    static int64_t done_frame = -1;   // (once per program per measured frame)
    {
        std::lock_guard lk(dm);
        const int64_t f = (int64_t)texture_frame_no();
        if (f != done_frame) done.clear(), done_frame = f;
        if (!done.insert(prog ^ ((uint64_t)stage << 56)).second) return;
    }
    const auto dump = [&](const char* kind, int slot, const nvn_p_bind_buf& b) {
        if (!b.addr || !b.size) return;
        // (uniform buffers whole up to 2 KB and with the stream's inline updates, ub_read: what the shader reads)
        const bool ubo = kind[0] == 'u';
        // (RX_VK_DUMP_LIVE=1: storage buffers as guest memory holds them now, not as submitted: what the CPU wrote since)
        static const bool live = getenv("RX_VK_DUMP_LIVE") != nullptr;
        const auto read = [&](uint64_t a, void* dst, uint64_t bytes) {
            return ubo ? ub_read(a, dst, bytes) : live ? host->gpu_read(a, dst, bytes) : memory_read_as_submitted(a, dst, bytes);
        };
        const uint64_t n = std::min<uint64_t>(b.size, ubo ? 0x800 : 0x400) & ~15ull;
        std::vector<uint32_t> w(n / 4);
        if (w.empty() || !read(b.addr, w.data(), n)) return;
        uint64_t nonzero = 0, total = 0;   // (over the whole binding, up to 16 MB: is anything in it at all)
        uint64_t first_nz = UINT64_MAX;    // (and where the data past the first 64 bytes starts, 64 words of it shown)
        std::vector<uint32_t> at_nz;
        {
            const uint64_t all = std::min<uint64_t>(b.size, 16ull << 20) & ~3ull;
            std::vector<uint32_t> big(all / 4);
            if (!big.empty() && read(b.addr, big.data(), all))
                for (size_t i = 0; i < big.size(); i++) {
                    nonzero += big[i] != 0, total++;
                    if (big[i] && i >= 16 && first_nz == UINT64_MAX) first_nz = i * 4;
                }
            if (first_nz != UINT64_MAX && first_nz >= 0x400)
                at_nz.assign(big.begin() + first_nz / 4, big.begin() + std::min<size_t>(big.size(), first_nz / 4 + 64));
        }
        std::string text;
        char line[240];
        snprintf(line, sizeof line, "%016llx s%d %s%d at %#llx size %#llx (game memory: %llu of %llu words non-zero; first past "
                 "+0x40 at %s%llx):\n", (unsigned long long)prog, stage, kind, slot, (unsigned long long)b.addr,
                 (unsigned long long)b.size, (unsigned long long)nonzero, (unsigned long long)total, first_nz == UINT64_MAX ? "none " : "+",
                 (unsigned long long)(first_nz == UINT64_MAX ? 0 : first_nz));
        text += line;
        for (size_t i = 0; i < w.size(); i += 4) {
            snprintf(line, sizeof line, "  +%03zx: %08x %08x %08x %08x\n", i * 4, w[i], w[i + 1], w[i + 2], w[i + 3]);
            text += line;
        }
        for (size_t i = 0; i + 4 <= at_nz.size(); i += 4) {
            snprintf(line, sizeof line, "  +%06llx: %08x %08x %08x %08x\n", (unsigned long long)(first_nz + i * 4), at_nz[i], at_nz[i + 1],
                     at_nz[i + 2], at_nz[i + 3]);
            text += line;
        }
        std::lock_guard bl(stats_bind_mu);
        if (stats_dumps.size() < 256) stats_dumps.push_back(text);
    };
    for (int i = 0; i < 16; i++) dump("ubo", i, s.ubo[stage][i]);
    for (int i = 0; i < 16; i++) dump("ssbo", i, s.ssbo[stage][i]);
}
bool stats_prog_wanted(uint64_t key) {
    static const std::vector<uint64_t> want = [] {
        std::vector<uint64_t> v;
        for (const char* q = getenv("RX_VK_DRAW_STATS_PROGS"); q && *q;) {
            char* end;
            const uint64_t k = strtoull(q, &end, 16);
            if (end == q) break;
            v.push_back(k);
            q = *end ? end + 1 : end;
        }
        return v;
    }();
    return std::find(want.begin(), want.end(), key) != want.end();
}
bool stats_frame_now();   // (below: the frame being measured is the current one)
/// (EvilOOT) RX_VK_SKIP_FILE=<file>: the draws of the programs listed in it (hex keys, comma or newline separated) left out,
/// the file read again whenever it changes (checked every 300 ms): a bisection in one run, from the scene being looked at
bool skip_live(uint64_t key) {
    static const char* path = getenv("RX_VK_SKIP_FILE");
    if (!path || !*path) return false;
    static std::mutex mu;
    static std::vector<uint64_t> keys;
    static std::atomic<int64_t> next_check{0};
    static int64_t last_size = -1, last_time = -1;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::lock_guard lk(mu);
    if (now >= next_check.load(std::memory_order_relaxed)) {
        next_check.store(now + 300, std::memory_order_relaxed);
        // (size and write time, portably: the Linux build has no GetFileAttributesExA)
        std::error_code ec;
        const auto fsize = std::filesystem::file_size(path, ec);
        const auto ftime = ec ? std::filesystem::file_time_type{} : std::filesystem::last_write_time(path, ec);
        if (!ec) {
            const int64_t size = (int64_t)fsize;
            const int64_t time = (int64_t)ftime.time_since_epoch().count();
            if (size != last_size || time != last_time) {
                last_size = size, last_time = time;
                keys.clear();
                if (FILE* f = fopen(path, "rb")) {
                    std::string text;
                    char buf[4096];
                    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
                    fclose(f);
                    for (const char* q = text.c_str(); *q;) {
                        char* e;
                        const uint64_t k = strtoull(q, &e, 16);
                        if (e == q) { q++; continue; }
                        keys.push_back(k);
                        q = e;
                    }
                    std::sort(keys.begin(), keys.end());
                }
                logf("vk: RX_VK_SKIP_FILE: %zu programs left out now", keys.size());
            }
        } else if (!keys.empty()) {
            keys.clear(), last_size = last_time = -1;
        }
    }
    return std::binary_search(keys.begin(), keys.end(), key);
}
void stats_note_bind(uint64_t prog, int stage, uint64_t addr, uint64_t size, bool written, bool table, uint32_t cbuf = 0,
                     uint32_t off = 0, uint64_t raw = 0, uint64_t vkbuf = 0, uint64_t vkoff = 0) {
    if (!stats_frame_now()) return;
    std::lock_guard lk(stats_bind_mu);
    if (stats_binds.size() < 200000) stats_binds.push_back({prog, addr, size, stage, written, table, cbuf, off, raw, vkbuf, vkoff});
}

/// The recording being built (each recording thread's own)
thread_local VkCommandBuffer rec_cmd = VK_NULL_HANDLE;
thread_local uint64_t rec_value = 0;

// ---- per recording: a descriptor pool and an upload stream (reset when the recording is reused) ----
struct RecordingRes {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> extra;   // (pools added when the first ran out)
    // (the stream in use: the first, or one a recording that outgrew it was moved to. Descriptors written before the move hold
    // the old buffer's range, so the old ones stay until the slot's next recording, which the GPU's use of this one is done by)
    VkBuffer stream = VK_NULL_HANDLE;
    VmaAllocation stream_alloc = nullptr;
    uint8_t* mapped = nullptr;
    uint64_t used = 0, size = 0;
    struct Stream { VkBuffer buffer; VmaAllocation alloc; uint8_t* mapped; uint64_t size; };
    std::vector<Stream> streams;   // (the first, then the ones made on demand: kept for the slot's next recordings)
    size_t si = 0;                 // (the one in use)
    uint64_t before = 0;           // (what the recording used of the streams before this one)
    uint32_t idle = 0;             // (recordings of the slot that stayed in the first stream)
    // (hookshot: flat (flat_map.h): an unordered_map put a malloc into every c1 upload kept, and a free into the next recording's clear)
    FlatMap<uintptr_t, std::pair<VkBuffer, uint64_t>> c1_at;   // (c1 of a stage, by the stage's address: uploaded once a recording)
    // (a uniform buffer copied as submitted (RX_VK_UB_SNAPSHOT), by its address and size: the copy serves the recording's later
    // draws of it while the capture view is the same, as a read then would give the same bytes. RX_VK_UB_SNAPSHOT_REUSE=0: a copy
    // for every binding. Hookshot a5028dc5, #1795)
    struct UbSnap { uint64_t addr, n, view; VkBuffer buf; uint64_t at; };
    std::unordered_map<uint64_t, UbSnap> ub_snap;
};
// (a recording's first stream: one for each of the kRecordings slots. RX_VK_STREAM_MB=<n>: that size, 2 by default; a recording
// that needs more is given a bigger stream for the rest of it (RX_VK_STREAM_GROW_OFF=1: draws are skipped instead, and the default
// is 24 MB as before); the most a recording used is logged: "the recording streams")
bool stream_grow_off() {
    static const bool off = getenv("RX_VK_STREAM_GROW_OFF") != nullptr;
    return off;
}
const uint64_t kStream = (getenv("RX_VK_STREAM_MB") ? (uint64_t)std::max(1, atoi(getenv("RX_VK_STREAM_MB"))) : stream_grow_off() ? 24u : 2u) << 20;
std::atomic<uint64_t> stream_peak{0}, stream_extra{0}, stream_grown{0};
std::deque<RecordingRes> res;   // (a deque: a recording thread keeps its own in place while another is added)
thread_local RecordingRes* cur = nullptr;

VkDescriptorPool make_pool() {
    // (each recording's pool: 8192 sets of up to 32768 descriptors a type, for the programs with more descriptors than the push limit,
    // 24 or 48 pools at once, and the driver may keep host memory for all of that. A pool that runs out is followed by another of the same
    // size (cur->extra), so a smaller one is safe, and costs a pool made and destroyed per recording in a scene that needs more.
    // RX_VK_DESC_POOL_DIV=<n> (1..32): a n-th of both)
    static const uint32_t div = getenv("RX_VK_DESC_POOL_DIV") ? (uint32_t)std::clamp(atoi(getenv("RX_VK_DESC_POOL_DIV")), 1, 32) : 1;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768 / div}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16384 / div},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768 / div}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096 / div},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048 / div}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 2048 / div}};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 8192 / div;
    ci.poolSizeCount = (uint32_t)std::size(sizes);
    ci.pPoolSizes = sizes;
    VkDescriptorPool p = VK_NULL_HANDLE;
    vkCreateDescriptorPool(dev.device, &ci, nullptr, &p);
    return p;
}

constexpr VkBufferUsageFlags kStreamUse = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;   // (vertex too: a binding served from a copy, gpu_buffer_bind)
/// The recording goes on in its next stream (made when the slot has none yet, twice the last one's size up to 32 MB, or the size asked)
bool stream_next(uint64_t need) {
    RecordingRes& r = *cur;
    if (r.si + 1 < r.streams.size() && r.streams[r.si + 1].size >= need) {
        r.si++;
    } else {
        if (stream_grow_off()) return false;
        RecordingRes::Stream s{};
        s.size = std::max<uint64_t>((need + (1u << 20) - 1) & ~((1ull << 20) - 1), std::min<uint64_t>(r.size * 2, 32ull << 20));
        void* mapped = nullptr;
        MemKindScope mem_kind_(MemKind::Stream);   // (TICK-038: RX_VK_RES_LOG)
        if (!create_buffer(s.size, kStreamUse, true, s.buffer, s.alloc, &mapped, true) || !mapped) return false;
        s.mapped = (uint8_t*)mapped;
        stream_extra += s.size, stream_grown++;
        static int logged;
        if (logged++ < 8)
            logf("vk: a recording needed more than its %llu MB stream: one of %llu MB made (%llu MB of them in all)",
                 (unsigned long long)(r.size >> 20), (unsigned long long)(s.size >> 20), (unsigned long long)(stream_extra.load() >> 20));
        // (one made while the old ones stay: a bigger one that fits here goes before the smaller ones kept after it)
        r.streams.insert(r.streams.begin() + (ptrdiff_t)++r.si, s);
    }
    r.before += r.used;
    r.stream = r.streams[r.si].buffer, r.stream_alloc = r.streams[r.si].alloc, r.mapped = r.streams[r.si].mapped;
    r.size = r.streams[r.si].size, r.used = 0;
    return true;
}
/// Room for n bytes at align in the recording's stream, moving it to a bigger one when there is none (false: no room)
bool stream_reserve(uint64_t n, uint64_t align) {
    if (((cur->used + align - 1) & ~(align - 1)) + n <= cur->size) return true;
    if (stream_next(n + align)) return true;
    static int logged;
    if (logged++ < 4) logf("vk: the recording's stream is full (%llu MB)", (unsigned long long)(cur->size >> 20));
    return false;
}
/// Space in the recording's stream (UINT64_MAX when there is none; the buffer is cur->stream and cur->mapped after the call)
uint64_t stream_alloc(uint64_t n, uint64_t align) {
    if (!stream_reserve(n, align)) return UINT64_MAX;
    const uint64_t at = (cur->used + align - 1) & ~(align - 1);
    cur->used = at + n;
    return at;
}

VkBuffer zero_buffer = VK_NULL_HANDLE;   // (attributes the program reads that no stream supplies; 64 KB of zeros)
VmaAllocation zero_alloc = nullptr;

// ---- NVN state to Vulkan ----
VkCompareOp compare_op(uint32_t f) {
    return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS;   // (NEVER = 1 .. ALWAYS = 8)
}
VkStencilOp stencil_op(uint32_t o) {
    return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP;   // (KEEP = 1 .. DECR_WRAP = 8)
}
VkBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO;
    case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(uint32_t e) {
    switch (e) {
    case NVN_BLEND_EQUATION_SUB: return VK_BLEND_OP_SUBTRACT;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case NVN_BLEND_EQUATION_MIN: return VK_BLEND_OP_MIN;
    case NVN_BLEND_EQUATION_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
/// Vulkan topology (false: one Vulkan has none of: quads, quad strips, polygons, line loops)
bool topology(int prim, VkPrimitiveTopology& t) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
    case NVN_DRAW_PRIMITIVE_LINES: t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLES: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
    case NVN_DRAW_PRIMITIVE_POLYGON: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
    case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY; return true;
    case NVN_DRAW_PRIMITIVE_PATCHES: {
        static int logged;
        if (logged++ < 5) logf("vk: a draw with patches");
        t = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        return true;
    }
    case NVN_DRAW_PRIMITIVE_QUADS: t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;   // (indices made: draw())
    default: return false;
    }
}
int topology_class(VkPrimitiveTopology t) {
    switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 0;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return 1;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return 3;
    default: return 2;
    }
}
/// NVN vertex attribute format -> Vulkan (gpu/render/rx_draw.cpp nvn_attribute's table)
VkFormat vertex_format(int f) {
    static const VkFormat r8[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT};
    static const VkFormat r16[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT};
    static const VkFormat r32[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT};
    static const VkFormat rg8[] = {VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT};
    static const VkFormat rg16[] = {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT};
    static const VkFormat rg32[] = {VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT};
    static const VkFormat rgb8[] = {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8_SINT};
    static const VkFormat rgb16[] = {VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16_SINT};
    static const VkFormat rgb32[] = {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32_SINT};
    static const VkFormat rgba8[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT};
    static const VkFormat rgba16[] = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT};
    static const VkFormat rgba32[] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT};
    static const struct { int lo; const VkFormat* f; } ranges[] = {
        {0x01, r8}, {0x05, r16}, {0x0A, r32}, {0x0D, rg8}, {0x11, rg16}, {0x16, rg32},
        {0x19, rgb8}, {0x1D, rgb16}, {0x22, rgb32}, {0x25, rgba8}, {0x29, rgba16}, {0x2E, rgba32}};
    static const int counts[] = {4, 5, 3, 4, 5, 3, 4, 5, 3, 4, 5, 3};
    for (int i = 0; i < 12; i++)
        if (f >= ranges[i].lo && f < ranges[i].lo + counts[i]) return ranges[i].f[f - ranges[i].lo];
    if (f >= 0x51 && f <= 0x68) {   // (*_UI2F / *_I2F: integers converted to float by the shader (UnsignedScaled /
        // SignedScaled attributes): the 32-bit ones were read as float bits before, and R32 has no scaled format)
        static const VkFormat us[12] = {VK_FORMAT_R8_UINT, VK_FORMAT_R16_UINT, VK_FORMAT_R32_UINT, VK_FORMAT_R8G8_UINT,
                                        VK_FORMAT_R16G16_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R8G8B8_UINT,
                                        VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R8G8B8A8_UINT,
                                        VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R32G32B32A32_UINT};
        static const VkFormat ss[12] = {VK_FORMAT_R8_SINT, VK_FORMAT_R16_SINT, VK_FORMAT_R32_SINT, VK_FORMAT_R8G8_SINT,
                                        VK_FORMAT_R16G16_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R8G8B8_SINT,
                                        VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R8G8B8A8_SINT,
                                        VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R32G32B32A32_SINT};
        return ((f - 0x51) & 1 ? ss : us)[(f - 0x51) / 2];
    }
    switch (f) {
    case NVN_FORMAT_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case NVN_FORMAT_RGB10A2UI: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case NVN_FORMAT_RGB10A2SN: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case NVN_FORMAT_RGB10A2I: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case NVN_FORMAT_RGB10A2_UI2F: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case NVN_FORMAT_RGB10A2_I2F: return VK_FORMAT_A2B10G10R10_SINT_PACK32;
    case NVN_FORMAT_R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case NVN_FORMAT_BGRA8: case NVN_FORMAT_BGRX8: return VK_FORMAT_B8G8R8A8_UNORM;
    case NVN_FORMAT_RGBX8: return VK_FORMAT_R8G8B8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}
/// 3-component 8/16-bit vertex formats are not vertex formats everywhere: read 4 (the 4th past the attribute)
VkFormat supported_vertex_format(VkFormat f) {
    static std::mutex mu;   // (pipelines are built on workers)
    std::lock_guard lk(mu);
    static std::unordered_map<int, VkFormat> known;
    auto it = known.find((int)f);
    if (it != known.end()) return it->second;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, f, &p);
    VkFormat out = f;
    if (!(p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) {
        switch (f) {
        case VK_FORMAT_R8G8B8_UNORM: out = VK_FORMAT_R8G8B8A8_UNORM; break;
        case VK_FORMAT_R8G8B8_SNORM: out = VK_FORMAT_R8G8B8A8_SNORM; break;
        case VK_FORMAT_R8G8B8_UINT: out = VK_FORMAT_R8G8B8A8_UINT; break;
        case VK_FORMAT_R8G8B8_SINT: out = VK_FORMAT_R8G8B8A8_SINT; break;
        case VK_FORMAT_R8G8B8_USCALED: out = VK_FORMAT_R8G8B8A8_USCALED; break;
        case VK_FORMAT_R8G8B8_SSCALED: out = VK_FORMAT_R8G8B8A8_SSCALED; break;
        case VK_FORMAT_R16G16B16_SFLOAT: out = VK_FORMAT_R16G16B16A16_SFLOAT; break;
        case VK_FORMAT_R16G16B16_UNORM: out = VK_FORMAT_R16G16B16A16_UNORM; break;
        case VK_FORMAT_R16G16B16_SNORM: out = VK_FORMAT_R16G16B16A16_SNORM; break;
        case VK_FORMAT_R16G16B16_UINT: out = VK_FORMAT_R16G16B16A16_UINT; break;
        case VK_FORMAT_R16G16B16_SINT: out = VK_FORMAT_R16G16B16A16_SINT; break;
        case VK_FORMAT_R16G16B16_USCALED: out = VK_FORMAT_R16G16B16A16_USCALED; break;
        case VK_FORMAT_R16G16B16_SSCALED: out = VK_FORMAT_R16G16B16A16_SSCALED; break;
        default: break;
        }
    }
    known[(int)f] = out;
    return out;
}

// ---- pipeline layouts and pipelines ----
// (+ the guest page table's address, a task draw's first, and the fallback's two buffer addresses)
constexpr uint32_t kPushSize = Shader::Backend::SPIRV::MESH_FALLBACK_PUSH_END;

void ensure_layout(Program& p) {
    if (p.layout) return;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    uint64_t total = 0;   // (64-bit: a sum of 32-bit counts)
    auto add = [&](VkDescriptorType type, VkShaderStageFlags stage, const auto& descriptors) {
        for (const auto& d : descriptors) {
            bindings.push_back({(uint32_t)bindings.size(), type, d.count, stage, nullptr});
            p.binding_types.push_back(type);
            total += d.count;
        }
    };
    for (const Stage& st : p.stages) {   // (Eden's order, as the SPIR-V back end numbered them)
        add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, st.vk_stage, st.info.constant_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, st.vk_stage, st.info.storage_buffers_descriptors);
        add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, st.vk_stage, st.info.texture_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, st.vk_stage, st.info.image_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, st.vk_stage, st.info.texture_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, st.vk_stage, st.info.image_descriptors);
    }
    p.push = dev.push_descriptor && vkCmdPushDescriptorSetKHR && total <= dev.max_push_descriptors;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (p.push) li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    li.bindingCount = (uint32_t)bindings.size();
    li.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &p.set_layout);
    VkShaderStageFlags stages = 0;
    for (const Stage& st : p.stages) stages |= st.vk_stage;
    const VkPushConstantRange range{stages, 0, kPushSize};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &p.set_layout;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &range;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout);
    p.push_stages = stages;
    for (const Stage& st : p.stages) p.uses_render_area |= st.info.uses_render_area;
    p.uses_push = p.uses_render_area;
    for (const Stage& st : p.stages) p.uses_push |= st.info.uses_rescaling_uniform;
    static const bool follow_graphics = !getenv("RX_VK_FOLLOW_GRAPHICS") || atoi(getenv("RX_VK_FOLLOW_GRAPHICS")) != 0;
    for (const Stage& st : p.stages)   // (the page table; EvilOOT: and graphics stages with RX_VK_FOLLOW_GRAPHICS)
        p.uses_push |= ((st.vk_stage & (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) ||
                        follow_graphics) &&
                       st.info.uses_global_memory;
    for (const Stage& st : p.stages) p.uses_push |= (st.vk_stage & VK_SHADER_STAGE_TASK_BIT_EXT) != 0;   // (its first workgroup)
    if (p.fallback) p.uses_push = true;   // (its records' and payloads' addresses)
    std::atomic_thread_fence(std::memory_order_release);   // (the layout last: a thread that sees it sees the rest)
    p.layout = layout;
}

// ---- the pipeline cache, on disk ----
VkPipelineCache pcache = VK_NULL_HANDLE;
namespace { void ledger_drop_all(); }
bool pcache_loaded = false;   // (TICK-038: it began with this device's and driver's data from the file)
std::string pcache_path;
std::atomic<int64_t> pcache_built_since_save{0};
// (HeyListen) RX_VK_PCACHE_MISSES=0: every pipeline made counts toward a save of the cache, as before
bool pcache_misses_only() {
    static const bool v = !getenv("RX_VK_PCACHE_MISSES") || atoi(getenv("RX_VK_PCACHE_MISSES")) != 0;
    return v;
}
// (hookshot) The size of the cache's data when it was last read or written. The count above takes in every pipeline made
// with the cache, the ones it already held too: the precompile's ~13,000 at each start, all found in the cache, made the
// first check in play (present 300) write the whole cache again, 1.1 GB, at every start, the file the same 1146 MB each
// time. On a SATA SSD that write took 4-10 s and left the disk's reads slow (10-450 ms) for the minutes after, the game's
// own reads too. A save asks the driver the data's size first (no data is copied for that) and writes only a different
// size: data the cache took in since. The pipeline creation feedback cannot tell the new ones apart: NVIDIA's driver reports
// most precompiled pipelines as not found in the app's cache (~460 of ~12,900 here), which it then serves from its own.
std::atomic<size_t> pcache_saved_size{0};
void pcache_init() {
    if (pcache) return;
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    pcache_path = dir + "mkw_vk_pipelines.bin";
    std::vector<uint8_t> data;
    if (FILE* f = fopen(pcache_path.c_str(), "rb")) {
        _fseeki64(f, 0, SEEK_END);
        const int64_t sz = _ftelli64(f);
        _fseeki64(f, 0, SEEK_SET);
        if (sz > 32) {
            data.resize((size_t)sz);
            if (fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
        fclose(f);
    }
    // (data of another device or driver is dropped before Vulkan sees it)
    if (data.size() >= 32) {
        uint32_t head_size, version, vendor, device;
        std::memcpy(&head_size, data.data(), 4);
        std::memcpy(&version, data.data() + 4, 4);
        std::memcpy(&vendor, data.data() + 8, 4);
        std::memcpy(&device, data.data() + 12, 4);
        if (head_size < 32 || head_size > data.size() || version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||   // (hookshot)
            vendor != dev.props.vendorID || device != dev.props.deviceID ||
            std::memcmp(data.data() + 16, dev.props.pipelineCacheUUID, VK_UUID_SIZE))
            data.clear();
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(dev.device, &ci, nullptr, &pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;
        vkCreatePipelineCache(dev.device, &ci, nullptr, &pcache);
    }
    pcache_loaded = pcache && ci.initialDataSize >= 32;
    if (!pcache_loaded && shader_key_on()) ledger_drop_all();   // (what the ledger lists was built into a cache that is not this one)
    // (what it holds as read: a save of data that size has nothing to add to the file)
    size_t held = 0;
    if (pcache && vkGetPipelineCacheData(dev.device, pcache, &held, nullptr) == VK_SUCCESS) pcache_saved_size = held;
    logf("vk: pipeline cache %s (%zu KB)", pcache_path.c_str(), data.size() >> 10);
}

struct Targets {
    Texture* color[8]{};
    VkImageView color_view[8]{};
    VkFormat color_format[8]{};
    uint32_t color_level[8]{}, color_layer[8]{}, depth_level = 0, depth_layer = 0;   // (the levels and layers viewed)
    Texture* depth = nullptr;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    int n = 0;
    uint32_t width = 0, height = 0;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

/// A graphics pipeline: built on a worker (vk_jobs.h); ready once pipeline is set (null: it failed)
struct PipeEntry {
    std::atomic<bool> ready{false};
    std::atomic<VkPipeline> pipeline{VK_NULL_HANDLE};   // (a fast-linked one first, the worker's full build over it)
    std::atomic<const Program*> prog{nullptr};   // (the program object whose stages and layout it was built from: graphics_pipeline)
};
std::unordered_map<uint64_t, std::unique_ptr<PipeEntry>> pipelines;   // (the replay thread's)
// (TICK-052: st_draws to st_mesh are bumped at every draw by every recording thread: a cache line a thread, split_count.h)
SplitCount st_draws, st_skipped, st_dispatches, st_passes, st_mesh;
int64_t st_pending_skips, st_waits;
int64_t st_dispatch_unordered;   // (hookshot: dispatches right after another left without a barrier, RX_VK_GAME_ORDER)
// (hookshot) skips by the line that made them, reported with the frame statistics (draw_skip_report)
namespace {
struct SkipSite { std::atomic<int> line{0}; std::atomic<int64_t> n{0}; int64_t last = 0; };
SkipSite skip_sites[48];
int skip_slot(int line) {
    for (auto& k : skip_sites) {
        int z = 0;
        if (k.line.load() == line || k.line.compare_exchange_strong(z, line) || k.line.load() == line)
            return (int)(&k - skip_sites);
    }
    return 47;
}
}  // namespace
thread_local bool occ_skipped_here = false;   // (TICK-033: a draw left out after its occlusion query began)
#define SKIP_HERE() do { static const int slot_ = skip_slot(__LINE__); skip_sites[slot_].n.fetch_add(1, std::memory_order_relaxed); st_skipped++; occ_skipped_here = true; return; } while (0)
void skip_report_impl(double frames) {
    char line[600];
    int n = snprintf(line, sizeof line, "vk: skips a frame by vk_draw.cpp line:");
    bool any = false;
    for (auto& k : skip_sites) {
        const int64_t v = k.n.load(std::memory_order_relaxed);
        if (k.line.load() && v != k.last && n < (int)sizeof line - 32) {
            n += snprintf(line + n, sizeof line - n, " %d: %.1f", k.line.load(), (v - k.last) / frames);
            any = true;
        }
        k.last = v;
    }
    if (any) logf("%s", line);
}
std::atomic<int64_t> st_pipelines, st_links, st_link_us, st_lib_hits, st_lib_cold, st_lib_built, st_lib_build_us;
std::atomic<int64_t> st_pipe_fresh_skips;   // (TICK-008) draws left out for a pipeline in the building, into targets made this frame
std::atomic<int64_t> st_stolen_skips;   // (hookshot) draws left out for the frame in which their own wait ran their translation
std::atomic<int64_t> st_pipeline_us;
double st_wait_ms;
double st_shader_wait_ms;   // (st_wait_ms's waits for a program's translation; the rest are for its pipeline)

bool depth_aspect(VkFormat f) {
    return is_depth_format(f) && f != VK_FORMAT_S8_UINT;
}

// (hookshot) A driver's compiler that faults on a shader (amdvlk64.dll on a mesh program, Hookshot issue 26) costs that one draw, not the game
#ifdef _WIN32
__declspec(noinline) VkResult guarded_graphics(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo* ci, VkPipeline* pl) {
    __try {
        return vkCreateGraphicsPipelines(dev.device, cache, 1, ci, nullptr, pl);
    } __except (1) {
        return VK_ERROR_UNKNOWN;
    }
}
__declspec(noinline) VkResult guarded_compute_once(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    __try {
        return vkCreateComputePipelines(dev.device, cache, 1, ci, nullptr, pl);
    } __except (1) {
        return (VkResult)-77;   // (a fault in the driver, told apart from what it returns)
    }
}
#else
// (linux: no structured exceptions, so the fault arrives as a signal on the compiling thread: an armed window covers the driver
// call alone, and a fault in it is reported as the call's failure, as the Windows guard above reports an access violation. A fault
// outside it goes to whoever had the signal before, so the game's own crash report is unchanged)
static thread_local sigjmp_buf fault_jmp;
static thread_local volatile sig_atomic_t fault_armed;
static struct sigaction fault_previous[2];
static const int fault_signals[2] = {SIGSEGV, SIGBUS};

static void on_driver_fault(int sig, siginfo_t* si, void* ctx) {
    if (fault_armed) {
        fault_armed = 0;
        siglongjmp(fault_jmp, 1);
    }
    const struct sigaction* p = &fault_previous[sig == SIGSEGV ? 0 : 1];
    if ((p->sa_flags & SA_SIGINFO) && p->sa_sigaction) {
        p->sa_sigaction(sig, si, ctx);
    } else if (p->sa_handler && p->sa_handler != SIG_IGN && p->sa_handler != SIG_DFL) {
        p->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
    }
}

static void install_driver_fault_guard() {
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 2; i++) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_sigaction = on_driver_fault;
            sa.sa_flags = SA_SIGINFO;
            sigemptyset(&sa.sa_mask);
            sigaction(fault_signals[i], &sa, &fault_previous[i]);
        }
    });
}

VkResult guarded_graphics(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo* ci, VkPipeline* pl) {
    install_driver_fault_guard();
    if (sigsetjmp(fault_jmp, 1) != 0) {
        logf("vk: the driver faulted compiling a graphics pipeline: that one is left out");
        return VK_ERROR_UNKNOWN;
    }
    fault_armed = 1;
    const VkResult r = vkCreateGraphicsPipelines(dev.device, cache, 1, ci, nullptr, pl);
    fault_armed = 0;
    return r;
}
VkResult guarded_compute_once(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    install_driver_fault_guard();
    if (sigsetjmp(fault_jmp, 1) != 0) {
        logf("vk: the driver faulted compiling a compute pipeline: that one is left out");
        return (VkResult)-77;
    }
    fault_armed = 1;
    const VkResult r = vkCreateComputePipelines(dev.device, cache, 1, ci, nullptr, pl);
    fault_armed = 0;
    return r;
}
#endif
/// (Hookshot f1eb23c9, Chad-) AMD's proprietary compiler fails (-13) or faults (-77) on some programs, the Market's and Kakariko's
/// dogs among them (RX 6600 XT, driver 0x800161): a pipeline it fails on is built once more with
/// VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT, which it builds, instead of the draws being left out. Not for the memory errors,
/// which the optimizer does not cause, nor for the cold ask's VK_PIPELINE_COMPILE_REQUIRED, which is no failure.
/// RX_VK_NO_UNOPT_RETRY=1: no second build (a failed pipeline stays left out)
static bool retry_without_optimizer(VkResult r, VkPipelineCreateFlags flags) {
    static const bool off = std::getenv("RX_VK_NO_UNOPT_RETRY") != nullptr;
    return !off && dev.props.vendorID == 0x1002 && r != VK_SUCCESS && r != VK_ERROR_OUT_OF_HOST_MEMORY &&
           r != VK_ERROR_OUT_OF_DEVICE_MEMORY && r != VK_PIPELINE_COMPILE_REQUIRED &&
           !(flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT);
}
/// A compute pipeline the driver's compiler fails on is built again without its optimizer, as a graphics one is (build_graphics)
VkResult guarded_compute(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    const VkResult r = guarded_compute_once(cache, ci, pl);
    if (!retry_without_optimizer(r, ci->flags)) return r;
    VkComputePipelineCreateInfo again = *ci;
    again.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    *pl = VK_NULL_HANDLE;
    const VkResult second = guarded_compute_once(cache, &again, pl);
    static std::atomic<int> told{0};
    if (told++ < 20)
        logf("vk: a compute pipeline failed (%d) with the driver's optimizer: %s", (int)r,
             second == VK_SUCCESS ? "made without it" : "not made without it either");
    return second;
}

// (longshot #190) Intel's Windows driver faults in vkGetPipelineCacheData (igvk64.dll, an access violation) when it fetches a
// cache's data while other threads build pipelines with that cache: the game closes with no message. Vulkan allows both at once;
// on Intel the fetches and the builds take turns: a build holds this shared, a fetch alone, around the vkGetPipelineCacheData
// calls only (never across a file's write). The background save does not queue behind the builds: it polls for a moment with no
// build under way and is put off to the next request when there is none; after kFetchPutOffs put-offs in a row it queues, for a
// bounded time, so back-to-back builds cannot put it off for good. A forced save (the prepare, Restart now, the exit) waits.
// RX_VK_PCACHE_FETCH_LOCK=0: off, =1: on any GPU; unset: on for Intel (vendor 0x8086) under Windows only
std::shared_timed_mutex pcache_fetch_mu;
bool pcache_fetch_lock_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_PCACHE_FETCH_LOCK");
#ifdef _WIN32
        return e ? atoi(e) != 0 : dev.props.vendorID == 0x8086;
#else
        return e && atoi(e) != 0;
#endif
    }();
    return on;
}
/// (issue 496) compute pipelines built one at a time, on a thread of their own (jobs_push_serial): Intel's by default.
/// RX_VK_COMPUTE_SERIAL=1 or 0 says so on any GPU
bool serial_builds_on() {
    static const bool on = getenv("RX_VK_COMPUTE_SERIAL") ? getenv("RX_VK_COMPUTE_SERIAL")[0] == '1' : dev.props.vendorID == 0x8086;
    return on;
}
/// (longshot #404) On Intel's one-at-a-time builds a dispatch's first pipeline is made without the optimizer, the optimized one
/// after it (compute_optimize). RX_VK_COMPUTE_QUICK_FIRST=0 builds the optimized one only, as before; =1 does it wherever builds
/// are one at a time (RX_VK_COMPUTE_SERIAL)
bool intel_quick_compute() {
    static const bool on = !getenv("RX_VK_COMPUTE_QUICK_FIRST") || atoi(getenv("RX_VK_COMPUTE_QUICK_FIRST")) != 0;
    return on;
}
thread_local int pcache_fetch_depth = 0;   // (a fetch inside a fetch on the same thread)
constexpr int kFetchPollMs = 2000;    // (the background save's patience, polled)
constexpr int kFetchPutOffs = 3;      // (saves put off in a row before the next one queues)
constexpr int kFetchQueueMs = 10000;  // (how long a queued fetch waits)
/// A fetch of a pipeline cache's data, the builds held off while it lasts (held() says whether it was had).
/// wait_ms < 0: waits until the builds under way are done. queue: waits in the lock, as a writer (new builds wait behind it), at
/// most wait_ms; not queue: polled for wait_ms, had only at a moment no build is under way (a writer waiting in the lock would
/// stall the builds behind it, the replay thread's draw-time asks among them)
struct PcacheFetch {
    bool owns = false, outer = true;
    explicit PcacheFetch(int wait_ms, bool queue = true) {
        if (!pcache_fetch_lock_on()) return;
        if (pcache_fetch_depth > 0) {
            outer = false, pcache_fetch_depth++;
            return;
        }
        if (wait_ms < 0) pcache_fetch_mu.lock(), owns = true;
        else if (queue) owns = pcache_fetch_mu.try_lock_for(std::chrono::milliseconds(wait_ms));
        else
            for (const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
                 !(owns = pcache_fetch_mu.try_lock()) && std::chrono::steady_clock::now() < until;)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (owns) pcache_fetch_depth = 1;
    }
    ~PcacheFetch() {
        if (!outer) pcache_fetch_depth--;
        else if (owns) pcache_fetch_depth = 0, pcache_fetch_mu.unlock();
    }
    PcacheFetch(const PcacheFetch&) = delete;
    PcacheFetch& operator=(const PcacheFetch&) = delete;
    bool held() const { return !pcache_fetch_lock_on() || !outer || owns; }
};
/// A pipeline built with a cache, while no fetch of one is under way (PcacheFetch)
struct PcacheUse {
    bool on = false;
    explicit PcacheUse(VkPipelineCache cache) : on(cache && pcache_fetch_lock_on() && pcache_fetch_depth == 0) {
        if (on) pcache_fetch_mu.lock_shared();
    }
    ~PcacheUse() {
        if (on) pcache_fetch_mu.unlock_shared();
    }
    PcacheUse(const PcacheUse&) = delete;
    PcacheUse& operator=(const PcacheUse&) = delete;
};
/// (the guards with the cache held from a fetch)
/// (longshot #404) A pipeline the driver refused for want of memory (VK_ERROR_OUT_OF_DEVICE_MEMORY or _HOST_MEMORY), counted:
/// texture_frame gives textures up when the count grows (st_driver_out_of_memory)
VkResult note_out_of_memory(VkResult r) {
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY || r == VK_ERROR_OUT_OF_HOST_MEMORY)
        st_driver_out_of_memory.fetch_add(1, std::memory_order_relaxed);
    return r;
}
VkResult guarded_graphics_held(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo* ci, VkPipeline* pl) {
    PcacheUse use(cache);
    return note_out_of_memory(guarded_graphics(cache, ci, pl));
}
VkResult guarded_compute_held(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    PcacheUse use(cache);
    return note_out_of_memory(guarded_compute(cache, ci, pl));
}
/// (longshot #404) Before play on Intel's one-at-a-time builds (the prepare's warm pass, precompile_run): ci asked without the
/// optimizer, the pipeline a dispatch's first build asks for, so that a dispatch finds it in the cache. With the optimizer one of
/// the game's compute pipelines took Intel's compiler up to 389.5 s on a UHD Graphics (i3-1115G4). true: asked so (the optimized
/// build is then owed to the program's first dispatch)
bool precompile_compute_quick(VkComputePipelineCreateInfo& ci) {
    if (!serial_builds_on() || !intel_quick_compute() || (ci.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT)) return false;
    ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    return true;
}
VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part = -1,
                          bool* cold = nullptr);
// (longshot #252) vkCreateShaderModule has no guard of its own. Translation workers call it, and on Windows their stack into
// igvk64 is short: an access violation there ends the process. The same answer as the pipeline guards: that program is left out.
#ifdef _WIN32
__declspec(noinline) VkResult guarded_shader_module(const VkShaderModuleCreateInfo* ci, VkShaderModule* m) {
    __try {
        return vkCreateShaderModule(dev.device, ci, nullptr, m);
    } __except (1) {
        *m = VK_NULL_HANDLE;   // (whatever the driver wrote before it faulted is not a module)
        logf("vk: the driver faulted creating a shader module (exception %#lx): that program is left out",
             (unsigned long)GetExceptionCode());
        return (VkResult)-77;
    }
}
#else
VkResult guarded_shader_module(const VkShaderModuleCreateInfo* ci, VkShaderModule* m) {
    install_driver_fault_guard();
    if (sigsetjmp(fault_jmp, 1) != 0) {
        *m = VK_NULL_HANDLE;
        logf("vk: the driver faulted creating a shader module: that program is left out");
        return (VkResult)-77;
    }
    fault_armed = 1;
    const VkResult r = vkCreateShaderModule(dev.device, ci, nullptr, m);
    fault_armed = 0;
    return r;
}
#endif
/// (longshot #252) Windows Intel: an ask left cold because a worker held Intel's compiler gate (IntelDriverGate). There is no
/// lookup of the driver's cache or ours that does not enter the driver, so the pipeline may be in a cache all the same: its draw
/// is left out, as a cold one is, until a worker has made it. Counted and logged (the first 8, then at each power of two)
std::atomic<int64_t> st_intel_gate_busy{0};
void intel_gate_busy() {
    const int64_t n = ++st_intel_gate_busy;
    if (n <= 8 || (n & (n - 1)) == 0)
        logf("vk: asked cold: Intel's gate busy (a worker compiles); left to a worker, %lld so far", (long long)n);
}
/// (longshot #252) a compute pipeline made through the guard, one thread at a time in Intel's compiler on Windows
VkResult gated_compute(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    IntelDriverGate gate;   // (outside the cache's fetch lock that guarded_compute_held takes: a fetch never takes the gate)
    return guarded_compute_held(cache, ci, pl);
}
thread_local bool driver_cache_only = false;   // (predict_run's: pipelines for the driver's own cache, not into ours)

/// A target's write mask: the state's channel mask, nothing for a target the fragment shader leaves unwritten
/// (Maxwell keeps its texels; Vulkan writes undefined values: TOTK's maxwell.md 1.1)
uint32_t target_write_mask(const Program& p, const NvnState& s, int i) {
    const Stage* fs = nullptr;
    for (const Stage& st : p.stages)
        if (st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = &st;
    if (!fs || i >= 8 || !fs->info.stores_frag_color[i]) return 0;
    // (amd-fix, Lichii #353) and none of the components it does not write: Vulkan writes undefined values there, AMD its
    // export registers' garbage (NaN and saturated pixels above OoT's horizon, which the bloom spread). RX_VK_ALL_COMPONENTS=1: off
    static const bool all = getenv("RX_VK_ALL_COMPONENTS") != nullptr;
    const uint32_t written = fs->info.frag_color_components[i];
    return (s.chmask.mask >> (4 * i)) & 0xF & (all || !written ? 0xFu : (uint32_t)written);
}
thread_local const Program* cur_program = nullptr;   // (the draw's, for its dynamic write masks)
/// (hookshot) What target_write_mask reads of a program, as one word: whether its fragment stage stores each output and which
/// components it writes of it (6 bits each). Two programs with the same word make the same masks from the same state
uint64_t program_wm_sig(const Program& p) {
    if (const int64_t known = p.wm_sig.load(std::memory_order_relaxed); known >= 0) return (uint64_t)known;
    const Stage* fs = nullptr;
    for (const Stage& st : p.stages)
        if (st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = &st;
    uint64_t sig = 0;
    for (int i = 0; fs && i < 8; i++)
        if (fs->info.stores_frag_color[i]) {
            const uint32_t written = fs->info.frag_color_components[i];
            sig |= (uint64_t)(1u | (written ? 2u : 0u) | (written & 0xFu) << 2) << (6 * i);
        }
    p.wm_sig.store((int64_t)sig, std::memory_order_relaxed);   // (replay threads may ask at once: the same word)
    return sig;
}

// ---- (hookshot) The precompile, as TotK Recomp's (runtime/vk_pipeline.c vk_pipeline_prewarm, keys in pipeline-keys.bin): each
// pipeline the game asks for is noted in hookshot_pipelines.bin, by what built it; a release build, before the game starts,
// makes every kept program ready and builds every noted pipeline on all the CPUs (the driver's own cache, mkw_vk_pipelines.bin,
// makes that quick), so that nothing is built during play. RX_VK_PRECOMPILE=1: in any build; =0: off. A build with
// RX_RELEASE=1 (nx2/build.py) has it on. The notes are written in every build ----
std::mutex note_mu;
FILE* note_file = nullptr;
std::unordered_set<uint64_t> noted;   // (a pipeline once in the file, however many runs meet it)
std::string pipe_notes_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "hookshot_pipelines.bin";
}

// (TICK-008) The notes file, format 2. A note is a program (key, variant) with the draw state its pipeline was made for; it holds no
// compiled code, so a change of the shader translation does not make it wrong, only its pipelines stale. The header says what the
// notes were written for:
//   format      this layout of the header and of a record
//   key_version bumped by hand when pipeline_key() or the way a note is built changes meaning
//   note_size   sizeof(PipeNote), and state: HOOKSHOT_STATE_STAMP (nx2/build.py: the hash of vk_state.h, vk_api.h, vk_draw.h, rx_nvn.h)
//   translator  program_cache_stamp() when it was written (HOOKSHOT_SHADER_STAMP: the shader translation)
// A file whose format, key_version, note_size or state differs is of no use and is started again. One whose translator differs is
// CARRIED: its notes are kept, every pipeline is made again from them with the new translation (never trusted as built: the
// precompile recomputes each key from the state) and the file is rewritten with the new translator stamp. The old format (the
// translator stamp and the size, 16 bytes) is read when its stamp is the current one. RX_VK_NOTES_STRICT=1: a translator
// that differs drops the notes, as before.
constexpr uint32_t kNoteFormat = 2, kPipeKeyVersion = 1;
#ifdef HOOKSHOT_STATE_STAMP
constexpr uint64_t kStateStamp = HOOKSHOT_STATE_STAMP;
#else
constexpr uint64_t kStateStamp = 0;   // (a build that does not say: notes are never carried to another translator)
#endif
struct NoteHead {
    char magic[8];
    uint32_t format, key_version;
    uint64_t note_size, state, translator;
};
NoteHead note_head_now() {
    NoteHead h{};
    std::memcpy(h.magic, "HKPIPES2", 8);
    h.format = kNoteFormat, h.key_version = kPipeKeyVersion, h.note_size = sizeof(PipeNote), h.state = kStateStamp;
    h.translator = program_cache_stamp();
    return h;
}
enum class NoteFile { Missing, Current, Carried, Old, Unusable };
/// What the file at f (positioned at its start) is; first is where its records begin
NoteFile note_file_state(FILE* f, long* first) {
    uint8_t buf[sizeof(NoteHead)]{};
    const size_t got = fread(buf, 1, sizeof buf, f);
    const NoteHead now = note_head_now();
    if (got >= sizeof(NoteHead) && !std::memcmp(buf, now.magic, 8)) {
        NoteHead h;
        std::memcpy(&h, buf, sizeof h);
        *first = (long)sizeof(NoteHead);
        if (h.format != now.format || h.key_version != now.key_version || h.note_size != now.note_size || h.state != now.state ||
            (now.state == 0 && h.translator != now.translator))
            return NoteFile::Unusable;
        if (h.translator == now.translator) return NoteFile::Current;
        return getenv("RX_VK_NOTES_STRICT") ? NoteFile::Unusable : NoteFile::Carried;
    }
    if (got >= 16) {   // (the old header: the translator stamp, sizeof(PipeNote))
        uint64_t head[2];
        std::memcpy(head, buf, 16);
        *first = 16;
        if (head[0] == now.translator && head[1] == sizeof(PipeNote)) return NoteFile::Old;
    }
    return NoteFile::Unusable;
}
/// (hookshot) Targets a pipeline can be made with: at most 8 colour targets, a sample count Vulkan has, a topology it has.
/// The notes and the state list are files: what they say is checked before a pipeline is made from it
bool targets_ok(uint32_t n, uint32_t samples, uint32_t topo) {
    return n <= 8 && samples >= 1 && samples <= 64 && !(samples & (samples - 1)) && topo <= VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
}
/// (hookshot) Formats a pipeline can be made with: each colour target's none or one this device draws to, the depth target's
/// one it uses as depth. They come from the notes and the shareable state list, and any number reached the driver as a
/// VkFormat (a crash in the shader preparation, at every start, until the file was deleted)
bool formats_ok(uint32_t n, const uint32_t color[8], uint32_t has_depth, uint32_t depth) {
    if (!dev.physical) return true;   // (no device to ask: the pipeline is not made either)
    auto supports = [](uint32_t f, VkFormatFeatureFlags need) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, (VkFormat)f, &fp);
        return (fp.optimalTilingFeatures & need) == need;
    };
    for (uint32_t i = 0; i < n && i < 8; i++)
        if (color[i] != VK_FORMAT_UNDEFINED && !supports(color[i], VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) return false;
    return !has_depth || depth == VK_FORMAT_UNDEFINED ||
           (is_depth_format((VkFormat)depth) && supports(depth, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
}
/// (hookshot) a note read from hookshot_pipelines.bin: its counts in range, its booleans 0 or 1, and no host pointers (the
/// render targets' objects of the run that wrote it mean nothing here)
bool note_sane(PipeNote& n) {
    if (!targets_ok(n.n, n.samples, n.topo) || n.s.nvattr < 0 || n.s.nvattr > 16 ||
        !formats_ok(n.n, n.color_format, n.has_depth, n.depth_format))
        return false;
    n.s.rt = nvn_p_set_rt{};
    for (bool* b : {&n.s.has_prog, &n.s.restart, &n.s.depth_clamp, &n.s.raster_discard, &n.s.render_enabled}) {
        unsigned char v;
        std::memcpy(&v, b, 1);
        v = v != 0;
        std::memcpy(b, &v, 1);
    }
    return n.s.prog.nstage <= 6;
}
/// The records of a notes file from where they start, each pipeline (by the key it was written with) once
std::vector<std::unique_ptr<PipeNote>> note_records(FILE* f, long first) {
    std::vector<std::unique_ptr<PipeNote>> notes;
    std::unordered_set<uint64_t> seen;
    fseek(f, first, SEEK_SET);
    for (;;) {
        auto rec = std::make_unique<PipeNote>();
        if (fread(rec.get(), sizeof(PipeNote), 1, f) != 1) break;
        if (!note_sane(*rec)) continue;
        if (seen.insert(rec->key).second) notes.push_back(std::move(rec));
    }
    return notes;
}
/// The notes of this build (hookshot_pipelines.bin), each pipeline once; carried is set when they come from another translator
std::vector<std::unique_ptr<PipeNote>> pipe_notes_read(bool* carried = nullptr) {
    std::vector<std::unique_ptr<PipeNote>> notes;
    if (carried) *carried = false;
    if (FILE* f = fopen(pipe_notes_path().c_str(), "rb")) {
        long first = 0;
        const NoteFile st = note_file_state(f, &first);
        if (st == NoteFile::Current || st == NoteFile::Carried || st == NoteFile::Old) {
            notes = note_records(f, first);
            if (carried) *carried = st == NoteFile::Carried;
        }
        fclose(f);
    }
    return notes;
}
/// The file open for appending, in the current format (note_mu held). A file of the old format or another translator is rewritten
/// first with its notes (in a file beside it, then moved over it); an unusable one is started again
void notes_open_locked() {
    if (note_file) return;
    const std::string path = pipe_notes_path();
    long first = 0;
    NoteFile st = NoteFile::Missing;
    std::vector<std::unique_ptr<PipeNote>> keep;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        st = note_file_state(f, &first);
        if (st == NoteFile::Carried || st == NoteFile::Old || st == NoteFile::Current) keep = note_records(f, first);
        if (st == NoteFile::Current) {   // (a record cut short by a killed run: the file is rewritten whole, or the next note would be misaligned)
            fseek(f, 0, SEEK_END);
            if ((ftell(f) - first) % (long)sizeof(PipeNote)) st = NoteFile::Old;
        }
        fclose(f);
    }
    for (const auto& n : keep) noted.insert(n->key);
    if (st == NoteFile::Current) {
        note_file = fopen(path.c_str(), "ab");
        if (note_file) return;
    }
    const NoteHead head = note_head_now();
    const std::string tmp = path + ".new";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        fwrite(&head, sizeof head, 1, f);
        for (const auto& n : keep) fwrite(n.get(), sizeof(PipeNote), 1, f);
        const bool ok = fflush(f) == 0;
        fclose(f);
        if (ok && MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (st == NoteFile::Carried || st == NoteFile::Old)
                logf("vk: pipeline notes: %zu carried over to this build (%s)", keep.size(),
                     st == NoteFile::Old ? "old file format" : "the shader translation changed: their pipelines are made again");
            note_file = fopen(path.c_str(), "ab");
            return;
        }
        remove(tmp.c_str());
    }
    noted.clear();   // (could not rewrite: started again)
    note_file = fopen(path.c_str(), "wb");
    if (note_file) fwrite(&head, sizeof head, 1, note_file);
}
/// A note's targets, as its draw had them
Targets note_targets(const PipeNote& n) {
    Targets at{};
    at.n = (int)n.n;
    at.samples = (VkSampleCountFlagBits)n.samples;
    at.depth_format = (VkFormat)n.depth_format;
    at.depth = n.has_depth ? (Texture*)1 : nullptr;   // (never read through: whether there is one)
    for (int k = 0; k < 8; k++) at.color_format[k] = (VkFormat)n.color_format[k];
    return at;
}
/// A note, appended (on a worker: the file is never touched on the replay thread)
void state_record(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, const uint8_t color_types[8]);
uint64_t pipeline_key(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo);
void pipeline_note(uint64_t key, const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo,
                   const uint8_t* color_types = nullptr) {
    static const bool off = getenv("RX_VK_NO_PROGRAM_CACHE") != nullptr;
    if (off) return;
    if (color_types) state_record(p, s, at, topo, color_types);
    auto rec = std::make_unique<PipeNote>();
    rec->key = key, rec->prog = p.key, rec->variant = p.variant, rec->topo = (uint32_t)topo, rec->n = (uint32_t)at.n;
    rec->samples = (uint32_t)at.samples, rec->depth_format = (uint32_t)at.depth_format, rec->has_depth = at.depth != nullptr;
    for (int i = 0; i < 8; i++) rec->color_format[i] = (uint32_t)at.color_format[i];
    rec->s = s;
    std::lock_guard lk(note_mu);
    notes_open_locked();   // (HeyListen, TICK-008: the notes file in our format, carried notes kept; returns at once when open)
    if (note_file && noted.insert(key).second) fwrite(rec.get(), sizeof(PipeNote), 1, note_file), fflush(note_file);
}

// ---- (hookshot) The compute programs play dispatches, each key once, in hookshot_computes.bin beside the executable. The precompile
// made every kept compute program ready and kept a pipeline for each: 10,328 of them, a few GB of RAM that grew a 16 GB PC out of memory
// ("memory busted", out-of-memory crashes, Discord and issues 1228/1245, 2026-11-09), where play dispatches a few hundred. With the list
// only those are kept ready; the rest stay on disk and are made when first dispatched, from the driver's and our pipeline cache (the
// prepare warmed both: milliseconds, on a worker). Keys only, no stamp: a key no longer kept is just made in play.
// RX_VK_PRECOMPILE_COMPUTES=all: every kept compute program, as before; =none: none ----
constexpr char kComputesMagic[8] = {'H', 'K', 'C', 'O', 'M', 'P', '1', 0};
std::mutex compute_note_mu;
FILE* compute_note_file = nullptr;
std::unordered_set<uint64_t> computes_noted;
std::string computes_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "hookshot_computes.bin";
}
std::unordered_set<uint64_t> computes_read() {
    std::unordered_set<uint64_t> keys;
    if (FILE* f = fopen(computes_path().c_str(), "rb")) {
        char magic[8] = {};
        if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, kComputesMagic, 8))
            for (uint64_t k = 0; fread(&k, 8, 1, f) == 1;) keys.insert(k);
        fclose(f);
    }
    return keys;
}
void compute_note(uint64_t key) {
    static const bool off = getenv("RX_VK_NO_PROGRAM_CACHE") != nullptr;
    if (off) return;
    std::lock_guard lk(compute_note_mu);
    if (!compute_note_file) {
        computes_noted = computes_read();
        const bool append = !computes_noted.empty();   // (none read: a new file, or one that is not a list)
        compute_note_file = fopen(computes_path().c_str(), append ? "ab" : "wb");
        if (compute_note_file && !append) fwrite(kComputesMagic, 1, 8, compute_note_file);
    }
    if (compute_note_file && computes_noted.insert(key).second) fwrite(&key, 8, 1, compute_note_file), fflush(compute_note_file);
}

// ---- (hookshot) The pipeline state list: each pipeline the game asks for, by what it is made of and nothing of the game's code
// (its stages' code keys, the vertex input and colour output types its program was translated for, the draw state baked into it), in
// hookshot_pipeline_states.bin beside the executable. Unlike hookshot_pipelines.bin it is good for any build of the back end: when
// the shaders are prepared for a new one (prepare_run), every pipeline in it is noted for the precompile, its program translated
// first when the seed's are not that one, so that a new release, or a new install with a list from someone else's play
// (hookshot_pipeline_states_shipped.bin, nx2/make_game.py: work/pipeline_states.bin), builds them before the game and not in
// play. Only hashes, formats and state: the stages themselves come from the player's own game (hookshot_shaders.bin) ----
constexpr char kStateMagic[8] = {'H', 'K', 'S', 'T', 'A', 'T', 'E', '1'};
struct StateRec {
    uint64_t code[4];        // the stages' code keys: vertex (mesh), fragment, task, the header-only stage (0: none)
    uint64_t variant;        // the program's (its answers about the textures)
    uint32_t mesh, topo, n, samples, depth_format, has_depth;
    uint32_t color_format[8];
    uint8_t color_types[8];  // (Shader::AttributeType of each target: what the fragment outputs are translated for)
    uint64_t blend[8];       // (nvn_blend_st)
    uint32_t vattr[16], nvattr;   // (nvn_va_st: the vertex attributes' formats)
    uint32_t stride[16], divisor[16];
    uint32_t blend_en, chmask, poly_mode, depth_clamp, a2c, pad;
    // the program variant's texture answers: by descriptor (Program::answers: stage << 28 | descriptor << 12 | image << 8 | type), and
    // as the translation asked them (Program::questions); a variant with more is noted without them, and made in play
    uint32_t nanswers, nquestions;
    uint32_t answers[24];
    uint8_t questions[32];
};
static_assert(sizeof(StateRec) == 528 && std::is_trivially_copyable_v<StateRec>, "the state list's record is a file format");
std::string state_list_path(const char* name) {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + name;
}
uint64_t state_hash(const StateRec& r) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint8_t* b = (const uint8_t*)&r;
    for (size_t i = 0; i < sizeof r; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
/// The records of a state list (none when it is not one)
std::vector<StateRec> state_list_read(const std::string& path) {
    std::vector<StateRec> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char magic[8];
    uint32_t size = 0;
    if (fread(magic, 8, 1, f) == 1 && !memcmp(magic, kStateMagic, 8) && fread(&size, 4, 1, f) == 1 && size == sizeof(StateRec)) {
        StateRec r;
        while (fread(&r, sizeof r, 1, f) == 1) {
            if (r.nanswers > std::size(r.answers) || r.nquestions > std::size(r.questions) || r.nvattr > 16 ||
                !targets_ok(r.n, r.samples, r.topo) || !formats_ok(r.n, r.color_format, r.has_depth, r.depth_format))
                continue;   // (hookshot: a pipeline is made from the rest: its counts must be ones it can have)
            out.push_back(r);
        }
    }
    fclose(f);
    return out;
}
std::mutex state_mu;
FILE* state_file = nullptr;
std::unordered_set<uint64_t> state_seen;
/// (state_mu held) the state list opened for this run's new pipelines, the records it holds hashed. precompile_run opens it before
/// play: the first new pipeline in play read and hashed the whole list on the thread replaying, 8-18 ms
void state_open_locked() {
    const std::string path = state_list_path("hookshot_pipeline_states.bin");
    const std::vector<StateRec> had = state_list_read(path);
    for (const StateRec& o : had) state_seen.insert(state_hash(o));
    FILE* probe = fopen(path.c_str(), "rb");
    const bool keep = probe && !had.empty();
    if (probe) fclose(probe);
    state_file = fopen(path.c_str(), keep ? "ab" : "wb");
    if (state_file && !keep) {
        const uint32_t size = sizeof(StateRec);
        fwrite(kStateMagic, 8, 1, state_file), fwrite(&size, 4, 1, state_file);
    }
}
void state_record(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, const uint8_t color_types[8]) {
    static const bool off = getenv("RX_VK_NO_STATE_LIST") != nullptr;
    if (off) return;
    StateRec r{};
    std::memcpy(r.code, p.codes, sizeof r.code);   // (kept with the program: the stages' lock is not taken on the draw's path)
    if (!r.code[0] || p.compute) return;
    r.variant = p.variant;
    r.mesh = p.mesh || p.fallback ? 1 : 0, r.topo = (uint32_t)topo, r.n = (uint32_t)at.n, r.samples = (uint32_t)at.samples;
    r.depth_format = (uint32_t)at.depth_format, r.has_depth = at.depth != nullptr;
    for (int i = 0; i < 8; i++) r.color_format[i] = (uint32_t)at.color_format[i], r.color_types[i] = color_types[i];
    for (int i = 0; i < 16; i++) r.vattr[i] = s.vattr[i], r.stride[i] = s.vstream[i].stride, r.divisor[i] = s.vstream[i].divisor;
    r.nvattr = (uint32_t)s.nvattr;
    for (int i = 0; i < 8; i++) std::memcpy(&r.blend[i], &s.blend[i], 8);
    r.blend_en = (uint32_t)s.color.blend_en, r.chmask = s.chmask.mask, r.poly_mode = (uint32_t)s.poly.mode;
    r.depth_clamp = s.depth_clamp ? 1 : 0, r.a2c = s.ms.a2c ? 1 : 0;
    if (p.answers.size() <= 24 && p.questions.size() <= 32) {
        r.nanswers = (uint32_t)p.answers.size(), r.nquestions = (uint32_t)p.questions.size();
        for (size_t i = 0; i < p.answers.size(); i++) {
            const Program::Answer& a = p.answers[i];
            r.answers[i] = (uint32_t)(a.stage & 0xF) << 28 | (uint32_t)(a.desc & 0xFFFF) << 12 | (a.image ? 1u : 0u) << 8 | a.type;
        }
        std::memcpy(r.questions, p.questions.data(), p.questions.size());
    }
    const uint64_t h = state_hash(r);
    std::lock_guard lk(state_mu);
    if (!state_file) state_open_locked();
    if (state_file && state_seen.insert(h).second) fwrite(&r, sizeof r, 1, state_file), fflush(state_file);
}
/// (HL_SHIPPED_STATES) The shipped list's contents by a hash, and the hash state_list_prepare last went through, in
/// hookshot_pipeline_states_shipped.done: a list that came after the shaders were prepared (a list shipped later, or a folder
/// prepared before it had one) was never noted, the prepare being done for that translator and seed, and its pipelines were all
/// made in play. 0: no list
uint64_t shipped_list_hash() {
    uint64_t h = 0xCBF29CE484222325ull;
    FILE* f = fopen(state_list_path("hookshot_pipeline_states_shipped.bin").c_str(), "rb");
    if (!f) return 0;
    uint8_t buf[65536];
    size_t total = 0;
    for (size_t got; (got = fread(buf, 1, sizeof buf, f)) > 0; total += got)
        for (size_t i = 0; i < got; i++) h = (h ^ buf[i]) * 1099511628211ull;
    fclose(f);
    return total > 12 ? h : 0;
}
uint64_t shipped_list_done() {
    uint64_t h = 0;
    if (FILE* f = fopen(state_list_path("hookshot_pipeline_states_shipped.done").c_str(), "rb")) {
        if (fread(&h, 8, 1, f) != 1) h = 0;
        fclose(f);
    }
    return h;
}
bool shipped_list_pending() {
    static const bool on = !getenv("HL_SHIPPED_STATES") || atoi(getenv("HL_SHIPPED_STATES")) != 0;
    if (!on || getenv("RX_VK_NO_STATE_LIST")) return false;
    const uint64_t h = shipped_list_hash();
    const bool pending = h && h != shipped_list_done();
    if (pending) logf("hl pipe: the shipped pipeline state list is new since the shaders were prepared: its pipelines are noted now");
    return pending;
}
/// (prepare_run, the seed open) every pipeline of the state lists (the player's own and the shipped one): its program translated
/// when the program cache lacks it, then noted in hookshot_pipelines.bin for precompile_run to build. RX_VK_NO_STATE_LIST=1: off
/// background (a lazy update, in play): only the programs are made, to the program cache and the memory, on a few workers at idle
/// priority; no pipeline is noted and the shipped list is not marked as gone through, so that the next start does it before the game
void state_list_prepare(void (*progress)(size_t done, size_t total), bool background = false) {
    if (getenv("RX_VK_NO_STATE_LIST")) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<StateRec> recs = state_list_read(state_list_path("hookshot_pipeline_states_shipped.bin"));
    const size_t shipped = recs.size();
    if (const uint64_t h = shipped && !background ? shipped_list_hash() : 0) {   // (gone through: state_list_pending is false until it changes)
        if (FILE* f = fopen(state_list_path("hookshot_pipeline_states_shipped.done").c_str(), "wb")) fwrite(&h, 8, 1, f), fclose(f);
    }
    for (const StateRec& r : state_list_read(state_list_path("hookshot_pipeline_states.bin"))) recs.push_back(r);
    if (recs.empty()) return;
    // the programs: each once (by its stages and the types it was translated for)
    struct Want { const StateRec* r; uint64_t key = 0; };
    std::vector<Want> want;
    std::unordered_map<uint64_t, size_t> by_program;
    std::vector<size_t> rec_program(recs.size());
    for (size_t i = 0; i < recs.size(); i++) {
        const StateRec& r = recs[i];
        uint64_t h = 0xCBF29CE484222325ull;
        const auto add = [&h](const void* d, size_t n) {
            for (size_t k = 0; k < n; k++) h = (h ^ ((const uint8_t*)d)[k]) * 1099511628211ull;
        };
        add(r.code, sizeof r.code), add(&r.mesh, 4), add(r.color_types, 8);
        for (int k = 0; k < 16; k++) {   // (the input types, not the formats: what the program key holds)
            const uint8_t t = (uint8_t)attribute_type_of(r.vattr[k]);
            add(&t, 1);
        }
        const auto [it, fresh] = by_program.emplace(h, want.size());
        if (fresh) want.push_back({&r});
        rec_program[i] = it->second;
    }
    std::atomic<size_t> next{0}, made{0}, missing{0};
    const auto worker = [&] {
        ub_thread_off();   // (a background thread: see ub_thread_off; the pipeline state list's builds raced the replay's updates)
        SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_IDLE : THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            const size_t i = next++;
            if (i >= want.size()) return;
            const StateRec& r = *want[i].r;
            std::unique_ptr<Program> p;
            // (a mesh program on a GPU without mesh shaders is not made; one with a header-only stage is made in play)
            if (!r.code[3] && (!r.mesh || dev.mesh_shader)) p = seed_program_of(r.code, r.mesh != 0, r.vattr, r.color_types, &want[i].key);
            if (!want[i].key) missing++;
            if (p) {
                if (!p->failed) made++;
                for (Stage& st : p->stages)
                    if (st.module) vkDestroyShaderModule(dev.device, st.module, nullptr);
            }
            if (progress) progress(i, want.size());
        }
    };
    std::vector<std::thread> threads;
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    const unsigned workers = background ? std::clamp(cpus / 4, 2u, 4u) : std::clamp(cpus / 2, 1u, 32u);
    for (unsigned i = 0; i < workers; i++) threads.emplace_back(worker);
    for (std::thread& t : threads) t.join();
    // then each pipeline, noted as a draw would note it
    std::unordered_set<uint64_t> keys;
    for (const Want& w : want)
        if (w.key) keys.insert(w.key);
    // (these keys only: the kept compute programs stay on disk for precompile_run, which builds their pipelines. In play they are
    // not read in: the draws that need one read it, and the programs a draw is using are not ours to make layouts for)
    if (!background) program_cache_load_some(keys, [](Program& p) { ensure_layout(p); }, false);
    // the variants made by other textures (the seed's are for nothing bound): translated with the answers the game's gave
    std::vector<size_t> variant_of;   // (a record of each variant the cache lacks)
    {
        std::unordered_set<uint64_t> seen;
        for (size_t i = 0; i < recs.size(); i++) {
            const StateRec& r = recs[i];
            const uint64_t key = want[rec_program[i]].key;
            if (key && (r.nanswers || r.nquestions) && !(background ? program_variant_kept(key, r.variant) : program_find(key, r.variant) != nullptr) && seen.insert(mix(key, r.variant)).second)
                variant_of.push_back(i);
        }
    }
    std::atomic<size_t> variants_made{0};
    next = 0;
    const auto variant_worker = [&] {
        SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_IDLE : THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            const size_t i = next++;
            if (i >= variant_of.size()) return;
            const StateRec& r = recs[variant_of[i]];
            std::vector<uint8_t> questions(r.questions, r.questions + r.nquestions);
            std::vector<Program::Answer> answers(r.nanswers);
            for (uint32_t k = 0; k < r.nanswers; k++)
                answers[k] = {(uint16_t)(r.answers[k] >> 28), (uint16_t)(r.answers[k] >> 12 & 0xFFFF), (uint8_t)(r.answers[k] & 0xFF),
                              (r.answers[k] >> 8 & 1) != 0};
            if (const Program* p = seed_variant_of(r.code, r.mesh != 0, r.vattr, r.color_types, questions, answers)) {
                if (p->variant == r.variant) variants_made++;
                if (!background) ensure_layout(const_cast<Program&>(*p));
            }
        }
    };
    threads.clear();
    for (unsigned i = 0; i < workers; i++) threads.emplace_back(variant_worker);
    for (std::thread& t : threads) t.join();
    if (background) {
        logf("vk: state list (in play): %zu pipelines, %zu programs (%zu translated now, %zu with stages not in the seed), %zu variants by "
             "their textures translated, in %.1f s on %u workers", recs.size(), want.size(), made.load(), missing.load(), variants_made.load(),
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), workers);
        return;
    }
    size_t noted_n = 0, no_variant = 0;
    auto s = std::make_unique<NvnState>();
    for (size_t i = 0; i < recs.size(); i++) {
        const StateRec& r = recs[i];
        const uint64_t key = want[rec_program[i]].key;
        if (!key) continue;
        const Program* p = program_find(key, r.variant);
        if (!p) {
            no_variant++;
            continue;
        }
        *s = NvnState{};
        for (int k = 0; k < 16; k++) s->vattr[k] = r.vattr[k], s->vstream[k].stride = r.stride[k], s->vstream[k].divisor = r.divisor[k];
        s->nvattr = (int)r.nvattr;
        for (int k = 0; k < 8; k++) std::memcpy(&s->blend[k], &r.blend[k], 8);
        s->color.blend_en = r.blend_en, s->chmask.mask = r.chmask, s->poly.mode = r.poly_mode;
        s->depth_clamp = r.depth_clamp != 0, s->ms.a2c = (uint8_t)r.a2c;
        Targets at{};
        at.n = (int)r.n, at.samples = (VkSampleCountFlagBits)r.samples, at.depth_format = (VkFormat)r.depth_format;
        at.depth = r.has_depth ? (Texture*)1 : nullptr;   // (never read through: whether there is one)
        for (int k = 0; k < 8; k++) at.color_format[k] = (VkFormat)r.color_format[k];
        if (!p->layout) ensure_layout(const_cast<Program&>(*p));
        pipeline_note(pipeline_key(*p, *s, at, (VkPrimitiveTopology)r.topo), *p, *s, at, (VkPrimitiveTopology)r.topo);
        noted_n++;
    }
    logf("vk: state list: %zu pipelines (%zu shipped), %zu programs (%zu translated now, %zu with stages not in the seed), %zu variants "
         "by their textures (%zu made), %zu noted, %zu whose program variant was not made, in %.1f s", recs.size(), shipped,
         want.size(), made.load(), missing.load(), variant_of.size(), variants_made.load(), noted_n, no_variant,
         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

// ---- (hookshot) Stage libraries, as TotK Recomp's runtime/vk_pipeline.c: the part of a pipeline that costs is its shaders, and with
// VK_EXT_graphics_pipeline_library they are built apart from what a draw adds (vertex layout, target formats). prepare_run builds
// each program's two shader libraries once, for the driver's own cache to keep; a draw that needs a pipeline nothing has built gets
// them back from that cache, links them on the spot, and draws at once. The worker's full build then takes over. Needs the blend,
// polygon, clamp and alpha-to-coverage state dynamic, or those would be in the libraries too. RX_VK_GPL=1: on
// (a library the driver does not have compiled is not compiled in the draw: asked for with
// VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, it comes back at once either way. Cold, the draw is left out or waits
// for the worker's full build, as without libraries, and a worker builds the library after it, for the program's next pipeline.
// Needs pipelineCreationCacheControl. RX_VK_GPL_SYNC (set to anything): compiled in the draw, as before) ----
bool gpl_wanted() {
    static const bool on = dev.graphics_pipeline_library && dev.eds3_blend && dev.eds3_polygon && dev.eds3_clamp && dev.eds3_a2c &&
                           getenv("RX_VK_GPL") != nullptr;   // (off until the driver step is safe: linked cold, the libraries stall the draw)
    return on;
}
bool gpl_ask_only() {
    static const bool on = dev.vk13.pipelineCreationCacheControl && !getenv("RX_VK_GPL_SYNC");
    return on;
}
std::mutex gpl_mu;
std::unordered_map<uint64_t, VkPipeline> gpl_libs;   // (null: cold and a worker's to build, or it failed. Not asked for again)
/// cold_key: set to the library's key when the driver would have had to compile it (the caller has a worker build it: gpl_build_cold)
VkPipeline gpl_part(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part, uint64_t key,
                    uint64_t* cold_key) {
    key = mix(key, (uint64_t)part);
    {
        std::lock_guard lk(gpl_mu);
        if (auto it = gpl_libs.find(key); it != gpl_libs.end()) return it->second;
    }
    const bool ask = (part == 1 || part == 2) && gpl_ask_only();   // (the two with shaders in them)
    bool cold = false;
    VkPipeline made = build_graphics(p, s, at, topo, part, ask ? &cold : nullptr);
    if (ask && made) st_lib_hits++;
    st_lib_cold += cold;
    std::lock_guard lk(gpl_mu);
    const auto [it, fresh] = gpl_libs.emplace(key, made);
    if (!fresh && made) vkDestroyPipeline(dev.device, made, nullptr);
    if (fresh && cold && cold_key) *cold_key = key;
    return it->second;
}
/// (a worker's: a library gpl_part found cold. One that fails stays null, so it is built once at most)
void gpl_build_cold(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part, uint64_t key) {
    const auto t0 = std::chrono::steady_clock::now();
    const VkPipeline made = build_graphics(p, s, at, topo, part);
    // (what the cold libraries cost the workers: their jobs are in the one queue with the pipelines and translations of the
    // draws left out, and nothing else says how long those waited behind them)
    st_lib_build_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!made) return;
    st_lib_built++;
    std::lock_guard lk(gpl_mu);
    gpl_libs[key] = made;
}
/// cold: the keys of the pre-rasterization and fragment libraries this call found cold (0: not)
VkPipeline gpl_link(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, uint64_t cold[2]) {
    if (at.n < 0 || at.n > 8) return VK_NULL_HANDLE;   // (hookshot: at most 8 colour targets)
    const uint64_t program = mix(p.key, p.variant);
    uint64_t input = (uint64_t)topology_class(topo) + 1;
    if (!p.mesh && !p.fallback) {
        const Shader::Info& vs = p.stages[0].info;
        for (int i = 0; i < 16; i++)
            input = mix(input, vs.loads.Generic(i) ? (uint64_t)s.vattr[i] << 8 | (i < s.nvattr) : 0);
        for (int i = 0; i < 16; i++) input = mix(input, (uint64_t)s.vstream[i].divisor);
    }
    uint64_t output = (uint64_t)at.samples << 8 | (uint64_t)at.n;
    for (int i = 0; i < at.n; i++) output = mix(output, (uint64_t)at.color_format[i]);
    output = mix(output, at.depth ? (uint64_t)at.depth_format : 0);
    VkPipeline libs[4];
    uint32_t n = 0;
    if (!p.mesh) libs[n++] = gpl_part(p, s, at, topo, 0, input, nullptr);
    libs[n++] = gpl_part(p, s, at, topo, 1, program, &cold[0]);
    libs[n++] = gpl_part(p, s, at, topo, 2, mix(program, (uint64_t)at.samples), &cold[1]);
    libs[n++] = gpl_part(p, s, at, topo, 3, output, nullptr);
    for (uint32_t i = 0; i < n; i++)
        if (!libs[i]) return VK_NULL_HANDLE;
    VkPipelineLibraryCreateInfoKHR li{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    li.libraryCount = n;
    li.pLibraries = libs;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &li;
    ci.layout = p.layout;
    VkPipeline pl = VK_NULL_HANDLE;
    IntelDriverGate gate;
    if (guarded_graphics(VK_NULL_HANDLE, &ci, &pl) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pl;
}

/// The pipeline entry of a draw (a new one is queued for a worker; RX_VK_SYNC_PIPELINES=1 builds it here)
/// What a graphics pipeline was built from, so the next draw of the same material does not hash it again
bool pipe_same(uint64_t prog, uint64_t variant, int topo, uint32_t samples, int n, uint32_t depth, const uint32_t* formats,
               int nvattr, const uint32_t* vattr, const uint32_t* div, uint32_t blend_en, const uint64_t* blend,
               uint32_t chmask, uint32_t poly, uint32_t clamp, uint32_t a2c, uint64_t cprog, uint64_t cvariant, int ctopo,
               uint32_t csamples, int cn, uint32_t cdepth, const uint32_t* cformats, int cnvattr, const uint32_t* cvattr,
               const uint32_t* cdiv, uint32_t cblend_en, const uint64_t* cblend, uint32_t cchmask, uint32_t cpoly,
               uint32_t cclamp, uint32_t ca2c) {
    if (prog != cprog || variant != cvariant || topo != ctopo || samples != csamples || n != cn || depth != cdepth ||
        nvattr != cnvattr || blend_en != cblend_en || chmask != cchmask || poly != cpoly || clamp != cclamp || a2c != ca2c)
        return false;
    for (int i = 0; i < 8; i++)
        if (formats[i] != cformats[i] || blend[i] != cblend[i]) return false;
    for (int i = 0; i < 16; i++)
        if (vattr[i] != cvattr[i] || div[i] != cdiv[i]) return false;
    return true;
}
struct PipeFast {
    bool on = false;
    PipeEntry* pe = nullptr;
    uint64_t key = 0;   // (the pipeline's key as it was recorded: the zone recorder's own note of the fast hit)
    uint64_t prog = 0, variant = 0;
    int topo = 0, n = 0, nvattr = 0;
    uint32_t samples = 0, depth = 0, blend_en = 0, chmask = 0, poly = 0, clamp = 0, a2c = 0;
    uint32_t formats[8]{}, vattr[16]{}, div[16]{};
    uint64_t blend[8]{};
};
thread_local PipeFast pipe_fast[2];

/// (hookshot) the stage a graphics pipeline's vertex input comes from (a fallback program's first stage is a compute one)
const Shader::Info* vertex_info(const Program& p) {
    for (const Stage& st : p.stages)
        if (st.vk_stage == VK_SHADER_STAGE_VERTEX_BIT) return &st.info;
    return p.stages.empty() ? nullptr : &p.stages[0].info;
}

/// (TICK-008) The identity of a graphics pipeline: what is baked into it (the rest is dynamic state). The precompile makes it again from a note's
/// state and does not trust the key the note was written with
/// (the key from the program's own seed: pipeline_key's, or the ledger's, whose seed is the program's fingerprint)
static uint64_t pipeline_key_from(uint64_t seed, const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo) {
    if (at.n < 0 || at.n > 8) return 0;   // (hookshot: at most 8 colour targets)
    const int topo_c = topology_class(topo);
    uint64_t key = mix(seed, (uint64_t)topo_c << 8 | (uint64_t)at.samples);
    const Shader::Info& vs = *vertex_info(p);
    if (!p.mesh && !p.fallback) {
        for (int i = 0; i < 16; i++)
            if (vs.loads.Generic(i)) key = mix(key, (uint64_t)s.vattr[i] << 8 | (i < s.nvattr));
        for (int i = 0; i < 16; i++) key = mix(key, (uint64_t)s.vstream[i].divisor);
    }
    for (int i = 0; i < at.n; i++) {
        key = mix(key, (uint64_t)at.color_format[i]);
        if (!dev.eds3_blend) {
            uint64_t b;
            std::memcpy(&b, &s.blend[i], 8);
            key = mix(key, (s.color.blend_en >> i & 1) ? b : 0);
        }
    }
    key = mix(key, (uint64_t)at.depth_format);
    if (!dev.eds3_blend) {
        uint64_t wm = 0;
        for (int i = 0; i < at.n; i++) wm |= (uint64_t)target_write_mask(p, s, i) << (4 * i);
        key = mix(key, wm << 8 | s.color.blend_en);
    }
    if (!dev.eds3_polygon) key = mix(key, (uint64_t)s.poly.mode);
    if (!dev.eds3_clamp) key = mix(key, (uint64_t)s.depth_clamp);
    if (!dev.eds3_a2c) key = mix(key, (uint64_t)s.ms.a2c);
    return key;
}
uint64_t pipeline_key(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo) {
    return pipeline_key_from(mix(p.key, p.variant), p, s, at, topo);
}

// ---- (hookshot, RX_VK_SHADER_KEY) What the driver has compiled, hookshot_compiled.bin ----
// A change of the shader translation drops every kept program (the stamp, vk_shader.cpp disk_stamp), and every program is translated
// again; most of the change's programs come out the same. What the driver was handed for them last time (stage libraries, compute
// pipelines, predicted pipelines) is still in its own cache (the driver keys that on the SPIR-V and the state, not on our stamp) and
// in mkw_vk_pipelines.bin, so building it again only to find it there is time spent for nothing: the ledger lists, by a hash of
// what the driver sees (Program::fp, plus the state of a pipeline), the things already built here. An entry is the 64-bit id; the
// file's header says what the ids are good for (the device, the driver, the layout of the pipeline's state: kStateStamp and
// kPipeKeyVersion, the pipeline cache's own data being there); anything else starts the list again. An entry added is written when
// the prepare or the pipeline cache save commits it (ledger_commit): the cache's data is on disk by then too
namespace {
std::mutex ledger_mu;
std::unordered_set<uint64_t> ledger_have;
std::vector<uint64_t> ledger_pending;
FILE* ledger_out = nullptr;
bool ledger_ready = false;
std::atomic<int64_t> ledger_reused{0}, ledger_built{0};
constexpr char kLedgerMagic[8] = {'H', 'K', 'C', 'O', 'M', 'P', 'L', '1'};
std::string ledger_path() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string dir(exe, n);
    return dir.substr(0, dir.find_last_of("\\/") + 1) + "hookshot_compiled.bin";
}
uint64_t ledger_context() {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto add = [&h](const void* d, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)d)[i]) * 1099511628211ull;
    };
    const uint64_t fixed[] = {kStateStamp, kPipeKeyVersion, dev.props.vendorID, dev.props.deviceID, dev.props.driverVersion,
                              dev.props.apiVersion, (uint64_t)dev.mesh_shader | (uint64_t)dev.mesh_fallback << 1 | (uint64_t)dev.task_mesh_fallback << 7 |
                              (uint64_t)dev.eds3_blend << 2 | (uint64_t)dev.eds3_polygon << 3 | (uint64_t)dev.eds3_clamp << 4 |
                              (uint64_t)dev.eds3_a2c << 5 | (uint64_t)dev.push_descriptor << 6};
    add(fixed, sizeof fixed);
    add(dev.props.pipelineCacheUUID, VK_UUID_SIZE);
    return h;
}
/// (ledger_mu held) the file read, or started again when it is another device's, driver's or layout's
void ledger_open_locked() {
    if (ledger_ready) return;
    ledger_ready = true;
    const std::string path = ledger_path();
    const uint64_t context = ledger_context();
    int64_t good = 0;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char magic[8];
        uint64_t ctx = 0;
        if (fread(magic, 8, 1, f) == 1 && !memcmp(magic, kLedgerMagic, 8) && fread(&ctx, 8, 1, f) == 1 && ctx == context) {
            uint64_t id;
            good = 16;
            while (fread(&id, 8, 1, f) == 1) ledger_have.insert(id), good += 8;
        }
        fclose(f);
    }
    if (good) {
        if (FILE* f = fopen(path.c_str(), "r+b")) _chsize_s(_fileno(f), good), fclose(f);   // (a half entry cut off)
        ledger_out = fopen(path.c_str(), "ab");
    } else {
        ledger_out = fopen(path.c_str(), "wb");
        if (ledger_out) fwrite(kLedgerMagic, 8, 1, ledger_out), fwrite(&context, 8, 1, ledger_out), fflush(ledger_out);
    }
    logf("vk: shader key: %zu things the driver has built are listed (hookshot_compiled.bin)", ledger_have.size());
}
bool ledger_has(uint64_t id) {
    if (!id) return false;
    std::lock_guard lk(ledger_mu);
    ledger_open_locked();
    return ledger_have.count(id) != 0;
}
size_t ledger_size() {
    std::lock_guard lk(ledger_mu);
    ledger_open_locked();
    return ledger_have.size();
}
void ledger_add(uint64_t id) {
    if (!id) return;
    std::lock_guard lk(ledger_mu);
    ledger_open_locked();
    if (ledger_have.insert(id).second) ledger_pending.push_back(id), ledger_built++;
}
/// The pipeline cache has no data of ours (a new file, or another driver's): what the list says was built is not in it
void ledger_drop_all() {
    std::lock_guard lk(ledger_mu);
    ledger_open_locked();
    if (ledger_have.empty() && ledger_pending.empty()) return;
    ledger_have.clear(), ledger_pending.clear();
    if (ledger_out) fclose(ledger_out), ledger_out = nullptr;
    ledger_out = fopen(ledger_path().c_str(), "wb");
    const uint64_t context = ledger_context();
    if (ledger_out) fwrite(kLedgerMagic, 8, 1, ledger_out), fwrite(&context, 8, 1, ledger_out), fflush(ledger_out);
}
void ledger_commit() {
    std::lock_guard lk(ledger_mu);
    if (!ledger_out || ledger_pending.empty()) return;
    fwrite(ledger_pending.data(), 8, ledger_pending.size(), ledger_out);
    fflush(ledger_out);
    ledger_pending.clear();
}
uint64_t ledger_id_seen(uint64_t fp) { return fp ? mix(fp, 0x7365656Eull) : 0; }
thread_local std::vector<uint64_t>* fingerprint_probe = nullptr;   // (update_changed_share: the fingerprints met, not listed)
uint64_t ledger_id_libs(const Program& p) { return p.fp ? mix(p.fp, 0x6C6962ull) : 0; }
uint64_t ledger_id_compute(const Program& p) { return p.fp ? mix(p.fp, 0x6373ull) : 0; }
uint64_t ledger_id_pipeline(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo) {
    return p.fp ? mix(pipeline_key_from(mix(p.fp, 0x706970ull), p, s, at, topo), 1) : 0;
}
}  // namespace

PipeEntry* graphics_pipeline(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, const uint8_t color_types[8]) {
    // the key: what is baked into it (the rest is dynamic state)
    // (hookshot: and which variant: p.key is the same for all the variants of a program by its textures' types, and a draw of
    // one ran another's shaders under its own descriptors)
    uint32_t formats[8]{};
    uint32_t vattr[16]{}, divs[16]{};
    uint64_t blend[8]{};
    for (int i = 0; i < 8 && i < at.n; i++) formats[i] = (uint32_t)at.color_format[i];
    for (int i = 0; i < 16; i++) vattr[i] = s.vattr[i], divs[i] = s.vstream[i].divisor;
    for (int i = 0; i < 8; i++) std::memcpy(&blend[i], &s.blend[i], 8);
    const int topo_c = topology_class(topo);
    const uint32_t blend_en = (uint32_t)s.color.blend_en, chmask = s.chmask.mask, poly = (uint32_t)s.poly.mode;
    const uint32_t clamp = s.depth_clamp ? 1u : 0u, a2c = s.ms.a2c ? 1u : 0u;
    for (const PipeFast& f : pipe_fast)
        if (f.on && f.pe &&
            pipe_same((uint64_t)(uintptr_t)&p, p.variant, topo_c, (uint32_t)at.samples, at.n, (uint32_t)at.depth_format,
                      formats, s.nvattr, vattr, divs, blend_en, blend, chmask, poly, clamp, a2c, f.prog, f.variant, f.topo,
                      f.samples, f.n, f.depth, f.formats, f.nvattr, f.vattr, f.div, f.blend_en, f.blend, f.chmask, f.poly,
                      f.clamp, f.a2c)) {
            FINE_N(FC_PIPE_FAST, 1);
            zone_note_state(f.key);   // (the fast hit keeps the key of its entry: the set lookup, as on the slow path)
            return f.pe;
        }
    auto remember = [&](PipeEntry* e, uint64_t pkey) {
        if (!e) return e;
        zone_note_state(pkey);   // (every slow-path lookup notes the pipeline's key: the set lookup, once a key per map)
        if (!(pipe_fast[0].on && pipe_fast[0].pe == e)) pipe_fast[1] = pipe_fast[0];
        PipeFast& f = pipe_fast[0];
        f.on = true, f.pe = e, f.key = pkey, f.prog = (uint64_t)(uintptr_t)&p, f.variant = p.variant, f.topo = topo_c;
        f.samples = (uint32_t)at.samples, f.n = at.n, f.depth = (uint32_t)at.depth_format, f.nvattr = s.nvattr;
        f.blend_en = blend_en, f.chmask = chmask, f.poly = poly, f.clamp = clamp, f.a2c = a2c;
        std::memcpy(f.formats, formats, sizeof formats);
        std::memcpy(f.vattr, vattr, sizeof vattr);
        std::memcpy(f.div, divs, sizeof divs);
        std::memcpy(f.blend, blend, sizeof blend);
        return e;
    };
    // (Hookshot 65e2cb91 / 8b1e96ad: a pipeline belongs to the program object it was built from.) The key names a program by its
    // code and its variant's hash, and two program objects can have both the same: the one the precompile found for a note
    // (program_find) and the one a draw picks. A draw then ran one program's shaders under the other's layout, descriptors and
    // constants; with the scene's lit programs a light loop read garbage for its count and the GPU never finished the draw (device
    // lost near water). Another object with the same key gets a pipeline of its own, under a key of its own (not noted: it holds
    // an address); a thread's map is by key and object too. RX_VK_PIPE_BY_PROGRAM=0: shared by the key alone, as before
    static const bool by_program = [] { const char* e = getenv("RX_VK_PIPE_BY_PROGRAM"); return !(e && env_value_off(e)); }();
    const uint64_t pkey = pipeline_key(p, s, at, topo);   // (the pipeline's key as the notes and the recorder know it)
    uint64_t key = pkey;
    const uint64_t own = mix(key, (uint64_t)(uintptr_t)&p);
    const uint64_t mkey = by_program ? own : key;   // (a thread's map: by key and object)
    static thread_local FlatMap<uint64_t, PipeEntry*> mine;   // (entries live as long as the cache; hookshot: flat, flat_map.h)
    if (auto m = mine.find(mkey); m != mine.end()) return FINE_X(FC_PIPE_MAP, 1), remember(m->second, pkey);
    PROF_EXACT();   // (new to this thread, or new: rare, and a pipeline made can take milliseconds)
    ParLock lk;
    auto it = pipelines.find(key);
    bool own_key = false;
    if (by_program && it != pipelines.end()) {
        const Program* made_for = it->second->prog.load(std::memory_order_acquire);
        if (made_for && made_for != &p) {
            static std::atomic<int> told{0};
            if (told++ < 12)
                logf("vk: pipeline %016llx was made for another program object of %016llx (variant %016llx): this one gets its own",
                     (unsigned long long)key, (unsigned long long)p.key, (unsigned long long)p.variant);
            key = own, own_key = true;
            it = pipelines.find(key);
        }
    }
    if (it != pipelines.end()) return remember(mine[mkey] = it->second.get(), pkey);
    pcache_init();
    PipeEntry* e = pipelines.emplace(key, std::make_unique<PipeEntry>()).first->second.get();
    e->prog.store(&p, std::memory_order_release);
    static const bool sync = getenv("RX_VK_SYNC_PIPELINES") != nullptr;
    if (sync) {
        if (!own_key) pipeline_note(key, p, s, at, topo, color_types);   // (its own key holds an address: nothing to note)
        e->pipeline = build_graphics(p, s, at, topo);
        e->ready = true;
        return remember(e, pkey);
    }
    // (TICK-008) Nothing of the pipeline is done on this thread: a worker notes it, links it from the libraries when they are on
    // (RX_VK_GPL: the draw goes ahead on the linked pipeline) and builds it whole. A draw that finds it not ready is left out.
    // (Sync 11: upstream links on the draw's thread; here the worker does, with upstream's cold libraries built after it)
    struct Job {
        PipeEntry* e;
        const Program* p;
        NvnState s;
        Targets at;
        VkPrimitiveTopology topo;
        uint64_t key;
        bool note;                // (false: the entry's key is its own, with an address in it: not noted)
        uint8_t color_types[8];   // (upstream's pipeline state list, state_record)
        uint64_t cold[2];         // (the libraries gpl_link found cold: built by workers after it)
    };
    auto job = std::make_shared<Job>();
    job->e = e, job->p = &p, job->s = s, job->at = at, job->topo = topo, job->key = key, job->note = !own_key, job->cold[0] = job->cold[1] = 0;
    std::memcpy(job->color_types, color_types, 8);
    jobs_push([job] {
        if (job->note) pipeline_note(job->key, *job->p, job->s, job->at, job->topo, job->color_types);
        // (linked from the libraries: the draw goes ahead, and the full build below replaces it. Not linked when a library is
        // cold: the entry stays not ready until the full build)
        if (gpl_wanted() && !job->p->fallback) {
            const auto t0 = std::chrono::steady_clock::now();
            const VkPipeline linked = gpl_link(*job->p, job->s, job->at, job->topo, job->cold);
            st_link_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
            st_links++;
            if (linked) {
                job->e->pipeline = linked;
                job->e->ready.store(true, std::memory_order_release);
            }
        }
        // (over a linked one, which is left alone: draws recorded with it may still be on their way)
        if (const VkPipeline full = build_graphics(*job->p, job->s, job->at, job->topo)) job->e->pipeline = full;
        job->e->ready.store(true, std::memory_order_release);
        // (the cold libraries after it and behind what is queued, with no flag: the draw waits for the pipeline above, and
        // jobs_wait runs one job)
        for (int i = 0; i < 2; i++)
            if (job->cold[i])
                jobs_push([job, i] { gpl_build_cold(*job->p, job->s, job->at, job->topo, i + 1, job->cold[i]); });
    }, false, &e->ready);   // (a draw that waits for it runs it at once if it is still queued: jobs_wait)
    return remember(e, pkey);
}

/// part: -1 the whole pipeline, or the graphics pipeline library it stands for: 0 vertex input, 1 pre-rasterization, 2 fragment
/// shader, 3 fragment output
/// cold: made only if the driver has it compiled already; set, and nothing made, when it would have had to compile
VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part, bool* cold) {
    // (TICK-008, a test: RX_VK_PIPE_DELAY_MS=<ms> makes each whole pipeline take that much longer, as a driver with a cold cache would,
    // to see what the draws do while pipelines are in the building)
    static const int delay_ms = getenv("RX_VK_PIPE_DELAY_MS") ? atoi(getenv("RX_VK_PIPE_DELAY_MS")) : 0;
    if (delay_ms > 0 && part < 0 && !driver_cache_only) Sleep((DWORD)delay_ms);
    if (at.n < 0 || at.n > 8) return VK_NULL_HANDLE;   // (hookshot: at most 8 colour targets)
    const Shader::Info& vs = *vertex_info(p);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (const Stage& st : p.stages) {
        // (hookshot: the fallback's dispatches are not graphics stages. Only a fallback program's: with native mesh shaders
        // the task and mesh stages are the pipeline's own, and a mesh program built without them was a fragment stage
        // alone, which drew none of its geometry and then lost the device, issue 640)
        if (p.fallback && (st.vk_stage & (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)))
            continue;
        VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        si.stage = st.vk_stage;
        si.module = st.module;
        si.pName = "main";
        pin_subgroup(si);
        stages.push_back(si);
    }
    // vertex input: the attributes the vertex stage reads; those without a stream read zeros (binding 15)
    std::vector<VkVertexInputBindingDescription> vb;
    std::vector<VkVertexInputAttributeDescription> va;
    bool stream_used[16]{};
    bool zero_used = false;
    if (!p.mesh && !p.fallback)
        for (uint32_t i = 0; i < 16; i++) {
            if (!vs.loads.Generic(i)) continue;
            const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
            const VkFormat f = (int)i < s.nvattr ? vertex_format(a.fmt) : VK_FORMAT_UNDEFINED;
            if (f == VK_FORMAT_UNDEFINED || a.stream >= 15) {
                va.push_back({i, 15, VK_FORMAT_R32_SFLOAT, 0});   // (an unset attribute reads (0, 0, 0, 1))
                zero_used = true;
                continue;
            }
            va.push_back({i, a.stream, supported_vertex_format(f), a.offset & 0x3FFF});
            stream_used[a.stream] = true;
        }
    for (uint32_t b = 0; b < 15; b++)
        if (stream_used[b])
            vb.push_back({b, s.vstream[b].stride,
                          s.vstream[b].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    if (zero_used) vb.push_back({15, 0, VK_VERTEX_INPUT_RATE_VERTEX});
    // (a divisor over 1: one element per that many instances (VK_EXT_vertex_attribute_divisor); it read one per
    // instance before)
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> vdivs;
    for (uint32_t b = 0; b < 15; b++)
        if (stream_used[b] && s.vstream[b].divisor > 1) {
            static std::atomic<int> logged{0};
            if (logged++ < 8)
                logf("vk: vertex stream %u divisor %u%s", b, s.vstream[b].divisor, dev.vertex_divisor ? "" : " (not supported: 1)");
            if (dev.vertex_divisor) vdivs.push_back({b, std::min(s.vstream[b].divisor, dev.max_vertex_divisor)});
        }
    {   // (each NVN vertex format once: what the game draws with)
        static std::mutex mu;
        static bool seen[256];
        std::lock_guard lk(mu);
        for (const auto& d : va)
            if (d.binding != 15) {
                const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[d.location]);
                if (a.fmt < 256 && !seen[a.fmt]) seen[a.fmt] = true, logf("vk: vertex format NVN %#x -> vk %d", (unsigned)a.fmt, (int)d.format);
            }
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vdi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT};
    vdi.vertexBindingDivisorCount = (uint32_t)vdivs.size();
    vdi.pVertexBindingDivisors = vdivs.data();
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)vb.size();
    vi.pVertexBindingDescriptions = vb.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)va.size();
    vi.pVertexAttributeDescriptions = va.data();
    if (!vdivs.empty()) vi.pNext = &vdi;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    // (dynamic: left out, or each value of them would be another library)
    rs.depthClampEnable = !dev.eds3_clamp && s.depth_clamp;
    rs.polygonMode = dev.eds3_polygon || s.poly.mode > 1 ? VK_POLYGON_MODE_FILL : s.poly.mode == 0 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_LINE;
    rs.lineWidth = 1.0f;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pvs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pvs.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;   // (NVN's default, as OpenGL's)
    if (dev.provoking_vertex) rs.pNext = &pvs;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = at.samples;
    ms.alphaToCoverageEnable = s.ms.a2c && !dev.eds3_a2c && !no_a2c();
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb[8]{};
    for (int i = 0; i < at.n; i++) {
        const nvn_blend_st& b = s.blend[i];
        cb[i].blendEnable = (s.color.blend_en >> i & 1) && at.color_format[i] ? VK_TRUE : VK_FALSE;
        cb[i].srcColorBlendFactor = blend_factor(b.src_rgb);
        cb[i].dstColorBlendFactor = blend_factor(b.dst_rgb);
        cb[i].colorBlendOp = blend_op(b.eq_rgb);
        cb[i].srcAlphaBlendFactor = blend_factor(b.src_a);
        cb[i].dstAlphaBlendFactor = blend_factor(b.dst_a);
        cb[i].alphaBlendOp = blend_op(b.eq_a);
        cb[i].colorWriteMask = target_write_mask(p, s, i);
    }
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = (uint32_t)at.n;
    bs.pAttachments = cb;
    std::vector<VkDynamicState> dyn = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_LINE_WIDTH,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE};
    if (!p.mesh) {
        dyn.push_back(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);
        dyn.push_back(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE);
        dyn.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
    }
    if (dev.eds3_blend) {
        dyn.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
        dyn.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
        dyn.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
    }
    if (dev.eds3_polygon) dyn.push_back(VK_DYNAMIC_STATE_POLYGON_MODE_EXT);
    if (dev.eds3_clamp) dyn.push_back(VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT);
    if (dev.eds3_a2c) dyn.push_back(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = (uint32_t)dyn.size();
    dy.pDynamicStates = dyn.data();
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = (uint32_t)at.n;
    ri.pColorAttachmentFormats = at.color_format;
    if (at.depth) {
        if (depth_aspect(at.depth_format)) ri.depthAttachmentFormat = at.depth_format;
        if (has_stencil(at.depth_format)) ri.stencilAttachmentFormat = at.depth_format;
    }
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = (uint32_t)stages.size();
    ci.pStages = stages.data();
    ci.pVertexInputState = p.mesh ? nullptr : &vi;
    ci.pInputAssemblyState = p.mesh ? nullptr : &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &bs;
    ci.pDynamicState = &dy;
    ci.layout = p.layout;
    VkGraphicsPipelineLibraryCreateInfoEXT library{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    std::vector<VkPipelineShaderStageCreateInfo> part_stages;
    if (part >= 0) {
        static const VkGraphicsPipelineLibraryFlagsEXT parts[4] = {
            VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
            VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
        library.flags = parts[part];
        ri.pNext = &library;
        ci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        for (const VkPipelineShaderStageCreateInfo& si : stages)
            if ((part == 2 && si.stage == VK_SHADER_STAGE_FRAGMENT_BIT) || (part == 1 && si.stage != VK_SHADER_STAGE_FRAGMENT_BIT))
                part_stages.push_back(si);
        ci.stageCount = (uint32_t)part_stages.size();
        ci.pStages = part_stages.data();
        ci.pVertexInputState = part == 0 ? &vi : nullptr;
        ci.pInputAssemblyState = part == 0 ? &ia : nullptr;
        ci.pViewportState = part == 1 ? &vp : nullptr;
        ci.pRasterizationState = part == 1 ? &rs : nullptr;
        ci.pMultisampleState = part >= 2 ? &ms : nullptr;
        ci.pDepthStencilState = part == 2 ? &ds : nullptr;
        ci.pColorBlendState = part == 3 ? &bs : nullptr;
        ci.layout = part == 1 || part == 2 ? p.layout : VK_NULL_HANDLE;
        if (part != 3) {
            ri.colorAttachmentCount = 0, ri.pColorAttachmentFormats = nullptr;
            ri.depthAttachmentFormat = ri.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
        }
    }
    VkPipeline pl = VK_NULL_HANDLE;
    {   // (local) RX_VK_NOOPT=1: build every pipeline without the driver's optimizer. AMD's proprietary compiler crashes
        // (-13 here) or takes minutes on some Ocarina shaders it builds in seconds unoptimized (Lichii #200, #210).
        static const bool noopt = getenv("RX_VK_NOOPT") != nullptr;
        if (noopt) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        // (from PR 430, YunaraDebugs: on AMD the task and mesh programs (OoT's foliage culling, following pointers through its
        // tables) hung the GPU when the driver's optimizer built them: a device lost one to four minutes into Kokiri, each time with
        // another of them first (RX 6600, issues 461 and 484); never in runs where they were built without it. They are built
        // without it on AMD's own drivers. RX_VK_MESH_OPT=1: with it everywhere; RX_VK_MESH_OPT=0: without it on every AMD driver.
        // (issue 14, after upstream Hookshot eed4d912a / #1965: on Mesa's RADV it is the other way round: built WITHOUT the
        // optimizer these programs run so long that the GPU is reset (Steam Deck, RDNA 2; a Deck tester's hang gone with
        // RX_VK_MESH_OPT=1, two areas, frame 4380, 33 fps). So RADV builds them with it, as every other driver)
        static const bool amd_own = [] {
            VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 pr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
            vkGetPhysicalDeviceProperties2(dev.physical, &pr);
            return pr.properties.vendorID == 0x1002 &&
                   (drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY || drv.driverID == VK_DRIVER_ID_AMD_OPEN_SOURCE);
        }();
        static const bool amd_any = [] {
            VkPhysicalDeviceProperties pr{};
            vkGetPhysicalDeviceProperties(dev.physical, &pr);
            return pr.vendorID == 0x1002;
        }();
        static const char* const mesh_env = getenv("RX_VK_MESH_OPT");
        const bool unoptimized = mesh_env ? (atoi(mesh_env) == 0 && amd_any) : amd_own;
        if (unoptimized) {
            bool mesh = false;
            for (const Stage& st : p.stages) mesh |= (st.vk_stage & (VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) != 0;
            if (mesh) {
                ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
                static std::atomic<int> told{0};
                if (told++ < 1) logf("vk: task and mesh programs built without the driver's optimizer (AMD's own driver; RX_VK_MESH_OPT=1: with it)");
            }
        }
    }
    if (cold) ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    // (HeyListen, RX_VK_PCACHE_MISSES: whether the pipeline cache had it. A hit adds nothing to the cache: counted as built, the
    // 1.2 GB cache was fetched and written again at every scene load, whose pipelines all come from it)
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo feedback_ci{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    feedback_ci.pPipelineCreationFeedback = &feedback;
    if (pcache_misses_only() && part < 0 && !driver_cache_only) feedback_ci.pNext = ci.pNext, ci.pNext = &feedback_ci;
    VkResult r;
    {
        // (longshot #252, Windows Intel: one thread in the compiler. A draw's ask compiles nothing and must not wait behind a
        // worker's compile: while the gate is held it is answered cold, as a pipeline the cache lacks is)
        IntelDriverGate gate(cold != nullptr);
        if (gate.busy) return intel_gate_busy(), *cold = true, VK_NULL_HANDLE;
        r = guarded_graphics_held(part >= 0 || driver_cache_only ? VK_NULL_HANDLE : pcache, &ci, &pl);
        // (d1d6caa7, and Hookshot f1eb23c9 for every graphics pipeline: Chad-) the AMD Windows compiler can reject an ordinary
        // graphics pipeline with its optimizer on (-13: the Market's and Kakariko's dogs on an RX 6600 XT; the cards that run task +
        // mesh draws as compute, issue 996): once more without it, which it builds. Not for the memory errors
        if (retry_without_optimizer(r, ci.flags)) {
            const VkResult first = r;
            ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
            pl = VK_NULL_HANDLE;
            r = guarded_graphics_held(part >= 0 || driver_cache_only ? VK_NULL_HANDLE : pcache, &ci, &pl);
            static std::atomic<int> told{0};
            if (told++ < 20)
                logf("vk: %s %016llx failed (%d) with the driver's optimizer: %s", part >= 0 ? "a library of" : "graphics pipeline for",
                     (unsigned long long)p.key, (int)first, r == VK_SUCCESS ? "made without it" : "not made without it either");
        }
    }
    if (cold && r == VK_PIPELINE_COMPILE_REQUIRED) return *cold = true, VK_NULL_HANDLE;
    if (part >= 0) {
        if (r != VK_SUCCESS) {
            static std::atomic<int> logged;
            if (logged++ < 10) logf("vk: library %d of %016llx not made (%d)", part, (unsigned long long)p.key, (int)r);
            return VK_NULL_HANDLE;
        }
        return pl;
    }
    if (r != VK_SUCCESS) {
        static std::atomic<int> logged;
        if (logged++ < 10) logf("vk: graphics pipeline for %016llx not made (%d)", (unsigned long long)p.key, (int)r);
        pl = VK_NULL_HANDLE;
    }
    st_pipelines++;
    if (pl && shader_key_on()) ledger_add(ledger_id_pipeline(p, s, at, topo));
    if (!driver_cache_only &&
        !(pcache_misses_only() && (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
          (feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)))
        pcache_built_since_save++;
    st_pipeline_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    return pl;
}

// ---- samplers (by NVN sampler parameters) ----
/// (longshot #442) What vkCreateSampler reads of the create info sampler_make fills in, less what no sample can reach: the anisotropy
/// of a sampler without anisotropic filtering (Vulkan ignores it), the border colour of one no address mode clamps to its border.
/// Two keys whose infos are equal get samplers that sample alike, so they share one (sampler_book). The comparison of a sampler
/// that does not compare stays in it: a driver may compare anyway (the amd-fix note below, RX_VK_DREF_FROM_SAMPLER=1). 32-bit
/// words without padding, compared as bytes (a bias of -0 apart from one of +0: two samplers, as two keys were)
struct SamplerInfo {
    uint32_t mag, min, mip, u, v, w;
    float bias, aniso, min_lod, max_lod;
    uint32_t aniso_on, compare_on, compare_op, border;
    float custom[4];      // (a custom border colour's, else 0)
    uint32_t reduction;   // (VK_SAMPLER_REDUCTION_MODE_* + 1; 0: none)
    bool operator==(const SamplerInfo& o) const { return !std::memcmp(this, &o, sizeof o); }
};
static_assert(sizeof(SamplerInfo) == 19 * 4, "SamplerInfo is compared as bytes: no padding");
struct SamplerInfoHash {
    size_t operator()(const SamplerInfo& i) const {
        uint32_t w[sizeof i / 4];
        std::memcpy(w, &i, sizeof i);
        uint64_t h = 0x5A;
        for (const uint32_t x : w) h = mix(h, x);
        return (size_t)h;
    }
};
SamplerInfo sampler_info(const VkSamplerCreateInfo& ci) {
    SamplerInfo i{};
    i.mag = ci.magFilter, i.min = ci.minFilter, i.mip = ci.mipmapMode;
    i.u = ci.addressModeU, i.v = ci.addressModeV, i.w = ci.addressModeW;
    i.bias = ci.mipLodBias, i.min_lod = ci.minLod, i.max_lod = ci.maxLod;
    i.aniso_on = ci.anisotropyEnable, i.aniso = ci.anisotropyEnable ? ci.maxAnisotropy : 0.0f;
    i.compare_on = ci.compareEnable, i.compare_op = ci.compareOp;
    const VkSamplerAddressMode b = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    const bool border = ci.addressModeU == b || ci.addressModeV == b || ci.addressModeW == b;
    i.border = border ? (uint32_t)ci.borderColor : 0u;
    for (auto* n = static_cast<const VkBaseInStructure*>(ci.pNext); n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT && border)
            std::memcpy(i.custom, reinterpret_cast<const VkSamplerCustomBorderColorCreateInfoEXT*>(n)->customBorderColor.float32,
                        sizeof i.custom);
        else if (n->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO)
            i.reduction = (uint32_t)reinterpret_cast<const VkSamplerReductionModeCreateInfo*>(n)->reductionMode + 1u;
    }
    return i;
}
/// (longshot #442) The samplers sampler_make made: one for each SamplerInfo, and the keys that resolve to each, under the replay's
/// lock (ParLock; the atomics are read without it). A driver makes at most maxSamplerAllocationCount samplers (NVIDIA about 4000,
/// NVK 4096): past that vkCreateSampler fails. The renderer never destroyed one, and made a new set at each Anisotropic filtering,
/// Render resolution or Upscaler change (HeyListen issue 30: a frame rate that falls over long sessions). So before a new one would
/// go past the room sampler_room keeps, the least recently resolved one is retired: out of every table at once (the keys here, each
/// thread's `mine`, sampler_for's tables, the texture slots' copies of it and the descriptors kept by reuse: retired_gen,
/// samplers_changed), and destroyed once the GPU is past every recording made so far (vk_retirement_value), since a thread's own
/// tables may still hand it to the draws it is recording until it reads retired_gen again. Those tables hide a sampler's use from
/// the book, so from half the retiring point on (watching) their entries last a present: each sampler in use resolves here once a
/// present
struct SamplerBook {
    struct Made { VkSampler s; uint64_t used; };       // (used: tick when a key last resolved to it here; only the order to retire in)
    struct Retired { VkSampler s; uint64_t after; };   // (destroyed once the timeline reaches after; 0: never)
    std::unordered_map<SamplerInfo, Made, SamplerInfoHash> made;   // (the owner)
    std::unordered_map<uint64_t, Made*> keys;   // (sampler_make's key -> its sampler in made; several keys may share one)
    std::vector<Retired> retired;
    uint64_t tick = 0;
    std::atomic<uint64_t> retired_gen{0};   // (moves at each retirement: each thread's tables drop what they hold)
    std::atomic<bool> watching{false};      // (near the device's limit, for good: the threads' tables last a present)
    // (for the log lines and the count in the VRAM line, read without the lock)
    std::atomic<uint32_t> made_n{0}, retired_n{0}, shared_keys{0};
    std::atomic<uint64_t> retirements{0}, destroyed{0}, refused{0};
};
SamplerBook sampler_book;
/// The retired samplers the GPU is done with (the timeline at completed), destroyed
void samplers_reap(uint64_t completed) {
    SamplerBook& b = sampler_book;
    std::erase_if(b.retired, [&](const SamplerBook::Retired& r) {
        if (!r.after || r.after > completed) return false;
        vkDestroySampler(dev.device, r.s, nullptr);
        b.destroyed.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
    b.retired_n.store((uint32_t)b.retired.size(), std::memory_order_relaxed);
}
/// The least recently resolved sampler retired (sampler_book). false: none, or no room to note it (it stays made: one not noted
/// would never be destroyed)
bool sampler_retire() {
    SamplerBook& b = sampler_book;
    if (b.made.empty()) return false;
    auto lru = b.made.begin();
    for (auto it = b.made.begin(); it != b.made.end(); ++it)
        if (it->second.used < lru->second.used) lru = it;
    try {
        b.retired.push_back({lru->second.s, vk_retirement_value()});
    } catch (const std::exception&) {
        return false;
    }
    const SamplerBook::Made* gone = &lru->second;
    std::erase_if(b.keys, [gone](const auto& kv) { return kv.second == gone; });
    b.made.erase(lru);
    b.retired_gen.fetch_add(1, std::memory_order_acq_rel);
    b.made_n.store((uint32_t)b.made.size(), std::memory_order_relaxed);
    b.retired_n.store((uint32_t)b.retired.size(), std::memory_order_relaxed);
    b.shared_keys.store((uint32_t)b.keys.size(), std::memory_order_relaxed);
    samplers_changed();   // (sampler_for's tables and the descriptors kept by reuse resolve theirs again)
    const uint64_t n = b.retirements.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || n % 1000 == 0)
        logf("vk: samplers: %zu made, the device allows %u: the least recently used retired (%llu so far), each destroyed once "
             "the GPU is past the recordings that may use it", b.made.size() + 1, dev.props.limits.maxSamplerAllocationCount,
             (unsigned long long)n);   // (made + 1: as many as there were)
    return true;
}
/// Whether sampler_make may make one more sampler. The room: maxSamplerAllocationCount (4000, Vulkan's least, when not known) less
/// a reserve of 64 (a quarter of a smaller limit) for the renderer's other samplers (the blits', the upscalers', frame generation's)
/// and an upscaler SDK's. From 3/4 of it the least recently resolved ones are retired, one for each made; the last 1/4 is for those
/// made while retired ones are still the GPU's. Full only when the GPU may still use every one of those: then nothing is retired and
/// the draw waits for a later present (a failed key's rule)
bool sampler_room() {
    SamplerBook& b = sampler_book;
    const uint64_t limit = dev.props.limits.maxSamplerAllocationCount ? dev.props.limits.maxSamplerAllocationCount : 4000;
    const uint64_t room = limit - std::min<uint64_t>(64, limit / 4), keep = room - room / 4;
    if (!b.retired.empty()) samplers_reap(vk_timeline_reached());
    if (b.made.size() + b.retired.size() >= room) {
        static std::atomic<int> told{0};
        if (told++ < 8)
            logf("vk: samplers: %zu made and %zu retired that the GPU may still use fill the %llu this device allows "
                 "(maxSamplerAllocationCount %u): a draw needing a new one waits for a later present", b.made.size(),
                 b.retired.size(), (unsigned long long)room, dev.props.limits.maxSamplerAllocationCount);
        return false;
    }
    if (b.made.size() >= keep / 2) b.watching.store(true, std::memory_order_relaxed);
    while (b.made.size() >= keep && sampler_retire()) {}
    return true;
}
/// (the VRAM line of the periodic log, vk_texture.cpp) the samplers made, those retired and not yet destroyed, and those retired in all
void sampler_counts_read(uint32_t* made, uint32_t* retired_waiting, uint64_t* retired_total, uint64_t* destroyed, uint32_t* limit) {
    const SamplerBook& b = sampler_book;
    *made = b.made_n.load(std::memory_order_relaxed), *retired_waiting = b.retired_n.load(std::memory_order_relaxed);
    *retired_total = b.retirements.load(std::memory_order_relaxed), *destroyed = b.destroyed.load(std::memory_order_relaxed);
    *limit = dev.props.limits.maxSamplerAllocationCount;
}
VkSamplerAddressMode wrap(uint8_t w) {
    switch (w) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
// (hookshot) RX_VK_ANISO=<n> (default 0: the game's own): the samplers that filter linearly between texels and between mip levels
// get at least n-times anisotropic filtering (the game's value if it asks for more; the device's limit if n is above it): surfaces seen
// at a slant keep their detail, for some GPU time. Part of every sampler's key below, so no cached sampler is one made for another n
uint32_t aniso_override() {
    static const uint32_t n = getenv("RX_VK_ANISO") ? (uint32_t)std::clamp(atoi(getenv("RX_VK_ANISO")), 0, 16) : 0u;
    return n;
}
/// compare: the shader reads depth with a reference (a sampler without one cannot compare)
VkSampler sampler_make(int id, bool compare);
// (each texture of each draw found its sampler through reg_mu, a copy of the registration, a hash of it and a map lookup:
// ~48000 times a frame at 3000 draws. Now a thread's table by id and compare, good while no sampler registration's contents
// changed (sampler_registrations_generation). RX_VK_SAMPLER_CACHE=0 turns off both this table and the parameters' cache inside
// sampler_make; =1 or the Sampler cache row turns the parameters' cache on (this table stays on, as it is without the option)
bool sampler_cache_on();   // (hookshot, RX_VK_SAMPLER_CACHE and the Sampler cache row: defined below with the descriptor fingerprint's)
VkSampler sampler_for(int id, bool compare) {
    static const bool on = !getenv("RX_VK_SAMPLER_CACHE") || atoi(getenv("RX_VK_SAMPLER_CACHE")) != 0;
    if (!on || id < 0 || id >= 4096) return sampler_make(id, compare);
    struct Entry { uint64_t gen; VkSampler s; uint64_t frame; };
    static thread_local Entry table[2][4096];   // (gen 0: never filled; the generation starts at 1)
    const uint64_t gen = sampler_registrations_generation();   // (read first: a registration after it makes the next call miss)
    // (near the device's sampler limit, an entry lasts a present: the book sees the samplers in use, sampler_book)
    const uint64_t frame = sampler_book.watching.load(std::memory_order_relaxed) ? texture_frame_no() : 0;
    Entry& e = table[compare ? 1 : 0][id];
    if (e.gen == gen && e.frame == frame) return e.s;
    const VkSampler made = sampler_make(id, compare);
    if (made) e = {gen, made, frame};   // (a failed creation is retryable without a new registration)
    return made;
}
VkSampler sampler_make(int id, bool compare) {
    nvn_samp_par p{};
    bool known = false;
    if (sampler_cache_on()) {   // (hookshot: what an id holds stays cached with its version on this thread: resolving an id
        static thread_local std::unordered_map<int, std::pair<uint32_t, nvn_samp_par>> seen;   // (that was not registered
        const uint32_t v = sampler_slot_version(id);                                            // (anew takes no lock)
        if (auto it = seen.find(id); it != seen.end() && it->second.first == v)
            p = it->second.second, known = true;
        else if (sampler_of(id, &p)) seen[id] = {v, p}, known = true;   // (ids are < 4096: the table cannot outgrow them)
    } else
        known = sampler_of(id, &p);
    if (!known) p.minf = NVN_MIN_FILTER_LINEAR, p.magf = NVN_MAG_FILTER_LINEAR, p.lod_max = 1000.0f;
    uint64_t key = mix(compare ? 0xC0 : 0xAB, aniso_override());
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&p);
    for (size_t i = 0; i < sizeof p; i += 8) {
        uint64_t w = 0;
        std::memcpy(&w, b + i, std::min<size_t>(8, sizeof p - i));
        key = mix(key, w);
    }
    static thread_local std::unordered_map<uint64_t, VkSampler> mine;   // (dropped at a retirement, its samplers may be going, and
    static thread_local uint64_t mine_gen = 0, mine_frame = 0;          // at each present near the limit: sampler_for's note)
    const uint64_t g = sampler_book.retired_gen.load(std::memory_order_acquire);
    const uint64_t f = sampler_book.watching.load(std::memory_order_relaxed) ? texture_frame_no() : 0;
    if (g != mine_gen || f != mine_frame) mine.clear(), mine_gen = g, mine_frame = f;
    if (auto m = mine.find(key); m != mine.end()) return m->second;
    ParLock lk;
    SamplerBook& book = sampler_book;
    if (auto it = book.keys.find(key); it != book.keys.end()) {
        it->second->used = ++book.tick;
        try { mine.emplace(key, it->second->s); } catch (const std::exception&) {}   // (the book still owns it)
        return it->second->s;
    }
    // (longshot #442) A key whose sampler could not be made (the driver's limit, or its refusal) gets one attempt a present: unrelated
    // keys go on, and a later present retries without a new registration
    static uint64_t failed_frame = UINT64_MAX;
    static std::unordered_set<uint64_t> failed;
    static bool tracking_failed = false;
    const uint64_t frame = texture_frame_no();
    if (failed_frame != frame) {
        failed.clear();
        failed_frame = frame;
        tracking_failed = false;
    }
    if (tracking_failed || failed.count(key)) return VK_NULL_HANDLE;
    auto remember_failure = [&] {
        book.refused.fetch_add(1, std::memory_order_relaxed);
        try { failed.insert(key); } catch (const std::exception&) { tracking_failed = true; }
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = p.magf == NVN_MAG_FILTER_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = (p.minf == NVN_MIN_FILTER_NEAREST || p.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST ||
                    p.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = (p.minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p.minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR)
                        ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    {   // (hookshot, issue 1117: blocky halos, smoke and fog shafts since v2.0.6, gone with every sampler linear. The nearest ones
        // named, with the game's own words for them; RX_VK_FORCE_LINEAR=1: every sampler linear, for the A/B)
        static const bool force_linear = getenv("RX_VK_FORCE_LINEAR") != nullptr;
        if (ci.magFilter == VK_FILTER_NEAREST || ci.minFilter == VK_FILTER_NEAREST) {
            static std::atomic<int> told{0};
            if (told++ < 24)
                logf("vk: sampler %d is nearest: min filter %u, mag filter %u, wrap %u/%u/%u, lod %g..%g bias %g, aniso %g, compare %d%s", id,
                     (unsigned)p.minf, (unsigned)p.magf, (unsigned)p.ws, (unsigned)p.wt, (unsigned)p.wr, (double)p.lod_min,
                     (double)p.lod_max, (double)p.lod_bias, (double)p.aniso, (int)compare, force_linear ? " (made linear: RX_VK_FORCE_LINEAR)" : "");
            if (force_linear) ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
        }
    }
    ci.addressModeU = wrap(p.ws), ci.addressModeV = wrap(p.wt), ci.addressModeW = wrap(p.wr);
    ci.mipLodBias = std::clamp(p.lod_bias, -dev.props.limits.maxSamplerLodBias, dev.props.limits.maxSamplerLodBias);
    float aniso = p.aniso;
    {   // (RX_VK_ANISO: what the game asks for is told once for each value, 16 at most: what it sets is not known otherwise)
        static std::mutex am;
        static std::unordered_set<uint32_t> asked;
        uint32_t bits;
        std::memcpy(&bits, &p.aniso, sizeof bits);
        std::lock_guard ag(am);
        if (asked.size() < 16 && asked.insert(bits).second)
            logf("vk: sampler anisotropy: the game asks for %g (min filter %u, mip filter %s; RX_VK_ANISO=%u)", (double)p.aniso, (unsigned)p.minf,
                 ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? "linear" : "nearest", aniso_override());
    }
    if (aniso_override() && ci.minFilter == VK_FILTER_LINEAR && ci.mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR)
        aniso = std::max(aniso, (float)aniso_override());
    ci.anisotropyEnable = aniso > 1.0f && dev.features.samplerAnisotropy;
    ci.maxAnisotropy = std::clamp(aniso, 1.0f, dev.props.limits.maxSamplerAnisotropy);
    ci.compareEnable = compare && p.cmp_mode != 0;
    ci.compareOp = compare_op(p.cmp_func);
    {   // (amd-fix) A depth-reference sample through a sampler that does not compare is undefined in Vulkan: NVIDIA's driver
        // compares anyway, AMD's does not (OoT samples this way: compare mode 0, function 0). A shader that compares gets
        // a comparing sampler: the game's function when it set one, LEQUAL otherwise. RX_VK_DREF_FROM_SAMPLER=1: as before
        static const bool as_before = getenv("RX_VK_DREF_FROM_SAMPLER") != nullptr;
        if (compare && !as_before && !ci.compareEnable) {
            ci.compareEnable = VK_TRUE;
            ci.compareOp = p.cmp_func >= 1 && p.cmp_func <= 8 ? compare_op(p.cmp_func) : VK_COMPARE_OP_LESS_OR_EQUAL;
        }
        static std::mutex lm;
        static std::unordered_set<uint32_t> told;
        std::lock_guard lg(lm);
        if (compare && told.size() < 16 && told.insert((uint32_t)p.cmp_mode << 8 | p.cmp_func).second)
            logf("vk: depth-compare sampler: the game's compare mode %u function %u -> compare %s op %d (filters min %d mag %d)",
                 p.cmp_mode, p.cmp_func, ci.compareEnable ? "on" : "off", (int)ci.compareOp, (int)ci.minFilter, (int)ci.magFilter);
    }
    {   // (hookshot) A depth comparison through a nearest filter is one test a pixel: shadow edges in stair steps, the shadow map's
        // texels as squares over Hyrule Field's hills (Discord, 2026-11-09). Linear, the GPU tests the four nearest texels and blends
        // them (percentage-closer filtering), the soft edge the console's picture has. RX_VK_SHADOW_PCF=0: the game's filter
        static const bool pcf = !getenv("RX_VK_SHADOW_PCF") || atoi(getenv("RX_VK_SHADOW_PCF")) != 0;
        if (pcf && ci.compareEnable) ci.magFilter = ci.minFilter = VK_FILTER_LINEAR;
    }
    ci.minLod = std::max(0.0f, p.lod_min);
    // (no mipmap filter: the base level only, as NVN samples it)
    ci.maxLod = (p.minf == NVN_MIN_FILTER_NEAREST || p.minf == NVN_MIN_FILTER_LINEAR) ? ci.minLod + 0.25f
                                                                                       : std::max(ci.minLod, p.lod_max);
    VkSamplerCustomBorderColorCreateInfoEXT bc{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    const bool black = !p.border[0] && !p.border[1] && !p.border[2];
    const bool white = p.border[0] == 1.0f && p.border[1] == 1.0f && p.border[2] == 1.0f;
    if (black && !p.border[3]) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (black && p.border[3] == 1.0f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (white && p.border[3] == 1.0f) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (dev.custom_border_color) {
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        std::memcpy(bc.customBorderColor.float32, p.border, 16);
        bc.format = VK_FORMAT_UNDEFINED;
        ci.pNext = &bc;
    } else {
        ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (p.reduction && !compare) {   // (min / max filtering)
        static VkSamplerReductionModeCreateInfo rm{VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
        rm.reductionMode = p.reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX;
        rm.pNext = (void*)ci.pNext;
        ci.pNext = &rm;
    }
    const SamplerInfo info = sampler_info(ci);
    if (auto it = book.made.find(info); it != book.made.end()) {   // (another key's sampler samples alike: shared, not made again)
        it->second.used = ++book.tick;
        try { book.keys.emplace(key, &it->second); } catch (const std::exception&) {}   // (only the keys' table: made owns it)
        book.shared_keys.store((uint32_t)book.keys.size(), std::memory_order_relaxed);
        return it->second.s;
    }
    if (!sampler_room()) {   // (the device's samplers all in the GPU's hands: none made past its limit)
        remember_failure();
        return VK_NULL_HANDLE;
    }
    VkSampler sm = VK_NULL_HANDLE;
    if (vkCreateSampler(dev.device, &ci, nullptr, &sm) != VK_SUCCESS) {
        remember_failure();   // (an error's output handle is undefined, not a resource to cache or destroy)
        return VK_NULL_HANDLE;
    }
    SamplerBook::Made* made = nullptr;
    try {
        made = &book.made.emplace(info, SamplerBook::Made{sm, ++book.tick}).first->second;
    } catch (const std::exception&) {
        vkDestroySampler(dev.device, sm, nullptr);   // (only the new sampler whose ownership was not committed)
        remember_failure();
        return VK_NULL_HANDLE;
    }
    try { book.keys.emplace(key, made); } catch (const std::exception&) {}   // (only the keys' table: the next look finds it by info)
    book.made_n.store((uint32_t)book.made.size(), std::memory_order_relaxed);
    book.shared_keys.store((uint32_t)book.keys.size(), std::memory_order_relaxed);
    return sm;
}

VkImageViewType view_type(Shader::TextureType t) {
    switch (t) {
    case Shader::TextureType::Color1D: return VK_IMAGE_VIEW_TYPE_1D;
    case Shader::TextureType::ColorArray1D: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case Shader::TextureType::ColorArray2D: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case Shader::TextureType::Color3D: return VK_IMAGE_VIEW_TYPE_3D;
    case Shader::TextureType::ColorCube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case Shader::TextureType::ColorArrayCube: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}
VkFormat image_format(Shader::ImageFormat f) {
    switch (f) {
    case Shader::ImageFormat::R8_UINT: return VK_FORMAT_R8_UINT;
    case Shader::ImageFormat::R8_SINT: return VK_FORMAT_R8_SINT;
    case Shader::ImageFormat::R16_UINT: return VK_FORMAT_R16_UINT;
    case Shader::ImageFormat::R16_SINT: return VK_FORMAT_R16_SINT;
    case Shader::ImageFormat::R32_UINT: return VK_FORMAT_R32_UINT;
    case Shader::ImageFormat::R32G32_UINT: return VK_FORMAT_R32G32_UINT;
    case Shader::ImageFormat::R32G32B32A32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

// Dummy descriptors for what does not resolve (the null descriptor where the device has it)
struct Dummies {
    Texture* tex[7]{};
    VkImageView view[7]{};
} dummies;

/// A handle word pair as the shader reads it (Eden's TexturePair: primary, optionally ORed with a secondary)
template <class D>
uint32_t handle_of(const NvnState& s, const Stage& st, const D& d, uint32_t k) {
    if constexpr (requires { d.handle_program; }) {   // (read from a storage buffer: its handle program's; of a table, element k's)
        if (d.handle_program != ~0U) return stage_handle(s, st, d.handle_program, k);
    }
    const uint32_t step = 1u << d.size_shift;
    uint32_t h = stage_cbuf_word(s, st, d.cbuf_index, d.cbuf_offset + k * step);
    if constexpr (requires { d.has_secondary; }) {
        if (d.has_secondary) {
            const uint32_t a = h << d.shift_left;
            const uint32_t b = stage_cbuf_word(s, st, d.secondary_cbuf_index, d.secondary_cbuf_offset + k * step)
                               << d.secondary_shift_left;
            h = a | b;
        }
    }
    return h;
}

/// (Fritchii d603d98bb, HandleIsStoragePointer) The handle word of texture k is where a storage buffer of the same program
/// keeps its 8-byte address (and the handle is not computed per draw). (Lichii 282a585a) Or the translation saw the word
/// reach the handle only as a load's address (TextureDescriptor::handle_is_pointer_word), storage buffer or not
bool handle_is_storage_pointer(const Shader::Info& info, const Shader::TextureDescriptor& d, uint32_t k) {
    static const bool off = getenv("RX_VK_HANDLE_POINTER") && getenv("RX_VK_HANDLE_POINTER")[0] == '1';
    if (off || d.handle_program != ~0U) return false;
    if (d.handle_is_pointer_word) return true;
    const uint32_t at = d.cbuf_offset + (k << d.size_shift);
    for (const auto& sb : info.storage_buffers_descriptors) {
        if (sb.cbuf_index != d.cbuf_index) continue;
        for (uint32_t j = 0; j < sb.count; j++) {
            const uint32_t a = sb.cbuf_offset + j * 16;
            if (at >= a && at < a + 8) return true;
        }
    }
    return false;
}

/// The view a texture handle samples (null: nothing resolves)
/// The handles a recording resolved, with what they gave (draws bind the same textures again and again: the slot
/// lookup, the cache's key and the view's are made once a recording, and again when a slot is registered anew)
struct HandleHit {
    VkImageView view;
    Texture* tex;
    uint64_t destroy_gen;
    uint32_t level;   // (the image level the view starts at)
    uint32_t version; // (the slot's, when resolved)
    uint32_t tex_epoch;   // (the texture epoch it was last touched in)
    bool sampling;    // (its use reads the texture's contents)
};
// (hookshot: flat (flat_map.h), searched at every texture binding; cleared whole only, past 65536, in draw_begin_recording)
TLS_OBJECT(handle_hits, FlatMap<uint64_t, HandleHit>)
// (hookshot) RX_VK_TRACE_TEX=<w>x<h>,...: each draw sampling a texture of that size is logged with its state, every 30th frame, 600 lines: a
// model that does not show is found by its textures, and whether the game draws it, and how, read from the line
thread_local const Texture* trace_tex_hit = nullptr;
thread_local uint32_t trace_tex_mask = 0;
bool trace_tex_wanted(const Texture* t) {
    static const std::vector<std::pair<uint32_t, uint32_t>> want = [] {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        if (const char* e = getenv("RX_VK_TRACE_TEX"))
            for (const char* q = e; *q;) {
                uint32_t w = 0, h = 0;
                if (sscanf(q, "%ux%u", &w, &h) == 2) v.push_back({w, h});
                q = strchr(q, ',');
                if (!q) break;
                q++;
            }
        return v;
    }();
    for (size_t i = 0; i < want.size(); i++)
        if (t->width == want[i].first && t->height == want[i].second) trace_tex_mask |= 1u << i;
    return !want.empty() && trace_tex_mask == (1u << want.size()) - 1;   // (a draw with all of them)
}
thread_local VkFormat last_view_format = VK_FORMAT_UNDEFINED;   // (hookshot: RX_VK_LOG_IMG, the view last made)
bool log_img_wanted(uint64_t key) {   // (hookshot) RX_VK_LOG_IMG=<key prefix,...>: its texture and image bindings, 8 times
    static const char* want = getenv("RX_VK_LOG_IMG");
    if (!want) return false;
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
    for (const char* q = want; *q;) {
        const char* e = q;
        while (*e && *e != ',') e++;
        if (e > q && !strncmp(hex, q, (size_t)(e - q))) return true;
        q = *e ? e + 1 : e;
    }
    return false;
}

VkImageView texture_view_of(uint32_t handle, VkImageViewType want, ViewUse use, VkFormat storage_as = VK_FORMAT_UNDEFINED,
                            Texture** tex_out = nullptr, uint32_t* level_out = nullptr) {
    const uint32_t version = texture_slot_version((int)(handle & 0xFFFFF));
    const uint64_t dgen = texture_destroy_generation();
    const uint64_t key = (uint64_t)(handle & 0xFFFFF) | (uint64_t)want << 20 | (uint64_t)use << 28 | (uint64_t)storage_as << 32;
    static const bool log_hits = getenv("RX_VK_LOG_HITS") != nullptr;
    static int64_t st_hit, st_new, st_ver, st_gen, st_n;
    const auto it = handle_hits().find(key);
    if (log_hits) {   // (the counters were a second search of this map on every binding, with the log off)
        if (++st_n % 200000 == 0)
            logf("vk: handle hits %lld, new %lld, slot re-registered %lld, a texture destroyed %lld", (long long)st_hit,
                 (long long)st_new, (long long)st_ver, (long long)st_gen);
        if (it == handle_hits().end()) st_new++;
        else if (it->second.version != version) st_ver++;
        else if (it->second.destroy_gen != dgen) st_gen++;
        else st_hit++;
    }
    // (TICK-052) a texture gone since the view was resolved that was not its own: the view stands (texture_kept_since)
    if (it != handle_hits().end() && it->second.version == version && it->second.destroy_gen != dgen && texture_gone_ring_on() &&
        texture_kept_since(it->second.tex, it->second.destroy_gen, dgen))
        it->second.destroy_gen = dgen;
    if (par_stats_on)
        par_why(it == handle_hits().end() ? PW_VIEW_NEW : it->second.version != version ? PW_VIEW_VERSION
                : it->second.destroy_gen != dgen ? PW_VIEW_DGEN : PW_COUNT);
    if (it != handle_hits().end() && it->second.version == version && it->second.destroy_gen == dgen) {
        if (it->second.tex && it->second.tex_epoch != texture_epoch()) {   // (once a submission: its age and contents)
            if (!texture_touch_quick(it->second.tex, it->second.sampling)) {
                if (par_stats_on) par_why(PW_TOUCH_VIEW);
                ParLock lk;
                texture_touch(it->second.tex, it->second.sampling);
            }
            it->second.tex_epoch = texture_epoch();
        }
        if (tex_out) *tex_out = it->second.tex;
        if (level_out) *level_out = it->second.level;
        return it->second.view;
    }
    ParLock lk;
    nvn_tex t;
    nvn_view v;
    bool has_view = false;
    VkImageView view = VK_NULL_HANDLE;
    Texture* tex = nullptr;
    uint32_t level = 0;
    const bool known = texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view);
    if (known) {
        tex = texture_get(&t, use == ViewUse::Sample || use == ViewUse::Storage);
        if (tex) view = texture_view(tex, &t, has_view ? &v : nullptr, want, use, &last_view_format, storage_as);
        if (tex) level = image_level(tex, has_view ? view_level(&v) : 0);
    }
    if (!view) {   // (what a shader reads as the dummy instead: once per texture slot)
        static std::unordered_map<uint32_t, int> told;
        if (told.size() < 64 && !told[handle & 0xFFFFF]++) {
            if (!known)
                logf("vk: texture slot %#x (handle %#x) is not registered: sampled as the dummy", handle & 0xFFFFF, handle);
            else
                logf("vk: texture slot %#x (handle %#x): %s; NVN target %d format %#x %ux%ux%u levels %u gpu %#llx, view %s "
                     "(format %#x target %d), wanted view type %d use %d",
                     handle & 0xFFFFF, handle, tex ? "no view of it" : "no image for it", t.d.target, t.d.format, t.d.width,
                     t.d.height, t.d.depth, t.d.levels, (unsigned long long)t.gpu, has_view ? "yes" : "no",
                     has_view && v.set_format ? v.format : 0, has_view && v.set_target ? v.target : -1, (int)want, (int)use);
        }
    }
    handle_hits()[key] = {view, tex, dgen, level, version, texture_epoch(), use == ViewUse::Sample || use == ViewUse::Storage};
    if (tex_out) *tex_out = tex;
    if (level_out) *level_out = level;
    return view;
}

/// A uniform buffer's descriptor range: the bank uploaded (driver banks, c1) or the user buffer in its pool
struct BankCache {
    const Program* prog = nullptr;
    int stage = -1;
    uint32_t index = 0, size = 0;
    uint64_t at = 0;
    VkBuffer buf = VK_NULL_HANDLE;   // (the stream it is in: a recording may have moved to a bigger one since)
    uint64_t bind_gen = ~0ull;   // (the bindings it was made for: the same and no new grid, the same bank)
    uint32_t grid[3]{};
    std::vector<uint8_t> bytes;
};
TLS_OBJECT(bank_cache, std::array<std::array<BankCache, kNvnStages>, 2>)   // [c0 or c2][NVN stage]: the last upload (draws in a row share most of it)
// (AyyBeeDee20738, PR 252) An indirect dispatch's grid as its program reads it from c0+0..0xc. dispatch() fills it from guest
// memory as the recording saw it, but OoT's foliage culling chains write the next pass's group counts on the GPU in the same
// frame: the program read stale counts (its own work bounds) and culled plants or kept last frame's. grid_patch_want: c0 gets an
// upload of its own, grid_patch_at, into which dispatch() copies the 12 bytes of arguments on the GPU just before the dispatch,
// as the console's hardware sees them. RX_VK_NO_GRID_PATCH=1: as before
thread_local bool grid_patch_want = false;
thread_local uint64_t grid_patch_at = UINT64_MAX;
thread_local VkBuffer grid_patch_buf = VK_NULL_HANDLE;   // (the stream c0 went to: PR 585's streams can move on)

// (HeyListen, HL_UBO_MEMO=1, default off; ubo_memo.h) a uniform slot's answer kept per thread while the pools' stamp, the update list's
// view and the replay segment stand. HL_UBO_MEMO_CHECK=1 (implies the switch): every hit is also worked out and compared (the buffer, the
// offset, the range), the mismatches counted, the first twelve logged, a line every 10 s. Left off (the memo skips ledger marks and the
// capture's exclusion notes of a hit): RX_VK_LEDGER, RX_VK_CAP_ONLY_GPU, RX_VK_UB_SNAPSHOT, RX_VK_UB_CUR_ONLY
int ubo_memo_mode() {
    static const int mode = [] {
        const char* c = getenv("HL_UBO_MEMO_CHECK");
        const char* e = getenv("HL_UBO_MEMO");
        int v = c && *c && *c != '0' ? 2 : e && *e && *e != '0' ? 1 : 0;
        if (!v) return 0;
        const char* cap = getenv("RX_VK_CAP_ONLY_GPU");
        // (RX_VK_CAP_ONLY_GPU is on by default: unset counts as on)
        if (ledger_on() || !cap || (*cap && *cap != '0') || getenv("RX_VK_UB_SNAPSHOT") || getenv("RX_VK_UB_CUR_ONLY")) {
            logf("hl ubo memo: left off (RX_VK_LEDGER, RX_VK_CAP_ONLY_GPU, RX_VK_UB_SNAPSHOT or RX_VK_UB_CUR_ONLY is set)");
            return 0;
        }
        logf("hl ubo memo: the uniform slots' answers kept while the pools, the update list and the segment stand%s (HL_UBO_MEMO=1)",
             v == 2 ? ", every hit compared with the full lookup (HL_UBO_MEMO_CHECK=1)" : "");
        return v;
    }();
    return mode;
}
using UboMemoSlots = ubomemo::Entry[kNvnStages][16];
UboMemoSlots& ubo_memo_slots() {
    thread_local UboMemoSlots slots;
    return slots;
}
void ubo_memo_compare(const VkDescriptorBufferInfo& memo, const VkDescriptorBufferInfo& full, uint64_t addr, uint64_t n, int stage,
                      uint32_t index) {
    static std::atomic<uint64_t> compared{0}, bad{0}, last_log{0};
    compared++;
    if (memo.buffer != full.buffer || memo.offset != full.offset || memo.range != full.range) {
        if (bad++ < 12)
            logf("hl ubo memo check: MISMATCH stage %d slot %u addr %#llx +%#llx: memo %p+%#llx (%#llx), full %p+%#llx (%#llx)", stage,
                 index, (unsigned long long)addr, (unsigned long long)n, (void*)memo.buffer, (unsigned long long)memo.offset,
                 (unsigned long long)memo.range, (void*)full.buffer, (unsigned long long)full.offset, (unsigned long long)full.range);
    }
    const uint64_t now = GetTickCount64();
    uint64_t prev = last_log.load();
    if (prev == 0) last_log.compare_exchange_strong(prev, now);
    else if (now - prev >= 10000 && last_log.compare_exchange_strong(prev, now))
        logf("hl ubo memo check: %llu memo hits compared, %llu mismatches", (unsigned long long)compared.load(),
             (unsigned long long)bad.load());
}

VkDescriptorBufferInfo uniform_range(const NvnState& s, const Stage& st, uint32_t index) {
    VkDescriptorBufferInfo bi{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
    const uint64_t align = std::max<uint64_t>(dev.props.limits.minUniformBufferOffsetAlignment, 16);
    uint32_t used = index < Shader::Info::MAX_CBUFS ? st.info.constant_buffer_used_sizes[index] : 0;
    // Global fallbacks read descriptor words added after the IR's constant-bank usage was collected.
    if (st.info.uses_global_memory)
        for (const auto& d : st.info.storage_buffers_descriptors)
            if (d.cbuf_index == index) used = std::max(used, d.cbuf_offset + d.count * 16u);
    if (index == 0 || index == 2) {   // the driver's banks: only what the program reads of them
        FINE_SCOPE(FT_BANK);
        FINE_N(FC_BANK_CALLS, 1);
        const uint32_t n = std::clamp<uint32_t>((used + 15) & ~15u, 16, 0xA00);
        BankCache& c = bank_cache()[index == 2][std::clamp(st.nvn_stage, 0, kNvnStages - 1)];
        if (index == 0 && st.nvn_stage == 5 && grid_patch_want) {   // (PR 252) an upload of its own, patched on the GPU
            const uint64_t at = stream_alloc(n, align);
            if (at == UINT64_MAX) return bi;
            driver_bank_fill(s, 0, 5, cur->mapped + at, n);
            grid_patch_at = at, grid_patch_buf = cur->stream;
            c.at = UINT64_MAX;   // (not shared: its first words change on the GPU)
            return {cur->stream, at, n};
        }
        const bool grid_same = st.nvn_stage != 5 || !std::memcmp(c.grid, s.grid, sizeof c.grid);
        static const bool no_bank_cache = getenv("RX_VK_NO_DESC_REUSE") != nullptr;   // (A/B: filled every time)
        // (hookshot) a graphics stage's c2 is made from its textures, samplers and images, its c0 from its storage buffers: it stands while
        // those do, whatever else was bound (a uniform buffer for each draw). RX_VK_BANK_GEN_OFF=1: while nothing at all was bound anew)
        static const bool stage_gen_off = getenv("RX_VK_BANK_GEN_OFF") != nullptr;
        const int bank_stage = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        const uint64_t gen = stage_gen_off || bank_stage > 4 ? s.bind_gen : index == 2 ? s.tex_gen[bank_stage] : s.ssbo_gen[bank_stage];
        if (!no_bank_cache && c.at != UINT64_MAX && c.bind_gen == gen && grid_same && c.size >= n) {
            FINE_N(FC_BANK_GEN, 1);
            return {c.buf, c.at, n};
        }
        static thread_local std::vector<uint8_t> tmp;
        tmp.resize(n);
        FINE_N(FC_BANK_FILLS, 1);
        FINE_N(FC_BANK_FILL_BYTES, n);
        driver_bank_fill(s, (int)index, st.nvn_stage, tmp.data(), n);
        static const bool check = getenv("RX_VK_CHECK_BANKS") != nullptr;   // (the fill against the word path)
        if (check)
            for (uint32_t off = 0; off < n; off += 4) {
                const uint32_t w = driver_bank_word(s, (int)index, st.nvn_stage, off);
                static int bad;
                if (std::memcmp(&w, tmp.data() + off, 4) && bad++ < 20)
                    logf("vk: bank c%u stage %d word %#x: fill %#x, word path %#x", index, st.nvn_stage, off,
                         *(const uint32_t*)(tmp.data() + off), w);
            }
        c.bind_gen = gen;
        std::memcpy(c.grid, s.grid, sizeof c.grid);
        if (c.at != UINT64_MAX && c.size >= n && c.bytes.size() >= n && !std::memcmp(c.bytes.data(), tmp.data(), n) &&
            c.prog != nullptr) {
            FINE_N(FC_BANK_SAME, 1);
            return {c.buf, c.at, n};
        }
        const uint64_t at = stream_alloc(n, align);
        if (at == UINT64_MAX) return bi;
        FINE_N(FC_BANK_NEW, 1);
        FINE_N(FC_BANK_NEW_BYTES, n);
        std::memcpy(cur->mapped + at, tmp.data(), n);
        c.prog = (const Program*)1, c.at = at, c.buf = cur->stream, c.size = n, c.bytes = tmp;
        return {cur->stream, at, n};
    }
    if (index == Shader::Optimization::PsychoV35::kSettingsBank) {   // the renderer's own: PsychoV35's grading, as the menu has it
        constexpr uint64_t n = (Shader::Optimization::PsychoV35::kSettingCount * sizeof(float) + 15) & ~uint64_t{15};
        const uint64_t at = stream_alloc(n, align);
        if (at == UINT64_MAX) return bi;
        float bank[n / sizeof(float)] = {};
        psychov35_bank(bank);
        std::memcpy(cur->mapped + at, bank, n);
        return {cur->stream, at, n};
    }
    if (index == 1) {
        FINE_SCOPE(FT_C1);
        FINE_N(FC_C1_CALLS, 1);
        const uintptr_t key = (uintptr_t)&st;
        auto it = cur->c1_at.find(key);
        const uint64_t n = std::max<uint64_t>((st.c1.size() + 15) & ~15ull, 16);
        if (it != cur->c1_at.end()) return {it->second.first, it->second.second, n};
        const uint64_t at = stream_alloc(n, align);
        if (at == UINT64_MAX) return bi;
        std::memset(cur->mapped + at, 0, n);
        std::memcpy(cur->mapped + at, st.c1.data(), st.c1.size());
        cur->c1_at[key] = {cur->stream, at};
        return {cur->stream, at, n};
    }
    if (index >= 3 && index < 3 + 16) {
        FINE_SCOPE(FT_UBO);
        FINE_N(FC_UBO_CALLS, 1);
        const nvn_p_bind_buf& u = s.ubo[std::clamp(st.nvn_stage, 0, kNvnStages - 1)][index - 3];
        if (!u.addr) return bi;
        const uint64_t want = used ? std::max<uint64_t>(used, 16) : std::max<uint64_t>(u.size, 16);
        const uint64_t n = std::min<uint64_t>(std::min<uint64_t>(want, u.size ? std::max<uint64_t>(u.size, 16) : want),
                                              dev.props.limits.maxUniformBufferRange);
        // (HeyListen, HL_UBO_MEMO: the answer of this slot's last build, while nothing that decided it moved: ubo_memo.h. The
        // answer is kept only when it was the pool's buffer itself, which `direct` says below)
        const int memo_stage = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        const int memo_mode = ubo_memo_mode();
        ubomemo::Entry* memo = nullptr;
        ubomemo::Key memo_key;
        VkDescriptorBufferInfo memo_hit{};
        bool memo_hit_found = false;
        if (memo_mode && u.addr < (1ull << 40) && !s.ubo_bytes[memo_stage][index - 3]) {
            memo = &ubo_memo_slots()[memo_stage][index - 3];
            uint64_t limit = 0;
            memo_key = {u.addr, n, gpu_bind_stamp(), ub_view(&limit), limit, texture_segment()};
            uint64_t a, b;
            if (memo->hit(memo_key, &a, &b)) {
                memo_hit = {(VkBuffer)(uintptr_t)a, b, n};
                if (memo_mode == 1) return memo_hit;
                memo_hit_found = true;   // (HL_UBO_MEMO_CHECK: the answer is worked out too, and compared)
            }
        }
        bool direct = false;   // (the answer below is the pool's buffer, no copy: the one kind the memo keeps)
        const auto full = [&]() -> VkDescriptorBufferInfo {
        const LedgerUse ledger_scope_(kUseUniform);   // (RX_VK_LEDGER: the block's memory, whichever way it reaches the GPU below)
        ledger_mark(u.addr, n, kUseUniform);
        // (HeyListen, HL_UBO_INLINE: a slot bound with its bytes: those, with the stream's updates over them as ub_overlay
        // does, copied into the stream; past the bytes the bind carried, zeros. No guest read, no mirror)
        if (const uint8_t* bytes = s.ubo_bytes[std::clamp(st.nvn_stage, 0, kNvnStages - 1)][index - 3]) {
            const uint64_t have = s.ubo_bytes_n[std::clamp(st.nvn_stage, 0, kNvnStages - 1)][index - 3];
            const uint64_t at = stream_alloc(n, align);
            if (at == UINT64_MAX) return bi;
            uint8_t* p = cur->mapped + at;
            std::memcpy(p, bytes, std::min(n, have));
            if (n > have) std::memset(p + have, 0, n - have);
            ub_overlay_onto(u.addr, n, p);
            FINE_N(FC_UBO_STREAM, 1);
            return {cur->stream, at, n};
        }
        // (updates the stream made to this range in this submission or ones still in flight: a copy with them in)
        // (room first: the overlay copies into the stream's mapping, which a move to a bigger stream in the middle would change)
        if (stream_reserve(n, align))
            if (const uint64_t at = ub_overlay(u.addr, n, cur->mapped, [&](uint64_t bytes) { return stream_alloc(bytes, align); });
                at != UINT64_MAX) {
                FINE_N(FC_UBO_OVERLAY, 1);
                return {cur->stream, at, n};
            }
        // (RX_VK_UB_SNAPSHOT=1: the contents as they are now, in the stream, not the pool the GPU reads later)
        // (on by default on integrated GPUs and on Intel when the pools have no VRAM mirror (vk_memory.cpp mirror_on() off): the
        // GPU then reads the game's memory itself when it runs the frame, and by then the game has written the next frame's
        // uniforms over the ones submitted (the per-object matrices are in memory it reuses frame after frame). On Intel the
        // depth prepass drew the Kokiri ground with other objects' matrices, out of sight: the depth there stayed far, and the
        // fog pass painted sky over the ground (the white forest on Intel Core Ultra iGPUs and Arc cards: Hookshot #2464,
        // Longshot #446). On a Steam Deck, bound in place, 1 of 3 starts froze in a conversation (a capture's and a readback's
        // write-back never came) where 0 of ~25 did with the copy; the copy's cost there is ~1 fps (Hookshot #1795). A mirror
        // is made at the submission and does not see the rewrite. Discrete GPUs with no mirror (GCN, RX_VK_MIRROR=0) keep the
        // default off until it is measured there. RX_VK_UB_SNAPSHOT=0: off)
        static const bool snapshot = [] {
            const char* e = getenv("RX_VK_UB_SNAPSHOT");
            const bool on = e ? !env_value_off(e)
                              : !pools_mirrored() && (dev.props.vendorID == 0x8086 ||
                                                      dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
            logf("vk: uniform buffers %s (RX_VK_UB_SNAPSHOT=%d)", on ? "copied as submitted" : "bound in place", on ? 1 : 0);
            return on;
        }();
        BufferRef b = snapshot ? BufferRef{} : gpu_buffer_bind(u.addr, n, false);
        if (b && b.offset % align == 0) return direct = true, VkDescriptorBufferInfo{b.buffer, b.offset, n};
        static const bool snap_reuse = [] {   // (unset: on; set, as env_on reads it)
            const char* e = getenv("RX_VK_UB_SNAPSHOT_REUSE");
            return !e || !env_value_off(e);
        }();
        const uint64_t snap_key = u.addr * 0x9E3779B97F4A7C15ull ^ n;
        const uint64_t view = snapshot && snap_reuse ? memory_capture_view() : 0;
        static std::atomic<int64_t> snap_hits{0};
        if (view) {
            auto it = cur->ub_snap.find(snap_key);
            if (it != cur->ub_snap.end() && it->second.addr == u.addr && it->second.n == n && it->second.view == view) {
                snap_hits.fetch_add(1, std::memory_order_relaxed);
                return {it->second.buf, it->second.at, n};
            }
        }
        const uint64_t at = stream_alloc(n, align);   // (unaligned or outside the pools: copied into the stream)
        if (at == UINT64_MAX) return bi;
        FINE_N(FC_UBO_STREAM, 1);
        // (as the submission had it, not as the game has rewritten it since: the captures' pages)
        // (a copy that could not be read is not kept for the recording's later bindings: each gets its own, as before)
        if (memory_read_as_submitted(u.addr, cur->mapped + at, n) && view) cur->ub_snap[snap_key] = {u.addr, n, view, cur->stream, at};
        static std::atomic<int64_t> copied{0};
        if (copied.fetch_add(1, std::memory_order_relaxed) % 5000 == 0)
            logf("vk: uniform buffer %#llx +%#llx copied into the stream (%s; %lld so, %lld reused copies)", (unsigned long long)u.addr,
                 (unsigned long long)n, b ? "unaligned" : "outside the pools", (long long)copied.load(std::memory_order_relaxed),
                 (long long)snap_hits.load(std::memory_order_relaxed));
        return {cur->stream, at, n};
        };
        const VkDescriptorBufferInfo r = full();
        if (memo) {
            if (memo_hit_found) ubo_memo_compare(memo_hit, r, u.addr, n, memo_stage, index);
            // (the key as read before the answer: a list that moved meanwhile, or a stamp the answer itself moved, is not kept)
            uint64_t limit = 0;
            const uint64_t epoch_after = ub_view(&limit);
            if (direct && ubomemo::storable(memo_key.epoch, epoch_after) && limit == memo_key.limit)
                memo->store({u.addr, n, gpu_bind_stamp(), memo_key.epoch, memo_key.limit, memo_key.seg}, (uint64_t)(uintptr_t)r.buffer,
                            r.offset);
            else
                memo->drop();
        }
        return r;
    }
    return bi;
}

struct DescWriter {
    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufs;
    std::vector<VkDescriptorImageInfo> imgs;
    std::vector<VkBufferView> views;
    const Program* prog = nullptr;   // (the program they were built for: a dispatch builds its own over a draw's)
    void reset() {
        writes.clear(), bufs.clear(), imgs.clear(), views.clear();
    }
};
TLS_OBJECT(dw, DescWriter)
TLS_OBJECT(draw_sampled, std::vector<const Texture*>)   // (the textures the descriptors being built sample or store)
TLS_OBJECT(gfx_sampled, std::vector<const Texture*>)    // (the last draw's: kept while its descriptors are reused)

/// Fills dw with the program's descriptors for the bound state (bindings in Eden's order). Storage the program
/// writes is noted: textures it stores into are GPU-written, buffers it stores into are stamped (vk_memory.cpp).
struct DumpWritten { Texture* tex; uint32_t level; };
TLS_OBJECT(dump_written, std::vector<DumpWritten>)
// (RX_VK_DUMP_SAMPLED=prog[,prog...]: in a dump, the textures those programs sample too, each once, 96 at most)
TLS_OBJECT(dump_sampled, std::vector<Texture*>)
bool dump_sampled_wanted(uint64_t key) {
    static const std::vector<uint64_t> want = [] {
        std::vector<uint64_t> v;
        if (const char* e = getenv("RX_VK_DUMP_SAMPLED"))
            for (const char* q = e; *q;) {
                char* end;
                const uint64_t k = strtoull(q, &end, 16);
                if (end == q) break;
                v.push_back(k);
                q = *end ? end + 1 : end;
            }
        return v;
    }();
    return std::find(want.begin(), want.end(), key) != want.end();
}   // (RX_VK_DUMP: the storage images the descriptors let the program write)
TLS_OBJECT(dump_inputs, std::string)               // (and what it samples)
int64_t st_dummy_textures;               // (textures sampled as the dummy: no image for the handle)

void dump_input(char kind, uint32_t index, uint32_t handle, const Texture* tex) {
    char b[160];
    if (!tex)
        snprintf(b, sizeof b, " %c%u:none(%#x)", kind, index, handle);
    else
    {
        nvn_tex t;
        nvn_view v;
        bool has_view = false;
        const int nf = texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view)
                           ? (has_view && v.set_format ? (int)v.format : (int)t.d.format) : -1;
        snprintf(b, sizeof b, " %c%u:#%u:%llx/f%d/n%#x/%ux%ux%u%s%s", kind, index, tex->id, (unsigned long long)tex->gpu,
                 (int)tex->format, nf, tex->width, tex->height, tex->type == VK_IMAGE_TYPE_3D ? tex->depth : tex->layers,
                 tex->gpu_written ? "/G" : "", tex->uploaded ? "/U" : "");
    }
    dump_inputs() += b;
}

/// A descriptor over 2 ms is logged, 40 times, and only while RX_VK_SLOW_DESC is set. Off, the clock is not read.
bool slow_desc_timing() {
    static const int on = getenv("RX_VK_SLOW_DESC") != nullptr;
    return on;
}

// (hookshot) one switch read two ways, like the Robust buffers row of vk_main.cpp: the environment variable says it for test
// runs (any value, 0 off); else the row's line in hookshot_options.ini beside the game; else off.
bool menu_env_on(const char* env_name, const char* ini_key_name) {
    if (const char* e = getenv(env_name)) return atoi(e) != 0;
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);   // (the game folder, not bin beside the launcher: vk_paths.h)
    std::string ini(exe, n);
    ini = ini.substr(0, ini.find_last_of("\\/") + 1) + "hookshot_options.ini";
    const size_t klen = strlen(ini_key_name);
    int v = 0;
    if (FILE* f = fopen(ini.c_str(), "r")) {
        char line[80];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, ini_key_name, klen) && line[klen] == '=') {
                v = atoi(line + klen + 1);
                break;
            }
        fclose(f);
    }
    return v != 0;
}
/// (hookshot) the Descriptor reuse row / RX_VK_DESC_SLOTGEN=1: a draw keeps its descriptors through registrations of the
/// texture slots it does not sample. What it does sample is checked by slot version, and the samplers by their
/// registrations' generation, instead of the global one every registration moves. Its ready line is like the frame interpolation's.
bool desc_slotgen_on() {
    static int on = -1;
    if (on < 0) {
        on = menu_env_on("RX_VK_DESC_SLOTGEN", "descriptor_reuse");
        if (on)
            logf("vk: descriptor slotgen ready: draws keep their descriptors through the registrations of slots they do not "
                 "sample (menu: Descriptor reuse)");
    }
    return on;
}
/// (hookshot) the Sampler cache row / RX_VK_SAMPLER_CACHE=1: what a sampler id holds is kept next to its version on the replay
/// thread, so resolving an id that was not registered anew takes no lock (the registrations' mutex contends with the texture streaming)
bool sampler_cache_on() {
    static int on = -1;
    if (on < 0) {
        on = menu_env_on("RX_VK_SAMPLER_CACHE", "sampler_cache");
        if (on) logf("vk: sampler cache ready: sampler ids answer their parameters without the registrations' lock (menu: Sampler cache)");
    }
    return on;
}
// (RX_VK_DESC_SLOTGEN) What the descriptors being built read of the registrations: the version of every slot whose handle they
// resolved. The next draw of the same bindings keeps them while none of those versions moved. A handle read as 0 is noted: of a
// slot that is not registered, only a registration (the global generation) can tell us that it is one now.
TLS_OBJECT(desc_fp, std::vector<uint64_t>)   // ((version << 20 | slot), a handle read)
thread_local bool desc_fp_dummy = false;      // (some handle read 0)
thread_local uint64_t desc_samp_seen = 0;     // (the samplers' generation when the build began)
thread_local uint64_t desc_destroy_seen = 0;  // (the eviction generation: destroy_later retires a view without touching any slot or sampler generation)
void desc_fp_note(uint32_t handle) {
    if (!desc_slotgen_on()) return;
    const uint32_t slot = handle & 0xFFFFF;
    desc_fp().push_back((uint64_t)texture_slot_version((int)slot) << 20 | slot);
    desc_fp_dummy |= handle == 0;
}

/// (a descriptor that took over 2 ms to resolve: logged, 40 of them)
void slow_descriptor(std::chrono::steady_clock::time_point t0, const char* what, uint64_t addr, uint64_t size) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    static int logged;
    if (ms >= 2.0 && logged++ < 40)
        logf("vk: slow descriptor %.1f ms: %s %#llx size %#llx", ms, what, (unsigned long long)addr, (unsigned long long)size);
}

// ---- RX_VK_WATCH_PROG: a program's bindings, the last 40 kept and logged when the device is lost ----
bool watched(uint64_t key) {   // (RX_VK_WATCH_PROG=<key>[,<key>...])
    static const std::vector<uint64_t> keys = [] {
        std::vector<uint64_t> v;
        for (const char* q = getenv("RX_VK_WATCH_PROG"); q && *q;) {
            v.push_back(strtoull(q, nullptr, 16));
            while (*q && *q != ',') q++;
            if (*q) q++;
        }
        return v;
    }();
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}
uint64_t watch_prog() {
    static const uint64_t w = getenv("RX_VK_WATCH_PROG") ? strtoull(getenv("RX_VK_WATCH_PROG"), nullptr, 16) : 0;
    return w;
}
std::mutex watch_mu;
std::string watch_ring[40];
int watch_at = 0;
void watch_note(const char* fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    std::lock_guard lk(watch_mu);
    watch_ring[watch_at++ % 40] = line;
}

/// (RX_VK_DUMP_SSBO=<prog>[,...]: in a dump, those programs' storage buffers, in guest memory and as the GPU has them)
bool dump_ssbo_wanted(uint64_t key) {
    static const std::vector<uint64_t> want = [] {
        std::vector<uint64_t> v;
        for (const char* q = getenv("RX_VK_DUMP_SSBO"); q && *q;) {
            char* end;
            const uint64_t k = strtoull(q, &end, 16);
            if (end == q) break;
            v.push_back(k);
            q = *end ? end + 1 : end;
        }
        return v;
    }();
    return std::find(want.begin(), want.end(), key) != want.end();
}
/// (and the GPU's copies of them, before or after the dispatch)
void dump_ssbos(VkCommandBuffer cmd, const Program& p, const char* when) {
    if (!dump_on() || !dump_ssbo_wanted(p.key)) return;
    uint32_t sb = 0;
    for (const VkWriteDescriptorSet& w : dw().writes) {
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || !w.pBufferInfo) continue;
        for (uint32_t i = 0; i < w.descriptorCount; i++, sb++) {
            const VkDescriptorBufferInfo& b = w.pBufferInfo[i];
            if (!b.buffer || b.range == VK_WHOLE_SIZE) continue;
            char what[128];
            snprintf(what, sizeof what, "%03d prog %016llx sb%u %s", dump_seq(), (unsigned long long)p.key, sb, when);
            dump_buffer(cmd, b.buffer, b.offset, b.range, what);
        }
    }
}

// The table-entry flip log is diagnostic. It stops after 60 lines, and after this many looks: a game that
// never flips was taking a mutex and updating a map for every storage-buffer table entry of every draw.
// unlimited (RX_VK_LOG_TABLE=1) keeps looking until the 60 lines.
bool table_track_open(int logged, int seen, int unlimited) {
    constexpr int kTableTrackLooks = 65536;
    return logged < 60 && (unlimited || seen < kTableTrackLooks);
}

// (TICK-007) A program's texture descriptors as this thread last resolved them: a draw whose handle for a slot is the one before, with
// the texture slot, the sampler slot and the texture set unchanged since (their versions and the destroy generation), takes the view,
// the texture and the sampler from here instead of handle_hits' search and sampler_for's (which takes reg_mu, shared by all the
// recording threads, and hashes the sampler's parameters). The handle itself is read again every draw (from the driver bank, a
// uniform buffer or a handle program): only its resolution to Vulkan objects is kept, for 4 handles a slot. Per thread (parallel recording), no lock.
// RX_VK_NO_SLOT_CACHE=1: every slot resolved as before
struct TexSlot {
    uint32_t handle = 0, version = 0, samp_version = 0, tex_epoch = 0;
    uint64_t destroy_gen = ~0ull;
    uint64_t samp_retired = 0, samp_frame = 0;   // (longshot #442: sampler_book.retired_gen, and the present near the sampler limit: see sampler_stamp)
    VkImageView view = VK_NULL_HANDLE;
    Texture* tex = nullptr;
    VkSampler sampler = VK_NULL_HANDLE;
    bool valid = false;
};
constexpr size_t kTexWays = 4;   // (the handles kept a slot)
struct ProgSlots {
    uint64_t key = 0;
    std::vector<TexSlot> tex;
};
TLS_OBJECT(prog_slots, std::unordered_map<const Program*, ProgSlots>)

// (hookshot, issue 14, RX_VK_NULL_WRITE) A storage buffer a program writes through whose address resolved to nothing: the descriptor was
// a null one (Vulkan drops its writes). RADV hangs the graphics ring on a submission that writes at a tiny GPU address (0 or 0x5000:
// "GPUVM fault", a texture-unit write, Steam Deck and RX 9070 XT), where NVIDIA's driver drops such a write silently. Counted by why
// it resolved to nothing, one log line for each program and descriptor. With the switch on, a buffer of our own is bound there
// instead of the null descriptor, so every write of a draw or dispatch lands in memory (or past its 64 bytes, where the robust access
// drops it) and no descriptor has base address 0. 64 bytes only: a program's search of its bindings takes the binding's length as the
// end of the range it serves, and a longer one would take over reads the page table serves better.
// RX_VK_NULL_WRITE=1: the scratch buffer; unset or 0: the null descriptor as before (the cases still counted and logged)
enum NullWriteWhy { kNwAddressZero, kNwNotBuffer, kNwNotBound, kNwWhys };
std::atomic<int64_t> null_writes[kNwWhys];
constexpr uint64_t kNullScratchBytes = 64;
VkBuffer null_scratch = VK_NULL_HANDLE;
bool null_write_on() {
    // (off by default for now: a slot a program also reads would read the scratch's last writes instead of the null
    // descriptor's zeros, and it is not shown yet to cure the RADV hang; RX_VK_NULL_WRITE=1 for the Deck's test)
    static const bool on = getenv("RX_VK_NULL_WRITE") && atoi(getenv("RX_VK_NULL_WRITE")) != 0;
    return on;
}
/// The scratch buffer (made on first use, host-visible and zeroed, never read back), or no buffer if it could not be made
VkDescriptorBufferInfo null_write_binding() {
    static std::once_flag once;
    static VmaAllocation mem = nullptr;
    std::call_once(once, [] {
        void* mapped = nullptr;
        if (!create_buffer(kNullScratchBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, null_scratch, mem, &mapped) || !mapped)
            null_scratch = VK_NULL_HANDLE;
        else std::memset(mapped, 0, kNullScratchBytes);
    });
    return {null_scratch, 0, kNullScratchBytes};
}
void null_write_note(const Program& p, int stage, uint32_t cbuf, uint32_t off, uint32_t k, uint32_t count, uint64_t addr, uint64_t size,
                     NullWriteWhy why) {
    null_writes[why]++;
    static std::mutex mu;
    static std::unordered_set<uint64_t> told;
    std::lock_guard lk(mu);
    if (!told.insert(p.key ^ ((uint64_t)off << 40) ^ ((uint64_t)cbuf << 32) ^ ((uint64_t)k << 24) ^ ((uint64_t)stage << 56)).second ||
        told.size() > 200)
        return;
    static const char* const names[kNwWhys] = {"its address is 0 (the slot was never set)",
                                               "its address and size do not name a buffer (not guest memory, or a table entry past its table)",
                                               "its pool has no buffer for it yet (not imported, or the range does not fit)"};
    logf("vk: written storage buffer resolves to nothing: program %016llx stage %d ssbo %u of %u (c%u+%#x) addr %#llx size %#llx: %s; %s",
         (unsigned long long)p.key, stage, k, count, cbuf, off, (unsigned long long)addr, (unsigned long long)size, names[why],
         null_write_on() ? "a scratch buffer is bound" : "left null");
}

/// (hookshot) A program's descriptor counts in one word (Program::desc_counts): buffers, images, texel views and sampled
/// textures, 16 bits each. ~0: not counted, or a count that does not fit (then they are counted at every build, as before)
uint64_t desc_counts_pack(size_t nbuf, size_t nimg, size_t nview, size_t ntex) {
    constexpr size_t kMost = 0xfffe;   // (so that no packing is ~0)
    if (nbuf > kMost || nimg > kMost || nview > kMost || ntex > kMost) return ~0ull;
    return (uint64_t)nbuf | (uint64_t)nimg << 16 | (uint64_t)nview << 32 | (uint64_t)ntex << 48;
}
void desc_counts_unpack(uint64_t packed, size_t* nbuf, size_t* nimg, size_t* nview, size_t* ntex) {
    *nbuf = (size_t)(packed & 0xffff), *nimg = (size_t)(packed >> 16 & 0xffff), *nview = (size_t)(packed >> 32 & 0xffff),
    *ntex = (size_t)(packed >> 48 & 0xffff);
}

// (ported from hookshot 53eb0441, pixl27, RX_VK_METER_ZERO) where an exposure program's sums were bound, for its dispatch's reset
struct MeterOut { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, size = 0; };
thread_local MeterOut meter_out;
void build_descriptors(const NvnState& s, const Program& p) {
    if (p.key == 0x435a0138b9013040ull || p.key == 0x592079442d7129daull) meter_out = {};
    // (hookshot) the thread's writer asked for once: each out is a call and a thread-local read (__tls_get_addr in the shared
    // library), and a build made dozens of them. The same object for the thread's life, reset in place here
    DescWriter& out = dw();
    out.reset();
    out.prog = &p;
    desc_fp().clear(), desc_fp_dummy = false;   // (RX_VK_DESC_SLOTGEN: the samplers' generation is read before the first handle,
    desc_samp_seen = sampler_registrations_generation();   // (so one registered during the build still forces the next rebuild)
    desc_destroy_seen = texture_destroy_generation();      // (an eviction retires views behind the slots' backs: it bumps only this)
    draw_sampled().clear();
    dump_written().clear();
    dump_inputs().clear();
    // (hookshot) how many of each the program has: its own, the same at every build. Counted once and kept with the program
    // (desc_counts): every build walked six lists a stage only for these four numbers. RX_VK_DESC_COUNTS=0: counted each time
    size_t nbuf = 0, nimg = 0, nview = 0, ntex = 0;
    static const bool keep_counts = !getenv("RX_VK_DESC_COUNTS") || strcmp(getenv("RX_VK_DESC_COUNTS"), "0") != 0;
    const uint64_t kept = keep_counts ? p.desc_counts.load(std::memory_order_relaxed) : ~0ull;
    if (kept != ~0ull) {
        desc_counts_unpack(kept, &nbuf, &nimg, &nview, &ntex);
    } else {
        for (const Stage& st : p.stages) {
            for (const auto& d : st.info.constant_buffer_descriptors) nbuf += d.count;
            for (const auto& d : st.info.storage_buffers_descriptors) nbuf += d.count;
            for (const auto& d : st.info.texture_buffer_descriptors) nview += d.count;
            for (const auto& d : st.info.image_buffer_descriptors) nview += d.count;
            for (const auto& d : st.info.texture_descriptors) nimg += d.count, ntex += d.count;
            for (const auto& d : st.info.image_descriptors) nimg += d.count;
        }
        // (kept only once the program is whole: its stages are filled in by a worker before `ready`)
        if (keep_counts && p.ready.load(std::memory_order_acquire)) {
            const uint64_t packed = desc_counts_pack(nbuf, nimg, nview, ntex);
            if (packed != ~0ull) p.desc_counts.store(packed, std::memory_order_relaxed);
        }
    }
    out.bufs.reserve(nbuf), out.imgs.reserve(nimg), out.views.reserve(nview);   // (stable pointers below)
    static const bool no_slot_cache = getenv("RX_VK_NO_SLOT_CACHE") != nullptr;
    TexSlot* slots = nullptr;
    if (!no_slot_cache && ntex) {
        auto& table = prog_slots();
        ProgSlots& ps = table[&p];
        if (ps.key != p.key || ps.tex.size() != ntex * kTexWays) ps.key = p.key, ps.tex.assign(ntex * kTexWays, TexSlot{});
        slots = ps.tex.data();
    }
    size_t tslot = 0;
    const uint64_t dgen_now = slots ? texture_destroy_generation() : 0;
    FINE_N(FC_DESCS, (int64_t)(nbuf + nimg + nview));
    uint32_t binding = 0;
    auto write = [&](VkDescriptorType type, uint32_t count) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = binding++;
        w.descriptorCount = count;
        w.descriptorType = type;
        return w;
    };
    const uint64_t salign = std::max<uint64_t>(storage_binding_align(), 4);   // (the translator's: vk_internal.h)
    for (const Stage& st : p.stages) {
        const Shader::Info& info = st.info;
        const int ns = st.nvn_stage;
        for (const auto& d : info.constant_buffer_descriptors) {
            VkWriteDescriptorSet w = write(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, d.count);
            const size_t first = out.bufs.size();
            for (uint32_t k = 0; k < d.count; k++) {
                const auto t0 = slow_desc_timing() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                out.bufs.push_back(uniform_range(s, st, d.index + k));
                if (slow_desc_timing()) slow_descriptor(t0, "uniform buffer", d.index + k, out.bufs.back().range);
            }
            w.pBufferInfo = &out.bufs[first];
            out.writes.push_back(w);
        }
        for (const auto& d : info.storage_buffers_descriptors) {
            FINE_SCOPE(FT_STORAGE);
            FINE_N(FC_SSBO_DESCS, d.count);
            VkWriteDescriptorSet w = write(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, d.count);
            const size_t first = out.bufs.size();
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t off = d.cbuf_offset + k * 16;
                if (d.count == 1 && d.cbuf_index == 0 && d.cbuf_offset == 0xff0) {   // (EvilOOT, RX_VK_MISS_LOG)
                    out.bufs.push_back(miss_binding(p.key));
                    continue;
                }
                // (AyyBeeDee20738, PR 138; EvilOOT the same before it, for OoT's culling tables in c4) through the stage's banks as
                // its program reads them (c1 from the program, c3+ from the bound uniform buffer with the stream's inline updates):
                // a storage buffer named there read 0 from the driver banks and was bound to nothing
                const uint64_t addr = stage_cbuf_word(s, st, d.cbuf_index, off) |
                                      (uint64_t)stage_cbuf_word(s, st, d.cbuf_index, off + 4) << 32;
                uint64_t size = stage_cbuf_word(s, st, d.cbuf_index, off + 8);
                // (EvilOOT) a table entry that is a bare pointer (its size word 0, as OoT's culling setup keeps them): bound up to
                // 1 MB of its pool, as the mesh pointer table's regions are; the shader takes the binding's length as its end.
                // Not marked written: the 1 MB is a guess past the real buffer, and its copy back to guest memory overwrote
                // the CPU's own data beside it (a pool header's pointer, the game crashed). What the GPU writes there (the
                // culling passes' group counts) is read by later GPU work, which binds the same mirror.
                bool bare = false;
                // (Fritchii 8193f926f, jpkto, Lichii #351) A read-only one is the base of an object pool the shader reads
                // anywhere in: bound to the pool's end, as far as a storage buffer reaches (at most 1 GB). OoT's froxel fog
                // finds its light probes this way, up to ~130 MB past the pointer: the probes past the binding read 0 and the
                // fog took an overbright constant from them (the white haze, zones overexposed or too dark). Written ones keep
                // 1 MB. RX_VK_BARE_READ_MB=<MB> caps it (1: as before)
                static const uint64_t bare_read = [] {
                    const uint64_t reach = std::min<uint64_t>(dev.props.limits.maxStorageBufferRange, 1ull << 30);
                    const uint64_t cap = getenv("RX_VK_BARE_READ_MB") ? (uint64_t)atoll(getenv("RX_VK_BARE_READ_MB")) << 20 : reach;
                    return std::max<uint64_t>(1ull << 20, std::min(cap, reach - std::min<uint64_t>(reach, 64ull << 10)));
                }();
                if (d.count > 1 && !size && addr && !(addr & 3) && host->gpu_guest(addr))
                    if (const uint64_t end = gpu_pool_end(addr); end > addr)
                        size = std::min<uint64_t>(end - addr, d.is_written ? (1ull << 20) : bare_read) & ~3ull, bare = true;
                // (EvilOOT) the pointer table's regions (c0+0x880, vk_bank.cpp pointer_table) are guesses too: GPU-only, as above
                if (d.cbuf_index == 0 && d.cbuf_offset == 0x880 && d.count == 3) bare = true;
                // (Fritchii's storage buffer pass, Info::native_global_bounds) a program whose accesses pick among candidate
                // pointers at run time: those are bare 8-byte arguments, the word after one another argument (a stride, a
                // handle), not an NVN size. Unless that word looks like this pointer's size (its high half 0, inside the
                // pool), bound to the pool's end, 8 MB at most (Fritchii's limit), 1 MB and GPU-only if written, as the
                // bare table entries above; the shaders take the binding's length as the end
                if (info.native_global_bounds && d.count == 1 && d.cbuf_index != 0 && addr && !(addr & 3) &&
                    host->gpu_guest(addr)) {
                    const uint64_t end = gpu_pool_end(addr);
                    const bool sized = size && !stage_cbuf_word(s, st, d.cbuf_index, off + 12) && end > addr && size <= end - addr;
                    if (!sized && end > addr)
                        size = std::min<uint64_t>(end - addr, d.is_written ? (1ull << 20) : (8ull << 20)) & ~3ull, bare = true;
                }
                const uint64_t base = addr & ~(salign - 1);
                uint64_t n = std::max<uint64_t>(((size + (addr - base)) + 3) & ~3ull, 16);
                {   // (hookshot, RX_VK_SSBO_STRETCH=1: a read-only buffer bound to its pool's end, up to 64 MB. Lichii bf3f395c9: the grass's
                    // task and mesh programs read records 12-18 MB past a base whose size word said less, read 0 and culled every blade)
                    static const bool stretch = getenv("RX_VK_SSBO_STRETCH") != nullptr;
                    if (stretch && !d.is_written && addr && d.count == 1)
                        if (const uint64_t end = gpu_pool_end(base); end > base + n)
                            n = std::max<uint64_t>(n, std::min<uint64_t>(end - base, 64ull << 20) & ~uint64_t{3});
                }
                // (EvilOOT) RX_VK_SSBO_WIDE=<MB>, experiment: single storage buffers bound from their base to the end of their
                // pool (that many MB at most), so that a pointer loaded through the buffer and used with the buffer's binding
                // (the pointer pass's split accesses) reads its target instead of 0 past the descriptor's size. Only the
                // descriptor's own range is marked written (copied back to guest memory): the rest stays the GPU's.
                uint64_t wide_n = 0;
                {
                    static const uint64_t wide = getenv("RX_VK_SSBO_WIDE") ? (uint64_t)atoll(getenv("RX_VK_SSBO_WIDE")) << 20 : 0;
                    if (wide && d.count == 1 && addr)
                        if (const uint64_t end = gpu_pool_end(base); end > base + n) wide_n = std::min(end - base, wide) & ~3ull;
                }
                const auto t0 = slow_desc_timing() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                const LedgerUse ledger_scope_(kUseStorage);   // (RX_VK_LEDGER)
                // (hookshot) a storage buffer table's entries (count > 1, Info::global_tables): the ones past the table's
                // end are whatever the constant buffer holds there, bound only when they look like a buffer
                const bool plausible = d.count == 1 || (size && size <= (256ull << 20) && host->gpu_guest(base));
                BufferRef b = addr && plausible ? gpu_buffer_bind(base, n, d.is_written && !bare) : BufferRef{};
                // (AyyBeeDee20738, PR 138) a table entry's region that does not fit its pool this moment bound nothing, and the next
                // frame bound it again: its reads flipped between the data and 0, the picture between bright and dark. Smaller,
                // down to 1 MB, rather than nothing (and past what is bound the shaders' search now goes on to the page table)
                static const bool no_shrink = getenv("RX_VK_NO_TABLE_SHRINK") != nullptr;   // (A/B: unbound as before)
                if (!b.buffer && d.count > 1 && addr && plausible && !no_shrink) {
                    uint64_t m = n;
                    if (const uint64_t end = gpu_pool_end(base); end > base) m = std::min<uint64_t>(m, end - base);
                    for (; !b.buffer && m >= (1ull << 20); m = (m / 2) & ~3ull) {
                        b = gpu_buffer_bind(base, m, d.is_written && !bare);
                        if (b.buffer) {   // (EvilOOT: the descriptor's range what was bound, so the search ends there)
                            n = m;
                            break;
                        }
                    }
                }
                // (EvilOOT) a bare entry the program writes through: its first 64 bytes marked written (the same mirror), so
                // the counts and records the culling setup writes there survive the mirror's syncs from guest memory and go
                // back to it, as on the console; the rest of the guessed 1 MB stays the GPU's (copied back whole, it
                // overwrote the CPU's own data beside it)
                if (b && bare && d.is_written && d.cbuf_offset != 0x880) {
                    const uint64_t head = std::min<uint64_t>(n, (addr - base) + 0x40);
                    (void)gpu_buffer_bind(base, (head + 3) & ~3ull, true);
                }
                // (RX_VK_PROTECT_BARE=1: the whole range kept for the GPU against the CPU's page syncs, vk_memory.cpp prot_pages)
                if (b && bare && d.is_written)
                    if (const uint64_t guest = host->gpu_guest(base)) memory_protect_gpu(guest, n);
                if (b && wide_n) {   // (the same buffer, its binding longer, nothing more marked written)
                    const BufferRef w = gpu_buffer_bind(base, wide_n, false);
                    if (w && w.buffer == b.buffer && w.offset == b.offset) b = w, n = wide_n;
                }
                // (hookshot) all a program may write through it, past what is marked written (a bare entry's guessed range, the
                // pointer table's, a wide binding's): not copied back, but a GPU write hazard for the readback refresh of a later
                // recording in the submission (vk_memory.cpp memory_rb_refresh)
                if (b && d.is_written && (bare || wide_n)) memory_note_gpu_write(base, n);
                {   // (EvilOOT) the wanted programs' storage descriptors in order, once: descriptor n = the IR's "#n"
                    static std::mutex lm;
                    static std::unordered_set<uint64_t> told;
                    static const bool stats_env = getenv("RX_VK_DRAW_STATS") != nullptr;
                    if (stats_env && stats_prog_wanted(p.key)) {
                        const size_t n_desc = (size_t)(&d - info.storage_buffers_descriptors.data());
                        std::lock_guard lk(lm);
                        const uint64_t tk = p.key ^ ((uint64_t)ns << 56) ^ ((uint64_t)n_desc << 48) ^ ((uint64_t)k << 40);
                        if (told.insert(tk).second)
                            logf("vk: descriptors of %016llx stage %d: #%zu[%u] c%u+%#x%s -> %#llx size %#llx (bound %s)", (unsigned long long)p.key,
                                 ns, n_desc, k, (unsigned)d.cbuf_index, off, d.is_written ? " written" : "", (unsigned long long)addr,
                                 (unsigned long long)size, b ? "yes" : "NO");
                    }
                }
                // (ported from hookshot 53eb0441, pixl27) OoT's exposure meter: where its two programs bind their block
                if ((p.key == 0x435a0138b9013040ull || p.key == 0x592079442d7129daull) && d.is_written && b && n == 0x1040) {
                    memory_meter_range(host->gpu_guest(base), n);
                    meter_out = {b.buffer, b.offset + (addr - base), n - (addr - base)};
                }
                // (census: every single descriptor, and table entries that hold something, with where they came from)
                if (d.count == 1 || addr) {
                    // (unresolved: the uniform buffer the word was read from, its address and size, in the vk fields)
                    const nvn_p_bind_buf* ub = d.cbuf_index >= 3 && d.cbuf_index < 3 + 16
                                                   ? &s.ubo[std::clamp(ns, 0, kNvnStages - 1)][d.cbuf_index - 3] : nullptr;
                    const bool none = !(addr && plausible);
                    stats_note_bind(p.key, ns, none ? 0 : base, none ? size : n, d.is_written, d.count > 1, d.cbuf_index, off, addr,
                                    none && ub ? ub->addr : (uint64_t)(uintptr_t)b.buffer, none && ub ? ub->size : b.offset);
                }
                static std::atomic<int> table_told{0}, table_seen{0};
                static const int table_unlimited = getenv("RX_VK_LOG_TABLE") != nullptr;
                if (d.count > 1 && table_track_open(table_told.load(std::memory_order_relaxed),
                                                    table_seen.load(std::memory_order_relaxed), table_unlimited)) {
                    table_seen.fetch_add(1, std::memory_order_relaxed);   // (hookshot) a table entry bound one frame and not the next, 60 lines, then it stops looking
                    static std::mutex tm;
                    static std::unordered_map<uint64_t, std::pair<bool, uint64_t>> was;   // -> bound, its address
                    static int told;
                    const uint64_t key = p.key ^ ((uint64_t)off << 48) ^ ((uint64_t)ns << 60);
                    const bool bound = b.buffer != VK_NULL_HANDLE;
                    std::lock_guard lk(tm);
                    auto it = was.find(key);
                    if (it != was.end() && it->second.first != bound && told < 60) {
                        told++, table_told++;
                        logf("vk: table entry %s at frame %llu: prog %016llx ssbo %u of %u (c%u+%#x) addr %#llx size %#llx "
                             "(before %#llx)", bound ? "bound again" : "UNBOUND", (unsigned long long)texture_frame_no(),
                             (unsigned long long)p.key, k, d.count, (unsigned)d.cbuf_index, off, (unsigned long long)addr,
                             (unsigned long long)size, (unsigned long long)it->second.second);
                    }
                    was[key] = {bound, addr};
                }
                {   // (hookshot) RX_VK_LOG_WRITEBACK=1: the programs that bind a big written storage buffer (each written
                    // range is copied back to guest memory at the submission's end), 40 of them
                    static const bool log_wb = getenv("RX_VK_LOG_WRITEBACK") != nullptr;
                    static int logged;
                    if (log_wb && d.is_written && n >= (8ull << 20) && logged < 40) {
                        static std::vector<std::pair<uint64_t, uint32_t>> seen;
                        const std::pair<uint64_t, uint32_t> key{p.key, d.cbuf_offset + k * 16};
                        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
                            seen.push_back(key);
                            logged++;
                            logf("vk: written storage buffer of %.1f MB: prog %016llx ssbo %u of %u (c%u+%#x) at %#llx, stage %d",
                                 n / 1048576.0, (unsigned long long)p.key, k, d.count, (unsigned)d.cbuf_index, off,
                                 (unsigned long long)addr, ns);
                        }
                    }
                }
                if (watch_prog() && p.key == watch_prog()) {   // (RX_VK_WATCH_PROG=<key>: that program's bindings)
                        watch_note("vk: watch %016llx: ssbo %u (c%u+%#x%s) addr %#llx size %#llx -> buffer %p offset %#llx range %#llx; grid %u %u %u",
                             (unsigned long long)p.key, k, (unsigned)d.cbuf_index, off, d.is_written ? ", written" : "",
                             (unsigned long long)addr, (unsigned long long)size, (void*)b.buffer,
                             (unsigned long long)b.offset, (unsigned long long)n, s.grid[0], s.grid[1], s.grid[2]);
                }
                if (slow_desc_timing()) slow_descriptor(t0, d.is_written ? "storage buffer (written)" : "storage buffer", base, n);
                if (dump_on() && dump_ssbo_wanted(p.key)) {   // (RX_VK_DUMP_SSBO: where it is, and what the CPU's memory holds)
                    std::vector<uint32_t> words((size_t)std::min<uint64_t>(n, 65536) / 4);
                    size_t nz = 0;
                    const bool read = b && addr && !words.empty() && host->gpu_read(base, words.data(), words.size() * 4);
                    if (read)
                        for (uint32_t w : words) nz += w != 0;
                    logf("vk: dump ssbo %03d prog %016llx stage %d sb%u (c%u+%#x%s): addr %#llx size %#llx bound %s; guest "
                         "memory: %s%zu of %zu words not zero", dump_seq(), (unsigned long long)p.key, ns, k,
                         (unsigned)d.cbuf_index, off, d.is_written ? ", written" : "", (unsigned long long)addr,
                         (unsigned long long)size, b ? "yes" : "NO", read ? "" : "(unreadable) ", nz, words.size());
                }
                {   // (RX_VK_PEEK_PROG=<key prefix>: that program's storage buffers' first words in guest memory, and what the
                    // first 64-bit word points at: the foliage culling's inputs)
                    static const char* peek = getenv("RX_VK_PEEK_PROG");
                    static std::atomic<int> peeked{0};
                    char hex[24];
                    if (peek) snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);   // (only with the switch: once per buffer per draw)
                    if (peek && addr && !strncmp(hex, peek, strlen(peek)) && peeked++ < 60) {
                        uint32_t w[8]{}, q[8]{};
                        const bool ok = host->gpu_read(addr, w, sizeof w);
                        const uint64_t ptr = (uint64_t)w[1] << 32 | w[0];
                        const bool ok2 = ptr && host->gpu_read(ptr, q, sizeof q);
                        logf("vk: peek %s sb%u (c%u+%#x%s) addr %#llx size %#llx: %s%08x %08x %08x %08x %08x %08x %08x %08x; at %#llx: %s%08x %08x %08x %08x %08x %08x",
                             hex, k, (unsigned)d.cbuf_index, off, d.is_written ? ", written" : "", (unsigned long long)addr,
                             (unsigned long long)size, ok ? "" : "(unreadable) ", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7],
                             (unsigned long long)ptr, ok2 ? "" : "(unreadable) ", q[0], q[1], q[2], q[3], q[4], q[5]);
                    }
                }
                if (addr && plausible && !b) {   // (a storage buffer that could not be bound: the program reads zeros from it)
                    static std::atomic<int> unbound{0};
                    if (unbound++ < 40)
                        logf("vk: storage buffer NOT bound (reads zeros): program %016llx stage %d c%u+%#x addr %#llx size "
                             "%#llx%s", (unsigned long long)p.key, ns, (unsigned)d.cbuf_index, off,
                             (unsigned long long)addr, (unsigned long long)size, d.is_written ? " (written)" : "");
                }
                if (!b && d.is_written && !(d.count == 1 && d.cbuf_index == 0 && d.cbuf_offset == 0xff0))
                    null_write_note(p, ns, d.cbuf_index, off, k, d.count, addr, size,
                                    !addr ? kNwAddressZero : plausible ? kNwNotBound : kNwNotBuffer);
                VkDescriptorBufferInfo scratch{};
                if (!b && d.is_written && null_write_on()) scratch = null_write_binding();
                out.bufs.push_back(b ? VkDescriptorBufferInfo{b.buffer, b.offset, n}
                                    : scratch.buffer ? scratch
                                    : VkDescriptorBufferInfo{dev.null_descriptor ? VK_NULL_HANDLE : zero_buffer, 0,
                                                             dev.null_descriptor ? VK_WHOLE_SIZE : 16});
                {   // (RX_VK_LOG_WRITTEN=1: every storage buffer range a program writes, once, 300 of them)
                    static const bool on = getenv("RX_VK_LOG_WRITTEN") != nullptr;
                    if (on && d.is_written) {
                        static std::mutex wm;
                        static std::unordered_set<uint64_t> seen;
                        std::lock_guard lk(wm);
                        if (seen.size() < 300 && seen.insert(p.key ^ (addr >> 12) * 0x9E3779B97F4A7C15ull).second)
                            logf("vk: writes storage: prog %016llx c%u+%#x addr %#llx size %#llx frame %llu", (unsigned long long)p.key,
                                 (unsigned)d.cbuf_index, off, (unsigned long long)addr, (unsigned long long)size, (unsigned long long)texture_frame_no());
                    }
                }
                if (b && d.is_written) {
                    const auto t1 = slow_desc_timing() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    const uint64_t guest = host->gpu_guest(base);
                    if (guest) { ParLock lk; memory_stamp_gpu(guest, n); }
                    if (slow_desc_timing()) slow_descriptor(t1, "stamp", base, n);
                }
            }
            w.pBufferInfo = &out.bufs[first];
            out.writes.push_back(w);
        }
        auto texel_views = [&](const auto& list, VkDescriptorType type, bool storage) {
            for (const auto& d : list) {
                VkWriteDescriptorSet w = write(type, d.count);
                const size_t first = out.views.size();
                for (uint32_t k = 0; k < d.count; k++) {
                    const uint32_t handle = handle_of(s, st, d, k);
                    desc_fp_note(handle);   // (RX_VK_DESC_SLOTGEN)
                    nvn_tex t;
                    nvn_view v;
                    bool has_view = false;
                    VkBufferView bv = VK_NULL_HANDLE;
                    if (handle && texture_of((int)(handle & 0xFFFFF), &t, &v, &has_view)) {
                        ParLock lk;
                        bv = texel_buffer_view(&t, has_view ? &v : nullptr, storage);
                    }
                    out.views.push_back(bv);
                }
                w.pTexelBufferView = &out.views[first];
                out.writes.push_back(w);
            }
        };
        texel_views(info.texture_buffer_descriptors, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, false);
        texel_views(info.image_buffer_descriptors, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, true);
        for (const auto& d : info.texture_descriptors) {
            FINE_SCOPE(FT_TEX);
            VkWriteDescriptorSet w = write(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, d.count);
            const size_t first = out.imgs.size();
            const VkImageViewType vt = view_type(d.type);
            // (Lichii bd0171f7) A handle table's array: the shader indexes it per pixel with an index it read itself (OoT's
            // clustered lights: a light index from the pixel's list), and element k is table entry k's texture (that light's
            // shadow map). The handles of all elements at once: the constant words and storage addresses the program reads
            // are read once
            static thread_local std::vector<uint32_t> table;
            const bool is_table = d.handle_program != ~0U && d.count > 1;
            if (is_table) table.resize(d.count), stage_handles(s, st, d.handle_program, d.count, table.data());
            for (uint32_t k = 0; k < d.count; k++) {
                // (Fritchii d603d98bb) A handle word where a storage buffer of the same program keeps its address is not a
                // handle: OoT's water samples each light's texture with a handle loaded per pixel from the light buffer, and
                // the translation falls back to the first constant word on its path, c0[0x520], the light buffer's address.
                // Its low bits named an unrelated texture that changed with the double-buffered buffer: the water flickered
                // every other frame. No texture there now. RX_VK_HANDLE_POINTER=1: the old handle
                // (fragment stages only, the water's case: in other stages (the foliage culling's) the textures went missing and
                // the foliage was culled whenever Link swung his sword. RX_VK_HANDLE_POINTER_ALL=1: every stage, as before)
                // (Handle tables now give those samples their own textures: this is for what the translation still cannot
                // compute per draw)
                static const bool all_stages = getenv("RX_VK_HANDLE_POINTER_ALL") != nullptr;
                const uint32_t handle = is_table ? table[k]
                                        : ((all_stages || st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT) && handle_is_storage_pointer(info, d, k))
                                            ? 0u : handle_of(s, st, d, k);
                desc_fp_note(handle);   // (RX_VK_DESC_SLOTGEN)
                Texture* tex = nullptr;
                upload_defer_ok = st.vk_stage != VK_SHADER_STAGE_COMPUTE_BIT;   // (a draw's: its new ASTC textures may wait a frame)
                VkImageView view = VK_NULL_HANDLE;
                VkSampler sampler = VK_NULL_HANDLE;
                // (a few handles a slot, by the handle's hash: a program drawn with one texture, then another, and back)
                TexSlot* sl = slots ? &slots[tslot++ * kTexWays + ((handle * 0x9E3779B1u) >> 30) % kTexWays] : nullptr;
                // (the versions read before resolving: a registration after them is seen at the next draw)
                const uint32_t version = sl ? texture_slot_version((int)(handle & 0xFFFFF)) : 0;
                const uint32_t samp_version = sl ? sampler_slot_version((int)(handle >> 20)) : 0;
                // (longshot #442: a slot's sampler is kept only while no sampler has been retired since, and, near the device's limit,
                // for a present: the book sees the samplers in use. A retired one may be destroyed once the GPU is past the recordings)
                const uint64_t samp_retired = sampler_book.retired_gen.load(std::memory_order_acquire);
                const uint64_t samp_frame = sampler_book.watching.load(std::memory_order_relaxed) ? texture_frame_no() : 0;
                if (sl && sl->valid && sl->samp_retired == samp_retired && sl->samp_frame == samp_frame) {}
                else if (sl) sl->valid = false;
                if (sl && sl->valid && sl->handle == handle && sl->version == version && sl->samp_version == samp_version &&
                    sl->destroy_gen != dgen_now && texture_gone_ring_on() && texture_kept_since(sl->tex, sl->destroy_gen, dgen_now))
                    sl->destroy_gen = dgen_now;   // (TICK-052: as texture_view_of's)
                if (par_stats_on && sl)
                    par_why(!sl->valid ? PW_SLOT_NEW : sl->handle != handle ? PW_SLOT_HANDLE
                            : sl->version != version || sl->samp_version != samp_version ? PW_SLOT_VERSION
                            : sl->destroy_gen != dgen_now ? PW_SLOT_DGEN : PW_COUNT);
                if (sl && sl->valid && sl->handle == handle && sl->version == version && sl->samp_version == samp_version &&
                    sl->destroy_gen == dgen_now) {
                    view = sl->view, tex = sl->tex, sampler = sl->sampler;
                    FINE_N(FC_TEX_HIT, 1);
                    if (tex && sl->tex_epoch != texture_epoch()) {   // (as texture_view_of's hit: once a submission)
                        if (!texture_touch_quick(tex, true)) {
                            if (par_stats_on) par_why(PW_TOUCH_SLOT);
                            ParLock lk;
                            texture_touch(tex, true);
                        }
                        sl->tex_epoch = texture_epoch();
                    }
                } else {
                    FINE_N(FC_TEX_MISS, 1);
                    FINE_SCOPE(FT_TEXMISS);
                    view = handle ? texture_view_of(handle, vt, ViewUse::Sample, VK_FORMAT_UNDEFINED, &tex) : VK_NULL_HANDLE;
                    // (Lichii bd0171f7) A depth-compare sample of a colour texture is undefined and differs between vendors: a
                    // handle read from memory (an unused or stale table entry) that names one binds nothing
                    if (is_table && view && tex && d.is_depth && !is_depth_format(tex->format)) {
                        static std::atomic<int> told{0};
                        if (told++ < 8)
                            logf("vk: handle table: element %u of program %016llx names texture #%u (format %d) for a depth "
                                 "comparison: bound as nothing", k, (unsigned long long)p.key, tex->id, (int)tex->format);
                        view = VK_NULL_HANDLE, tex = nullptr;
                    }
                    sampler = sampler_for((int)(handle >> 20), d.is_depth);
                    if (sl)
                        *sl = TexSlot{handle, version, samp_version, texture_epoch(), dgen_now, samp_retired, samp_frame, view, tex, sampler, true};
                }
                upload_defer_ok = false;
                // (Lichii bd0171f7) the shader clamps its index to the last element: a table that names a texture there may run
                // past the array, and its lights from that index on all take this one's texture. Said once
                if (is_table && k + 1 == d.count && view) {
                    static std::atomic<bool> told{false};
                    if (!told.exchange(true))
                        logf("vk: handle table: last element %u of program %016llx names texture #%u: the table may have more "
                             "entries than the array, and those take this texture (said once)", k, (unsigned long long)p.key,
                             tex ? tex->id : 0);
                }
                if (dump_on()) dump_input('t', (uint32_t)(out.imgs.size() - 0), handle, view ? tex : nullptr);
                if (view && tex && trace_tex_wanted(tex)) trace_tex_hit = tex;
                if (log_img_wanted(p.key)) {
                    static std::atomic<int> told{0};
                    if (told++ < 40)
                        logf("vk: img prog %016llx texture %u: handle %#x, shader integer %d type %d depth %d, view format "
                             "%d, texture #%d format %d %ux%ux%u x%u levels %u sparse %d view %p", (unsigned long long)p.key, k,
                             handle, (int)d.is_integer, (int)d.type, (int)d.is_depth, (int)last_view_format,
                             tex ? (int)tex->id : -1, tex ? (int)tex->format : -1, tex ? tex->width : 0, tex ? tex->height : 0,
                             tex ? tex->depth : 0, tex ? tex->layers : 0, tex ? tex->levels : 0, tex ? (int)tex->sparse : 0,
                             (void*)view);
                }
                if (dump_on() && view && tex && dump_sampled_wanted(p.key)) {
                    static std::unordered_set<uint32_t> done;
                    static int last_seq = 0;
                    if (dump_seq() < last_seq) done.clear();
                    last_seq = dump_seq();
                    if (done.size() < 96 && done.insert(tex->id).second) dump_sampled().push_back(tex);
                }
                if (watch_prog() && p.key == watch_prog())
                    watch_note("vk: watch %016llx: texture %u handle %#x -> #%u %#llx %ux%ux%u image %p view %p%s",
                               (unsigned long long)p.key, k, handle, tex ? tex->id : 0, (unsigned long long)(tex ? tex->gpu : 0),
                               tex ? tex->width : 0, tex ? tex->height : 0, tex ? tex->layers : 0, tex ? (void*)tex->image : nullptr,
                               (void*)view, view ? "" : " (dummy)");
                if (view && tex && tex->gpu_written) draw_sampled().push_back(tex);
                if (!view) view = dummies.view[(int)vt], st_dummy_textures += handle != 0;
                out.imgs.push_back({sampler, view, VK_IMAGE_LAYOUT_GENERAL});
            }
            w.pImageInfo = &out.imgs[first];
            out.writes.push_back(w);
        }
        for (const auto& d : info.image_descriptors) {
            VkWriteDescriptorSet w = write(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, d.count);
            const size_t first = out.imgs.size();
            const VkImageViewType vt = view_type(d.type);
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(s, st, d, k);
                desc_fp_note(handle);   // (RX_VK_DESC_SLOTGEN)
                Texture* tex = nullptr;
                uint32_t level = 0;
                VkImageView view = handle ? texture_view_of(handle, vt, ViewUse::Storage, image_format(d.format), &tex, &level)
                                          : VK_NULL_HANDLE;
                if (view && tex && d.is_written) { ParLock lk; texture_written(tex, "a storage image of a draw"); }
                if (view && tex) draw_sampled().push_back(tex);
                if (log_img_wanted(p.key)) {
                    static std::atomic<int> told{0};
                    if (told++ < 16)
                        logf("vk: img prog %016llx image %u: handle %#x, shader integer %d format %d (view as %d), "
                             "view format %d, texture format %d", (unsigned long long)p.key, k, handle,
                             (int)d.is_integer, (int)d.format, (int)image_format(d.format),
                             (int)last_view_format, tex ? (int)tex->format : -1);
                }
                if (tex && tex->sparse && d.is_written) {
                    static int logged;
                    if (logged++ < 20)
                        logf("vk: program %016llx stores into sparse texture %#llx (handle %#x, view format %d, %s)",
                             (unsigned long long)p.key, (unsigned long long)tex->gpu, handle, (int)image_format(d.format),
                             view ? "view made" : "NO view");
                }
                if (view && tex && d.is_written && (dump_on() || probe_on())) dump_written().push_back({tex, level});
                if (dump_on()) dump_input(d.is_written ? 'W' : 'R', (uint32_t)out.imgs.size(), handle, view ? tex : nullptr);
                if (!view && !dev.null_descriptor) view = dummies.view[(int)vt];
                out.imgs.push_back({VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
            }
            w.pImageInfo = &out.imgs[first];
            out.writes.push_back(w);
        }
    }
}

/// Binds the descriptors built: pushed, or a set from the recording's pool
// (push descriptors keep what is not pushed again: a program drawn again pushes only the bindings that changed,
// while nothing else was bound at its bind point in this command buffer)
struct Pushed {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    size_t n = 0;                                // (the writes as pushed)
    std::vector<std::vector<uint64_t>> writes;   // (each write's descriptors, as pushed; RX_VK_PUSH_STATE_OLD=1)
    // (hookshot) the same in one vector of words and where each write's begin: a program change was a vector of vectors resized to the
    // new program's writes, each new one grown word by word (3 allocations a write, and the ones cut off freed: a dozen of each a draw)
    std::vector<uint64_t> words;
    std::vector<uint32_t> at;
};
TLS_OBJECT(pushed, std::array<Pushed, 2>)   // [graphics, compute]
} // namespace

/// (vk_internal.h, longshot #442) The samplers made, those retired and not yet destroyed, those retired and destroyed in all, and the device's limit
void sampler_counts(uint32_t* made, uint32_t* retired_waiting, uint64_t* retired_total, uint64_t* destroyed, uint32_t* limit) {
    sampler_counts_read(made, retired_waiting, retired_total, destroyed, limit);
}
std::atomic<int64_t> st_driver_out_of_memory{0};
void draw_skip_report(double frames) { skip_report_impl(frames), fine_report(frames); }
void descriptors_disturbed() {
    pushed()[0].cmd = pushed()[1].cmd = VK_NULL_HANDLE;
}
bool stream_holds(const uint8_t* base, uint64_t at, uint64_t n) {
    return cur && cur->mapped == base && n && at + n >= at && at + n <= cur->used;
}
bool mesh_args_ready();             // (below: the task programs' firsts)
extern uint64_t mesh_args_address;
namespace {

void write_words(const VkWriteDescriptorSet& w, std::vector<uint64_t>& out) {
    out.clear();
    out.push_back((uint64_t)w.descriptorType << 32 | w.dstBinding);
    for (uint32_t k = 0; k < w.descriptorCount; k++) {
        if (w.pBufferInfo) {
            const VkDescriptorBufferInfo& b = w.pBufferInfo[k];
            out.push_back((uint64_t)b.buffer), out.push_back(b.offset), out.push_back(b.range);
        } else if (w.pImageInfo) {
            const VkDescriptorImageInfo& i = w.pImageInfo[k];
            out.push_back((uint64_t)i.sampler), out.push_back((uint64_t)i.imageView);
        } else if (w.pTexelBufferView) {
            out.push_back((uint64_t)w.pTexelBufferView[k]);
        }
    }
}

/// write_words' words appended to out (the words of the pushed state, a write after another)
void words_append(const VkWriteDescriptorSet& w, std::vector<uint64_t>& out) {
    out.push_back((uint64_t)w.descriptorType << 32 | w.dstBinding);
    for (uint32_t k = 0; k < w.descriptorCount; k++) {
        if (w.pBufferInfo) {
            const VkDescriptorBufferInfo& b = w.pBufferInfo[k];
            out.push_back((uint64_t)b.buffer), out.push_back(b.offset), out.push_back(b.range);
        } else if (w.pImageInfo) {
            const VkDescriptorImageInfo& i = w.pImageInfo[k];
            out.push_back((uint64_t)i.sampler), out.push_back((uint64_t)i.imageView);
        } else if (w.pTexelBufferView) {
            out.push_back((uint64_t)w.pTexelBufferView[k]);
        }
    }
}
/// Whether the words in [o, end) are write_words' of w (1), or not (0: they are after the call). -1: w is not the write they were made for
/// (its words do not fill them exactly), and nothing was touched
int words_same_store(const VkWriteDescriptorSet& w, uint64_t* o, const uint64_t* end) {
    const size_t per = w.pBufferInfo ? 3 : w.pImageInfo ? 2 : w.pTexelBufferView ? 1 : 0;
    if ((size_t)(end - o) != 1 + (size_t)w.descriptorCount * per) return -1;
    bool same = true;
    auto put = [&](uint64_t v) {
        same &= *o == v;
        *o++ = v;
    };
    put((uint64_t)w.descriptorType << 32 | w.dstBinding);
    for (uint32_t k = 0; k < w.descriptorCount; k++) {
        if (w.pBufferInfo) {
            const VkDescriptorBufferInfo& b = w.pBufferInfo[k];
            put((uint64_t)b.buffer), put(b.offset), put(b.range);
        } else if (w.pImageInfo) {
            const VkDescriptorImageInfo& i = w.pImageInfo[k];
            put((uint64_t)i.sampler), put((uint64_t)i.imageView);
        } else if (w.pTexelBufferView) {
            put((uint64_t)w.pTexelBufferView[k]);
        }
    }
    return same ? 1 : 0;
}

bool bind_descriptors(VkCommandBuffer cmd, VkPipelineBindPoint bp, const Program& p) {
    DescWriter& wr = dw();   // (hookshot, once: the accessor is a call and a thread-local read at each use. The same object for the thread's life: build_descriptors resets it in place)
    if (wr.writes.empty()) return true;
    Pushed& ps = pushed()[bp == VK_PIPELINE_BIND_POINT_COMPUTE];
    if (p.push) {
        static const bool whole = getenv("RX_VK_PUSH_ALL") != nullptr;   // (A/B: every binding every time)
        static const bool old_state = getenv("RX_VK_PUSH_STATE_OLD") != nullptr;   // (A/B: a vector of words a write, as before)
        if (whole || ps.cmd != cmd || ps.layout != p.layout || ps.n != wr.writes.size()) {
            if (g_fine) {
                fine_n[FC_PUSH_FULL]++, fine_n[FC_PUSH_WRITES] += (int64_t)wr.writes.size();
                for (const VkWriteDescriptorSet& w : wr.writes) fine_n[FC_PUSH_DESCS] += w.descriptorCount;
            }
            {
                FINE_SCOPE(FT_PUSHCALL);
                vkCmdPushDescriptorSetKHR(cmd, bp, p.layout, 0, (uint32_t)wr.writes.size(), wr.writes.data());
            }
            ps.cmd = cmd, ps.layout = p.layout, ps.n = wr.writes.size();
            if (old_state) {
                ps.writes.resize(wr.writes.size());
                for (size_t i = 0; i < wr.writes.size(); i++) write_words(wr.writes[i], ps.writes[i]);
            } else {
                ps.words.clear(), ps.at.clear();
                for (const VkWriteDescriptorSet& w : wr.writes) ps.at.push_back((uint32_t)ps.words.size()), words_append(w, ps.words);
            }
            return true;
        }
        static thread_local std::vector<VkWriteDescriptorSet> changed;
        static thread_local std::vector<uint64_t> words;
        changed.clear();
        if (old_state) {
            for (size_t i = 0; i < wr.writes.size(); i++) {
                write_words(wr.writes[i], words);
                if (words != ps.writes[i]) changed.push_back(wr.writes[i]), ps.writes[i].swap(words);
            }
        } else {   // (the same layout, so the same count of words in each write: compared and kept in place)
            for (size_t i = 0; i < wr.writes.size(); i++) {
                const uint64_t* end = ps.words.data() + (i + 1 < ps.at.size() ? ps.at[i + 1] : ps.words.size());
                const int same = words_same_store(wr.writes[i], ps.words.data() + ps.at[i], end);
                if (same < 0) {   // (not so: nothing is known of what is pushed; the whole set again)
                    ps.cmd = VK_NULL_HANDLE;
                    return bind_descriptors(cmd, bp, p);
                }
                if (!same) changed.push_back(wr.writes[i]);
            }
        }
        if (!changed.empty()) {
            if (g_fine) {
                fine_n[FC_PUSH_DELTA]++, fine_n[FC_PUSH_WRITES] += (int64_t)changed.size();
                for (const VkWriteDescriptorSet& w : changed) fine_n[FC_PUSH_DESCS] += w.descriptorCount;
            }
            FINE_SCOPE(FT_PUSHCALL);
            vkCmdPushDescriptorSetKHR(cmd, bp, p.layout, 0, (uint32_t)changed.size(), changed.data());
        } else {
            FINE_N(FC_PUSH_NONE, 1);
        }
        return true;
    }
    ps.cmd = VK_NULL_HANDLE;   // (a set bound: what was pushed is not known to stay)
    if (g_fine) {
        fine_n[FC_POOL_SETS]++;
        for (const VkWriteDescriptorSet& w : wr.writes) fine_n[FC_POOL_DESCS] += w.descriptorCount;
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cur->extra.empty() ? cur->pool : cur->extra.back();
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(dev.device, &ai, &set) != VK_SUCCESS) {
        cur->extra.push_back(make_pool());
        ai.descriptorPool = cur->extra.back();
        if (vkAllocateDescriptorSets(dev.device, &ai, &set) != VK_SUCCESS) return false;
    }
    for (auto& w : wr.writes) w.dstSet = set;
    vkUpdateDescriptorSets(dev.device, (uint32_t)wr.writes.size(), wr.writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(cmd, bp, p.layout, 0, 1, &set, 0, nullptr);
    return true;
}

// ---- RX_VK_CHECKPOINTS: a marker before each draw and dispatch (VK_NV_device_diagnostic_checkpoints); when the
// device is lost, the markers the GPU reached say which program it was running ----
// (shape and a, b, c: what the work is, set once the draw or dispatch knows: checkpoint_work. 'x' dispatch of a x b x c groups;
// 'n' indirect dispatch; 'V' draw of a vertices, b instances; 'E' indexed draw of a indices, b instances; 'Q' quads drawn as
// a indices, b instances; 'F' fallback draw of a vertices; 'I' indirect draw of up to a records; 'M' mesh draw of a x b x c
// groups; 'N' indirect mesh draw of up to a records; 'C' the indirect conversion of a records, stride b; 0 unknown)
// (flags: what a mesh draw is, set by checkpoint_flags: kCpKnown it was noted, kCpTask a task stage, kCpCount its count read from a GPU buffer,
// kCpConverted its records the conversion's copy; the conversion's own markers, 's' fill and 'y' copy, name the arguments' address as prog)
constexpr uint8_t kCpKnown = 1, kCpTask = 2, kCpCount = 4, kCpConverted = 8;
struct Checkpoint { uint64_t prog; uint64_t frame; uint32_t seq; char kind; char shape; uint8_t flags; uint32_t a, b, c; };
Checkpoint checkpoints[1 << 16];
std::atomic<uint32_t> checkpoint_n{0};

// (VK_AMD_buffer_marker, where the NV checkpoints are missing: marker i's sequence number goes to word 2i when the GPU
// reaches it and to word 2i + 1 once all the work before it is done. Host memory, not VRAM: a GPU reset loses VRAM.)
VkBuffer marker_buffer = VK_NULL_HANDLE;
volatile uint32_t* marker_words = nullptr;

// (RX_VK_CHECKPOINTS=ext: the extension on, no markers: which of the two keeps the device from being lost)
bool checkpoint_markers() {
    static const bool on = !getenv("RX_VK_CHECKPOINTS") || strcmp(getenv("RX_VK_CHECKPOINTS"), "ext") != 0;
    return on;
}
bool checkpoints_on() {
    return ((dev.checkpoints && vkCmdSetCheckpointNV) || marker_words) && checkpoint_markers();
}
void set_checkpoint(VkCommandBuffer cmd, uint32_t i, uint32_t seq) {
    if (marker_words) {
        vkCmdWriteBufferMarkerAMD(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, marker_buffer, 8ull * i, seq);
        vkCmdWriteBufferMarkerAMD(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, marker_buffer, 8ull * i + 4, seq);
    } else {
        vkCmdSetCheckpointNV(cmd, (void*)(uintptr_t)(i + 1));
    }
}
// (TICK-061, D6, RX_VK_WORK_LOCAL=1) Every draw and dispatch took the next marker with ++checkpoint_n, one atomic the five recording
// threads of a parallel replay all bumped, and wrote its 24-byte entry next to another thread's. With the switch, a recording thread
// inside a parallel segment (work_seg_local) takes 64 numbers at a time (one fetch_add) and uses them in order: each marker still
// gets a number of its own (the blocks never overlap), its entry is written by one thread, and set_checkpoint records the same
// call with it. A block is used only in the frame it was taken in (texture_frame_no), and a frame sets ~3,000 markers of the
// ring's 65,536, so a block's entries are not reused by another thread while it holds it. What changes is the device-lost report
// only: neighbouring entries of the ring are then the same thread's draws, not the last ones of all threads in time. The NV
// checkpoints stay on as they were (VK_NV_device_diagnostic_checkpoints: the device was lost every run without them). The AMD buffer
// markers (marker_words) keep the global order: their report reads the numbers as a time order.
uint32_t checkpoint_seq() {
    if (!work_seg_local || marker_words) return ++checkpoint_n;
    thread_local uint32_t next = 0, end = 0;
    thread_local uint64_t frame = ~0ull;
    const uint64_t f = texture_frame_no();
    if (next == end || frame != f) {
        next = checkpoint_n.fetch_add(64) + 1, end = next + 64;
        frame = f;
    }
    return next++;
}
thread_local uint32_t checkpoint_last = 0;   // (this thread's latest marker: checkpoint_work)
void checkpoint(VkCommandBuffer cmd, const Program& p, char kind) {
    if (!checkpoints_on()) return;
    const uint32_t seq = checkpoint_seq(), i = (seq - 1) & 0xFFFF;
    checkpoints[i] = {p.key, texture_frame_no(), seq, kind, 0, 0, 0, 0, 0};
    checkpoint_last = seq;
    set_checkpoint(cmd, i, seq);
}
// (the work the thread's latest marker stands for, filled in when the call's counts are known: the stall report names its size)
void checkpoint_work(char shape, uint32_t a, uint32_t b = 0, uint32_t c = 0) {
    if (!checkpoint_last) return;
    Checkpoint& k = checkpoints[(checkpoint_last - 1) & 0xFFFF];
    if (k.seq != checkpoint_last) return;   // (the ring went round: the entry is another marker's)
    k.shape = shape, k.a = a, k.b = b, k.c = c;
}
bool mesh_has_task(const Program& p) {   // (a task stage in the program: the marker's flags; only with the markers on)
    for (const Stage& st : p.stages)
        if (st.vk_stage & VK_SHADER_STAGE_TASK_BIT_EXT) return true;
    return p.fallback_task;
}
// (what kind of mesh draw the thread's latest marker is: with a task stage, a count read from the GPU, its records converted)
void checkpoint_flags(bool task, bool counted, bool converted) {
    if (!checkpoint_last) return;
    Checkpoint& k = checkpoints[(checkpoint_last - 1) & 0xFFFF];
    if (k.seq != checkpoint_last) return;
    k.flags = (uint8_t)(kCpKnown | (task ? kCpTask : 0) | (counted ? kCpCount : 0) | (converted ? kCpConverted : 0));
}
/// What a marker's work is, for the report: "" when unknown.
std::string checkpoint_work_text(const Checkpoint& k) {
    char t[96];
    switch (k.shape) {
    case 'x': snprintf(t, sizeof t, ", %u x %u x %u groups", k.a, k.b, k.c); break;
    case 'n': snprintf(t, sizeof t, ", groups read from GPU memory"); break;
    case 'V': snprintf(t, sizeof t, ", %u vertices x %u instances", k.a, k.b); break;
    case 'E': snprintf(t, sizeof t, ", %u indices x %u instances", k.a, k.b); break;
    case 'Q': snprintf(t, sizeof t, ", %u indices (quads) x %u instances", k.a, k.b); break;
    case 'F': snprintf(t, sizeof t, ", %u vertices (fallback)", k.a); break;
    case 'I': snprintf(t, sizeof t, ", indirect, up to %u records", k.a); break;
    case 'M': snprintf(t, sizeof t, ", mesh %u x %u x %u groups%s", k.a, k.b, k.c,
                       !(k.flags & kCpKnown) ? "" : k.flags & kCpTask ? " (task+mesh)" : " (mesh only)"); break;
    case 'N': snprintf(t, sizeof t, ", mesh indirect, up to %u records%s%s%s", k.a,
                       !(k.flags & kCpKnown) ? "" : k.flags & kCpTask ? " (task+mesh)" : " (mesh only)",
                       k.flags & kCpCount ? ", count read from GPU memory" : "", k.flags & kCpConverted ? ", records converted" : ""); break;
    case 'C': snprintf(t, sizeof t, ", %u records, stride %u", k.a, k.b); break;
    default: return {};
    }
    return t;
}
const char* checkpoint_kind(char c) {
    return c == 'd' ? "draw of program" : c == 'c' ? "dispatch of program" : c == 'i' ? "INDIRECT dispatch of program"
         : c == 'm' ? "mirror sync of pool"
         : c == 'b' ? "write-back of pool" : c == 'w' ? "write-back diff pass, chunk count" : c == 'u' ? "upload of texture"
         : c == 'g' ? "GPU unswizzle of texture"
         : "?";
}
/// A marker's label for the report: its kind, told apart by what the draw is (the mesh draws are 'd' too, their shape says so) and by
/// the work the indirect conversion adds. "...program" and "... at" are followed by the key (the conversion has the arguments' address).
const char* checkpoint_label(const Checkpoint& k) {
    switch (k.kind) {
    case 'd': return k.shape == 'N' ? "INDIRECT MESH draw of program" : k.shape == 'M' ? "MESH draw of program"
                   : k.shape == 'F' ? "fallback MESH draw of program" : "draw of program";
    case 's': return "indirect-mesh CONVERSION fill, arguments at";
    case 'y': return "indirect-mesh CONVERSION copy, arguments at";
    case 'p': return "grid-patch copy for program";
    default: return checkpoint_kind(k.kind);
    }
}
/// What the markers in [from, to] are, counted by label ("12 draw of program, 3 INDIRECT MESH draw of program"): the work around the stuck
/// point as a census. Capped to the ring's 65535 and to 4096 markers a look.
std::string checkpoint_census(uint32_t from, uint32_t to) {
    if (to > from + 4095) to = from + 4095;
    std::vector<std::pair<std::string, uint32_t>> n;
    for (uint32_t s = from; s && s <= to; s++) {
        const Checkpoint& k = checkpoints[(s - 1) & 0xFFFF];
        if (k.seq != s) continue;
        const std::string label = checkpoint_label(k);
        auto it = std::find_if(n.begin(), n.end(), [&](const auto& e) { return e.first == label; });
        if (it == n.end()) n.emplace_back(label, 1);
        else it->second++;
    }
    std::string out;
    for (const auto& e : n) out += (out.empty() ? "" : ", ") + std::to_string(e.second) + " x " + e.first;
    return out.empty() ? "none" : out;
}

thread_local uint64_t fallback_payload_address = 0, fallback_records_address = 0;   // (the draw's fallback buffers)

void push_constants(VkCommandBuffer cmd, const Program& p, const Targets* at) {
    checkpoint(cmd, p, at ? 'd' : 'c');
    if (!p.uses_push) return;   // (most programs read none: a push a draw was ~5% of the replay's commands)
    FINE_N(FC_PUSHC, 1);
    uint8_t data[kPushSize]{};
    const float one = 1.0f;
    std::memcpy(data + Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET, &one, 4);
    {   // (guest memory's page table, for the pointers compute, task and mesh stages follow)
        static const uint64_t pages = memory_guest_pages();
        std::memcpy(data + Shader::Backend::SPIRV::GUEST_PAGES_PUSH_OFFSET, &pages, 8);
    }
    if (at && mesh_args_ready()) {   // (a task program's firsts: entry 0's 0 until its draw says otherwise, never address 0)
        const uint64_t firsts = mesh_args_address + 12;
        std::memcpy(data + Shader::Backend::SPIRV::MESH_FIRST_PUSH_OFFSET, &firsts, 8);
    }
    if (p.fallback) {   // (the fallback's records and payloads, pushed as addresses)
        std::memcpy(data + Shader::Backend::SPIRV::MESH_FALLBACK_PAYLOAD_PUSH_OFFSET, &fallback_payload_address, 8);
        std::memcpy(data + Shader::Backend::SPIRV::MESH_FALLBACK_RECORDS_PUSH_OFFSET, &fallback_records_address, 8);
    }
    if (p.uses_render_area && at) {
        // (at the block's own offset, 0, as Eden pushes it: the SPIR-V back end declares RenderAreaInfo at offset 0,
        // over the rescaling words. Placed after RescalingLayout, the shaders read zeros: every full-screen pass
        // that takes its texel size from it (SMAA's edges, weights and blending) sampled at infinity, and the
        // title's SMAA flagged 94% of the picture as edges: the red and streaked frames)
        const float ra[4] = {(float)at->width, (float)at->height, 0.0f, 0.0f};
        std::memcpy(data + Shader::Backend::SPIRV::RENDERAREA_LAYOUT_OFFSET, ra, sizeof ra);
    }
    vkCmdPushConstants(cmd, p.layout, p.push_stages, 0, kPushSize, data);
}

} // namespace

void device_fault_report() {
    if (!dev.device_fault || !vkGetDeviceFaultInfoEXT) {
        logf("vk: device fault: VK_EXT_device_fault not available");
        return;
    }
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (const VkResult r = vkGetDeviceFaultInfoEXT(dev.device, &counts, nullptr); r != VK_SUCCESS) {
        logf("vk: device fault: no information (%d)", (int)r);
        return;
    }
    std::vector<VkDeviceFaultAddressInfoEXT> addrs(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addrs.data(), info.pVendorInfos = vendor.data();
    counts.vendorBinarySize = 0;
    if (vkGetDeviceFaultInfoEXT(dev.device, &counts, &info) != VK_SUCCESS) return;
    logf("vk: device fault: %s", info.description);
    static const char* kinds[] = {"none", "read invalid", "write invalid", "execute invalid", "instruction pointer unknown",
                                  "instruction pointer invalid", "instruction pointer fault"};
    for (const auto& a : addrs)
        logf("vk:   %s at %#llx (precision %#llx)", (unsigned)a.addressType < 7 ? kinds[a.addressType] : "?",
             (unsigned long long)a.reportedAddress, (unsigned long long)a.addressPrecision);
    for (const auto& v : vendor)
        logf("vk:   vendor: %s (code %#llx, data %#llx)", v.description, (unsigned long long)v.vendorFaultCode,
             (unsigned long long)v.vendorFaultData);
}

void checkpoint_mark(VkCommandBuffer cmd, uint64_t key, char kind, char shape, uint32_t a, uint32_t b, uint32_t c) {
    if (!checkpoints_on()) return;
    const uint32_t seq = checkpoint_seq(), i = (seq - 1) & 0xFFFF;   // (TICK-061: see checkpoint_seq)
    checkpoints[i] = {key, texture_frame_no(), seq, kind, shape, 0, a, b, c};
    checkpoint_last = 0;
    set_checkpoint(cmd, i, seq);
}

namespace { void buffer_markers_watch(); }

void buffer_markers_init() {
    if (!vkCmdWriteBufferMarkerAMD) return;
    const VkDeviceSize size = 8ull << 16;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};
    if (vkCreateBuffer(dev.device, &bi, nullptr, &buffer) != VK_SUCCESS) return;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev.device, buffer, &req);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (int pass = 0; pass < 2 && type == UINT32_MAX; pass++)   // (system memory first: it outlasts a reset)
        for (uint32_t i = 0; i < dev.memory.memoryTypeCount && type == UINT32_MAX; i++) {
            const VkMemoryPropertyFlags f = dev.memory.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits >> i & 1) && (f & want) == want && (pass || !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
                type = i;
        }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    VkDeviceMemory memory{};
    void* words = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(dev.device, &ai, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev.device, buffer, nullptr);
        logf("vk: RX_VK_CHECKPOINTS: no host memory for the buffer markers");
        return;
    }
    vkBindBufferMemory(dev.device, buffer, memory, 0);
    if (vkMapMemory(dev.device, memory, 0, size, 0, &words) != VK_SUCCESS) {
        vkDestroyBuffer(dev.device, buffer, nullptr);
        vkFreeMemory(dev.device, memory, nullptr);
        return;
    }
    std::memset(words, 0, size);
    marker_buffer = buffer;
    marker_words = (volatile uint32_t*)words;
    logf("vk: RX_VK_CHECKPOINTS: VK_AMD_buffer_marker, a marker before each draw and dispatch (memory type %u)", type);
    buffer_markers_watch();
}

namespace {
// ---- when the watch first saw each marker reached (issue 14: how long the work in flight had been running) ----
// A cell a marker's slot of the ring: its sequence number in the high word, the steady clock's milliseconds (wrapping) in the low.
// Filled on each of the watch's looks (every 250 ms), so a time is the first look that found the marker reached, up to 0.25 s late.
std::atomic<uint64_t> reach_seen[1 << 16];
uint32_t steady_ms() {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// What the report says of marker seq: when it was first seen reached, or that no look saw it.
std::string marker_seen_text(uint32_t seq, uint32_t now_ms) {
    const uint64_t cell = reach_seen[(seq - 1) & 0xFFFF].load(std::memory_order_relaxed);
    if ((uint32_t)(cell >> 32) != seq) return "not seen reached";
    char t[64];
    snprintf(t, sizeof t, "reached %.1f s ago", (uint32_t)(now_ms - (uint32_t)cell) / 1000.0);
    return t;
}
// ---- end of the ring ----

// (the markers the GPU reached whose earlier work never finished: the hang is in the work after the last marker that did)
void buffer_markers_report() {
    const uint32_t n = checkpoint_n.load(), first = n > 0xFFFF ? n - 0xFFFF : 1;
    uint32_t reached = 0, last_done = 0;
    std::vector<uint32_t> stuck;
    for (uint32_t s = first; s && s <= n; s++) {
        const uint32_t i = (s - 1) & 0xFFFF;
        const bool r = marker_words[2 * i] == s, d = marker_words[2 * i + 1] == s;
        reached += r;
        if (d) last_done = s;
        else if (r) stuck.push_back(s);
    }
    logf("vk: device lost: %u markers set, %u reached; the last whose earlier work all finished: %u", n, reached, last_done);
    const uint32_t now_ms = steady_ms();
    const auto line = [now_ms](const char* what, uint32_t s) {
        const Checkpoint& k = checkpoints[(s - 1) & 0xFFFF];
        logf("vk:   %s: marker %u (%s %016llx%s, frame %llu), %s", what, s, checkpoint_label(k), (unsigned long long)k.prog,
             checkpoint_work_text(k).c_str(), (unsigned long long)k.frame, marker_seen_text(s, now_ms).c_str());
    };
    for (uint32_t s = last_done > 6 ? last_done - 6 : 1; last_done && s < last_done; s++) line("finished before", s);
    // (from the last finished marker to the first reached one whose earlier work did not finish: what the GPU was running)
    const uint32_t start = last_done ? last_done : first;
    const uint32_t end = std::max(start, stuck.empty() ? start + 8 : std::min(stuck.front(), start + 16));
    for (uint32_t s = start; s && s <= end && s <= n; s++) line(s == last_done ? "RUNNING after" : "running", s);
    for (size_t j = 0; j < stuck.size() && j < 12; j++) line("reached, earlier work unfinished", stuck[j]);
    // (what is queued behind them: set by the CPU, never reached. Mesh draws, dispatches and the indirect conversion can sit here, out of
    // the window above, which only holds what the GPU reached)
    uint32_t lastr = last_done;
    for (uint32_t s = first; s && s <= n; s++)
        if (marker_words[2 * ((s - 1) & 0xFFFF)] == s) lastr = s;
    if (lastr < n) {
        logf("vk:   queued, never reached: markers %u..%u (%u)", lastr + 1, n, n - lastr);
        for (uint32_t s = lastr + 1; s <= n && s <= lastr + 8; s++) line("queued", s);
    } else {
        logf("vk:   queued, never reached: none");
    }
    const uint32_t cen_from = start > 600 ? start - 600 : 1;
    logf("vk:   work in the 600 markers before the stuck point (%u..%u): %s", cen_from, start, checkpoint_census(cen_from, start).c_str());
    if (lastr < n)
        logf("vk:   work queued behind it (%u..%u): %s", lastr + 1, std::min(n, lastr + 4096), checkpoint_census(lastr + 1, n).c_str());
}

/// (issue 14) The markers read while the GPU is stuck, not only once the device is lost. On a Steam Deck the ring timed out
/// and the submission never came back (no VK_ERROR_DEVICE_LOST, so checkpoints_report never ran), though the markers sit
/// in host memory the CPU can read at any time. A thread looks at them four times a second: when markers the GPU reached
/// have work unfinished and no marker has finished for RX_VK_CHECKPOINTS_STALL_S seconds (default 2: under amdgpu's
/// lockup timeout, so before the reset), buffer_markers_report is logged then, once a stall (three at most).
/// RX_VK_CHECKPOINTS_STALL_S=0: no watch, the report at the loss alone as before
struct MarkerScan {
    uint32_t done = 0, last_done = 0, pending = 0, n = 0;   // (done: in the last 65535; pending: reached, earlier work unfinished)
};
MarkerScan marker_scan(uint32_t note_ms = 0) {   // (note_ms: the watch's look, the clock's reading it stamps the markers it finds reached with)
    MarkerScan m;
    m.n = checkpoint_n.load();
    const uint32_t first = m.n > 0xFFFF ? m.n - 0xFFFF : 1;
    for (uint32_t s = first; s && s <= m.n; s++) {
        const uint32_t i = (s - 1) & 0xFFFF;
        const bool r = marker_words[2 * i] == s, d = marker_words[2 * i + 1] == s;
        if (d) m.done++, m.last_done = s;
        else if (r) m.pending++;
        if (r && note_ms) {
            const uint64_t cell = reach_seen[i].load(std::memory_order_relaxed);
            if ((uint32_t)(cell >> 32) != s) reach_seen[i].store((uint64_t)s << 32 | note_ms, std::memory_order_relaxed);
        }
    }
    return m;
}
// (the watch's call on each look: true when the GPU has been stuck stall_s seconds, then not again until a marker finishes.
// Progress is the GPU's alone: more markers finished, or a later one. The CPU recording more is not (it can go on a while
// with the GPU stuck), and the markers it records push the oldest out of the 65535, so fewer finished is not either)
struct MarkerStall {
    double stall_s = 2.0, since = 0;
    MarkerScan last;
    bool told = false;
    bool step(const MarkerScan& m, double now) {
        const bool moved = m.done > last.done || m.last_done > last.last_done;
        last = m;
        if (moved || !m.pending) {   // (progress, or nothing under way: the clock starts again)
            since = now, told = false;
            return false;
        }
        if (told || now - since < stall_s) return false;
        return told = true;
    }
};
double marker_stall_seconds() {
    const char* e = getenv("RX_VK_CHECKPOINTS_STALL_S");
    return e ? atof(e) : 2.0;
}
void buffer_markers_watch() {
    const double stall_s = marker_stall_seconds();
    if (!(stall_s > 0) || !marker_words) return;
    logf("vk: RX_VK_CHECKPOINTS: the markers are logged too when the GPU finishes none for %.1f s (RX_VK_CHECKPOINTS_STALL_S)",
         stall_s);
    std::thread([stall_s] {
        SetThreadDescription(GetCurrentThread(), L"mkw_vk marker watch");
        MarkerStall w;
        w.stall_s = stall_s;
        const auto t0 = std::chrono::steady_clock::now();
        for (int reports = 0; reports < 3;) {
            Sleep(250);
            const MarkerScan m = marker_scan(steady_ms());
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!w.step(m, now)) continue;
            reports++;
            logf("vk: GPU stalled: no marker finished for %.1f s, %u reached with earlier work unfinished (the markers now, "
                 "before a reset)", now - w.since, m.pending);
            buffer_markers_report();
        }
    }).detach();
}
} // namespace

void draw_records_log(uint64_t lo, uint64_t hi);
void checkpoints_report() {
    static bool done;
    if (done) return;
    done = true;
    if (getenv("RX_VK_FAULT_INFO")) device_fault_report();   // (now, while the process is still alive: the faulting address)
    {   // (RX_VK_FLASH_RECORD=<program,...>: those programs' last draws, sizes and all)
        const uint64_t f = texture_frame_no();
        draw_records_log(f > 6 ? f - 6 : 0, f);
    }
    // (checkpoints first: on NVIDIA vkGetDeviceFaultInfoEXT has not come back after a fault, which lost the report)
    logf("vk: device lost: reading %s", dev.checkpoints ? "the checkpoints"
                                        : marker_words ? "the buffer markers"
                                                       : "the fault info (RX_VK_CHECKPOINTS=1 for markers)");
    struct FaultLast { ~FaultLast() { if (std::getenv("RX_VK_FAULT_INFO")) device_fault_report(); } } fault_last;
    const auto watched = [] {
        std::lock_guard lk(watch_mu);
        for (int i = 0; i < 40; i++)
            if (!watch_ring[(watch_at + i) % 40].empty()) logf("%s", watch_ring[(watch_at + i) % 40].c_str());
    };
    if (marker_words) {
        buffer_markers_report();
        watched();
        return;
    }
    if (!dev.checkpoints || !vkGetQueueCheckpointDataNV) return;
    uint32_t n = 0;
    vkGetQueueCheckpointDataNV(dev.queue, &n, nullptr);
    std::vector<VkCheckpointDataNV> cps(n, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
    vkGetQueueCheckpointDataNV(dev.queue, &n, cps.data());
    logf("vk: device lost: %u checkpoints (%u markers set)", n, checkpoint_n.load());
    watched();
    for (const VkCheckpointDataNV& c : cps) {
        const uint32_t i = (uint32_t)(uintptr_t)c.pCheckpointMarker - 1;
        if (i >= (1u << 16)) continue;
        const Checkpoint& k = checkpoints[i];
        const auto kind = checkpoint_kind;
        logf("vk:   stage %#x: marker %u (%s %016llx%s, frame %llu)", (unsigned)c.stage, k.seq, checkpoint_label(k),
             (unsigned long long)k.prog, checkpoint_work_text(k).c_str(), (unsigned long long)k.frame);
        // (the ones before it, for context)
        for (uint32_t b = 1; b <= 6; b++) {
            const Checkpoint& o = checkpoints[(i - b) & 0xFFFF];
            logf("vk:     before: %s %016llx%s", checkpoint_label(o), (unsigned long long)o.prog, checkpoint_work_text(o).c_str());
        }
        // (and the ones after it: what the GPU never reached)
        for (uint32_t b = 1; b <= 8; b++) {
            const Checkpoint& o = checkpoints[(i + b) & 0xFFFF];
            logf("vk:     after: marker %u %s %016llx, frame %llu", o.seq, kind(o.kind), (unsigned long long)o.prog, (unsigned long long)o.frame);
        }
    }
}

namespace {

// ---- the open render pass ----
struct Pass {
    bool open = false;
    VkImageView color[8]{};
    Texture* tex[8]{};   // (RX_VK_DUMP: what the pass renders into, and the last program it drew with)
    uint32_t level[8]{}, layer[8]{};
    Texture* depth_tex = nullptr;
    uint32_t depth_level = 0, depth_layer = 0;
    uint64_t prog = 0;
    VkImageView depth = VK_NULL_HANDLE;
    int n = 0;
    uint32_t width = 0, height = 0;
    int draws = 0;           // (RX_VK_GPU_PROFILE: its draws, and its label's targets)
    uint32_t wm_or = 0, wm_n = 0;   // (RX_VK_PASS_TRACE: the colour write masks its draws set, target 0, and the targets)
    int clears = 0;          // (RX_VK_PASS_TRACE: its clears, and the first program it drew with)
    uint64_t first_prog = 0;
    VkFormat format0 = VK_FORMAT_UNDEFINED;
    std::vector<const Texture*> sampled;   // (rendered textures its draws sampled: a target among them is a hazard)
    uint16_t sampled_targets = 0;          // (colour i bit i, depth bit 8: targets earlier draws sampled)
    uint16_t written = 0;                  // (its targets written so far: colour i bit i, depth bit 8)
};
TLS_OBJECT(pass, Pass)
thread_local bool work_since_barrier = false;   // (GPU work recorded since the last full barrier: a dispatch orders after it)
thread_local VkCommandBuffer last_dispatch_cmd = VK_NULL_HANDLE;   // (the last direct or indirect dispatch's command buffer, and the
thread_local uint64_t last_dispatch_epoch = 0;                     // work count right after it: game_order's back-to-back test)
// what the command buffer has bound (draws skip what is already there)
thread_local bool last_desc_valid = false;
thread_local const Program* last_desc_prog = nullptr;
thread_local uint64_t last_desc_bind = 0, last_desc_reg = 0, last_desc_ub = 0, last_desc_dgen = 0;
// (RX_VK_DESC_SLOTGEN: the versions of the slots the last build resolved, the samplers' generation it saw, and whether it
// sampled an unregistered slot; the same draw keeps its descriptors while none of those moved)
TLS_OBJECT(last_desc_fp, std::vector<uint64_t>)
thread_local bool last_desc_fp_dummy = false;
thread_local uint64_t last_desc_samp = 0, last_desc_destroy = 0;
int64_t st_desc_slotgen_saves;   // (reuses the global registrations' generation check would have rebuilt for)
/// (RX_VK_DESC_SLOTGEN) Whether every slot the last build resolved still holds what it did: no registration since touched a
/// slot this draw samples, so its descriptors stand through the streaming's churn. A build that sampled an unregistered slot
/// keeps the old check: a registration is the only thing that could make such a handle resolve, and no slot version moved.
bool desc_slots_kept(uint64_t reg_gen) {
    if (last_desc_samp != sampler_registrations_generation()) return false;
    if (last_desc_destroy != texture_destroy_generation()) return false;   // (an eviction sweep: destroy_later retires the
    if (last_desc_fp_dummy && last_desc_reg != reg_gen) return false;      // (views without bumping any slot, so they alone stay silent)
    for (const uint64_t e : last_desc_fp())
        if (texture_slot_version((int)(e & 0xFFFFF)) != (uint32_t)(e >> 20)) return false;
    return true;
}
thread_local VkPipeline last_pipeline = VK_NULL_HANDLE;
struct LastVb { VkBuffer b[16]; VkDeviceSize o[16], z[16], s[16]; };
thread_local LastVb last_vb;
thread_local bool last_vb_valid = false;
// (the guest streams and index buffer already bound on this command buffer; a new recording drops them)
struct VertexSet {
    uint64_t stamp = 0;
    uint64_t addr[16]{};
    uint64_t size[16]{};
    uint32_t stride[16]{};
    bool on = false;
};
thread_local VertexSet vertex_set;
struct IndexBind {
    uint64_t buffer = 0, offset = 0;
    uint32_t type = 0;
    bool on = false;
};
thread_local IndexBind last_ib;
SplitCount st_desc_reused;   // (TICK-052: split_count.h)

thread_local uint64_t targets_serial = ~0ull;   // (the state's render targets resolved once per NOP_SET_RT)
thread_local Targets cached_targets;

Targets targets(NvnState& s) {
    if (targets_serial == s.rt_serial) return cached_targets;
    ParLock lk;
    Targets at;
    at.n = std::clamp(s.rt.n, 0, 8);
    for (int i = 0; i < at.n; i++) {
        const nvn_tex* t = s.rt.color[i];
        Texture* tex = t ? texture_get(t, false) : nullptr;
        if (tex && !(tex->aspect & VK_IMAGE_ASPECT_COLOR_BIT)) tex = nullptr;
        if (!tex) continue;
        const nvn_view* v = s.rt.has_cview[i] ? &s.rt.cview[i] : nullptr;
        VkFormat f = VK_FORMAT_UNDEFINED;
        const VkImageView view = texture_view(tex, t, v, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Attachment, &f);
        if (!view) continue;
        at.color[i] = tex, at.color_view[i] = view, at.color_format[i] = f;
        const uint32_t level = image_level(tex, view_level(v));
        at.color_level[i] = level, at.color_layer[i] = v ? view_layer(v) : 0;
        uint32_t w = std::max(1u, tex->width >> level), h = std::max(1u, tex->height >> level);
        // (a raw view of a compressed target is one texel a block: its extent in blocks. The texels' 64 x 64 over the 16 x 16
        // view of OoT's noise volumes put the render area past the attachment)
        const auto bc = [](VkFormat x) { return x >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && x <= VK_FORMAT_BC7_SRGB_BLOCK; };
        if (bc(tex->format) && !bc(f)) w = (w + 3) / 4, h = (h + 3) / 4;
        at.width = at.width ? std::min(at.width, w) : w;
        at.height = at.height ? std::min(at.height, h) : h;
        at.samples = tex->samples;
    }
    // (a texture at two slots: the later one left unattached, as nxnative does for BotW's G-buffer)
    for (int i = 0; i < at.n; i++)
        for (int j = 0; j < i; j++)
            if (at.color_view[i] && at.color_view[i] == at.color_view[j]) {
                at.color[i] = nullptr, at.color_view[i] = VK_NULL_HANDLE, at.color_format[i] = VK_FORMAT_UNDEFINED;
                break;
            }
    while (at.n > 0 && !at.color[at.n - 1]) at.n--;   // (trailing empty targets)
    if (s.rt.depth) {
        Texture* tex = texture_get(s.rt.depth, false);
        if (tex && (tex->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
            const nvn_view* v = s.rt.has_dview ? &s.rt.dview : nullptr;
            VkFormat f = VK_FORMAT_UNDEFINED;
            const VkImageView view = texture_view(tex, s.rt.depth, v, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Attachment, &f);
            if (view) {
                at.depth = tex, at.depth_view = view, at.depth_format = tex->format;
                const uint32_t level = image_level(tex, view_level(v));
                at.depth_level = level, at.depth_layer = v ? view_layer(v) : 0;
                const uint32_t w = std::max(1u, tex->width >> level), h = std::max(1u, tex->height >> level);
                at.width = at.width ? std::min(at.width, w) : w;
                at.height = at.height ? std::min(at.height, h) : h;
                if (!at.n) at.samples = tex->samples;
            }
        }
    }
    targets_serial = s.rt_serial;
    cached_targets = at;
    return at;
}

/// A draw with no target at all (a fragment shader that only writes storage): the pass is the viewport's area
Targets bare_targets(const NvnState& s, Targets at) {
    if (at.n || at.depth || at.width) return at;
    at.width = (uint32_t)std::max(1.0f, s.viewport[0][0] + s.viewport[0][2]);
    at.height = (uint32_t)std::max(1.0f, s.viewport[0][1] + s.viewport[0][3]);
    at.width = std::min(at.width, 16384u), at.height = std::min(at.height, 16384u);
    return at;
}

/// (hookshot, 17/11, issue 47 and the Discord's striped ground) Whether a draw left out of these targets is drawn again in the next
/// frames: every target was drawn in the frame before (the scene's own, redrawn each frame) and the draw covers it, its viewport
/// and scissor over nine tenths of it. Not a part the game draws once into a target it keeps: a tile, a decal, a shadow of a
/// cache it updates a piece at a time. Left out, such a piece was never drawn again, and that part of the target kept what was
/// there before until the area was loaded again: rectangular patches of the wrong texels over the ground, a few minutes into a
/// place never seen in this session (Flix's route in the Fire Temple, every time after a fresh start; logs with draws left out
/// in all four reports). Those wait for their pipeline instead
bool targets_redrawn(const Targets& at, const NvnState& s) {
    const uint64_t now = texture_frame_no();
    const auto recent = [&](const Texture* t) { return !t || t->last_written + 1 >= now; };
    for (int i = 0; i < at.n; i++)
        if (!recent(at.color[i])) return false;
    if (!recent(at.depth)) return false;
    if (!at.width || !at.height) return false;
    const float w = (float)at.width * 0.9f, h = (float)at.height * 0.9f;
    if (std::fabs(s.viewport[0][2]) < w || std::fabs(s.viewport[0][3]) < h) return false;
    if (s.scissor[2] > 0 && s.scissor[3] > 0 && ((float)s.scissor[2] < w || (float)s.scissor[3] < h)) return false;
    return true;
}
/// Whether the targets of a draw kept a picture from an earlier frame (a draw left out of them shows for one frame)
bool targets_persistent(const Targets& at) {
    const uint64_t now = texture_frame_no();
    for (int i = 0; i < at.n; i++)
        if (at.color[i] && at.color[i]->first_render_frame && at.color[i]->first_render_frame < now) return true;
    return at.depth && at.depth->first_render_frame && at.depth->first_render_frame < now;
}

void begin_pass(VkCommandBuffer cmd, const Targets& at) {
    Pass& ps = pass();   // (hookshot, once: the accessor is a call and a thread-local read at each use (__tls_get_addr in the shared library). The same object for the thread's life: draw_begin_recording resets it in place)
    bool same = ps.open && ps.depth == at.depth_view && ps.n == at.n && ps.width == at.width &&
                ps.height == at.height;
    for (int i = 0; i < at.n && same; i++) same = ps.color[i] == at.color_view[i];
    if (same) return;
    end_pass(cmd);
    pass_start_barrier(cmd);   // (what the pass before wrote, and the transfers before it: vk_texture.cpp)
    work_since_barrier = true;
    VkRenderingAttachmentInfo ca[8]{};
    bool marks = !texture_marked(at.depth);
    for (int i = 0; i < at.n && !marks; i++) marks = !texture_marked(at.color[i]);
    ParLock lk(marks);
    for (int i = 0; i < at.n; i++) {
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca[i].imageView = at.color_view[i];
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ps.color[i] = at.color_view[i];
        ps.tex[i] = at.color[i], ps.level[i] = at.color_level[i], ps.layer[i] = at.color_layer[i];
        texture_written(at.color[i], "a pass's colour target");
        texture_rendered(at.color[i]);
    }
    for (int i = at.n; i < 8; i++) ps.color[i] = VK_NULL_HANDLE;
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (at.depth) {
        da.imageView = at.depth_view;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        texture_written(at.depth, "a pass's depth target");
        texture_rendered(at.depth);
    }
    ps.depth = at.depth_view;
    ps.depth_tex = at.depth, ps.depth_level = at.depth_level, ps.depth_layer = at.depth_layer;
    ps.n = at.n, ps.width = at.width, ps.height = at.height;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {at.width, at.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = (uint32_t)at.n;
    ri.pColorAttachments = ca;
    if (at.depth && depth_aspect(at.depth_format)) ri.pDepthAttachment = &da;
    if (at.depth && has_stencil(at.depth_format)) ri.pStencilAttachment = &da;
    gprof_begin(cmd);
    ps.draws = 0, ps.format0 = at.n ? at.color_format[0] : at.depth_format;
    ps.wm_or = 0, ps.wm_n = 0;
    ps.clears = 0, ps.first_prog = 0;
    ps.sampled.clear(), ps.sampled_targets = 0, ps.written = 0;
    vkCmdBeginRendering(cmd, &ri);
    ps.open = true;
    st_passes++;
}

/// The dynamic state a draw sets, compared with what the command buffer has (only what changed is set again: most
/// draws repeat their neighbours' state, and these calls were a third of the replay's command time)
struct DynState {
    VkViewport vp;
    VkRect2D sc;
    float blend[4];
    uint32_t sref[2], scmp[2], swm[2];
    float bias[3];
    VkBool32 bias_en;
    VkCullModeFlags cull;
    VkFrontFace front;
    VkPrimitiveTopology topo;
    VkBool32 restart, dt, dw, st, discard, clamp, a2c;
    VkCompareOp dop;
    VkStencilOp sops[2][3];
    VkCompareOp scmpop[2];
    VkPolygonMode poly;
    int n;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
};
thread_local DynState last_dyn;
thread_local bool last_dyn_valid = false;
#define DYN_CHANGED(f) ((!last_dyn_valid || std::memcmp(&d.f, &last_dyn.f, sizeof d.f)) && (FINE_X(FC_DYN_CALLS, 1), true))

/// The inputs a draw's dynamic state is built from. Most draws repeat the draw before them.
bool dyn_same(const float* fp, int nf, const uint32_t* up, int nu, const uint8_t* raw, int nb, const float* cfp,
              const uint32_t* cup, const uint8_t* craw) {
    if (nf <= 0 || nu <= 0 || nb <= 0) return false;
    for (int i = 0; i < nf; i++)
        if (fp[i] != cfp[i]) return false;
    for (int i = 0; i < nu; i++)
        if (up[i] != cup[i]) return false;
    for (int i = 0; i < nb; i++)
        if (raw[i] != craw[i]) return false;
    return true;
}
constexpr int kDynF = 13, kDynU = 35;
struct DynFast {
    bool on = false;
    float fp[kDynF]{};
    uint32_t up[kDynU]{};
    uint8_t raw[160]{};
    int nb = 0;
    uint32_t wm0 = 0, wm_n = 0;
};
thread_local DynFast dyn_fast;
int dyn_pack(const NvnState& s, const Targets& at, VkPrimitiveTopology topo, bool mesh, float* fp, uint32_t* up,
             uint8_t* raw) {
    fp[0] = s.viewport[0][0], fp[1] = s.viewport[0][1], fp[2] = s.viewport[0][2], fp[3] = s.viewport[0][3];
    fp[4] = s.depth_near, fp[5] = s.depth_far;
    std::memcpy(fp + 6, s.blend_color, sizeof s.blend_color);
    fp[10] = s.poly_offset[0], fp[11] = s.poly_offset[1], fp[12] = s.poly_offset[2];
    up[0] = (uint32_t)s.scissor[0], up[1] = (uint32_t)s.scissor[1], up[2] = (uint32_t)s.scissor[2], up[3] = (uint32_t)s.scissor[3];
    up[4] = s.stencil_ref[0], up[5] = s.stencil_ref[1];
    up[6] = s.stencil_value_mask[0], up[7] = s.stencil_value_mask[1];
    up[8] = s.stencil_mask[0], up[9] = s.stencil_mask[1];
    up[10] = at.width, up[11] = at.height, up[12] = (uint32_t)at.n, up[13] = (uint32_t)at.depth_format;
    up[14] = at.depth != nullptr;
    up[15] = (uint32_t)s.color.blend_en, up[16] = s.chmask.mask;
    up[17] = (uint32_t)topo, up[18] = mesh ? 1u : 0u;
    up[19] = s.restart ? 1u : 0u, up[20] = s.raster_discard ? 1u : 0u, up[21] = s.depth_clamp ? 1u : 0u, up[22] = s.ms.a2c ? 1u : 0u;
    up[23] = (uint32_t)s.poly.cull, up[24] = (uint32_t)s.poly.front, up[25] = (uint32_t)s.poly.mode, up[26] = (uint32_t)s.poly.offset_en;
    for (int i = 0; i < 8; i++) up[27 + i] = (uint32_t)at.color_format[i];
    static_assert(sizeof(s.ds) + sizeof(s.blend) + 8 <= 160);
    int atb = 0;
    std::memcpy(raw + atb, &s.ds, sizeof s.ds);
    atb += (int)sizeof s.ds;
    std::memcpy(raw + atb, s.blend, sizeof s.blend);
    atb += (int)sizeof s.blend;
    // (hookshot) what the program makes of the write masks, not the program: draws change program far more often than they change that,
    // and each program change worked the whole state out again. RX_VK_DYN_PROG_KEY=1: the program, as before)
    static const bool by_program = getenv("RX_VK_DYN_PROG_KEY") != nullptr;
    const uint64_t prog = by_program || !cur_program ? (uint64_t)(uintptr_t)cur_program : program_wm_sig(*cur_program);
    std::memcpy(raw + atb, &prog, 8);
    return atb + 8;
}

void dynamic_state(VkCommandBuffer cmd, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, bool mesh) {
    Pass& ps = pass();   // (hookshot, once: the accessor is a call and a thread-local read at each use (__tls_get_addr in the shared library). The same object for the thread's life: draw_begin_recording resets it in place)
    float df[kDynF];
    uint32_t du[kDynU];
    uint8_t db[160];
    const int nb = dyn_pack(s, at, topo, mesh, df, du, db);
    if (!dump_on() && last_dyn_valid && dyn_fast.on && dyn_fast.nb == nb &&
        dyn_same(df, kDynF, du, kDynU, db, nb, dyn_fast.fp, dyn_fast.up, dyn_fast.raw)) {
        ps.wm_or |= dyn_fast.wm0, ps.wm_n = dyn_fast.wm_n;   // (the pass trace still sees the masks)
        FINE_N(FC_DYN_FAST, 1);
        return;
    }
    FINE_N(FC_DYN_FULL, 1);
    DynState d;
    std::memset(&d, 0, sizeof d);   // (padding compared too)
    // (RX_VK_FOOTPRINT=<key>,...: those programs' draws write depth wherever they rasterize, both faces, whatever is
    // there already: with RX_VK_DUMP_EACH, each one's footprint (diagnostics))
    static const std::vector<uint64_t> footprint = [] {
        std::vector<uint64_t> v;
        for (const char* q = getenv("RX_VK_FOOTPRINT"); q && *q;) {
            char* end;
            const uint64_t k = strtoull(q, &end, 16);
            if (end == q) break;
            v.push_back(k);
            q = *end ? end + 1 : end;
        }
        return v;
    }();
    const bool fp = dump_on() && at.depth && depth_aspect(at.depth_format) && cur_program && !footprint.empty() &&
                    std::find(footprint.begin(), footprint.end(), cur_program->key) != footprint.end();
    // the viewport upside down (negative height): NVN's NDC has +y up, Vulkan's down
    float vx = s.viewport[0][0], vy = s.viewport[0][1], vw = s.viewport[0][2], vh = s.viewport[0][3];
    if (vw == 0.0f && vh == 0.0f) vx = 0, vy = 0, vw = (float)at.width, vh = (float)at.height;
    d.vp = {vx, vy + vh, vw, -vh, std::clamp(s.depth_near, 0.0f, 1.0f), std::clamp(s.depth_far, 0.0f, 1.0f)};
    d.sc = {{0, 0}, {at.width, at.height}};
    if (s.scissor[2] > 0 && s.scissor[3] > 0) {
        const int32_t x0 = std::max(0, s.scissor[0]), y0 = std::max(0, s.scissor[1]);
        const int32_t x1 = std::min<int32_t>(s.scissor[0] + s.scissor[2], (int32_t)at.width);
        const int32_t y1 = std::min<int32_t>(s.scissor[1] + s.scissor[3], (int32_t)at.height);
        d.sc = {{x0, y0}, {(uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0)}};
    }
    std::memcpy(d.blend, s.blend_color, sizeof d.blend);
    for (int f = 0; f < 2; f++)
        d.sref[f] = s.stencil_ref[f] & 0xFF, d.scmp[f] = s.stencil_value_mask[f] & 0xFF, d.swm[f] = s.stencil_mask[f] & 0xFF;
    d.bias[0] = s.poly_offset[1], d.bias[1] = s.poly_offset[2], d.bias[2] = s.poly_offset[0];
    d.bias_en = s.poly.offset_en != 0;
    {   // (hookshot) The characters' own shadow maps (square, depth only, 768 and less; 1e26755d projects them on the screen): their
        // stored depth is moved away from the light, or they shadow themselves whole: Link and the ferns black in the shade, while
        // the ground behind them keeps their shadow. RX_VK_SHADOW_BIAS=<units>,<slope>; 0: as the game asks
        // (read once, thread-safely: parallel recording threads reach here at once)
        static float units = 250000.0f, slope = 0.0f;
        static const bool bias_read = [] {
            if (const char* e = getenv("RX_VK_SHADOW_BIAS")) sscanf(e, "%f,%f", &units, &slope);
            return true;
        }();
        (void)bias_read;
        if (units > 0 && at.n == 0 && at.width == at.height && at.width <= 768 && at.depth) {
            d.bias[0] += units, d.bias[2] += slope;
            d.bias_en = true;
        }
        // (amd-fix, 11-07) the sun's shadow cascades (square, depth only, past 768): RX_VK_CASCADE_BIAS=<units>,<slope> added;
        // RX_VK_LOG_CASCADE_BIAS=1: what the game asks for them
        if (at.n == 0 && at.width == at.height && at.width > 768 && at.depth) {
            static float cu = 0, cs = 0;
            static const bool parsed = [] {
                if (const char* e = getenv("RX_VK_CASCADE_BIAS")) sscanf(e, "%f,%f", &cu, &cs);
                return true;
            }();
            (void)parsed;
            static const bool log_on = getenv("RX_VK_LOG_CASCADE_BIAS") != nullptr;
            static int logged;
            if (log_on && logged < 40) {
                logged++;
                logf("vk: cascade %ux%u bias: enabled %d units %g clamp %g slope %g", at.width, at.height, (int)d.bias_en,
                     d.bias[0], d.bias[1], d.bias[2]);
            }
            if (cu != 0 || cs != 0) d.bias[0] += cu, d.bias[2] += cs, d.bias_en = true;
        }
    }
    d.cull = sw().no_cull ? VK_CULL_MODE_NONE
           : s.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT
           : s.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT
           : s.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    // The viewport is flipped (negative height), which reverses the screen-space winding, so NVN's front face maps
    // straight across: telling Vulkan the opposite is what culled every triangle (the frame was black).
    d.front = s.poly.front == NVN_FRONT_FACE_CCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (fp) d.cull = VK_CULL_MODE_NONE;
    d.topo = mesh ? VK_PRIMITIVE_TOPOLOGY_MAX_ENUM : topo;
    d.restart = !mesh && s.restart && topo != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topo != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
                topo != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && topo != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    const bool depth = at.depth && depth_aspect(at.depth_format);
    const bool stencil = at.depth && has_stencil(at.depth_format);
    d.dt = !sw().no_depth && depth && s.ds.dt;
    d.dw = !sw().no_depth && depth && s.ds.dw;
    d.dop = compare_op(s.ds.df);
    if (fp) d.dt = true, d.dw = true, d.dop = VK_COMPARE_OP_ALWAYS;
    {   // (hookshot) A colour pass after a depth pass tests EQUAL against what another program wrote. Intel's compiler does not give two
        // programs the same position to the last bit, and the ground came out in holes: the sky through them, a white forest.
        // A small margin towards the camera instead. RX_VK_EQ_BIAS=<units>, 0: as the game asks
        static const float eq_bias = getenv("RX_VK_EQ_BIAS") ? (float)atof(getenv("RX_VK_EQ_BIAS")) : dev.props.vendorID == 0x8086 ? 64.0f : 0.0f;
        if (eq_bias > 0 && d.dt && !d.dw && d.dop == VK_COMPARE_OP_EQUAL) {
            d.dop = VK_COMPARE_OP_LESS_OR_EQUAL;
            d.bias_en = VK_TRUE;
            d.bias[0] = -eq_bias, d.bias[1] = 0.0f, d.bias[2] = 0.0f;
        }
    }
    d.st = stencil && s.ds.st;
    d.sops[0][0] = stencil_op(s.ds.fail_f), d.sops[0][1] = stencil_op(s.ds.zpass_f), d.sops[0][2] = stencil_op(s.ds.zfail_f);
    d.sops[1][0] = stencil_op(s.ds.fail_b), d.sops[1][1] = stencil_op(s.ds.zpass_b), d.sops[1][2] = stencil_op(s.ds.zfail_b);
    d.scmpop[0] = compare_op(s.ds.sf_f), d.scmpop[1] = compare_op(s.ds.sf_b);
    d.discard = s.raster_discard && !sw().ignore_discard;
    {   // (diagnosis, default off) RX_VK_DISCARD_PROG=<key start>[,...]: those programs' draws run their geometry stages only (rasterizer
        // discard), so with RX_VK_PASS_TIMING=2 the run's time minus the normal run's is the pixel work: the picture is wrong
        static const std::vector<std::string> keys = [] {
            std::vector<std::string> v;
            for (const char* q = getenv("RX_VK_DISCARD_PROG"); q && *q;) {
                const char* e = q;
                while (*e && *e != ',') e++;
                v.emplace_back(q, e);
                q = *e ? e + 1 : e;
            }
            return v;
        }();
        if (!keys.empty() && cur_program) {
            char k[20];
            snprintf(k, sizeof k, "%016llx", (unsigned long long)cur_program->key);
            for (const std::string& w : keys)
                if (!w.empty() && strncmp(k, w.c_str(), w.size()) == 0) d.discard = VK_TRUE;
        }
    }
    d.poly = s.poly.mode == 0 ? VK_POLYGON_MODE_POINT : s.poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    static const bool fp_lines = getenv("RX_VK_FOOTPRINT_LINES") != nullptr;   // (the footprints as wireframes)
    if (fp && fp_lines) d.poly = VK_POLYGON_MODE_LINE;
    d.clamp = s.depth_clamp, d.a2c = s.ms.a2c != 0 && !no_a2c();
    d.n = dev.eds3_blend ? at.n : 0;
    for (int i = 0; i < d.n; i++) {
        const nvn_blend_st& b = s.blend[i];
        d.en[i] = (s.color.blend_en >> i & 1) && at.color_format[i] ? VK_TRUE : VK_FALSE;
        d.eq[i] = {blend_factor(b.src_rgb), blend_factor(b.dst_rgb), blend_op(b.eq_rgb), blend_factor(b.src_a),
                   blend_factor(b.dst_a), blend_op(b.eq_a)};
        d.wm[i] = target_write_mask(*cur_program, s, i);
    }
    if (dump_on()) {   // (RX_VK_DUMP: the state a program draws with, once per change)
        char line[400];
        int k = snprintf(line, sizeof line, "prog %016llx n%d depth %s dt%u dw%u dop%d st%u cull%u front%u vp %.0f,%.0f %.0fx%.0f z %.2f-%.2f sc %d,%d %ux%u:",
                         (unsigned long long)(cur_program ? cur_program->key : 0), at.n,
                         at.depth ? (depth ? "yes" : "stencil") : "no", d.dt, d.dw, (int)d.dop, d.st, d.cull, d.front,
                         d.vp.x, d.vp.y, d.vp.width, d.vp.height, d.vp.minDepth, d.vp.maxDepth, d.sc.offset.x, d.sc.offset.y,
                         d.sc.extent.width, d.sc.extent.height);
        for (int i = 0; i < at.n && k < (int)sizeof line - 40; i++)
            k += snprintf(line + k, sizeof line - k, " c%d:%s wm%x en%u %d/%d/%d", i, at.color[i] ? "" : "none", d.wm[i],
                          d.en[i], (int)d.eq[i].srcColorBlendFactor, (int)d.eq[i].dstColorBlendFactor,
                          (int)d.eq[i].colorBlendOp);
        static std::string last;
        if (last != line) last = line, logf("vk: dump-st %03d %s", dump_seq(), line);
    }
    if (DYN_CHANGED(vp)) vkCmdSetViewportWithCount(cmd, 1, &d.vp);
    if (DYN_CHANGED(sc)) vkCmdSetScissorWithCount(cmd, 1, &d.sc);
    if (DYN_CHANGED(blend)) vkCmdSetBlendConstants(cmd, d.blend);
    if (DYN_CHANGED(sref)) {
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, d.sref[0]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, d.sref[1]);
    }
    if (DYN_CHANGED(scmp)) {
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, d.scmp[0]);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, d.scmp[1]);
    }
    if (DYN_CHANGED(swm)) {
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, d.swm[0]);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, d.swm[1]);
    }
    if (DYN_CHANGED(bias)) vkCmdSetDepthBias(cmd, d.bias[0], d.bias[1], d.bias[2]);
    if (DYN_CHANGED(bias_en)) vkCmdSetDepthBiasEnable(cmd, d.bias_en);
    if (!last_dyn_valid) vkCmdSetLineWidth(cmd, 1.0f), vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
    if (DYN_CHANGED(cull)) vkCmdSetCullMode(cmd, d.cull);
    if (DYN_CHANGED(front)) vkCmdSetFrontFace(cmd, d.front);
    if (!mesh) {
        if (DYN_CHANGED(topo)) vkCmdSetPrimitiveTopology(cmd, d.topo);
        if (DYN_CHANGED(restart) || last_dyn.topo == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM) vkCmdSetPrimitiveRestartEnable(cmd, d.restart);
    }
    if (DYN_CHANGED(dt)) vkCmdSetDepthTestEnable(cmd, d.dt);
    if (DYN_CHANGED(dw)) vkCmdSetDepthWriteEnable(cmd, d.dw);
    if (DYN_CHANGED(dop)) vkCmdSetDepthCompareOp(cmd, d.dop);
    if (DYN_CHANGED(st)) vkCmdSetStencilTestEnable(cmd, d.st);
    if (DYN_CHANGED(sops) || DYN_CHANGED(scmpop)) {
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, d.sops[0][0], d.sops[0][1], d.sops[0][2], d.scmpop[0]);
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, d.sops[1][0], d.sops[1][1], d.sops[1][2], d.scmpop[1]);
    }
    if (DYN_CHANGED(discard)) vkCmdSetRasterizerDiscardEnable(cmd, d.discard);
    if (dev.eds3_polygon && DYN_CHANGED(poly)) vkCmdSetPolygonModeEXT(cmd, d.poly);
    if (dev.eds3_clamp && DYN_CHANGED(clamp)) vkCmdSetDepthClampEnableEXT(cmd, d.clamp);
    if (dev.eds3_a2c && DYN_CHANGED(a2c)) vkCmdSetAlphaToCoverageEnableEXT(cmd, d.a2c);
    if (d.n) {
        const bool same_n = last_dyn_valid && last_dyn.n == d.n;
        if (!same_n || std::memcmp(d.en, last_dyn.en, sizeof(VkBool32) * d.n))
            FINE_X(FC_DYN_CALLS, 1), vkCmdSetColorBlendEnableEXT(cmd, 0, (uint32_t)d.n, d.en);
        if (!same_n || std::memcmp(d.eq, last_dyn.eq, sizeof(VkColorBlendEquationEXT) * d.n))
            FINE_X(FC_DYN_CALLS, 1), vkCmdSetColorBlendEquationEXT(cmd, 0, (uint32_t)d.n, d.eq);
        if (!same_n || std::memcmp(d.wm, last_dyn.wm, sizeof(VkColorComponentFlags) * d.n))
            FINE_X(FC_DYN_CALLS, 1), vkCmdSetColorWriteMaskEXT(cmd, 0, (uint32_t)d.n, d.wm);
    }
    last_dyn = d;
    last_dyn_valid = true;
    dyn_fast.on = true, dyn_fast.nb = nb;
    std::memcpy(dyn_fast.fp, df, sizeof df);
    std::memcpy(dyn_fast.up, du, sizeof du);
    std::memcpy(dyn_fast.raw, db, (size_t)nb);
    dyn_fast.wm0 = d.n ? (uint32_t)d.wm[0] : 0, dyn_fast.wm_n = (uint32_t)d.n;
    ps.wm_or |= dyn_fast.wm0, ps.wm_n = dyn_fast.wm_n;
}
#undef DYN_CHANGED

/// Without robustness2's nullDescriptor a descriptor's image view must not be VK_NULL_HANDLE
/// (VUID-VkWriteDescriptorSet-descriptorType-02997): 1x1 black images (1D, 2D/cube, 3D) and a view of each type, made once
/// in the first recording (rec_cmd). Images that could not all be made are given back: the views stay null, as before
void make_dummies() {
    static VkImage img[3]{};
    static VmaAllocation mem[3]{};
    if (img[0] || dev.null_descriptor || !rec_cmd) return;
    const VkImageType type[3] = {VK_IMAGE_TYPE_1D, VK_IMAGE_TYPE_2D, VK_IMAGE_TYPE_3D};
    const uint32_t layers[3] = {1, 6, 1};
    for (int i = 0; i < 3; i++) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.flags = i == 1 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = type[i];
        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent = {1, 1, 1};
        ci.mipLevels = 1, ci.arrayLayers = layers[i];
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!create_image(ci, img[i], mem[i])) {
            for (int j = 0; j <= i; j++) {
                if (img[j]) destroy_image(img[j], mem[j]);
                img[j] = VK_NULL_HANDLE, mem[j] = nullptr;
            }
            static int logged;
            if (logged++ < 4) logf("vk: the dummy images (no nullDescriptor): not made");
            return;
        }
    }
    VkImageMemoryBarrier2 b[3]{};
    for (int i = 0; i < 3; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE, b[i].dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = img[i];
        b[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers[i]};
    }
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 3, d.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(rec_cmd, &d);
    const VkClearColorValue black{};
    for (int i = 0; i < 3; i++) {
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers[i]};
        vkCmdClearColorImage(rec_cmd, img[i], VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
    }
    for (int i = 0; i < 3; i++) {
        b[i].srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT, b[i].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b[i].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
        b[i].oldLayout = b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    vkCmdPipelineBarrier2(rec_cmd, &d);
    // (VkImageViewType order: 1D, 2D, 3D, CUBE, 1D_ARRAY, 2D_ARRAY, CUBE_ARRAY)
    const int src[7] = {0, 1, 2, 1, 0, 1, 1};
    const uint32_t view_layers[7] = {1, 1, 1, 6, 1, 6, 6};
    for (int v = 0; v < 7; v++) {
        if (v == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !dev.features.imageCubeArray) continue;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img[src[v]];
        vi.viewType = (VkImageViewType)v;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, view_layers[v]};
        if (vkCreateImageView(dev.device, &vi, nullptr, &dummies.view[v]) != VK_SUCCESS) dummies.view[v] = VK_NULL_HANDLE;
    }
}


// ---- clears: between the draws of a pass, cut by scissor 0 (as deko3d programs every clear), a channel mask with
// a full-screen triangle (vkCmdClearAttachments has no write mask; TOTK's maxwell.md 1.3) ----
VkRect2D clear_rect(const NvnState& s, uint32_t w, uint32_t h) {
    VkRect2D r{{0, 0}, {w, h}};
    if (s.scissor[2] > 0 && s.scissor[3] > 0) {
        const int32_t x0 = std::clamp(s.scissor[0], 0, (int32_t)w), y0 = std::clamp(s.scissor[1], 0, (int32_t)h);
        const int32_t x1 = std::clamp(s.scissor[0] + s.scissor[2], 0, (int32_t)w);
        const int32_t y1 = std::clamp(s.scissor[1] + s.scissor[3], 0, (int32_t)h);
        r = {{x0, y0}, {(uint32_t)std::max(0, x1 - x0), (uint32_t)std::max(0, y1 - y0)}};
    }
    return r;
}

/// The SPIR-V of a module with its Location 0 output moved to location (masked clears of target n)
std::vector<uint32_t> spirv_location(const uint32_t* w, size_t n, uint32_t location) {
    std::vector<uint32_t> out(w, w + n);
    for (size_t i = 5; i < out.size();) {
        const uint32_t op = out[i] & 0xFFFF, len = out[i] >> 16;
        if (!len) break;
        if (op == 71 && len >= 4 && out[i + 2] == 30) out[i + 3] = location;   // OpDecorate %x Location
        i += len;
    }
    return out;
}

std::unordered_map<uint64_t, VkPipeline> clear_pipes;
VkPipelineLayout clear_layout = VK_NULL_HANDLE;

VkPipeline clear_pipeline(const Targets& at, int index, uint32_t mask, int kind) {
    uint64_t key = mix((uint64_t)index << 8 | mask << 4 | (uint64_t)kind, (uint64_t)at.samples);
    for (int i = 0; i < at.n; i++) key = mix(key, (uint64_t)at.color_format[i]);
    key = mix(key, (uint64_t)at.depth_format);
    auto it = clear_pipes.find(key);
    if (it != clear_pipes.end()) return it->second;
    if (!clear_layout) {
        const VkPushConstantRange r{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &r;
        vkCreatePipelineLayout(dev.device, &li, nullptr, &clear_layout);
    }
    const uint32_t* fw = kind == 1 ? CLEAR_U_FRAG_SPV : kind == 2 ? CLEAR_I_FRAG_SPV : CLEAR_F_FRAG_SPV;
    const size_t fn = (kind == 1 ? sizeof CLEAR_U_FRAG_SPV : kind == 2 ? sizeof CLEAR_I_FRAG_SPV : sizeof CLEAR_F_FRAG_SPV) / 4;
    const std::vector<uint32_t> frag = spirv_location(fw, fn, (uint32_t)index);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule vm = VK_NULL_HANDLE, fm = VK_NULL_HANDLE;
    IntelDriverGate gate;   // (the modules and the pipeline: one thread in Intel's compiler on Windows, a fault leaves it out)
    mi.codeSize = sizeof CLEAR_VERT_SPV, mi.pCode = CLEAR_VERT_SPV;
    if (guarded_shader_module(&mi, &vm) != VK_SUCCESS) vm = VK_NULL_HANDLE;
    mi.codeSize = frag.size() * 4, mi.pCode = frag.data();
    if (guarded_shader_module(&mi, &fm) != VK_SUCCESS) fm = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT, st[0].module = vm, st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, st[1].module = fm, st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = at.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb[8]{};
    for (int i = 0; i < at.n; i++) cb[i].colorWriteMask = i == index ? mask : 0;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = (uint32_t)at.n, bs.pAttachments = cb;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = (uint32_t)at.n;
    ri.pColorAttachmentFormats = at.color_format;
    if (at.depth) {
        if (depth_aspect(at.depth_format)) ri.depthAttachmentFormat = at.depth_format;
        if (has_stencil(at.depth_format)) ri.stencilAttachmentFormat = at.depth_format;
    }
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &ri;
    ci.stageCount = 2, ci.pStages = st;
    ci.pVertexInputState = &vi, ci.pInputAssemblyState = &ia, ci.pViewportState = &vp, ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms, ci.pDepthStencilState = &ds, ci.pColorBlendState = &bs, ci.pDynamicState = &dy;
    ci.layout = clear_layout;
    VkPipeline pl = VK_NULL_HANDLE;
    if (vm && fm && guarded_graphics_held(pcache, &ci, &pl) != VK_SUCCESS) pl = VK_NULL_HANDLE;
    vkDestroyShaderModule(dev.device, vm, nullptr);
    vkDestroyShaderModule(dev.device, fm, nullptr);
    clear_pipes[key] = pl;
    return pl;
}

int format_kind(VkFormat f) {   // 0 float, 1 unsigned integer, 2 signed integer
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32: return 1;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32A32_SINT: return 2;
    default: return 0;
    }
}

int64_t st_clears, st_masked_clears;

/// A clear after draws of the open pass: in a pass of its own, after a full barrier (begin_pass makes it). In the pass
/// the draws before it may still read what it clears (a target they sample: the depth the fog and the lighting read):
/// vkCmdClearAttachments is ordered with the attachments' own writes only, and the GPU cleared tiles while a draw
/// before it still sampled them: a band of the picture, below or above a line that moved, for a frame (the fog's flat
/// colour over a Grand Prix intro, the shadows' dark band in its race). RX_VK_CLEAR_IN_PASS=1: in the pass, as before.
void clear_after_draws(VkCommandBuffer cmd) {
    static const bool in_pass = getenv("RX_VK_CLEAR_IN_PASS") != nullptr;
    if (in_pass || !pass().open || pass().draws == 0) return;
    static int logged;
    if (logged++ < 6)
        logf("vk: a clear after %d draws of its pass (%ux%u, %d colour targets%s, last program %016llx): a pass of its own",
             pass().draws, pass().width, pass().height, pass().n, pass().depth ? " + depth" : "", (unsigned long long)pass().prog);
    end_pass(cmd);
}

bool writes_sampled_target(uint16_t writes, uint16_t sampled_targets) {
    return (writes & sampled_targets) != 0;
}
/// A draw that samples a target this pass already wrote, or writes a target an earlier draw of this pass sampled: in
/// a pass of its own, after a full barrier (begin_pass makes it), as for the clears above: in one pass the GPU orders
/// the targets' writes with each other only, not with the draws' texture reads of them. (A draw that samples a target
/// it writes itself is left as it is.) Returns the targets the draw writes. RX_VK_NO_PASS_HAZARDS=1: not checked.
uint16_t draw_hazards(VkCommandBuffer cmd, const Program& p, const NvnState& s, const Targets& at) {
    Pass& ps = pass();   // (hookshot, once: the accessor is a call and a thread-local read at each use (__tls_get_addr in the shared library). The same object for the thread's life: draw_begin_recording resets it in place)
    uint16_t writes = 0;
    for (int i = 0; i < at.n; i++)
        if (at.color[i] && target_write_mask(p, s, i)) writes |= (uint16_t)(1u << i);
    if (at.depth && s.ds.dw) writes |= 1u << 8;
    {   // (hookshot: a draw that samples a target it writes itself reads it undefined in Vulkan (a feedback loop): named, a few times)
        static std::atomic<int> self_logged{0};
        if (self_logged.load(std::memory_order_relaxed) < 12)
            for (const Texture* t : gfx_sampled()) {
                int k = at.depth == t ? 8 : -1;
                for (int i = 0; i < at.n && k < 0; i++)
                    if (at.color[i] == t) k = i;
                if (k < 0 || !(writes >> k & 1)) continue;
                if (self_logged.fetch_add(1, std::memory_order_relaxed) < 12)
                    logf("vk: a draw (program %016llx) samples the %s it writes (%ux%u, %s)", (unsigned long long)p.key,
                         k == 8 ? "depth target" : "colour target", at.width, at.height,
                         k == 8 ? "read undefined while depth is written" : "read undefined where it writes");
                break;
            }
    }
    static const bool off = getenv("RX_VK_NO_PASS_HAZARDS") != nullptr;
    if (off || !ps.open) return writes;
    bool same = ps.depth == at.depth_view && ps.n == at.n && ps.width == at.width && ps.height == at.height;
    for (int i = 0; i < at.n && same; i++) same = ps.color[i] == at.color_view[i];
    if (!same) return writes;   // (other targets: begin_pass ends this pass with a barrier anyway)
    auto slot_of = [&](const Texture* t) {
        for (int i = 0; i < at.n; i++)
            if (at.color[i] == t) return i;
        return at.depth == t ? 8 : -1;
    };
    const char* kind = nullptr;
    for (const Texture* t : gfx_sampled()) {
        const int k = slot_of(t);
        if (k >= 0 && (ps.written >> k & 1) && !(writes >> k & 1)) { kind = "samples a target the pass wrote"; break; }
    }
    if (!kind && writes_sampled_target(writes, ps.sampled_targets))
        kind = "writes a target an earlier draw sampled";
    if (!kind) return writes;
    static int logged;
    if (logged++ < 8)
        logf("vk: a draw (program %016llx) %s, after %d draws of the pass (%ux%u): a pass of its own", (unsigned long long)p.key,
             kind, ps.draws, ps.width, ps.height);
    end_pass(cmd);
    return writes;
}

} // namespace

void clear_color_target(VkCommandBuffer cmd, NvnState& s, int index, const float rgba[4], uint32_t mask) {
    ParLock lk;
    mask &= 0xF;
    if (!mask) return;
    const Targets at = targets(s);
    if (index < 0 || index >= at.n || !at.color_view[index]) return;
    clear_after_draws(cmd);
    begin_pass(cmd, at);
    const uint32_t lw = at.width, lh = at.height;
    const VkRect2D rect = clear_rect(s, lw, lh);
    if (!rect.extent.width || !rect.extent.height) return;
    st_clears++, pass().clears++;
    if (mask == 0xF) {
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = (uint32_t)index;
        std::memcpy(&ca.clearValue.color, rgba, 16);
        const VkClearRect cr{rect, 0, 1};
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
        pass().written |= (uint16_t)(1u << index);
        return;
    }
    pass().written |= (uint16_t)(1u << index);
    pcache_init();
    const int kind = format_kind(at.color_format[index]);
    const VkPipeline pl = clear_pipeline(at, index, mask, kind);
    if (!pl) return;
    st_masked_clears++;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    last_dyn_valid = false, last_desc_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;
    vertex_set.on = false;   // (the next draw binds its vertex streams again, as last_vb_valid above asks)
    descriptors_disturbed();   // (the next draw pushes all its descriptors, not only those that changed)
    const VkViewport v{0, 0, (float)lw, (float)lh, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &rect);
    vkCmdPushConstants(cmd, clear_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, rgba);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void clear_depth_stencil(VkCommandBuffer cmd, NvnState& s, float depth, bool depth_mask, uint32_t stencil,
                         uint32_t stencil_mask) {
    ParLock lk;
    const Targets at = targets(s);
    if (!at.depth) return;
    VkImageAspectFlags aspect = 0;
    if (depth_mask && depth_aspect(at.depth_format)) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (stencil_mask && has_stencil(at.depth_format)) aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspect) return;
    if (stencil_mask && (stencil_mask & 0xFF) != 0xFF) {
        static int logged;
        if (logged++ < 4) logf("vk: stencil clear with write mask %#x: cleared whole", stencil_mask);
    }
    clear_after_draws(cmd);
    begin_pass(cmd, at);
    const VkRect2D rect = clear_rect(s, at.width, at.height);
    if (!rect.extent.width || !rect.extent.height) return;
    VkClearAttachment ca{};
    ca.aspectMask = aspect;
    ca.clearValue.depthStencil = {std::clamp(depth, 0.0f, 1.0f), stencil & 0xFF};
    const VkClearRect cr{rect, 0, 1};
    vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    pass().written |= 1u << 8;
    st_clears++, pass().clears++;
}

void clear_stats(int64_t* clears, int64_t* masked) {
    *clears = st_clears, *masked = st_masked_clears;
}

namespace {
} // namespace

void draw_begin_recording(int slot, VkCommandBuffer cmd, uint64_t value) {
    rec_cmd = cmd;
    rec_value = value;
    ParLock lk;
    if ((size_t)slot >= res.size()) res.resize((size_t)slot + 1);
    RecordingRes& r = res[slot];
    if (!r.pool) {
        r.pool = make_pool();
        void* mapped = nullptr;
        RecordingRes::Stream s{};
        s.size = kStream;
        MemKindScope mem_kind_(MemKind::Stream);   // (TICK-038: RX_VK_RES_LOG)
        create_buffer(s.size, kStreamUse, true, s.buffer, s.alloc, &mapped, true);
        s.mapped = (uint8_t*)mapped;
        static int logged;   // (TICK-019: the uniform data, inline data and indices the GPU reads from these)
        if (logged++ < 1) log_alloc_where("a recording stream", s.alloc);
        r.streams.push_back(s);
    } else {
        vkResetDescriptorPool(dev.device, r.pool, 0);
        for (VkDescriptorPool p : r.extra) vkDestroyDescriptorPool(dev.device, p, nullptr);
        r.extra.clear();
        // (the streams the last recording of the slot grew into: given back after 120 recordings that stayed in the first, the
        // slot's last one being done (recording_wait). RX_VK_STREAM_KEEP=1: kept)
        static const bool keep = getenv("RX_VK_STREAM_KEEP") != nullptr;
        if (r.streams.size() > 1 && !keep) {
            if (r.si > 0) r.idle = 0;
            else if (++r.idle >= 120) {
                for (size_t i = 1; i < r.streams.size(); i++) {
                    stream_extra -= r.streams[i].size;
                    destroy_buffer(r.streams[i].buffer, r.streams[i].alloc);
                }
                r.streams.resize(1);
            }
        }
    }
    {
        const RecordingRes::Stream& s = r.streams[0];
        r.si = 0, r.stream = s.buffer, r.stream_alloc = s.alloc, r.mapped = s.mapped, r.size = s.size;
    }
    if (r.before + r.used > stream_peak.load(std::memory_order_relaxed)) stream_peak.store(r.before + r.used, std::memory_order_relaxed);
    static int64_t begun;
    static std::atomic<size_t> c1_peak{0};   // (hookshot: the most c1 stages a recording uploaded: c1_at's size before its reset)
    if (r.c1_at.size() > c1_peak.load(std::memory_order_relaxed)) c1_peak.store(r.c1_at.size(), std::memory_order_relaxed);
    if (++begun % 3000 == 0)
        logf("vk: the recording streams: %zu of %llu MB, the most one held %llu KB; %llu made on demand (%llu MB kept now); "
             "c1 stages: at most %zu a recording", res.size(),
             (unsigned long long)(kStream >> 20), (unsigned long long)(stream_peak.load(std::memory_order_relaxed) >> 10),
             (unsigned long long)stream_grown.load(), (unsigned long long)(stream_extra.load() >> 20),
             c1_peak.load(std::memory_order_relaxed));
    r.used = 0, r.before = 0;
    r.c1_at.reset();   // (hookshot: its array kept: emptied at every recording's start, it fills again to about the same size)
    r.ub_snap.clear();
    cur = &r;
    pass() = Pass{};
    work_since_barrier = false;
    last_dyn_valid = false, last_desc_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;
    vertex_set.on = false, last_ib.on = false;
    descriptors_disturbed();
    static thread_local uint64_t hits_value = ~0ull;   // (resolved targets last a submission, through its parts: no CPU write is
    if (value != hits_value) {            // collected between them; resolved handles last while their slot and
        hits_value = value;               // texture do, and are touched again once a submission)
        targets_serial = ~0ull;
        if (handle_hits().size() > 65536) handle_hits().clear();
        if (prog_slots().size() > 8192) prog_slots().clear();
    }
    for (auto& row : bank_cache())
        for (BankCache& c : row) c = BankCache{}, c.at = UINT64_MAX;
    if (!zero_buffer) {
        void* mapped = nullptr;
        create_buffer(65536, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, zero_buffer, zero_alloc, &mapped);
        if (mapped) std::memset(mapped, 0, 65536);
        make_dummies();
    }
}

// ---- RX_VK_PASS_TRACE=1: the passes and dispatches of the last frames in stream order, logged around a flash (the
// one-frame glitches: what the frame that showed one did differently from its neighbours) ----
bool pass_trace_on() {
    static const bool v = getenv("RX_VK_PASS_TRACE") != nullptr;
    return v;
}
std::mutex trace_mu;
std::map<uint64_t, std::vector<std::pair<uint64_t, std::string>>> pass_traces;   // texture frame -> (order, line)
thread_local uint64_t trace_seq = 0;
void trace_line(const char* fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    const uint64_t key = (texture_segment() << 24) | (trace_seq++ & 0xFFFFFF);   // (segments: on several threads)
    std::lock_guard lk(trace_mu);
    auto& v = pass_traces[texture_frame_no()];
    if (v.size() < 6000) v.emplace_back(key, line);
    while (pass_traces.size() > 8) pass_traces.erase(pass_traces.begin());
}

// (hookshot) RX_VK_PROBE=1 (replay is serial then): the centre texel of every colour target after each pass and of
// every written image after each dispatch, kept for the last 16 frames. At a flash (RX_VK_FLASH) the entries of that
// frame that differ from both neighbours' are logged in order: the first is the pass that went wrong.
namespace {
constexpr uint32_t kProbeFrames = 16, kProbePerFrame = 1024;
struct ProbeEntry { uint64_t prog, gpu; uint32_t fmt, w, h; char kind; int draws; };
VkBuffer probe_buf = VK_NULL_HANDLE;
VmaAllocation probe_mem = nullptr;
void* probe_map = nullptr;
ProbeEntry probe_meta[kProbeFrames][kProbePerFrame];
uint32_t probe_n[kProbeFrames];
uint64_t probe_frame[kProbeFrames];
} // namespace
bool probe_on() {
    static const bool on = getenv("RX_VK_PROBE") != nullptr;
    return on;
}
// (RX_VK_PROBE=2: at speed: replay stays parallel, big images only, the picture's chain; the frame log's white frames dump
// the last 16 frames, probe_all_<n>.txt)
bool probe_lite() {
    static const bool on = getenv("RX_VK_PROBE") && atoi(getenv("RX_VK_PROBE")) == 2;
    return on;
}
bool probe_serial() { return probe_on() && !probe_lite(); }
std::mutex probe_mu;
void probe_image(VkCommandBuffer cmd, Texture* tex, uint32_t level, uint32_t layer, uint64_t prog, char kind, int draws = -1) {
    if (!probe_on() || !tex || !tex->image || tex->samples != VK_SAMPLE_COUNT_1_BIT || tex->astc || tex->bc ||
        !(tex->aspect & VK_IMAGE_ASPECT_COLOR_BIT))
        return;
    {   // (RX_VK_PROBE_MIN=<pixels>: smaller images too, the ambient cubemap's for one)
        static const uint32_t min_w = getenv("RX_VK_PROBE_MIN") ? (uint32_t)atoi(getenv("RX_VK_PROBE_MIN")) : 384;
        if (probe_lite() && (tex->width >> std::min(level, tex->levels - 1)) < min_w) return;
    }
    std::unique_lock lite_lk(probe_mu, std::defer_lock);
    if (probe_lite()) lite_lk.lock();
    if (!probe_buf && !create_buffer(uint64_t{kProbeFrames} * kProbePerFrame * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                     probe_buf, probe_mem, &probe_map))
        return;
    const uint64_t f = texture_frame_no();
    const uint32_t slot = (uint32_t)(f % kProbeFrames);
    if (probe_frame[slot] != f) probe_frame[slot] = f, probe_n[slot] = 0;
    if (probe_n[slot] >= kProbePerFrame) return;
    const uint32_t i = probe_n[slot]++;
    level = std::min(level, tex->levels - 1);
    const uint32_t w = std::max(1u, tex->width >> level), h = std::max(1u, tex->height >> level);
    probe_meta[slot][i] = {prog, tex->gpu, (uint32_t)tex->format, w, h, kind, draws};
    std::memset((uint8_t*)probe_map + (uint64_t{slot} * kProbePerFrame + i) * 16, 0, 16);
    full_barrier(cmd);
    const VkBufferImageCopy c{(uint64_t{slot} * kProbePerFrame + i) * 16, 0, 0,
                              {VK_IMAGE_ASPECT_COLOR_BIT, level, std::min(layer, tex->layers - 1), 1},
                              {(int32_t)(w / 2), (int32_t)(h / 2), 0}, {1, 1, 1}};
    vkCmdCopyImageToBuffer(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, probe_buf, 1, &c);
    full_barrier(cmd);
}
// (the uniform buffers of each pass's first draw: non-finite floats, a global constant gone wrong)
std::vector<std::string> probe_ubos[kProbeFrames];
uint64_t probe_ubo_frame[kProbeFrames];
void probe_draw(const NvnState& s, const Program& p, int pass_draws) {
    if (!probe_on() || pass_draws != 0) return;
    if (probe_lite()) {   // (RX_VK_PROBE_UBO=<key prefix,...>: at speed, those programs' constants only)
        static const char* want = getenv("RX_VK_PROBE_UBO");
        if (!want) return;
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)p.key);
        bool hit = false;
        for (const char* q = want; *q && !hit;) {
            const char* e = q;
            while (*e && *e != ',') e++;
            hit = e > q && !strncmp(hex, q, (size_t)(e - q));
            q = *e ? e + 1 : e;
        }
        if (!hit) return;
    }
    std::lock_guard ubo_lk(probe_mu);
    const uint64_t f = texture_frame_no();
    const uint32_t slot = (uint32_t)(f % kProbeFrames);
    if (probe_ubo_frame[slot] != f) probe_ubo_frame[slot] = f, probe_ubos[slot].clear();
    for (const Stage& st : p.stages) {
        const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        for (int i = 0; i < 16; i++) {
            const nvn_p_bind_buf& u = s.ubo[ns][i];
            if (!u.addr || !(i + 3 < Shader::Info::MAX_CBUFS && st.info.constant_buffer_used_sizes[i + 3])) continue;
            static thread_local std::vector<uint8_t> b;
            const uint64_t k = std::min<uint64_t>(u.size ? u.size : 4096, 16384);
            b.resize(k);
            if (!memory_read_as_submitted(u.addr, b.data(), k)) continue;
            int nan = 0, inf = 0, zero = 0, first_bad = -1;
            float big = 0;
            for (uint64_t j = 0; j + 4 <= k; j += 4) {
                uint32_t w;
                std::memcpy(&w, b.data() + j, 4);
                const uint32_t e = w & 0x7f800000u;
                if (e == 0x7f800000u) {
                    ((w & 0x7fffffu) ? nan : inf)++;
                    if (first_bad < 0) first_bad = (int)j;
                }
                else if (e >= 0x5f000000u) big++;
                zero += w == 0;
            }
            char line[200];
            snprintf(line, sizeof line, "U %llu %016llx s%d c%d %#llx +%#llx nan %d inf %d huge %.0f zero %d first %#x", (unsigned long long)f,
                     (unsigned long long)p.key, ns, i + 3, (unsigned long long)u.addr, (unsigned long long)k, nan, inf, big, zero, first_bad);
            probe_ubos[slot].push_back(line);
            static const bool hex = getenv("RX_VK_PROBE_UBO_HEX") != nullptr;   // (the words themselves: which constant went wrong)
            if (hex) {
                std::string h = "H " + std::to_string(f) + " " + std::string(line + 2 + std::to_string(f).size() + 1, 16) + " c" + std::to_string(i + 3) + " ";
                char w8[12];
                for (uint64_t j = 0; j + 4 <= k; j += 4) {
                    uint32_t w;
                    std::memcpy(&w, b.data() + j, 4);
                    snprintf(w8, sizeof w8, "%08x", w);
                    h += w8;
                }
                probe_ubos[slot].push_back(std::move(h));
            }
        }
        // (the storage buffers too: where each is, how long, and what guest memory holds of it)
        for (const auto& d : st.info.storage_buffers_descriptors) {
            for (uint32_t k = 0; k < d.count && k < 8; k++) {
                const uint32_t off = d.cbuf_offset + k * 16;
                // (amd-fix) through the stage's banks as its program reads them: a storage buffer named in c1 (the program's
                // own constants) read 0 from the driver banks and was bound to nothing
                const uint64_t addr = stage_cbuf_word(s, st, d.cbuf_index, off) |
                                      (uint64_t)stage_cbuf_word(s, st, d.cbuf_index, off + 4) << 32;
                const uint64_t size = stage_cbuf_word(s, st, d.cbuf_index, off + 8);
                static thread_local std::vector<uint8_t> sb;
                const uint64_t n = std::min<uint64_t>(size, 4ull << 20);
                sb.resize((size_t)n);
                uint64_t hash = 0xcbf29ce484222325ull, nz = 0;
                const bool read = addr && n && memory_read_as_submitted(addr, sb.data(), n);
                if (read)
                    for (uint64_t j = 0; j < n; j++) nz += sb[j] != 0, hash = (hash ^ sb[j]) * 0x100000001b3ull;
                char line[200];
                snprintf(line, sizeof line, "S %llu %016llx s%d c%u+%#x%s addr %#llx size %#llx read %d nz %llu hash %016llx", (unsigned long long)f,
                         (unsigned long long)p.key, ns, (unsigned)d.cbuf_index, off, d.is_written ? "w" : "r", (unsigned long long)addr,
                         (unsigned long long)size, (int)read, (unsigned long long)nz, (unsigned long long)hash);
                probe_ubos[slot].push_back(line);
            }
        }
    }
}
void probe_dump_all(const char* why) {
    if (!probe_lite() || !probe_map) return;
    static int left = 8, n = 0;
    if (left <= 0) return;
    left--;
    char path[64];
    snprintf(path, sizeof path, "probe_all_%d.txt", n++);
    logf("vk: %s: %s, frame %llu", why, path, (unsigned long long)texture_frame_no());
    std::lock_guard lk(probe_mu);
    if (FILE* o = fopen(path, "w")) {
        const uint64_t now = texture_frame_no();
        for (uint64_t f = now >= 15 ? now - 15 : 0; f <= now; f++) {
            const uint32_t slot = (uint32_t)(f % kProbeFrames);
            if (probe_frame[slot] != f) continue;
            for (uint32_t i = 0; i < probe_n[slot]; i++) {
                const ProbeEntry& e = probe_meta[slot][i];
                uint32_t w[4];
                std::memcpy(w, (uint8_t*)probe_map + (uint64_t{slot} * kProbePerFrame + i) * 16, 16);
                fprintf(o, "%llu %u %c %016llx %#llx %ux%u %u %08x %08x %08x %08x d%d\n", (unsigned long long)f, i, e.kind,
                        (unsigned long long)e.prog, (unsigned long long)e.gpu, e.w, e.h, e.fmt, w[0], w[1], w[2], w[3], e.draws);
            }
        }
        for (uint32_t slot = 0; slot < kProbeFrames; slot++)
            for (const std::string& l : probe_ubos[slot]) fprintf(o, "%s\n", l.c_str());
        fclose(o);
    }
}
void probe_log(uint64_t f) {
    if (!probe_on() || !probe_map) return;
    static int left = 30;
    if (left <= 0) return;
    const uint32_t a = (uint32_t)((f - 1) % kProbeFrames), b = (uint32_t)(f % kProbeFrames), c = (uint32_t)((f + 1) % kProbeFrames);
    if (probe_frame[a] != f - 1 || probe_frame[b] != f || probe_frame[c] != f + 1) {
        logf("vk: probe of frame %llu: its neighbours are gone", (unsigned long long)f);
        return;
    }
    left--;
    logf("vk: probe of flash frame %llu: %u / %u / %u entries", (unsigned long long)f, probe_n[a], probe_n[b], probe_n[c]);
    const auto word = [](uint32_t slot, uint32_t i, int k) {
        uint32_t w;
        std::memcpy(&w, (uint8_t*)probe_map + (uint64_t{slot} * kProbePerFrame + i) * 16 + k * 4, 4);
        return w;
    };
    char path[64];
    snprintf(path, sizeof path, "probe_%llu.txt", (unsigned long long)f);
    if (FILE* o = fopen(path, "w")) {   // (scripts/probe.py aligns the frames by program and target)
        for (const uint32_t slot : {a, b, c})
            for (uint32_t i = 0; i < probe_n[slot]; i++) {
                const ProbeEntry& e = probe_meta[slot][i];
                fprintf(o, "%llu %u %c %016llx %#llx %ux%u %u %08x %08x %08x %08x\n", (unsigned long long)probe_frame[slot], i, e.kind,
                        (unsigned long long)e.prog, (unsigned long long)e.gpu, e.w, e.h, e.fmt, word(slot, i, 0), word(slot, i, 1),
                        word(slot, i, 2), word(slot, i, 3));
            }
        for (const uint32_t slot : {a, b, c})
            for (const std::string& l : probe_ubos[slot]) fprintf(o, "%s%c", l.c_str(), 10);
        fclose(o);
    }
}

// (amd-fix, diagnosis) in a frame dump, the indirect mesh draws' arguments as the GPU has them: noted at the draw (inside
// the pass, where no copy can be recorded) and copied out once the pass ends ("dump buffer ... mesh indirect")
namespace {
TLS_OBJECT(dump_mesh_indirect, std::vector<std::pair<uint64_t, uint64_t>>)   // (program key, indirect address)
} // namespace
void end_pass(VkCommandBuffer cmd) {
    Pass& ps = pass();   // (hookshot, once: the accessor is a call and a thread-local read at each use (__tls_get_addr in the shared library). The same object for the thread's life: draw_begin_recording resets it in place)
    if (!ps.open) return;
    vkCmdEndRendering(cmd);
    ps.open = false;
    note_pass_end(cmd);   // (before the dumps and probes below: they record work, and the next pass's barrier is then a full one)
    if (dump_on() && !dump_mesh_indirect().empty()) {
        for (const auto& [key, addr] : dump_mesh_indirect()) {
            const BufferRef r = gpu_buffer_bind(addr, 16, false);
            char what[80];
            snprintf(what, sizeof what, "mesh indirect prog %016llx at %#llx", (unsigned long long)key, (unsigned long long)addr);
            if (r.buffer) dump_buffer(cmd, r.buffer, r.offset, 16, what);
        }
        dump_mesh_indirect().clear();
    }
    if (probe_on())
        for (int i = 0; i < ps.n; i++)
            if (ps.color[i] && ps.tex[i]) probe_image(cmd, ps.tex[i], ps.level[i], ps.layer[i], ps.prog, 'p', ps.draws);
    if (pass_trace_on()) {
        char sampled[160] = "";
        int k = 0;
        for (size_t i = 0; i < ps.sampled.size() && i < 6 && k < 140; i++)
            k += snprintf(sampled + k, sizeof sampled - k, " %#llx", (unsigned long long)ps.sampled[i]->gpu);
        trace_line("pass %ux%u f%d %dc%s wm%x/%u: %d draws %d clears, c0 %#llx/%u, depth %#llx/%u, progs %016llx..%016llx, "
                   "sampled%s", ps.width, ps.height, (int)ps.format0, ps.n, ps.depth ? "+d" : "", ps.wm_or, ps.wm_n, ps.draws,
                   ps.clears, (unsigned long long)(ps.n && ps.tex[0] ? ps.tex[0]->gpu : 0), ps.layer[0],
                   (unsigned long long)(ps.depth_tex ? ps.depth_tex->gpu : 0), ps.depth_layer,
                   (unsigned long long)ps.first_prog, (unsigned long long)ps.prog, sampled);
    }
    if (gprof_on()) {
        char label[160];
        snprintf(label, sizeof label, "pass %ux%u f%d %dc%s, %s draws, %zu sampled, first prog %016llx, last prog %016llx", ps.width, ps.height,
                 (int)ps.format0, ps.n, ps.depth ? "+d" : "", ps.draws > 256 ? ">256" : ps.draws > 32 ? "33-256" : ps.draws > 4 ? "5-32" : "0-4",
                 ps.sampled.size(), (unsigned long long)ps.first_prog, (unsigned long long)ps.prog);
        gprof_end(cmd, label, ps.draws);
    } else if (tprof_on()) {   // (the performance profile: the pass's kind and key; a dynamic resolution's small steps are one pass)
        const profile_report::PassKind kind = profile_report::pass_kind(ps.n, ps.depth != VK_NULL_HANDLE, ps.width, ps.height, ps.draws, tprof_output_pixels());
        tprof_end(cmd, kind, profile_report::pass_key((int)ps.format0, ps.n, ps.depth != VK_NULL_HANDLE, ps.width, ps.height, ps.first_prog));
    }
    if (dump_on())
        for (int i = 0; i < ps.n; i++)
            if (ps.color[i] && ps.tex[i]) {
                char what[64];
                snprintf(what, sizeof what, "pass c%d prog %016llx", i, (unsigned long long)ps.prog);
                dump_image(cmd, ps.tex[i], ps.level[i], ps.layer[i], what);
            }
    if (dump_on() && ps.depth && ps.depth_tex) {
        char what[64];
        snprintf(what, sizeof what, "pass depth prog %016llx", (unsigned long long)ps.prog);
        dump_image(cmd, ps.depth_tex, ps.depth_level, ps.depth_layer, what);
    }
    if (dump_on())
        for (Texture* t : dump_sampled()) {
            char what[64];
            snprintf(what, sizeof what, "sampled by %016llx", (unsigned long long)ps.prog);
            for (uint32_t l = 0; l < std::min<uint32_t>(t->layers, 2); l++) dump_image(cmd, t, 0, l, what);
            if (t->guest_size >= (8u << 20)) texture_log_guest(t);   // (and what the game's memory holds of it)
            for (uint32_t lv = 1; t->layers == 1 && lv < std::min<uint32_t>(t->levels, 8); lv++) {   // (every level)
                snprintf(what, sizeof what, "sampled by %016llx level %u", (unsigned long long)ps.prog, lv);
                dump_image(cmd, t, lv, 0, what);
            }
        }
    dump_sampled().clear();
    ps.prog = 0;
}

const std::vector<uint64_t>& record_progs();
void record_draw(const NvnState& s, const Program& p, const DrawArgs& d, const Targets& at);
void probe_draw(const NvnState& s, const Program& p, int pass_draws);
/// (RX_VK_DUMP_ARGS=<key>[,<key>...] (hex; 1: every draw): in a dump, each draw of those programs logged with its
/// arguments, its indirect arguments as guest memory holds them, its vertex buffers and the first words of the vertex
/// stage's uniform buffers)
void log_dump_draw(const NvnState& s, const Program& p, const DrawArgs& d) {
    static const std::vector<uint64_t> want = [] {
        std::vector<uint64_t> v;
        for (const char* q = getenv("RX_VK_DUMP_ARGS"); q && *q;) {
            char* end;
            const uint64_t k = strtoull(q, &end, 16);
            if (end == q) break;
            v.push_back(k);
            q = *end ? end + 1 : end;
        }
        return v;
    }();
    if (want.empty() || (want[0] != 1 && std::find(want.begin(), want.end(), p.key) == want.end())) return;
    char line[4096];
    int n = snprintf(line, sizeof line, "vk: dump-draw %03d prog %016llx prim %d count %d first %d inst %d bv %d bi %d idx %d/%d ibuf %#llx",
                     dump_seq(), (unsigned long long)p.key, d.prim, d.count, d.first, d.instances, d.base_vertex, d.base_instance,
                     (int)d.indexed, d.itype, (unsigned long long)d.ibuf);
    if (d.indirect) {
        uint32_t a[5] = {};
        host->gpu_read(d.indirect, a, sizeof a);
        uint32_t c = 0;
        if (d.count_addr) host->gpu_read(d.count_addr, &c, 4);
        n += snprintf(line + n, sizeof line - n, " ind %#llx [%u %u %u %u %u] cnt %#llx=%u max %d stride %d",
                      (unsigned long long)d.indirect, a[0], a[1], a[2], a[3], a[4], (unsigned long long)d.count_addr, c, d.max,
                      d.stride);
    }
    if (d.indexed && d.ibuf && s.nvattr > 0) {   // (the first indices, and attribute 0 of the vertices they take)
        const nvn_va_st a0 = *reinterpret_cast<const nvn_va_st*>(&s.vattr[0]);
        const uint32_t stride = s.vstream[a0.stream & 15].stride;
        const uint64_t isz = d.itype == 0 ? 1 : d.itype == 1 ? 2 : 4;
        uint8_t ib[64] = {};
        host->gpu_read(d.ibuf + (uint64_t)d.first * isz, ib, 12 * isz);
        n += snprintf(line + n, sizeof line - n, " IDX");
        for (int k = 0; k < 12; k++) {
            uint32_t ix = isz == 1 ? ib[k] : isz == 2 ? ((uint16_t*)ib)[k] : ((uint32_t*)ib)[k];
            if (k < 6 && ix != 0xFFFF && ix != 0xFFFFFFFF && s.vbuf[a0.stream & 15].address && stride) {
                float v[2] = {};
                host->gpu_read(s.vbuf[a0.stream & 15].address + (uint64_t)((int64_t)ix + d.base_vertex) * stride + a0.offset, v, 8);
                n += snprintf(line + n, sizeof line - n, " %u(%.4g,%.4g)", ix, v[0], v[1]);
            } else n += snprintf(line + n, sizeof line - n, " %u", ix);
        }
    }
    for (int i : {12, 11}) {   // (the vertex stage's c15 and c14 first: MKW's terrain patches' placement and camera)
        const nvn_p_bind_buf& u = s.ubo[0][i];
        if (!u.addr) continue;
        float f[16] = {};
        const bool upd = ub_overlay(u.addr, sizeof f, (uint8_t*)f, [](uint64_t) { return (uint64_t)0; }) != UINT64_MAX;
        if (!upd) host->gpu_read(u.addr, f, sizeof f);
        n += snprintf(line + n, sizeof line - n, " C%d%s %#llx+%#llx [", i + 3, upd ? "*" : "", (unsigned long long)u.addr,
                      (unsigned long long)u.size);
        for (int k = 0; k < 16; k++) n += snprintf(line + n, sizeof line - n, k ? " %.6g" : "%.6g", f[k]);
        n += snprintf(line + n, sizeof line - n, "]");
    }
    for (int i = 0; i < 4 && n < 1500; i++)
        if (s.vbuf[i].address) n += snprintf(line + n, sizeof line - n, " vb%d %#llx+%#llx", i, (unsigned long long)s.vbuf[i].address,
                                             (unsigned long long)s.vbuf[i].size);
    for (int i = 0; i < s.nvattr && i < 4 && n < 1500; i++) {   // (the attributes: format, offset, stream; the streams' strides)
        const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
        n += snprintf(line + n, sizeof line - n, " va%d f%u+%u s%u/%u", i, (unsigned)a.fmt, (unsigned)a.offset, (unsigned)a.stream,
                      s.vstream[a.stream & 15].stride);
    }
    for (int i = 0; i < 16 && n < 1500; i++) {   // (the vertex stage's storage buffers: their first 8 words)
        const nvn_p_bind_buf& b = s.ssbo[0][i];
        if (!b.addr) continue;
        uint32_t w[8] = {};
        host->gpu_read(b.addr, w, sizeof w);
        n += snprintf(line + n, sizeof line - n, " sb%d %#llx+%#llx [%x %x %x %x %x %x %x %x]", i, (unsigned long long)b.addr,
                      (unsigned long long)b.size, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    for (int i = 0; i < 16 && n < 1500; i++) {   // (the vertex stage's uniform buffers: their first 16 words)
        const nvn_p_bind_buf& u = s.ubo[0][i];
        if (!u.addr) continue;
        float f[16] = {};
        // (as the draw reads it: guest memory with the stream's uniform updates over it; '*': updated)
        const bool upd = ub_overlay(u.addr, sizeof f, (uint8_t*)f, [](uint64_t) { return (uint64_t)0; }) != UINT64_MAX;
        if (!upd) host->gpu_read(u.addr, f, sizeof f);
        n += snprintf(line + n, sizeof line - n, " c%d%s %#llx+%#llx [", i + 3, upd ? "*" : "", (unsigned long long)u.addr,
                      (unsigned long long)u.size);
        for (int k = 0; k < 16 && n < 1900; k++) n += snprintf(line + n, sizeof line - n, k ? " %.5g" : "%.5g", f[k]);
        n += snprintf(line + n, sizeof line - n, "]");
    }
    logf("%s", line);
}
extern int64_t presents_now;   // (vk_replay.cpp)
uint32_t stage_cbuf_word(const NvnState& s, const Stage& st, uint32_t index, uint32_t offset);   // (vk_shader.cpp)
// (hookshot) RX_VK_EXPOSURE_LOG=1: the final pass's exposure (c3+0x38 of its fragment stage, OoT's 73320e720a317817) at each
// frame, kept for exposure_log_dump when the frame log sees a white picture
struct ExposureRing { int64_t present; uint32_t word; };
std::mutex exposure_mu;
ExposureRing exposure_ring[32];
uint32_t exposure_at = 0;
bool exposure_log_on() {
    static const bool on = getenv("RX_VK_EXPOSURE_LOG") != nullptr;
    return on;
}
void exposure_note(const NvnState& s, const Program& p) {
    static std::unordered_map<uint64_t, bool> known;
    bool hit = false;
    {
        std::lock_guard lk(exposure_mu);
        auto it = known.find(s.prog.serial);
        if (it != known.end()) hit = it->second;
        else hit = known[s.prog.serial] = program_code_keys(s).find("73320e720a317817") != std::string::npos;
    }
    if (!hit) return;
    for (const Stage& st : p.stages)
        if (st.nvn_stage == 1) {
            const uint32_t w = stage_cbuf_word(s, st, 3, 0x38);
            std::lock_guard lk(exposure_mu);
            exposure_ring[exposure_at++ % 32] = {presents_now, w};
            static const bool every = getenv("RX_VK_EXPOSURE_LOG") && atoi(getenv("RX_VK_EXPOSURE_LOG")) == 2;
            if (every) {
                float f;
                std::memcpy(&f, &w, 4);
                logf("vk: exposure %llu %g", (unsigned long long)presents_now, f);
            }
        }
}
void exposure_log_dump(const char* why) {
    std::lock_guard lk(exposure_mu);
    char line[900];
    int n = snprintf(line, sizeof line, "vk: %s: exposure by present:", why);
    for (uint32_t i = 0; i < 32 && n < 860; i++) {
        const ExposureRing& e = exposure_ring[(exposure_at + i) % 32];
        float v;
        std::memcpy(&v, &e.word, 4);
        n += snprintf(line + n, sizeof line - n, " %lld:%.3g", (long long)e.present, v);
    }
    logf("%s", line);
}

// ---- (EvilOOT) RX_VK_DRAW_STATS=<dir>: what each program's draws produce on the GPU, for one frame. When <dir>/draw_stats.now
// appears (checked every half second; the file is removed), every draw of the next frame runs inside a pipeline statistics
// query: vertex / task / mesh shader invocations, primitives into and out of clipping, fragment shader invocations. Eight
// frames later the results are read and written to <dir>/draw_stats_<frame>.txt, one line per program, the programs whose
// draws run but output nothing first (a draw whose mesh threads run and whose primitives never reach the clipper is one that
// made no triangles: NaN positions, culled lists, empty records). ----
namespace {
constexpr uint32_t kStatsQueries = 32768;
struct StatsMeta {
    uint64_t prog;
    uint8_t mesh, indirect, indexed, args_read;
    int32_t count, instances, max, stride;
    uint64_t count_addr, args_addr;
    uint32_t guest[4], guest_count;   // (the same words as the game's memory held them when the draw was recorded)
    uint8_t dispatch;                 // (a compute dispatch: its grid, or its indirect arguments as above)
    uint32_t grid[3];
    uint64_t args_vkbuf, args_vkoff;  // (the Vulkan buffer and offset the arguments were read from)
};
VkQueryPool stats_pool = VK_NULL_HANDLE;
// (indirect draws: after the draw, the first 16 bytes of its arguments and its count word as the GPU holds them, 32 bytes a
// query: args[4], count)
VkBuffer stats_args = VK_NULL_HANDLE;
VmaAllocation stats_args_mem = nullptr;
uint32_t* stats_args_map = nullptr;
// (EvilOOT) RX_VK_WATCH_ADDRS=<gpu address>[,...] (hex, at most 4): 16 bytes at each, as the GPU holds them after every
// dispatch of the measured frame; the report lists the dispatches that changed them. Which pass writes a launch count.
constexpr uint32_t kWatchMax = 4;
VkBuffer stats_watch = VK_NULL_HANDLE;
VmaAllocation stats_watch_mem = nullptr;
uint32_t* stats_watch_map = nullptr;
std::vector<uint64_t> parse_hex_list(const char* q, size_t max) {
    std::vector<uint64_t> a;
    while (q && *q && a.size() < max) {
        char* end;
        const uint64_t x = strtoull(q, &end, 16);
        if (end == q) break;
        a.push_back(x);
        q = *end ? end + 1 : end;
    }
    return a;
}
// (RX_VK_WATCH_ARGS_OF=<program>[,<program>]: the launch arguments of those dispatches instead: their addresses differ
// from start to start, and the game alternates two sets frame by frame: the last two each program used, noted at every
// dispatch, watched from the measured frame on)
std::mutex argwatch_mu;
std::vector<std::pair<uint64_t, std::array<uint64_t, 2>>> watch_seen;   // (program -> its last two argument addresses)
std::vector<uint64_t> watch_live;   // (what the measured frame watches: set when it is armed)
void watch_note(uint64_t prog, uint64_t indirect) {
    static const std::vector<uint64_t> progs = parse_hex_list(getenv("RX_VK_WATCH_ARGS_OF"), 2);
    if (progs.empty() || !indirect || std::find(progs.begin(), progs.end(), prog) == progs.end()) return;
    std::lock_guard lk(argwatch_mu);
    for (auto& [p, a] : watch_seen)
        if (p == prog) {
            if (a[0] != indirect) a[1] = a[0], a[0] = indirect;
            return;
        }
    watch_seen.push_back({prog, {indirect, 0}});
}
// (RX_VK_WATCH_SB=<program>:<slot>:<offset>[,...], hex: a compute program's storage buffer slot + offset, as it binds it)
std::vector<uint64_t> watch_sb_seen;
void watch_note_sb(uint64_t prog, const NvnState& s) {
    struct Want { uint64_t prog; uint32_t slot; uint64_t off; };
    static const std::vector<Want> want = [] {
        std::vector<Want> v;
        for (const char* q = getenv("RX_VK_WATCH_SB"); q && *q && v.size() < 2;) {
            char* e;
            Want w{};
            w.prog = strtoull(q, &e, 16);
            if (*e != ':') break;
            w.slot = (uint32_t)strtoul(e + 1, &e, 16);
            if (*e != ':') break;
            w.off = strtoull(e + 1, &e, 16);
            v.push_back(w);
            q = *e ? e + 1 : e;
        }
        return v;
    }();
    for (const Want& w : want) {   // (slot 0x10 and up: uniform buffer slot - 0x10)
        const nvn_p_bind_buf* b = w.slot < 16 ? &s.ssbo[5][w.slot] : w.slot < 32 ? &s.ubo[5][w.slot - 16] : nullptr;
        if (w.prog != prog || !b || !b->addr) continue;
        const uint64_t a = b->addr + w.off;
        std::lock_guard lk(argwatch_mu);
        if (std::find(watch_sb_seen.begin(), watch_sb_seen.end(), a) == watch_sb_seen.end()) {
            watch_sb_seen.insert(watch_sb_seen.begin(), a);
            if (watch_sb_seen.size() > 2) watch_sb_seen.pop_back();
        }
    }
}
void watch_arm() {   // (stats_tick, as a frame is armed)
    static const std::vector<uint64_t> fixed = parse_hex_list(getenv("RX_VK_WATCH_ADDRS"), kWatchMax);
    std::lock_guard lk(argwatch_mu);
    watch_live.clear();
    for (const uint64_t a : fixed) watch_live.push_back(a & ~15ull);
    for (const uint64_t a : watch_sb_seen)
        if (watch_live.size() < kWatchMax) watch_live.push_back(a & ~15ull);
    for (const auto& [p, a] : watch_seen)
        for (const uint64_t x : a)
            if (x && watch_live.size() < kWatchMax) watch_live.push_back(x & ~15ull);
}
const std::vector<uint64_t>& watch_addrs() { return watch_live; }
bool watch_wanted() { return getenv("RX_VK_WATCH_ADDRS") || getenv("RX_VK_WATCH_ARGS_OF") || getenv("RX_VK_WATCH_SB"); }
bool stats_mesh_bits = false;
StatsMeta stats_meta[kStatsQueries];
std::atomic<uint32_t> stats_used{0};
std::atomic<int64_t> stats_frame{-1};   // (the frame measured; -1 none)
std::atomic<bool> stats_written{false};
std::mutex stats_mu;
const char* stats_dir() {
    static const char* d = getenv("RX_VK_DRAW_STATS");
    return d && *d ? d : nullptr;
}
bool stats_frame_now() {
    if (!stats_dir()) return false;
    const int64_t at = stats_frame.load(std::memory_order_relaxed);
    return at >= 0 && (int64_t)texture_frame_no() == at && !stats_written.load(std::memory_order_relaxed);
}
bool stats_make_pool() {
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    qi.queryCount = kStatsQueries;
    const VkQueryPipelineStatisticFlags base = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                               VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                                               VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                               VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                               VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
    qi.pipelineStatistics = base;
    if (dev.mesh_shader) {
        qi.pipelineStatistics = base | VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT |
                                VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
        if (vkCreateQueryPool(dev.device, &qi, nullptr, &stats_pool) == VK_SUCCESS) {
            stats_mesh_bits = true;
            return true;
        }
        qi.pipelineStatistics = base;
    }
    return vkCreateQueryPool(dev.device, &qi, nullptr, &stats_pool) == VK_SUCCESS;
}
/// (each draw) arms a capture when the trigger file is there; writes the results once the measured frame is long done
void stats_tick() {
    const char* dir = stats_dir();
    if (!dir) return;
    const int64_t frame = (int64_t)texture_frame_no();
    const int64_t at = stats_frame.load(std::memory_order_relaxed);
    if (at < 0) {
        static std::atomic<int64_t> next_check{0};
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now < next_check.load(std::memory_order_relaxed)) return;
        next_check.store(now + 500, std::memory_order_relaxed);
        const std::string trigger = std::string(dir) + "/draw_stats.now";   // ('/': both systems take it; '\\' made a file beside the folder on Linux)
        FILE* f = fopen(trigger.c_str(), "rb");
        if (!f) return;
        fclose(f);
        std::remove(trigger.c_str());
        std::lock_guard lk(stats_mu);
        if (stats_frame.load() >= 0) return;
        if (!stats_pool && !stats_make_pool()) {
            logf("vk: draw stats: no pipeline statistics query pool");
            return;
        }
        if (!stats_args) {
            void* m = nullptr;
            if (create_buffer((uint64_t)kStatsQueries * 32, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, stats_args, stats_args_mem, &m))
                stats_args_map = (uint32_t*)m;
        }
        if (stats_args_map) std::memset(stats_args_map, 0xEE, (size_t)kStatsQueries * 32);   // (0xEEEEEEEE: never copied)
        watch_arm();
        if (watch_wanted() && !stats_watch) {
            void* m = nullptr;
            if (create_buffer((uint64_t)kStatsQueries * kWatchMax * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, stats_watch, stats_watch_mem, &m))
                stats_watch_map = (uint32_t*)m;
        }
        if (stats_watch_map) std::memset(stats_watch_map, 0xEE, (size_t)kStatsQueries * kWatchMax * 16);
        vkResetQueryPool(dev.device, stats_pool, 0, kStatsQueries);
        {
            std::lock_guard bl(stats_bind_mu);
            stats_binds.clear();
            stats_ptrs.clear();
            stats_ptrs2.clear();
            stats_dumps.clear();
            stats_gpu_recs.clear();
        }
        {
            std::lock_guard ml(miss_mu);   // (the misses from here on: older frames' in-flight work may still add some)
            if (miss_map) std::memset(miss_map, 0, (size_t)kMissSlice * kMissSlices);
        }
        stats_used.store(0);
        stats_written.store(false);
        stats_frame.store(frame + 2);   // (a frame not yet started on any replay thread)
        logf("vk: draw stats: measuring frame %lld%s", (long long)(frame + 2), stats_mesh_bits ? " (with task and mesh counts)" : "");
        return;
    }
    if (frame < at + 8 || stats_written.load()) return;
    std::lock_guard lk(stats_mu);
    if (stats_written.exchange(true)) return;
    const uint32_t n = std::min(stats_used.load(), kStatsQueries);
    const uint32_t words = stats_mesh_bits ? 7 : 5;   // (in bit order: vs, clip in, clip out, fs, cs, [task, mesh])
    std::vector<uint64_t> r((size_t)n * words);
    VkResult res = n ? vkGetQueryPoolResults(dev.device, stats_pool, 0, n, r.size() * 8, r.data(), (VkDeviceSize)words * 8,
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)
                     : VK_SUCCESS;
    struct Sum {
        uint64_t draws = 0, vs = 0, clip_in = 0, clip_out = 0, fs = 0, cs = 0, task = 0, mesh = 0;
        bool is_mesh = false, indirect = false, is_dispatch = false;
        std::string args;   // (indirect: the GPU's argument words and count of its first draws)
    };
    std::map<uint64_t, Sum> by;
    for (uint32_t i = 0; i < n && res == VK_SUCCESS; i++) {
        const StatsMeta& m = stats_meta[i];
        Sum& x = by[m.prog];
        const uint64_t* v = &r[(size_t)i * words];
        x.draws++, x.vs += v[0], x.clip_in += v[1], x.clip_out += v[2], x.fs += v[3], x.cs += v[4];
        if (stats_mesh_bits) x.task += v[5], x.mesh += v[6];
        x.is_mesh |= m.mesh != 0, x.indirect |= m.indirect != 0, x.is_dispatch |= m.dispatch != 0;
        if (m.dispatch && x.draws == 1) x.args += " #" + std::to_string(i);   // (its place in the frame's recording)
        if (m.dispatch && !m.indirect && x.args.size() < 200) {
            char b[64];
            snprintf(b, sizeof b, " [grid %u %u %u]", m.grid[0], m.grid[1], m.grid[2]);
            x.args += b;
        }
        if (m.indirect && x.args.size() < 200) {
            char b[160];
            const uint32_t* a = stats_args_map ? stats_args_map + (size_t)i * 8 : nullptr;
            const uint32_t* g = m.guest;
            if (!a || !m.args_read)
                snprintf(b, sizeof b, " [@%llx gpu not read, mem %u %u %u %u]", (unsigned long long)m.args_addr, g[0], g[1], g[2], g[3]);
            else if (m.count_addr)
                snprintf(b, sizeof b, " [@%llx gpu %u %u %u %u cnt=%u | mem %u %u %u %u cnt=%u]", (unsigned long long)m.args_addr, a[0],
                         a[1], a[2], a[3], a[4], g[0], g[1], g[2], g[3], m.guest_count);
            else
                snprintf(b, sizeof b, " [@%llx gpu %u %u %u %u | mem %u %u %u %u]", (unsigned long long)m.args_addr, a[0], a[1], a[2],
                         a[3], g[0], g[1], g[2], g[3]);
            x.args += b;
        }
    }
    std::vector<std::pair<uint64_t, Sum>> rows(by.begin(), by.end());
    // (no output first: shaders ran and nothing reached the clipper, then nothing shaded)
    const auto rank = [](const Sum& x) {
        if (x.is_dispatch) return x.cs ? 5 : 4;   // (dispatches after the draws: those that ran no thread first)
        const bool ran = x.vs || x.mesh || x.task;
        return !ran ? 2 : !x.clip_in ? 0 : !x.fs ? 1 : 3;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) { return rank(a.second) < rank(b.second); });
    const std::string path = std::string(dir) + "/draw_stats_" + std::to_string(at) + ".txt";
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fprintf(f, "# frame %lld: %u draws, %zu programs (query result %d). kind prog draws | vs task mesh | clip_in clip_out | fs | note\n",
                (long long)at, n, rows.size(), (int)res);
        for (const auto& [prog, x] : rows) {
            const int k = rank(x);
            if (x.is_dispatch) {   // (compute: its dispatches, the threads they ran)
                fprintf(f, "comp %016llx %6llu | cs threads %12llu | %s%s\n", (unsigned long long)prog, (unsigned long long)x.draws,
                        (unsigned long long)x.cs, k == 4 ? "NO THREADS" : "", (x.indirect ? " (indirect)" + x.args : x.args).c_str());
                continue;
            }
            fprintf(f, "%s %016llx %6llu | %10llu %8llu %10llu | %10llu %10llu | %11llu | %s%s\n", x.is_mesh ? "mesh" : "draw",
                    (unsigned long long)prog, (unsigned long long)x.draws, (unsigned long long)x.vs, (unsigned long long)x.task,
                    (unsigned long long)x.mesh, (unsigned long long)x.clip_in, (unsigned long long)x.clip_out,
                    (unsigned long long)x.fs, k == 0 ? "RAN, NO PRIMITIVES" : k == 1 ? "primitives, no fragments" : k == 2 ? "no shader work" : "",
                    x.indirect ? (" (indirect)" + x.args).c_str() : "");
        }
        // who binds each indirect draw's argument words in this frame: written (W) or only read (r), by which programs
        std::vector<StatsBind> binds;
        {
            std::lock_guard bl(stats_bind_mu);
            binds = stats_binds;
        }
        // (RX_VK_DRAW_STATS_PROGS=<key>,...: every storage buffer those programs bound in this frame)
        if (const char* want = getenv("RX_VK_DRAW_STATS_PROGS")) {
            for (const char* q = want; *q;) {
                char* end;
                const uint64_t k = strtoull(q, &end, 16);
                if (end == q) break;
                q = *end ? end + 1 : end;
                fprintf(f, "\n# bindings of %016llx:\n", (unsigned long long)k);
                std::vector<std::pair<uint64_t, uint64_t>> seen;
                for (const StatsBind& b : binds) {
                    const std::pair<uint64_t, uint64_t> slot{b.addr ^ ((uint64_t)b.off << 40) ^ ((uint64_t)b.cbuf << 56), b.size};
                    if (b.prog != k || std::find(seen.begin(), seen.end(), slot) != seen.end()) continue;
                    seen.push_back(slot);
                    fprintf(f, "  s%d %s %#llx +%#llx  from c%u+%#x (raw %#llx)%s%s\n", b.stage, b.written ? "W" : "r",
                            (unsigned long long)b.addr, (unsigned long long)b.size, b.cbuf, b.off, (unsigned long long)b.raw,
                            b.table ? " (table entry)" : "", !b.addr ? "  <-- UNRESOLVED" : "");
                }
            }
        }
        {
            std::lock_guard bl(stats_bind_mu);
            for (const std::string& d : stats_dumps) fprintf(f, "\n%s", d.c_str());
            for (const GpuDumpRec& r : stats_gpu_recs) {   // (as the GPU held them after the wanted draws)
                if (!stats_gpu_map) break;
                const uint32_t* w = (const uint32_t*)(stats_gpu_map + (size_t)r.slot * kGpuDumpSlot);
                uint64_t nonzero = 0;
                uint32_t first = UINT32_MAX;
                for (uint32_t i = 0; i < r.bytes / 4; i++)
                    if (w[i]) nonzero++, first = std::min(first, i);
                fprintf(f, "\n%016llx s%d ssbo%d at %#llx AS THE GPU HELD IT after the %s (%llu of %u words non-zero, first at +%x):\n",
                        (unsigned long long)r.prog, r.stage, r.index, (unsigned long long)r.addr, r.stage == 5 ? "dispatch" : "draw",
                        (unsigned long long)nonzero, r.bytes / 4, first == UINT32_MAX ? 0 : first * 4);
                const uint32_t from = first == UINT32_MAX ? 0 : first & ~3u;
                for (uint32_t i = from; i + 4 <= r.bytes / 4 && i < from + 64; i += 4)
                    fprintf(f, "  +%05x: %08x %08x %08x %08x\n", i * 4, w[i], w[i + 1], w[i + 2], w[i + 3]);
            }
        }
        // (second level) pointers found in compute programs' pointer table regions that no binding or region of theirs covers
        {
            std::lock_guard bl(stats_bind_mu);
            std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> by;   // program -> (region, pointer)
            for (const StatsPtr& q : stats_ptrs2) by[q.prog].emplace_back(q.in_buffer, q.value);
            fprintf(f, "\n# second-level pointers no binding covers: %zu programs\n", by.size());
            for (auto& [prog, v] : by) {
                std::sort(v.begin(), v.end());
                v.erase(std::unique(v.begin(), v.end()), v.end());
                std::string t;
                for (size_t i = 0; i < v.size() && i < 10; i++) {
                    char b[96];
                    snprintf(b, sizeof b, " %#llx(in %#llx, pool end %#llx)", (unsigned long long)v[i].second,
                             (unsigned long long)v[i].first, (unsigned long long)gpu_pool_end(v[i].second));
                    t += b;
                }
                fprintf(f, "  %016llx %zu pointers:%s\n", (unsigned long long)prog, v.size(), t.c_str());
            }
        }
        // (RX_VK_MISS_LOG) the global memory accesses no binding held, since the capture was armed, by program: how many,
        // and the first addresses with the pool each lies in
        {
            std::lock_guard ml(miss_mu);
            std::vector<std::pair<uint32_t, uint64_t>> rows;   // (misses, program)
            if (miss_map)
                for (const auto& [prog, slot] : miss_slot)
                    if (const uint32_t m = miss_map[(size_t)slot * (kMissSlice / 4)]) rows.emplace_back(m, prog);
            std::sort(rows.rbegin(), rows.rend());
            fprintf(f, "\n# global memory misses (RX_VK_MISS_LOG): %zu programs\n", rows.size());
            for (const auto& [m, prog] : rows) {
                const uint32_t* w = miss_map + (size_t)miss_slot[prog] * (kMissSlice / 4);
                std::string addrs;
                std::vector<uint64_t> uniq;
                for (uint32_t i = 0; i < std::min<uint32_t>(m, 63); i++) {
                    const uint64_t a = w[1 + 2 * i] | (uint64_t)w[2 + 2 * i] << 32;
                    if (std::find(uniq.begin(), uniq.end(), a) != uniq.end() || uniq.size() >= 12) continue;
                    uniq.push_back(a);
                    char t[64];
                    snprintf(t, sizeof t, " %#llx(pool end %#llx)", (unsigned long long)a, (unsigned long long)gpu_pool_end(a));
                    addrs += t;
                }
                fprintf(f, "  %016llx %u misses:%s\n", (unsigned long long)prog, m, addrs.c_str());
            }
        }
        if (getenv("RX_VK_BLOCK_PROBE") && miss_map) {   // (EvilOOT) the probed programs' blocks: invocations that entered each
            std::lock_guard ml(miss_mu);
            for (const auto& [prog, slot] : miss_slot) {
                const uint32_t* w = miss_map + (size_t)slot * (kMissSlice / 4);
                fprintf(f, "\n# block probe %016llx (invocations entering IR Block $n, since the frame was armed)\n", (unsigned long long)prog);
                for (uint32_t b = 0; 128 + b < kMissSlice / 4; b++)
                    if (w[128 + b]) fprintf(f, "  $%u %u\n", b, w[128 + b]);
            }
        }
        if (const std::vector<uint64_t>& w = watch_addrs(); !w.empty() && stats_watch_map) {   // (RX_VK_WATCH_ADDRS)
            fprintf(f, "\n# watched addresses: bindings over them this frame (W written; tables left out)\n");
            for (const uint64_t a : w) {
                std::vector<std::string> seen;
                for (const StatsBind& b : binds) {
                    if (b.table || !b.addr || a < b.addr || a >= b.addr + b.size) continue;
                    char t[96];
                    snprintf(t, sizeof t, "%s%016llx@s%d[%#llx+%#llx]", b.written ? "W:" : "r:", (unsigned long long)b.prog, b.stage,
                             (unsigned long long)b.addr, (unsigned long long)b.size);
                    if (std::find(seen.begin(), seen.end(), t) == seen.end() && seen.size() < 40) seen.push_back(t);
                }
                fprintf(f, "  @%llx:", (unsigned long long)a);
                for (const std::string& s2 : seen) fprintf(f, " %s", s2.c_str());
                fprintf(f, "\n");
            }
            fprintf(f, "\n# watched addresses, after each dispatch (shown where they changed; #n = its place in the frame)\n");
            std::vector<std::array<uint32_t, 4>> last(w.size(), {0xEEEEEEEEu, 0xEEEEEEEEu, 0xEEEEEEEEu, 0xEEEEEEEEu});
            for (uint32_t i = 0; i < n; i++) {
                if (!stats_meta[i].dispatch) continue;
                for (size_t k = 0; k < w.size(); k++) {
                    const uint32_t* v = stats_watch_map + ((size_t)i * kWatchMax + k) * 4;
                    if (v[0] == 0xEEEEEEEEu && v[1] == 0xEEEEEEEEu) continue;   // (not copied)
                    std::array<uint32_t, 4> now{v[0], v[1], v[2], v[3]};
                    if (now == last[k]) continue;
                    fprintf(f, "  #%u %016llx: @%llx = %u %u %u %u (was %u %u %u %u)\n", i, (unsigned long long)stats_meta[i].prog,
                            (unsigned long long)w[k], now[0], now[1], now[2], now[3], last[k][0], last[k][1], last[k][2], last[k][3]);
                    last[k] = now;
                }
            }
        }
        // (census) single storage buffer descriptors that resolved to nothing in this frame, by program and slot: a shader
        // that reads or writes a buffer it was never given. Empty table entries are left out (tables have holes).
        {
            std::map<std::tuple<uint64_t, int, uint32_t, uint32_t>, std::tuple<uint64_t, bool, uint64_t, uint64_t, int>> unresolved;
            for (const StatsBind& b : binds)   // (-> raw, written, the uniform buffer's address and size, times seen)
                if (!b.table && !b.addr) {
                    auto& e = unresolved[{b.prog, b.stage, b.cbuf, b.off}];
                    e = {b.raw, b.written, b.vkbuf, b.vkoff, std::get<4>(e) + 1};
                }
            fprintf(f, "\n# unresolved single descriptors: %zu\n", unresolved.size());
            for (const auto& [k, v] : unresolved)
                fprintf(f, "  %016llx s%d c%u+%#x raw %#llx %s (x%d; from the uniform buffer at %#llx size %#llx)\n",
                        (unsigned long long)std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k),
                        (unsigned long long)std::get<0>(v), std::get<1>(v) ? "WRITTEN" : "read", std::get<4>(v),
                        (unsigned long long)std::get<2>(v), (unsigned long long)std::get<3>(v));
        }
        fprintf(f, "\n# indirect arguments: the storage buffers of this frame that hold them (%zu bindings noted)\n", binds.size());
        std::vector<uint64_t> done;
        for (uint32_t i = 0; i < n; i++) {
            const StatsMeta& m = stats_meta[i];
            if (!m.indirect || std::find(done.begin(), done.end(), m.args_addr) != done.end()) continue;
            done.push_back(m.args_addr);
            std::string who;
            std::vector<std::pair<uint64_t, bool>> seen;
            for (const StatsBind& b : binds) {
                if (m.args_addr < b.addr || m.args_addr >= b.addr + b.size) continue;
                const std::pair<uint64_t, bool> k{b.prog, b.written};
                if (std::find(seen.begin(), seen.end(), k) != seen.end()) continue;
                seen.push_back(k);
                char t[160];
                // (vk: the Vulkan buffer and the offset in it this binding gives the argument words; the read's own after the
                // list: different ones mean two copies of the same memory)
                snprintf(t, sizeof t, " %s%016llx@s%d[%#llx+%#llx]%s vk %llx+%#llx", b.written ? "W:" : "r:", (unsigned long long)b.prog,
                         b.stage, (unsigned long long)b.addr, (unsigned long long)b.size, b.table ? "(table)" : "",
                         (unsigned long long)b.vkbuf, (unsigned long long)(b.vkoff + (m.args_addr - b.addr)));
                if (who.size() < 3000) who += t;
            }
            fprintf(f, "args %#llx (draw prog %016llx, read from vk %llx+%#llx):%s\n", (unsigned long long)m.args_addr,
                    (unsigned long long)m.prog, (unsigned long long)m.args_vkbuf, (unsigned long long)m.args_vkoff,
                    who.empty() ? " NO BINDING HOLDS THEM" : who.c_str());
            // (and the dispatches whose small buffers hold a pointer into the buffer around them: 1 MB either side)
            std::string ptr_who;
            std::vector<uint64_t> pseen;
            for (const StatsPtr& q : stats_ptrs) {
                if (q.value + (1ull << 20) < m.args_addr || q.value > m.args_addr + (1ull << 20)) continue;
                if (std::find(pseen.begin(), pseen.end(), q.prog ^ q.value) != pseen.end()) continue;
                pseen.push_back(q.prog ^ q.value);
                char t[96];
                snprintf(t, sizeof t, " %016llx(%#llx in %#llx)", (unsigned long long)q.prog, (unsigned long long)q.value,
                         (unsigned long long)q.in_buffer);
                if (ptr_who.size() < 1200) ptr_who += t;
            }
            if (!ptr_who.empty()) fprintf(f, "     pointed to by:%s\n", ptr_who.c_str());
        }
        fclose(f);
    }
    logf("vk: draw stats: frame %lld, %u draws of %zu programs -> %s", (long long)at, n, rows.size(), path.c_str());
    stats_frame.store(-1);   // (ready for another trigger)
}
/// One draw's query, ended on every way out of draw()
struct StatsQuery {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t index = 0;
    DrawArgs own{};
    const DrawArgs* draw = &own;
    StatsQuery(VkCommandBuffer c, uint64_t prog, const DrawArgs& d) { begin(c, prog, d, false, nullptr); }
    /// a compute dispatch: its grid, or its indirect arguments (12 bytes, read back as a draw's)
    StatsQuery(VkCommandBuffer c, uint64_t prog, uint64_t indirect, uint32_t x, uint32_t y, uint32_t z) {
        if (stats_dir()) watch_note(prog, indirect);
        DrawArgs d{};
        d.indirect = indirect;
        const uint32_t grid[3] = {x, y, z};
        begin(c, prog, d, true, grid);
    }
    void begin(VkCommandBuffer c, uint64_t prog, const DrawArgs& d, bool dispatch, const uint32_t* grid) {
        if (!stats_dir() || !stats_pool) return;
        const int64_t at = stats_frame.load(std::memory_order_relaxed);
        if (at < 0 || (int64_t)texture_frame_no() != at || stats_written.load(std::memory_order_relaxed)) return;
        const uint32_t i = stats_used.fetch_add(1);
        if (i >= kStatsQueries) return;
        stats_meta[i] = {prog, (uint8_t)d.mesh, (uint8_t)(d.indirect != 0), (uint8_t)d.indexed, 0, d.count, d.instances, d.max,
                         d.stride, d.count_addr, d.indirect, {}, 0, (uint8_t)dispatch, {}, 0, 0};
        if (grid) std::memcpy(stats_meta[i].grid, grid, sizeof stats_meta[i].grid);
        if (d.indirect) {
            host->gpu_read(d.indirect, stats_meta[i].guest, dispatch ? 12 : 16);
            if (d.count_addr) host->gpu_read(d.count_addr, &stats_meta[i].guest_count, 4);
        }
        own = d;
        is_dispatch = dispatch;
        if (dispatch) copy_args(c, own, i);   // (before the dispatch: outside any pass already)
        vkCmdBeginQuery(c, stats_pool, i, 0);
        cmd = c, index = i;
    }
    bool is_dispatch = false;
    const NvnState* state = nullptr;   // (a wanted draw: its mesh stage's storage buffers copied from the GPU after it)
    uint64_t prog = 0;
    ~StatsQuery() {
        if (!cmd) return;
        vkCmdEndQuery(cmd, stats_pool, index);
        if (!is_dispatch) copy_args(cmd, *draw, index);   // (a dispatch's were copied before it ran: begin)
        if (is_dispatch) copy_watch(cmd, index);
        if (state) gpu_dump(cmd, *state, prog, is_dispatch);
    }
    /// (RX_VK_DRAW_STATS_PROGS) the first 1 KB of each storage buffer of the mesh stage, as the GPU holds it after the draw,
    /// once per program: into stats_gpu (host-visible), printed with the report beside the memory view
    static void gpu_dump(VkCommandBuffer cmd, const NvnState& s, uint64_t prog, bool is_dispatch_dump = false);
    static void copy_args(VkCommandBuffer cmd, const DrawArgs& d, uint32_t index);
    static void copy_watch(VkCommandBuffer cmd, uint32_t index);
};
void StatsQuery::copy_watch(VkCommandBuffer cmd, uint32_t index) {
    const std::vector<uint64_t>& w = watch_addrs();
    if (w.empty() || !stats_watch) return;
    end_pass(cmd);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    for (size_t k = 0; k < w.size(); k++) {
        const BufferRef r = gpu_buffer_bind(w[k], 16, false);
        if (!r) continue;
        VkBufferCopy c{r.offset, ((VkDeviceSize)index * kWatchMax + k) * 16, 16};
        vkCmdCopyBuffer(cmd, r.buffer, stats_watch, 1, &c);
    }
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &di);
}
void StatsQuery::gpu_dump(VkCommandBuffer cmd, const NvnState& s, uint64_t prog, bool is_dispatch_dump) {
    std::lock_guard bl(stats_bind_mu);
    for (const GpuDumpRec& r : stats_gpu_recs)
        if (r.prog == prog) return;
    if (!stats_gpu) {
        void* m = nullptr;
        if (!create_buffer((uint64_t)kGpuDumpSlot * kGpuDumpSlots, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, stats_gpu, stats_gpu_mem, &m))
            return;
        stats_gpu_map = (uint8_t*)m;
    }
    end_pass(cmd);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
    for (const int stage : {7, 6, 5})
        for (int i = 0; i < 16; i++) {
            if ((stage == 5) != is_dispatch_dump) continue;   // (EvilOOT: a dispatch's own, stage 5)
            const nvn_p_bind_buf& b = s.ssbo[stage][i];
            if (!b.addr || !b.size || stats_gpu_recs.size() >= kGpuDumpSlots) continue;
            const uint32_t bytes = (uint32_t)std::min<uint64_t>(b.size, kGpuDumpSlot) & ~3u;
            const BufferRef r = gpu_buffer_bind(b.addr, bytes, false);
            if (!r) continue;
            const uint32_t slot = (uint32_t)stats_gpu_recs.size();
            VkBufferCopy c{r.offset, (VkDeviceSize)slot * kGpuDumpSlot, bytes};
            vkCmdCopyBuffer(cmd, r.buffer, stats_gpu, 1, &c);
            stats_gpu_recs.push_back({prog, b.addr, slot, bytes, stage, i});
        }
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &di);
}
/// The arguments as the GPU holds them: copied outside the pass, which the next draw opens again, as RX_VK_DUMP_EACH
/// does. A dispatch's before the dispatch (what it launched with: it may reset them itself), a draw's after it.
void StatsQuery::copy_args(VkCommandBuffer cmd, const DrawArgs& d, uint32_t index) {
    {
        if (!d.indirect || !stats_args) return;
        const BufferRef args = gpu_buffer_bind(d.indirect, 16, false);
        const BufferRef cnt = d.count_addr ? gpu_buffer_bind(d.count_addr, 4, false) : BufferRef{};
        if (!args) return;
        stats_meta[index].args_vkbuf = (uint64_t)(uintptr_t)args.buffer, stats_meta[index].args_vkoff = args.offset;
        end_pass(cmd);
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        VkBufferCopy c{args.offset, (VkDeviceSize)index * 32, 16};
        vkCmdCopyBuffer(cmd, args.buffer, stats_args, 1, &c);
        if (cnt) {
            VkBufferCopy cc{cnt.offset, (VkDeviceSize)index * 32 + 16, 4};
            vkCmdCopyBuffer(cmd, cnt.buffer, stats_args, 1, &cc);
        }
        VkMemoryBarrier2 back{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, back.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        back.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        back.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_HOST_READ_BIT;
        di.pMemoryBarriers = &back;
        vkCmdPipelineBarrier2(cmd, &di);
        stats_meta[index].args_read = 1;
    }
}
} // namespace

// ---- (HeyListen, TICK-033) real occlusion counts: RX_VK_OCC_REAL=<mode> ----
// The samples-passed reports are written on the CPU in stream order with RX_VK_OCC_FIXED (4096, "everything visible"). Here
// every draw the replay records also runs inside a Vulkan occlusion query, one slot a draw (or dispatch) of the stream (its
// serial number in the stream, the same on the dry pass and on the parallel segments' threads), in a host-reset ring of 64K
// slots. A report's count is the sum of the slots between the reset before it and it, read once its submission is done.
//   0 (default): off, no queries.  1: measured and logged only (the fixed value is still written: the picture is unchanged).
//   2: the fixed value, or 0 when the same report address read 0 the last time it was measured (one frame late).
//   3: the last measured count itself (fixed until the address has one).
//   4: same frame: a render condition on a report not read yet ends the recording there (a parallel replay's dry pass stops
//      there, and its segments up to it are recorded), submits it and waits for the GPU; every report's count is then written
//      over the fixed value in guest memory before the condition reads it. OoT: one wait a frame (its ~900 test draws come
//      first, the first condition 550+ draws after the last of them).
//   5: one frame late, by the report's place in the frame: OoT's report addresses differ from frame to frame (none repeats
//      within 4 frames; modes 2 and 3 read some old frame's count of another object: the washed-out picture), but the k-th
//      report of a frame tests the same object as the k-th of the frame before (99% of zero / non-zero agree, standing).
//      Key: the program bound at the report, its window's draws, and its place among the frame's reports with those two.
//      The count measured for that key at most 3 frames before is written; the fixed value until there is one.
// RX_VK_OCC_LOG=<dir>: RX_VK_OCC_LOG_FRAMES frames (default 2) every RX_VK_OCC_LOG_EVERY presents (default 600) written to <dir>/occ_<present>.txt: every
// report (window, draws, real count), every render condition, every draw in a window with its pipeline statistics.
thread_local int64_t occ_draw_slot = -1;   // (the replay: the serial of the draw it hands to draw(); -1: none)
int occ_mode() {
    static const int m = getenv("RX_VK_OCC_REAL") ? atoi(getenv("RX_VK_OCC_REAL")) : 0;
    return m;
}
namespace {
constexpr uint32_t kOccRing = 1u << 16, kOccChunk = 1u << 12;
enum : uint8_t { OCC_BEGUN = 1, OCC_MESH = 2, OCC_INDIRECT = 4, OCC_SKIPPED = 8, OCC_STATS = 16, OCC_INDEXED = 32 };
struct OccMeta { uint64_t prog; uint8_t flags; };
OccMeta occ_meta[kOccRing];
VkQueryPool occ_pool = VK_NULL_HANDLE, occ_stats_pool = VK_NULL_HANDLE;
bool occ_precise = false, occ_stats_mesh = false, occ_failed = false;
std::once_flag occ_once;
uint64_t occ_chunk_value[kOccRing / kOccChunk];   // (the last submission value whose draws used each chunk: the replay thread's)
std::atomic<uint64_t> occ_log_lo{UINT64_MAX}, occ_log_hi{UINT64_MAX};   // (serials of the frames being logged)
const char* occ_log_dir() {
    static const char* d = getenv("RX_VK_OCC_LOG");
    return d && *d ? d : nullptr;
}
void occ_init() {
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    vkGetPhysicalDeviceFeatures2(dev.physical, &f);
    occ_precise = f.features.occlusionQueryPrecise != VK_FALSE;
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kOccRing;
    if (vkCreateQueryPool(dev.device, &qi, nullptr, &occ_pool) != VK_SUCCESS) {
        occ_failed = true;
        logf("vk: occlusion: no query pool; the fixed value stays");
        return;
    }
    vkResetQueryPool(dev.device, occ_pool, 0, kOccRing);
    if (occ_log_dir() && f.features.pipelineStatisticsQuery) {
        VkQueryPoolCreateInfo si{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        si.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        si.queryCount = kOccRing;
        const VkQueryPipelineStatisticFlags base =
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        si.pipelineStatistics = base;
        if (dev.mesh_shader) {
            si.pipelineStatistics = base | VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT |
                                    VK_QUERY_PIPELINE_STATISTIC_MESH_SHADER_INVOCATIONS_BIT_EXT;
            occ_stats_mesh = vkCreateQueryPool(dev.device, &si, nullptr, &occ_stats_pool) == VK_SUCCESS;
        }
        if (!occ_stats_mesh) {
            si.pipelineStatistics = base;
            vkCreateQueryPool(dev.device, &si, nullptr, &occ_stats_pool);
        }
        if (occ_stats_pool) vkResetQueryPool(dev.device, occ_stats_pool, 0, kOccRing);
    }
    logf("vk: occlusion: real counts, mode %d (1 log only, 2 zero-or-fixed, 3 counts), %u query ring%s%s", occ_mode(), kOccRing,
         occ_precise ? ", precise" : "", occ_stats_pool ? ", pipeline statistics in logged frames" : "");
}
}  // namespace
/// One draw's occlusion query (and in logged frames its pipeline statistics), ended on every way out of draw()
struct OccQuery {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    uint32_t slot = 0;
    bool stats = false;
    OccQuery(VkCommandBuffer c, uint64_t prog, const DrawArgs& d, bool stats_busy) {
        if (occ_draw_slot < 0 || !occ_pool) return;
        const uint64_t serial = (uint64_t)occ_draw_slot;
        slot = (uint32_t)(serial % kOccRing);
        uint8_t fl = OCC_BEGUN | (d.mesh ? OCC_MESH : 0) | (d.indirect ? OCC_INDIRECT : 0) | (d.indexed ? OCC_INDEXED : 0);
        stats = occ_stats_pool && !stats_busy && serial >= occ_log_lo.load(std::memory_order_relaxed) &&
                serial < occ_log_hi.load(std::memory_order_relaxed);
        if (stats) fl |= OCC_STATS;
        occ_meta[slot] = {prog, fl};
        occ_skipped_here = false;
        vkCmdBeginQuery(c, occ_pool, slot, occ_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        if (stats) vkCmdBeginQuery(c, occ_stats_pool, slot, 0);
        cmd = c;
    }
    ~OccQuery() {
        if (!cmd) return;
        if (stats) vkCmdEndQuery(cmd, occ_stats_pool, slot);
        vkCmdEndQuery(cmd, occ_pool, slot);
        if (occ_skipped_here) occ_meta[slot].flags |= OCC_SKIPPED;
    }
};
namespace {
struct OccPending {
    uint64_t addr, lo, hi, value, key;
    int64_t present;
    uint64_t seq;
    bool logged;
};
std::deque<OccPending> occ_pending;   // (the replay thread's, in stream order)
std::unordered_map<uint64_t, int> occ_pending_at;     // (report address -> reports pending there)
std::unordered_map<uint64_t, uint64_t> occ_latest;    // (report address -> the seq of its latest report)
struct OccKeyed { uint64_t count; int64_t present; };
std::unordered_map<uint64_t, OccKeyed> occ_keyed;     // (mode 5: key -> its last measured count)
std::unordered_map<uint64_t, uint32_t> occ_key_ord;   // (mode 5: this frame's reports so far by program and window)
std::unordered_map<uint64_t, uint64_t> occ_last;   // (report address -> its last measured count)
FILE* occ_log = nullptr;
uint64_t occ_seq = 0;
int64_t occ_presents = 0;
// (every 600 presents: reports a frame, of them 0, and the 0s by what their windows held)
struct OccTally {
    int64_t reports = 0, zero = 0, empty = 0, unrecorded = 0, skipped = 0, mesh = 0, indirect = 0, plain = 0, draws = 0;
    int64_t unknown_write = 0, zero_write = 0;
} occ_t;
void occ_resolve(size_t n) {   // (the first n pending: their submissions are done)
    if (!n) return;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (size_t i = 0; i < n; i++) lo = std::min(lo, occ_pending[i].lo), hi = std::max(hi, occ_pending[i].hi);
    std::vector<uint64_t> res;   // (per slot: count, available)
    std::vector<uint64_t> st;    // (logged slots: clip, fs, [task, mesh], available)
    const uint32_t sw = occ_stats_mesh ? 5 : 3;
    const bool span_ok = hi > lo && hi - lo <= kOccRing / 2;
    if (span_ok) {
        res.assign((size_t)(hi - lo) * 2, 0);
        for (uint64_t at = lo; at < hi;) {
            const uint32_t first = (uint32_t)(at % kOccRing), cnt = (uint32_t)std::min<uint64_t>(hi - at, kOccRing - first);
            vkGetQueryPoolResults(dev.device, occ_pool, first, cnt, (size_t)cnt * 16, &res[(size_t)(at - lo) * 2], 16,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            at += cnt;
        }
        if (occ_log && occ_stats_pool) {
            st.assign((size_t)(hi - lo) * sw, 0);
            for (uint64_t at = lo; at < hi;) {
                const uint32_t first = (uint32_t)(at % kOccRing), cnt = (uint32_t)std::min<uint64_t>(hi - at, kOccRing - first);
                vkGetQueryPoolResults(dev.device, occ_stats_pool, first, cnt, (size_t)cnt * sw * 8, &st[(size_t)(at - lo) * sw],
                                      (VkDeviceSize)sw * 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                at += cnt;
            }
        }
    }
    static uint64_t logged_upto = 0;   // (draw lines written once)
    for (size_t i = 0; i < n; i++) {
        const OccPending& r = occ_pending[i];
        uint64_t sum = 0;
        int draws = 0, unrec = 0, skipped = 0, mesh = 0, indirect = 0;
        if (span_ok)
            for (uint64_t k = r.lo; k < r.hi; k++) {
                const uint64_t* v = &res[(size_t)(k - lo) * 2];
                const OccMeta& m = occ_meta[k % kOccRing];
                draws++;
                if (!v[1] || !(m.flags & OCC_BEGUN)) { unrec++; continue; }
                sum += v[0];
                if (m.flags & OCC_SKIPPED) skipped++;
                if (m.flags & OCC_MESH) mesh++;
                if (m.flags & OCC_INDIRECT) indirect++;
            }
        occ_last[r.addr] = sum;
        if (r.key) occ_keyed[r.key] = {sum, r.present};
        if (auto pa = occ_pending_at.find(r.addr); pa != occ_pending_at.end() && --pa->second <= 0) occ_pending_at.erase(pa);
        if (occ_mode() >= 4 && span_ok && occ_latest[r.addr] == r.seq) host->gpu_write(r.addr, &sum, 8);   // (over the fixed value)
        occ_t.reports++;
        occ_t.draws += draws;
        if (!sum) {
            occ_t.zero++;
            if (!draws) occ_t.empty++;
            else if (unrec == draws) occ_t.unrecorded++;
            else if (skipped) occ_t.skipped++;
            else if (mesh) occ_t.mesh++;
            else if (indirect) occ_t.indirect++;
            else occ_t.plain++;
        }
        if (occ_log && r.logged) {
            fprintf(occ_log, "R %llu p%lld addr %#llx win %llu %llu draws %d unrec %d skipped %d mesh %d indirect %d count %llu\n",
                    (unsigned long long)r.seq, (long long)r.present, (unsigned long long)r.addr, (unsigned long long)r.lo,
                    (unsigned long long)r.hi, draws, unrec, skipped, mesh, indirect, (unsigned long long)sum);
            if (span_ok)
                for (uint64_t k = std::max(r.lo, logged_upto); k < r.hi; k++) {
                    const uint64_t* v = &res[(size_t)(k - lo) * 2];
                    const OccMeta& m = occ_meta[k % kOccRing];
                    const uint64_t* q = st.empty() ? nullptr : &st[(size_t)(k - lo) * sw];
                    const bool qa = q && q[sw - 1] && (m.flags & OCC_STATS);
                    fprintf(occ_log, "D %llu prog %016llx flags %#x avail %d samples %llu clip %lld fs %lld task %lld mesh %lld\n",
                            (unsigned long long)k, (unsigned long long)m.prog, (unsigned)m.flags, (int)v[1], (unsigned long long)v[0],
                            qa ? (long long)q[0] : -1, qa ? (long long)q[1] : -1, qa && occ_stats_mesh ? (long long)q[2] : -1,
                            qa && occ_stats_mesh ? (long long)q[3] : -1);
                }
            logged_upto = std::max(logged_upto, r.hi);
        }
    }
    occ_pending.erase(occ_pending.begin(), occ_pending.begin() + (ptrdiff_t)n);
    if (occ_log) fflush(occ_log);
}
void occ_poll(uint64_t done) {
    size_t n = 0;
    while (n < occ_pending.size() && occ_pending[n].value <= done) n++;
    occ_resolve(n);
}
}  // namespace
/// (the replay thread, at every draw and dispatch of the stream) the ring's next chunk reset from the host before its first use
void occ_advance(uint64_t serial, uint64_t value) {
    if (!occ_mode()) return;
    std::call_once(occ_once, occ_init);
    if (!occ_pool) return;
    const uint32_t chunk = (uint32_t)((serial % kOccRing) / kOccChunk);
    occ_chunk_value[chunk] = std::max(occ_chunk_value[chunk], value);
    if (serial % kOccChunk) return;
    // (entering a chunk: its last lap's draws done and its reports read, then reset)
    const uint64_t last = occ_chunk_value[chunk];
    if (last && last != value) vk_timeline_wait(last);
    size_t n = 0;
    while (n < occ_pending.size() && occ_pending[n].lo + kOccRing < serial + kOccChunk) n++;
    if (n) {
        if (occ_pending[n - 1].value > vk_timeline_completed()) vk_timeline_wait(occ_pending[n - 1].value);
        occ_resolve(n);
    }
    vkResetQueryPool(dev.device, occ_pool, chunk * kOccChunk, kOccChunk);
    if (occ_stats_pool) vkResetQueryPool(dev.device, occ_stats_pool, chunk * kOccChunk, kOccChunk);
    for (uint32_t i = 0; i < kOccChunk; i++) occ_meta[chunk * kOccChunk + i].flags = 0;
    occ_chunk_value[chunk] = value;
}
/// (the replay thread, a samples-passed report in stream order) the value to write: fixed, or from the last measured count
uint64_t occ_report(uint64_t addr, uint64_t lo, uint64_t hi, uint64_t value, uint64_t fixed, uint64_t prog) {
    const int mode = occ_mode();
    if (!mode || !occ_pool) return fixed;
    const bool logged = occ_log && lo >= occ_log_lo.load(std::memory_order_relaxed) && hi <= occ_log_hi.load(std::memory_order_relaxed);
    uint64_t key = 0;
    if (mode == 5) {
        const uint64_t pk = prog * 0x9E3779B97F4A7C15ull ^ (hi - lo) * 0xC2B2AE3D27D4EB4Full;
        const uint32_t ord = occ_key_ord[pk]++;
        key = (pk ^ ((uint64_t)ord * 0x165667B19E3779F9ull)) | 1;
    }
    occ_pending.push_back({addr, lo, hi, value, key, occ_presents, occ_seq, logged});
    occ_latest[addr] = occ_seq++;
    occ_pending_at[addr]++;
    if (mode == 4) return fixed;   // (the count written once read, before a condition reads it)
    if (mode == 1) return fixed;
    if (mode == 5) {
        const auto k = occ_keyed.find(key);
        if (k == occ_keyed.end() || occ_presents - k->second.present > 3) {
            occ_t.unknown_write++;
            return fixed;
        }
        if (!k->second.count) occ_t.zero_write++;
        return k->second.count;
    }
    const auto it = occ_last.find(addr);
    if (it == occ_last.end()) {
        occ_t.unknown_write++;
        return fixed;
    }
    if (!it->second) occ_t.zero_write++;
    if (mode == 2) return it->second ? fixed : 0;
    return it->second;
}
/// (mode 4) a render condition at addr reads a report not read yet
bool occ_cond_pending(uint64_t addr) {
    return occ_mode() == 4 && (occ_pending_at.count(addr) || occ_pending_at.count(addr + 16));
}
/// (mode 4, after the GPU finished everything recorded so far) every pending report read and written
void occ_resolve_all() {
    occ_resolve(occ_pending.size());
}
/// (mode 4: no submit was possible there) the conditions read what is written (the fixed value); the counts come later
void occ_give_up() {
    static int told;
    if (told++ < 8) logf("vk: occlusion: a condition on a report not read yet could not wait for it (the fixed value read)");
    occ_pending_at.clear();
}
/// (the replay thread, logged frames) a render condition as it was decided
void occ_cond(uint64_t addr, int mode, uint64_t x, uint64_t y, bool on, uint64_t serial) {
    if (!occ_log || serial < occ_log_lo.load(std::memory_order_relaxed) || serial >= occ_log_hi.load(std::memory_order_relaxed)) return;
    fprintf(occ_log, "C %llu p%lld at %llu addr %#llx mode %d x %llu y %llu %s\n", (unsigned long long)occ_seq++, (long long)occ_presents,
            (unsigned long long)serial, (unsigned long long)addr, mode, (unsigned long long)x, (unsigned long long)y, on ? "on" : "off");
}
/// (the replay thread, at each present) reports whose submissions are done read; the logged frames opened and closed
void occ_present(int64_t presents, uint64_t serial, uint64_t done) {
    if (!occ_mode() || !occ_pool) return;
    occ_presents = presents;
    occ_key_ord.clear();
    occ_poll(done);
    if (presents % 600 == 0 && occ_keyed.size() > 200000) occ_keyed.clear();
    if (const char* dir = occ_log_dir()) {
        static const int64_t every = getenv("RX_VK_OCC_LOG_EVERY") ? std::max(4, atoi(getenv("RX_VK_OCC_LOG_EVERY"))) : 600;
        static const int64_t frames = getenv("RX_VK_OCC_LOG_FRAMES") ? std::clamp(atoi(getenv("RX_VK_OCC_LOG_FRAMES")), 1, 16) : 2;
        if (presents % every == 0) {
            if (occ_log) fclose(occ_log);
            char path[600];
            snprintf(path, sizeof path, "%s/occ_%lld.txt", dir, (long long)presents);
            occ_log = fopen(path, "w");
            occ_log_hi.store(UINT64_MAX), occ_log_lo.store(serial);
        } else if (presents % every == frames && occ_log) {
            occ_log_hi.store(serial);
        }
    }
    if (presents % 600 == 0 && occ_t.reports) {
        const double f = 600.0;
        logf("vk: occlusion: %.0f reports a frame, %.0f read 0 (%.0f with no draws, %.0f none recorded, %.0f a draw skipped, %.0f with mesh "
             "draws, %.0f indirect, %.0f plain draws); %.1f draws a report; written: %.0f zero, %.0f not yet measured",
             occ_t.reports / f, occ_t.zero / f, occ_t.empty / f, occ_t.unrecorded / f, occ_t.skipped / f, occ_t.mesh / f, occ_t.indirect / f,
             occ_t.plain / f, occ_t.reports ? (double)occ_t.draws / occ_t.reports : 0.0, occ_t.zero_write / f, occ_t.unknown_write / f);
        occ_t = {};
    }
}

// ---- (EvilOOT) NVN2's indirect mesh draws in Vulkan's layout ----
// NVN2 keeps an indirect mesh draw as {task count, first task} (as its direct DrawMeshTasks(first, count) and NVIDIA's
// mesh shading always did); vkCmdDrawMeshTasksIndirectEXT reads {x, y, z} workgroups. Read as they were, "7 tasks from 0"
// was 7 x 0 x 1: no work, so every GPU-built mesh draw (OoT's foliage, grass, falling leaves, fireflies) drew nothing even
// once its counts were right. Before each, outside the pass, its records go to a ring as {count, 1, 1} (a fill of 1s,
// then each record's count word copied over x). RX_VK_MESH_IND_COUNT_WORD=1 takes the second word for the count
// instead; RX_VK_MESH_IND_RAW=1 draws from the records as they are, as before. A first task other than 0 has no Vulkan
// counterpart (as for the direct draws: drawn from 0).
namespace {
constexpr uint64_t kMeshRing = 4ull << 20;
VkBuffer mesh_ring = VK_NULL_HANDLE;
VmaAllocation mesh_ring_mem = nullptr;
std::mutex mesh_ring_mu;
uint64_t mesh_ring_at = 0;
VkBuffer mesh_dbg = VK_NULL_HANDLE;   // (RX_VK_G3_MESHCOUNT)
VmaAllocation mesh_dbg_mem = nullptr;
void* mesh_dbg_ptr = nullptr;
thread_local VkBuffer mesh_conv_buf = VK_NULL_HANDLE;   // (the converted records for the draw in progress)
thread_local uint64_t mesh_conv_off = 0;
bool mesh_indirect_convert(VkCommandBuffer cmd, const DrawArgs& d) {
    mesh_conv_buf = VK_NULL_HANDLE;
    static const bool raw = getenv("RX_VK_MESH_IND_RAW") && atoi(getenv("RX_VK_MESH_IND_RAW")) != 0;
    static const uint32_t word = getenv("RX_VK_MESH_IND_COUNT_WORD") ? (uint32_t)atoi(getenv("RX_VK_MESH_IND_COUNT_WORD")) & 3 : 0;
    // (amd-fix: the rewrite with the firsts in the draw does it, RX_VK_MESH_ARGS; this one drew every draw from its first 0)
    static const bool rewrite = getenv("RX_VK_MESH_ARGS") && atoi(getenv("RX_VK_MESH_ARGS")) != 0;   // (sharkiix version: off, as upstream)
    if (raw || rewrite || !d.mesh || !d.indirect) return false;
    PROF_EXACT();   // (a pass ended and three barriers: not one of the cheap per-draw steps PROF_DRAW samples)
    const uint32_t n = (uint32_t)std::clamp(d.max, 1, 4096);
    const uint32_t stride = d.stride > 0 ? (uint32_t)d.stride : 8;
    const BufferRef args = gpu_buffer_bind(d.indirect, (uint64_t)(n - 1) * stride + 4 * (word + 1), false);
    if (!args) return false;
    uint64_t off;
    {
        std::lock_guard lk(mesh_ring_mu);
        if (!mesh_ring) {
            void* unused = nullptr;
            if (!create_buffer(kMeshRing, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false, mesh_ring,
                               mesh_ring_mem, &unused))
                return false;
        }
        const uint64_t bytes = ((uint64_t)n * 12 + 15) & ~15ull;
        if (mesh_ring_at + bytes > kMeshRing) mesh_ring_at = 0;   // (4 MB: frames of draws before it comes round)
        off = mesh_ring_at;
        mesh_ring_at += bytes;
    }
    end_pass(cmd);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &di);
    // (RX_VK_CHECKPOINTS: the conversion's fill and copy are transfer work between the game's culling dispatch and the mesh draws, ours: a
    // marker each, at the arguments' address, so a stall report shows whether the GPU was in them)
    checkpoint_mark(cmd, d.indirect, 's', 'C', n, stride);
    vkCmdFillBuffer(cmd, mesh_ring, off, (uint64_t)n * 12, 1u);
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &di);
    std::vector<VkBufferCopy> regions(n);
    for (uint32_t i = 0; i < n; i++) regions[i] = {args.offset + (uint64_t)i * stride + 4 * word, off + (uint64_t)i * 12, 4};
    checkpoint_mark(cmd, d.indirect, 'y', 'C', n, stride);
    vkCmdCopyBuffer(cmd, args.buffer, mesh_ring, n, regions.data());
    {   // (diagnosis, default off) RX_VK_G3_MESHCOUNT=1: the same counts into a host-visible twin of the ring, for mesh_count_note
        static const bool on = getenv("RX_VK_G3_MESHCOUNT") != nullptr;
        if (on) {
            if (!mesh_dbg && !create_buffer(kMeshRing, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, mesh_dbg, mesh_dbg_mem, &mesh_dbg_ptr)) mesh_dbg = VK_NULL_HANDLE;
            if (mesh_dbg) {
                std::vector<VkBufferCopy> r2(n);
                for (uint32_t i = 0; i < n; i++) r2[i] = {args.offset + (uint64_t)i * stride + 4 * word, off + (uint64_t)i * 12, 4};
                vkCmdCopyBuffer(cmd, args.buffer, mesh_dbg, n, r2.data());
            }
        }
    }
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, mb.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &di);
    mesh_conv_buf = mesh_ring, mesh_conv_off = off;
    return true;
}
/// (RX_VK_G3_MESHCOUNT=1, diagnosis) an indirect mesh draw of program key put its record at ring offset off: 2000 draws on, the count the GPU
/// wrote there is read, and every 4000 of them the programs with the most workgroups are logged (draws, mean, largest, sum a frame)
void mesh_count_note(uint64_t key, uint64_t off) {
    static std::mutex mu;
    static std::deque<std::pair<uint64_t, uint64_t>> q;
    struct Agg { uint64_t n = 0, sum = 0, most = 0; };
    static std::unordered_map<uint64_t, Agg> agg;
    static uint64_t reads = 0;
    std::lock_guard lk(mu);
    q.emplace_back(key, off);
    if (q.size() <= 2000 || !mesh_dbg_ptr) return;
    const auto [k, o] = q.front();
    q.pop_front();
    const uint32_t v = *(volatile const uint32_t*)((const uint8_t*)mesh_dbg_ptr + o);
    Agg& a = agg[k];
    a.n++, a.sum += v, a.most = std::max<uint64_t>(a.most, v);
    if (++reads % 4000 != 0) return;
    std::vector<std::pair<uint64_t, uint64_t>> top;
    for (const auto& [kk, aa] : agg) top.emplace_back(aa.sum, kk);
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 14; i++) {
        const Agg& aa = agg[top[i].second];
        logf("vk: mesh counts prog %016llx: %llu draws, %.1f workgroups mean, %llu most, %llu in all", (unsigned long long)top[i].second,
             (unsigned long long)aa.n, (double)aa.sum / aa.n, (unsigned long long)aa.most, (unsigned long long)aa.sum);
    }
    agg.clear();
}
} // namespace
// (hookshot) NVN's indirect mesh draws hold (count, first) and Vulkan's (x, y, z): given as they were, every one drew y = first = 0 workgroups,
// with no error. They are most of the game's vegetation: the forest's trees, ferns and grass (GPU culling writes their counts, so they can only
// be moved on the GPU): each count is copied into this buffer of (count, 1, 1) just before its draw. The GPU runs the copies and draws in
// order, so a slot used again is safe. A first other than 0 has no Vulkan counterpart.
constexpr uint32_t kMeshArgs = 1u << 18;   // (a ring: entries are reused 262144 draws on, frames after the GPU read them)
VkBuffer mesh_args_buffer = VK_NULL_HANDLE;
VmaAllocation mesh_args_alloc = nullptr;
uint32_t* mesh_args_words = nullptr;
uint64_t mesh_args_address = 0;   // (its device address: a task program reads its draw's first workgroup at + 12, + 16 per draw)
// (hookshot) Indirect mesh draws' arguments, 16 bytes each as Vulkan takes them with the first after: {count, 1, 1, first}. NVN's are
// {count, 0, 0, first}: as they lay, every foliage draw had no workgroups in y and z and drew nothing. Entry 0 stays {0, 1, 1, 0}: the
// first of draws that have none
bool mesh_args_ready() {
    static const bool ok = [] {
        void* mapped = nullptr;
        if (!create_buffer((uint64_t)kMeshArgs * 16,
                           VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true,
                           mesh_args_buffer, mesh_args_alloc, &mapped))
            return false;
        const auto discard = [] {
            destroy_buffer(mesh_args_buffer, mesh_args_alloc);
            mesh_args_buffer = VK_NULL_HANDLE;
            mesh_args_alloc = nullptr;
            mesh_args_words = nullptr;
            mesh_args_address = 0;
            return false;
        };
        if (!mapped) return discard();
        uint32_t* w = (uint32_t*)mapped;
        mesh_args_words = w;
        for (uint32_t i = 0; i < kMeshArgs; i++) w[i * 4] = 0, w[i * 4 + 1] = 1, w[i * 4 + 2] = 1, w[i * 4 + 3] = 0;
        VkBufferDeviceAddressInfo da{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        da.buffer = mesh_args_buffer;
        mesh_args_address = vkGetBufferDeviceAddress(dev.device, &da);
        if (!mesh_args_address) return discard();
        return true;
    }();
    return ok;
}
uint32_t mesh_args_take(uint32_t n) {   // (n entries of the ring, never entry 0; n below kMeshArgs: % 0 otherwise)
    static std::atomic<uint32_t> next{1};
    const uint32_t at = next.fetch_add(n) % (kMeshArgs - n);
    return at + 1;
}

/// A vertex stream bound earlier this submission, still the same buffer (a mirror made since, or a new submission, is not)
bool vertex_bind_reuse(uint64_t addr, uint64_t size, uint64_t stamp, uint64_t cached_addr, uint64_t cached_size,
                       uint64_t cached_stamp, int cached) {
    return cached && addr && size && addr == cached_addr && size == cached_size && stamp == cached_stamp;
}
struct KeptVertex {
    uint64_t addr = 0, size = 0, stamp = 0;
    BufferRef ref{};
    int ok = 0;
};
thread_local KeptVertex kept_vb[16];

// ---- (hookshot, issue 640) a GPU without mesh shaders: the task and mesh stages as compute dispatches and a
// generated vertex stage, drawn through an ordinary graphics pipeline. Records and payloads live in two buffers a
// recording thread reuses draw after draw (one pair, not one a program: see below); the commands are ordered, so each
// dispatch runs after the last draw that read them.
namespace {
// (one pair for every fallback program a recording thread draws, not a pair a program: 95 programs' pairs were ~2.2 GB of
// VRAM on an RX 5500M, and when VRAM ran out a program's pair could not be made and its draws stopped for good. Each draw
// clears the slots it runs and a barrier orders it after the draw before, so every program can use the same records.)
struct FallbackBuffers {
    VkBuffer records = VK_NULL_HANDLE, payload = VK_NULL_HANDLE;
    VmaAllocation records_mem = nullptr, payload_mem = nullptr;
    uint64_t records_address = 0, payload_address = 0;
    uint64_t records_bytes = 0, payload_bytes = 0;
};
// (grown pairs stay: a command buffer recorded before the growth may still read them)
TLS_OBJECT(fallback_buffers, std::vector<FallbackBuffers>)
thread_local VkBuffer fallback_draw_records = VK_NULL_HANDLE;   // (the last fallback_record's: its header is the draw's command)

uint64_t fallback_buffer_address(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(dev.device, &info);
}

bool fallback_buffers_of(const Program& p, FallbackBuffers*& out) {
    if (!p.fallback_vertices_per_workgroup || !p.fallback_record_words) return false;
    const uint64_t capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
    // (the header, the draw's command, the allocator and the mesh dispatch, and then a fixed area every program takes records
    // from, a primitive's corners at a time: only the primitives the mesh workgroups output, not a slot's worst case each.
    // Slots a primitive could fill were ~650 MB for the floor grass's ~13900 slots; it outputs ~17 MB)
    const uint64_t records = (uint64_t)(Shader::Backend::SPIRV::MESH_FALLBACK_HEADER_WORDS +
                                        Shader::Backend::SPIRV::MESH_FALLBACK_RECORD_WORDS) * 4;
    // (per thread, not per program: one program needing more than this is left out instead of taking the budget)
    if (records > (128ull << 20)) return false;
    // (the map, a slot a started mesh workgroup, and a payload a task workgroup, its threads' meshlets sharing it)
    const uint64_t threads = std::max(1u, p.fallback_task_threads);
    const uint64_t payload = p.fallback_task ? capacity * 4 + (capacity + threads - 1) / threads * (uint64_t)p.fallback_payload_words * 4 : 0;
    auto& sets = fallback_buffers();
    for (auto it = sets.rbegin(); it != sets.rend(); ++it) {
        if (it->records && it->records_bytes >= records && (!payload || (it->payload && it->payload_bytes >= payload))) {
            out = &*it;
            return true;
        }
    }
    FallbackBuffers f;
    void* mapped = nullptr;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    // (longshot #551: only the payload outgrown: the newest pair's records, which every program takes from, go with the bigger payload.
    // Made again, each growth left another 64 MB arena on the thread for the session, that no draw used again: 80-195 MB on a Deck)
    const bool shared = !sets.empty() && sets.back().records && sets.back().records_bytes >= records;
    const auto drop_records = [&] {
        if (!shared) destroy_buffer(f.records, f.records_mem);
    };
    if (shared) {
        const FallbackBuffers& last = sets.back();
        f.records = last.records, f.records_mem = last.records_mem;
        f.records_address = last.records_address, f.records_bytes = last.records_bytes;
    } else {
        // (the records' header is the vertex stage's indirect draw and the mesh stage's indirect dispatch)
        if (!create_buffer(records, usage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false, f.records, f.records_mem, &mapped)) return false;
        f.records_address = fallback_buffer_address(f.records);
        f.records_bytes = records;
        if (!f.records_address) {
            drop_records();
            return false;
        }
    }
    if (payload) {
        if (!create_buffer(payload, usage, false, f.payload, f.payload_mem, &mapped)) {
            drop_records();
            return false;
        }
        f.payload_address = fallback_buffer_address(f.payload);
        f.payload_bytes = payload;
        if (!f.payload_address) {
            destroy_buffer(f.payload, f.payload_mem);
            drop_records();
            return false;
        }
    }
    sets.push_back(f);
    out = &sets.back();
    return true;
}

bool fallback_compute_pipelines(Program& p) {
    static std::mutex mu;
    // (the programs whose compute pipeline the driver refused or faulted on, by key and variant: not tried again. Each try
    // was one more driver fault, on every draw of the program: 12,160 faults for one program in a Kokiri Forest run on
    // RADV, the frame down to 2-3 fps (#1001); on a GTX 1070 the replay stopped presenting after the first one. Left out,
    // the program costs its draws, as the first failure already did)
    static std::set<std::pair<uint64_t, uint64_t>> failed;
    {
        // (the lock covers the set and the program's pipelines, not the compiles: on the workers and in the precompile several
        // programs compile at once, which one lock held through AMD's seconds-long compiles made one at a time. One program is
        // built by one caller: fallback_pipelines_ready and precompile_run take its compute_state first)
        std::lock_guard lk(mu);
        if (!p.fallback_compute.empty()) return true;
        if (failed.count({p.key, p.variant})) return false;
    }
    std::vector<VkPipeline> made(p.stages.size(), VK_NULL_HANDLE);
    bool any = false;
    for (size_t i = 0; i < p.stages.size(); i++) {
        const Stage& st = p.stages[i];
        if (st.vk_stage != VK_SHADER_STAGE_COMPUTE_BIT) continue;
        any = true;
        VkPipelineShaderStageCreateInfo si{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        si.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        si.module = st.module;
        si.pName = "main";
        pin_subgroup(si);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = si;
        ci.layout = p.layout;
        if (dev.task_mesh_fallback || getenv("RX_VK_NOOPT"))   // (d1d6caa7: AMD's Windows compiler, issue 996)
            ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        if (const VkResult r = gated_compute(pcache, &ci, &made[i]); r != VK_SUCCESS) {
            {
                std::lock_guard lk(mu);
                failed.insert({p.key, p.variant});   // (#1317: not compiled again at every draw)
            }
            // (what failed and against which limits: a GTX 1080 Ti's black screen after the intro on v2.0.11 had only "could not be
            // made", the result not said. -77: the driver faulted. The workgroup and shared memory are the mesh stage's, against the GPU's)
            static std::atomic<int> told{0};
            if (told++ < 8)
                logf("vk: fallback: the compute pipeline of %016llx (stage %zu of %zu, workgroup %ux%ux%u, shared memory %u bytes, the GPU's "
                     "most %u bytes and %u invocations) could not be made: %d; its draws are left out",
                     (unsigned long long)p.key, i, p.stages.size(), p.workgroup[0], p.workgroup[1], p.workgroup[2], p.shared_memory,
                     dev.props.limits.maxComputeSharedMemorySize, dev.props.limits.maxComputeWorkGroupInvocations, (int)r);
            for (VkPipeline m : made)
                if (m) vkDestroyPipeline(dev.device, m, nullptr);
            return false;
        }
        pcache_built_since_save++;
    }
    if (!any) return false;
    std::lock_guard lk(mu);
    p.fallback_compute = std::move(made);
    return true;
}

/// (hookshot) A fallback program's compute pipelines, compiled on a worker as a compute program's are (see dispatch()): made in
/// the draw, AMD's compiler took up to 12 s on one, every recording thread stopped behind it, and the game froze in the first
/// room with foliage (RX 5500M). The draw that starts the build waits up to 50 ms for it, which covers NVIDIA's compiles; a draw
/// while it still compiles is left out, as a dispatch is. compute_state, which only compute programs use otherwise: 0 before, 1
/// while it compiles, 2 after (made, or refused and left out for good: fallback_compute_pipelines)
bool fallback_pipelines_ready(Program& p) {
    if (p.compute_state.load(std::memory_order_acquire) == 2) return !p.fallback_compute.empty();
    int none = 0;
    const bool fresh = p.compute_state.compare_exchange_strong(none, 1);
    if (fresh) {
        { ParLock lk; pcache_init(); }
        Program* job = &p;
        auto build = [job] {
            fallback_compute_pipelines(*job);
            job->compute_state.store(2, std::memory_order_release);
        };
        // (issue 496: one at a time on Intel, as compute programs are: serial_builds_on)
        if (serial_builds_on()) jobs_push_serial(std::move(build), true);
        else jobs_push(std::move(build), true);
    }
    const auto t0 = std::chrono::steady_clock::now();
    while (fresh && p.compute_state.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50))
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    if (p.compute_state.load(std::memory_order_acquire) != 2) {
        static std::atomic<int> told{0};
        if (told++ < 40) logf("vk: fallback draw of %016llx left out: its compute pipelines are still compiling", (unsigned long long)p.key);
        st_pending_skips++;
        return false;
    }
    return !p.fallback_compute.empty();
}

/// The dispatches of a mesh draw, outside any render pass, and the vertex count its graphics draw then covers.
bool fallback_record(VkCommandBuffer cmd, Program& p, const DrawArgs& d) {
    if (d.count_addr) {   // (more than one draw in the records: each its own dispatch, not one)
        static std::atomic<int> told{0};
        if (told++ < 2) logf("vk: fallback: a mesh draw with a count buffer is left out (issue 640)");
        return false;
    }
    if (dw().prog != &p) {   // (the descriptors built are another program's: draw() rebuilds them for a fallback draw)
        static std::atomic<int> told{0};
        if (told++ < 2) logf("vk: fallback: the descriptors built are not %016llx's: its draw is left out", (unsigned long long)p.key);
        return false;
    }
    FallbackBuffers* f = nullptr;
    if (!fallback_buffers_of(p, f) || !fallback_pipelines_ready(p)) return false;
    fallback_payload_address = f->payload_address;
    fallback_records_address = f->records_address;
    const uint32_t capacity = Shader::Backend::SPIRV::MESH_FALLBACK_CAPACITY;
    end_pass(cmd);
    // (the shared pair: the last fallback draw on it, of any program of this thread, has read it before it is reset)
    full_barrier(cmd);
    const uint32_t groups{(uint32_t)std::clamp(d.groups[0], 0, (int)capacity)};
    // (the records' header: the draw's command with no vertices and one instance, the allocator at 0, and the mesh dispatch
    // with no workgroups, its y and z 1. The records past it need nothing: the draw reaches only those taken this time; nor
    // the payload buffer: the mesh dispatch reads only the map entries and payloads its task wrote. Fills that do not overlap:
    // two over the same bytes have no order between them without a barrier)
    static_assert(Shader::Backend::SPIRV::MESH_FALLBACK_DISPATCH_WORD == 5 && Shader::Backend::SPIRV::MESH_FALLBACK_HEADER_WORDS == 8);
    vkCmdFillBuffer(cmd, f->records, 0, 4, 0);    // (vertexCount)
    vkCmdFillBuffer(cmd, f->records, 4, 4, 1);    // (instanceCount)
    vkCmdFillBuffer(cmd, f->records, 8, 16, 0);   // (firstVertex, firstInstance, the allocator, the dispatch's x)
    vkCmdFillBuffer(cmd, f->records, 24, 8, 1);   // (the dispatch's y and z)
    fallback_draw_records = f->records;
    full_barrier(cmd);
    if (!bind_descriptors(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p)) return false;
    push_constants(cmd, p, nullptr);
    size_t stages[2] = {p.stages.size(), p.stages.size()};
    for (size_t i = 0; i < p.stages.size(); i++) {
        if (p.stages[i].vk_stage != VK_SHADER_STAGE_COMPUTE_BIT) continue;
        if (stages[0] == p.stages.size()) stages[0] = i;
        else stages[1] = i;
    }
    const bool indirect{d.indirect && mesh_conv_buf != VK_NULL_HANDLE};
    auto send = [&](size_t stage, bool mesh_stage) {
        if (stage >= p.fallback_compute.size() || !p.fallback_compute[stage]) return false;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.fallback_compute[stage]);
        if (mesh_stage && p.fallback_task) {
            // (the mesh workgroups the task's started threads counted into the records' header, each its slot in the map:
            // direct or indirect, the CPU need not know the task count (DrawMeshTasksIndirect records none))
            checkpoint_work('x', 0, 1, 1);   // (the count is the GPU's)
            vkCmdDispatchIndirect(cmd, f->records, (VkDeviceSize)Shader::Backend::SPIRV::MESH_FALLBACK_DISPATCH_WORD * 4);
        } else if (d.indirect) {
            if (!indirect) {
                static std::atomic<int> told{0};
                if (told++ < 2) logf("vk: fallback: an indirect mesh draw with no converted arguments is left out");
                return false;
            }
            checkpoint_work('n', 0);
            vkCmdDispatchIndirect(cmd, mesh_conv_buf, mesh_conv_off);
        } else {
            const uint32_t most{p.fallback_task ? (uint32_t)std::max(1u, capacity / std::max(1u, p.fallback_task_threads))
                                                : groups};
            const uint32_t n{mesh_stage ? groups : std::min(groups, most)};
            if (!n) return true;
            checkpoint_work('x', n, 1, 1);
            vkCmdDispatch(cmd, n, 1, 1);
        }
        return true;
    };
    if (p.fallback_task) {
        if (!send(stages[0], false)) return false;
        full_barrier(cmd);
        if (!send(stages[1], true)) return false;
    } else {
        if (!send(stages[0], true)) return false;
    }
    full_barrier(cmd);   // (the records and the draw's command, which the draw reads as indirect arguments)
    return true;
}
} // namespace

/// The vertex streams already bound on this command buffer (a new recording, or a mirror made since, is not)
bool vertex_set_same(uint64_t stamp, const uint64_t* addr, const uint64_t* size, const uint32_t* stride,
                     uint64_t cached_stamp, const uint64_t* cached_addr, const uint64_t* cached_size,
                     const uint32_t* cached_stride, int on) {
    if (!on || stamp != cached_stamp) return false;
    for (int i = 0; i < 16; i++) {
        const bool used = i != 15 && addr[i];
        if (!used) {
            if (cached_addr[i] || cached_stride[i]) return false;
            continue;
        }
        const uint64_t sz = size[i] ? size[i] : 1;
        if (cached_addr[i] != addr[i] || cached_size[i] != sz || cached_stride[i] != stride[i]) return false;
    }
    return true;
}
/// (hookshot) The vertex bindings that differ from the ones last bound on this command buffer: first..first+count, count 0
/// when none does. Only those are bound again: the driver takes every binding it is handed, and all 16 were handed to it
/// whenever one stream changed, the unused ones with the zero buffer again (1.1 % of the replay thread on a Steam Deck,
/// in the driver). Nothing bound on this command buffer yet (valid false): all 16
void vertex_bind_range(const VkBuffer* b, const VkDeviceSize* o, const VkDeviceSize* z, const VkDeviceSize* s, const VkBuffer* lb,
                       const VkDeviceSize* lo, const VkDeviceSize* lz, const VkDeviceSize* ls, bool valid, uint32_t* first,
                       uint32_t* count) {
    if (!valid) {
        *first = 0, *count = 16;
        return;
    }
    uint32_t a = 16, e = 0;
    for (uint32_t i = 0; i < 16; i++)
        if (b[i] != lb[i] || o[i] != lo[i] || z[i] != lz[i] || s[i] != ls[i]) {
            if (a == 16) a = i;
            e = i + 1;
        }
    *first = a == 16 ? 0 : a, *count = a == 16 ? 0 : e - a;
}
/// The index buffer already bound on this command buffer (a new recording is not)
bool index_bind_same(uint64_t buffer, uint64_t offset, uint32_t type, uint64_t cached_buffer, uint64_t cached_offset,
                     uint32_t cached_type, int on) {
    return on && buffer && buffer == cached_buffer && offset == cached_offset && type == cached_type;
}
void bind_index(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkIndexType type) {
    const uint64_t b = (uint64_t)(uintptr_t)buffer;
    if (index_bind_same(b, (uint64_t)offset, (uint32_t)type, last_ib.buffer, last_ib.offset, last_ib.type, last_ib.on))
        return;
    vkCmdBindIndexBuffer(cmd, buffer, offset, type);
    last_ib = {b, (uint64_t)offset, (uint32_t)type, true};
}

/// (hookshot, a diagnostic for the temporal upscaler's motion: vk_temporal.cpp) RX_VK_DLSS_TRACE=<frames>: every that many frames, one frame's
/// draws into targets of a size the upscaler can take as the scene (temporal_scene_size), logged: the program, its targets' formats, the geometry and the uniform buffers
static void dlss_trace_draw(const NvnState& s, const Program& p, const DrawArgs& d, const Targets& at) {
    static const int every = getenv("RX_VK_DLSS_TRACE") ? atoi(getenv("RX_VK_DLSS_TRACE")) : 0;
    if (every <= 0 || !temporal_scene_size(at.width, at.height)) return;
    const uint64_t f = texture_frame_no();
    if (f % (uint64_t)every != (uint64_t)every / 2) return;
    char line[3072];
    int n = snprintf(line, sizeof line, "vk: dlss-trace f%llu prog %016llx%s%s %ux%u c[", (unsigned long long)f,
                     (unsigned long long)p.key, d.mesh ? " mesh" : "", p.fallback ? "(fb)" : "", at.width, at.height);
    for (int i = 0; i < at.n && n < 1800; i++) n += snprintf(line + n, sizeof line - n, "%s%d", i ? "," : "", (int)at.color_format[i]);
    n += snprintf(line + n, sizeof line - n, "] d%d %s n%d i%d bv%d inst%d ib%llx g%d,%d,%d ind%llx", (int)at.depth_format,
                  d.indexed ? "idx" : "arr", d.count, d.first, d.base_vertex, d.instances, (unsigned long long)d.ibuf,
                  d.groups[0], d.groups[1], d.groups[2], (unsigned long long)d.indirect);
    for (int v = 0; v < 4 && n < 1800; v++)
        if (s.vbuf[v].address) n += snprintf(line + n, sizeof line - n, " vb%d=%llx/%llu/s%u", v, (unsigned long long)s.vbuf[v].address,
                                             (unsigned long long)s.vbuf[v].size, s.vstream[v].stride);
    n += snprintf(line + n, sizeof line - n, " va");
    for (int i = 0; i < s.nvattr && i < 16 && n < 1800; i++) {
        const nvn_va_st a = *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]);
        n += snprintf(line + n, sizeof line - n, "%s%x@%u:%u", i ? "," : "=", a.fmt, a.offset, a.stream);
    }
    for (const Stage& st : p.stages) {
        const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        n += snprintf(line + n, sizeof line - n, " st%d:", ns);
        for (int u = 0; u < 16 && n < 1900; u++)
            if (s.ubo[ns][u].addr) n += snprintf(line + n, sizeof line - n, " u%d=%llx/%llu", u, (unsigned long long)s.ubo[ns][u].addr,
                                                 (unsigned long long)s.ubo[ns][u].size);
        for (int u = 0; u < 16 && n < 1900; u++)
            if (s.ssbo[ns][u].addr) n += snprintf(line + n, sizeof line - n, " s%d=%llx/%llu", u, (unsigned long long)s.ssbo[ns][u].addr,
                                                  (unsigned long long)s.ssbo[ns][u].size);
    }
    logf("%s", line);
}

/// (hookshot) A skinned draw into the scene, for the temporal upscaler's actor motion (vk_temporal.cpp: DLSS's and FSR's). OoT's characters and the things that
/// move by themselves are indexed draws of one vertex stream: the position at attribute 0 (RGB32F or RGBA16F), the weights at 4
/// (and 5, RGBA8), the joints at 6 (and 7, RGBA16UI or RGBA8UI), the bone palette in the vertex stage's uniform buffer 6 (NVN's
/// binding 3: 3 x 4 matrices, a whole number of them) and the object's origin in its buffer 4 (binding 1, at 0x40)
static void skin_check(const NvnState& s, const Program& p, const DrawArgs& d, const Targets& at) {
    if (d.mesh || !d.indexed || d.indirect || d.instances != 1 || !at.n || !at.color[0] || !at.depth ||
        !temporal_scene_size(at.width, at.height) || !temporal_scene_format(at.color_format[0]) || s.nvattr < 7 ||
        !temporal_wanted())
        return;
    const auto attr = [&](int i) { return *reinterpret_cast<const nvn_va_st*>(&s.vattr[i]); };
    const nvn_va_st pos = attr(0);
    if ((pos.fmt != NVN_FORMAT_RGB32F && pos.fmt != NVN_FORMAT_RGBA16F) || pos.stream != 0) return;
    const nvn_p_bind_buf& bones = s.ubo[0][3];
    if (!bones.addr || bones.size < 48 || bones.size % 48) return;
    SkinnedDraw k;
    k.formats = pos.fmt == NVN_FORMAT_RGBA16F ? dlss_actor::kHalfPosition : 0;
    for (int set = 0; set < 2 && 6 + set < s.nvattr; set++) {
        const nvn_va_st w = attr(4 + set), j = attr(6 + set);
        if (w.fmt != NVN_FORMAT_RGBA8 || (j.fmt != NVN_FORMAT_RGBA16UI && j.fmt != NVN_FORMAT_RGBA8UI) || w.stream || j.stream ||
            w.offset % 4 || j.offset % 4)
            break;
        if (j.fmt == NVN_FORMAT_RGBA8UI) k.formats |= dlss_actor::kByteJoints << set;
        k.weights[set] = w.offset, k.joints[set] = j.offset, k.sets = set + 1;
    }
    const uint32_t stride = s.vstream[0].stride;
    if (!k.sets || !stride || stride % 4 || pos.offset % 4 || !s.vbuf[0].address) return;
    k.depth = at.depth, k.vbuf = s.vbuf[0].address, k.vsize = s.vbuf[0].size, k.stride = stride, k.pos = pos.offset;
    k.ibuf = d.ibuf, k.itype = d.itype, k.count = d.count, k.base_vertex = d.base_vertex;
    k.bones = bones.addr, k.bones_size = bones.size, k.object = s.ubo[0][1].addr, k.object_size = s.ubo[0][1].size;
    k.program = p.key;
    temporal_note_skinned_draw(k);
}

// ---- (TICK-061, F2) RX_VK_OCC_PROXY_SKIP=1 (off by default): draws that cannot change anything, left out of the command buffer ----
// OoT starts each frame with ~900 occlusion proxy tests: a draw between a samples-passed reset and its report. With the default
// occlusion (RX_VK_OCC_REAL off, RX_VK_OCC_FIXED >= 0) the report's count is the fixed value, written on the CPU in stream order
// (vk_replay.cpp NOP_REPORT_COUNTER), whatever the draws did, and the render conditions read those words: the draws' own GPU
// work is not observed. A draw is left out here only when it provably writes nothing and reads nothing a later command depends on:
// - no colour write: every bound colour target's write mask is 0 (target_write_mask: the same masks the pipeline and the dynamic
//   state use; no fragment stage writes no colour);
// - no depth write (no depth target, or its write off) and no stencil test (no stencil aspect, or the test off: then it writes no
//   stencil whatever its ops);
// - its program binds no storage buffer, storage image, image buffer, texture or texture buffer in any stage, touches no global
//   memory, and is neither a mesh nor a mesh fallback program (program_inert): a shader with no such resource has no memory to
//   write, and with no texture it samples nothing (so draw_hazards has no hazard to find for it and adds nothing to the pass);
// - no diagnosis that watches draws (dumps).
// What it keeps, so that the command buffer's structure and every texture's state stay as they were: everything before the
// draw's own commands (the program pick, the pipeline lookup and its not-ready rule: a draw whose pipeline is not ready is left out
// earlier, as before), and begin_pass: the pass
// ends and begins where it did, with the same barrier and the same texture_written / texture_rendered marks of its targets, and
// the pass counts the draw (pass().draws: a clear after it still gets a pass of its own). What it leaves out: the draw's
// descriptor build and bind, pipeline bind, push constants (and its NV checkpoint marker), dynamic state, vertex / index binds and
// the vkCmdDraw. The caches that skip repeated binds (last_pipeline, the descriptor reuse, dynamic state, vertex sets) describe
// what the command buffer has bound, and are left as the last recorded draw set them, which is what it still has bound.
bool occ_proxy_skip_on() {
    static const bool on = [] {
        const char* e = getenv("RX_VK_OCC_PROXY_SKIP");
        const char* fixed = getenv("RX_VK_OCC_FIXED");
        const bool v = e && atoi(e) != 0 && occ_mode() == 0 && (!fixed || atoll(fixed) >= 0);
        logf("vk: draws that write nothing and sample nothing: %s (RX_VK_OCC_PROXY_SKIP)", v ? "left out (their pass kept)" : "recorded, as before");
        return v;
    }();
    return on;
}
// (the answer kept by the program's key and variant, each thread its own: not in Program, whose size is part of the program
// cache's stamp (vk_shader.cpp disk_stamp: sizeof(Program)); a field there made every program cache on disk start again.
// A key and variant name one translation, so the same stages and the same answer)
bool program_inert(const Program& p) {
    thread_local std::unordered_map<uint64_t, bool> known;
    const uint64_t id = p.key ^ (p.variant * 0x9E3779B97F4A7C15ull) ^ (p.compute ? 1 : 0);
    if (const auto it = known.find(id); it != known.end()) return it->second;
    bool inert = !p.compute && !p.mesh && !p.fallback && !p.stages.empty();
    for (const Stage& st : p.stages) {
        const Shader::Info& i = st.info;
        if (!i.storage_buffers_descriptors.empty() || !i.image_descriptors.empty() || !i.image_buffer_descriptors.empty() ||
            !i.texture_descriptors.empty() || !i.texture_buffer_descriptors.empty() || i.stores_global_memory || i.uses_global_memory ||
            i.uses_atomic_image_u32 || i.num_global_tables)
            inert = false;
    }
    if (known.size() > 65536) known.clear();
    known.emplace(id, inert);
    return inert;
}
bool draw_writes_nothing(const Program& p, const NvnState& s, const Targets& at) {
    for (int i = 0; i < at.n; i++)
        if (at.color[i] && target_write_mask(p, s, i)) return false;
    if (at.depth && s.ds.dw) return false;
    if (at.depth && has_stencil(at.depth_format) && s.ds.st) return false;
    return true;
}
std::atomic<int64_t> st_occ_proxy_skips{0};
int64_t occ_proxy_skips() { return st_occ_proxy_skips.load(std::memory_order_relaxed); }

/// (RX_VK_HDR_FIND=1, for finding the game's tone-mapping pass: the first draw of each program that samples a floating-point scene
/// image (B10G11R11, RGB9E5, RGBA16F) and writes an 8-bit or 10-bit colour image, said once, with its target, what it samples and
/// the sizes of its uniform buffers. The key and the inputs are all that is kept of it)
static void hdr_find(const NvnState& s, const Program& p, const Targets& at) {
    static const bool on = getenv("RX_VK_HDR_FIND") != nullptr;
    if (!on || p.mesh || !at.n || !at.color[0]) return;
    const VkFormat f = at.color_format[0];
    if (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB && f != VK_FORMAT_B8G8R8A8_UNORM &&
        f != VK_FORMAT_B8G8R8A8_SRGB && f != VK_FORMAT_A2B10G10R10_UNORM_PACK32)
        return;
    bool scene = false;
    for (const Texture* t : gfx_sampled())
        scene |= t->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || t->format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 ||
                 t->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!scene) return;
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() >= 64 || !seen.insert(p.key ^ ((uint64_t)f << 56)).second) return;
    char line[900];
    int n = snprintf(line, sizeof line, "vk: HDR find: program %016llx (frame %llu) writes %ux%u vk %d (c0 #%u), targets %d, samples",
                     (unsigned long long)p.key, (unsigned long long)texture_frame_no(), at.width, at.height, (int)f, at.color[0]->id, at.n);
    for (const Texture* t : gfx_sampled())
        if (n < 600) n += snprintf(line + n, sizeof line - n, " #%u %ux%u vk %d;", t->id, t->width, t->height, (int)t->format);
    n += snprintf(line + n, sizeof line - n, " uniform buffers:");
    for (const Stage& st : p.stages) {
        const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        for (int i = 0; i < 16 && n < 800; i++)
            if (s.ubo[ns][i].addr && i + 3 < Shader::Info::MAX_CBUFS && st.info.constant_buffer_used_sizes[i + 3])
                n += snprintf(line + n, sizeof line - n, " s%d.c%d %#llx+%#llx (used %u);", ns, i + 3, (unsigned long long)s.ubo[ns][i].addr,
                              (unsigned long long)s.ubo[ns][i].size, (unsigned)st.info.constant_buffer_used_sizes[i + 3]);
    }
    logf("%s", line);
}

void draw(VkCommandBuffer cmd, NvnState& s, const DrawArgs& d) {
    Pass& ps = pass();   // (hookshot, once: the accessor is a call and a thread-local read at each use (__tls_get_addr in the shared library). The same object for the thread's life: draw_begin_recording resets it in place)
    if (par_stats_on) par_stats_draw();
    PROF_HOT(PROF_DRAW);
    FINE_N(FC_DRAWS, 1);
    stats_tick();
    mesh_indirect_convert(cmd, d);   // (EvilOOT: before the pass opens; sets mesh_conv_buf for the mesh draw below)
    VkPrimitiveTopology topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (!d.mesh && !topology(d.prim, topo)) {
        static int logged[32];
        if (d.prim >= 0 && d.prim < 32 && logged[d.prim]++ < 2) logf("vk: draws of NVN primitive %d are left out", d.prim);
        SKIP_HERE();
    }
    const Targets at = bare_targets(s, targets(s));
    if (!at.width || !at.height) SKIP_HERE();
    // (hookshot) the scene's camera and depth for the temporal upscalers: NVIDIA DLSS and AMD FSR 4 (vk_temporal.cpp) and AMD
    // FSR 3.1 (vk_fsr3_inputs.cpp). The vertex stage's uniform buffer 3 (NVN's binding 0)
    if (at.n && at.color[0] && at.depth) {
        temporal_note_scene_draw(at.color[0], at.width, at.height, at.depth, s.ubo[0][0].addr, s.ubo[0][0].size);
        fsr3_inputs::note_scene_draw(at.color[0], at.width, at.height, at.depth, s.ubo[0][0].addr, s.ubo[0][0].size);
    }
    uint8_t types[8];
    for (int i = 0; i < 8; i++) {
        const VkFormat f = i < at.n ? at.color_format[i] : VK_FORMAT_UNDEFINED;
        switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            types[i] = (uint8_t)Shader::AttributeType::UnsignedInt;
            break;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32B32A32_SINT:
            types[i] = (uint8_t)Shader::AttributeType::SignedInt;
            break;
        default: types[i] = (uint8_t)Shader::AttributeType::Float; break;
        }
    }
    const Program* cp;
    {
        PROF_HOT(PROF_PROGRAM);
        FINE_SCOPE(FT_PROGRAM);
        ParLock lk(d.mesh);   // (graphics programs: each thread's picks, program_pick's own lock)
        cp = program_for(s, d.mesh ? ProgramKind::Mesh : ProgramKind::Graphics, types);
    }
    if (cp) zone_note_program(cp->key);   // (the zone recorder's set lookup: on the program pick's own path, once a key per map)
    {   // (hookshot, a test) RX_VK_NO_MESH=1: task / mesh draws left out, to see what they draw; =2: the others left out
        static const int no_mesh = getenv("RX_VK_NO_MESH") ? atoi(getenv("RX_VK_NO_MESH")) : 0;
        if ((no_mesh == 1 && d.mesh) || (no_mesh == 2 && !d.mesh)) return;
    }
    if (!cp) {   // (hookshot) which draw has no program, 12 times: its stages' native code keys
        static std::atomic<int> told{0};
        if (told++ < 12) logf("vk: a draw without a program (frame %llu); its code:%s", (unsigned long long)texture_frame_no(),
                              program_code_keys(s).c_str());
        SKIP_HERE();
    }
    if (skip_live(cp->key)) return;   // (EvilOOT, RX_VK_SKIP_FILE)
    {   // (hookshot, RX_VK_SKIP_PROG=<key prefix,...>: draws too, their native code keys logged once; =-: none)
        // (OoT's 53509044401d78d2, by default only while its targets have no view: 128 draws, once while an area loads, that render
        // the slices of two 64^3 BC4 noise volumes through raw RGBA16UI views. Those views were not made (no attachment usage) and
        // the pass ran over no target at the texels' 64 x 64: on about half the first starts the device was lost (7 of 14), and it
        // was left out always. Left out, the volumes stayed zero and the foliage's LOD crossfade left trees bare at mid distance
        // (#1151). Where the driver takes the usage (vk_texture.cpp) the views are made and the pass is sized in blocks
        // (targets()); it runs there, and is still left out where they are not)
        static const bool default_list = !getenv("RX_VK_SKIP_PROG");
        if (default_list && cp->key == 0x53509044401d78d2ull && !at.n) {
            static std::atomic<int> told{0};
            if (told++ < 2)
                logf("vk: draws of program %016llx left out: its targets have no view (RX_VK_SKIP_PROG=- runs them anyway)",
                     (unsigned long long)cp->key);
            return;
        }
        // (hookshot, issues 6 and 7, 2026-11-14) OoT's 2b37a33d189014a8 and 0948a49a3c3068a8: two small box draws (24 indices) into a
        // half-size HDR target that read the 128 x 72 x 96 volume textures, every frame. On an RTX 5070 Ti the device was lost as
        // soon as Link entered water, and both of a player's reports named these two as the only draws in flight both times, the
        // second the last one begun (NV checkpoints, knoch23, v0.6.6). On this RTX 3060 Ti they never hang, but they are wrong
        // too: in the Market fountain Link's skin, hair and belt came out white (hs2864), and normal without them (hs2865).
        // Standing in the Market and Kokiri Forest without them, no difference beyond the anti-aliasing's jitter (hs2866 /
        // hs2867). Left out until what they compute is translated right; RX_VK_FOG_BOXES=1 runs them as before
        static const bool fog_boxes = getenv("RX_VK_FOG_BOXES") && atoi(getenv("RX_VK_FOG_BOXES")) != 0;
        if (!fog_boxes && (cp->key == 0x2b37a33d189014a8ull || cp->key == 0x0948a49a3c3068a8ull)) {
            static std::atomic<int> told{0};
            if (told++ < 2)
                logf("vk: draws of program %016llx left out: a GPU hang in water on RTX 50 cards, Link white in water elsewhere "
                     "(RX_VK_FOG_BOXES=1 runs them)", (unsigned long long)cp->key);
            return;
        }
        // (the list parsed once into key prefixes: it was a snprintf and a string compare on every draw)
        static const std::vector<std::pair<uint64_t, uint64_t>> skip_keys = [] {   // (value, mask) of each prefix
            std::vector<std::pair<uint64_t, uint64_t>> v;
            const char* list = getenv("RX_VK_SKIP_PROG") && strcmp(getenv("RX_VK_SKIP_PROG"), "-") ? getenv("RX_VK_SKIP_PROG") : "";
            for (const char* q = list; *q;) {
                const char* e = q;
                while (*e && *e != ',') e++;
                const size_t n = std::min<size_t>((size_t)(e - q), 16);
                if (n) {
                    const uint64_t value = strtoull(std::string(q, n).c_str(), nullptr, 16) << (4 * (16 - n));
                    const uint64_t mask = n == 16 ? ~0ull : ~(~0ull >> (4 * n));
                    v.push_back({value, mask});
                }
                q = *e ? e + 1 : e;
            }
            return v;
        }();
        for (const auto& [value, mask] : skip_keys)
            if ((cp->key & mask) == value) {
                static std::atomic<int> told{0};
                if (told++ < 8) logf("vk: draws of program %016llx left out (RX_VK_SKIP_PROG); its code:%s", (unsigned long long)cp->key,
                                     program_code_keys(s).c_str());
                return;
            }
    }
    {   // (hookshot, RX_VK_SKIP_CODE=<code key prefix,...>: the draws of every program with a stage of that native code left out,
        // decided once a program. RX_VK_SKIP_PROG names whole programs; a shared stage is in many. On the Steam Deck (RADV VANGOGH)
        // the GPU runs for seconds in the task/mesh programs that share the task shader 2c56dc1006942f74, a nested loop over a 2D
        // grid (a culling pass), until the kernel resets it at 10 s, in the Great Deku Tree and Kokiri Forest (issue 817). About
        // 30 programs use it; RX_VK_SKIP_CODE=2c56dc1006942f74 leaves them out and the Deck plays without the hang, but without
        // the models they draw too: a workaround, opt-in)
        static const char* skip_code_list = getenv("RX_VK_SKIP_CODE");
        if (skip_code_list && *skip_code_list) {
            static std::mutex memo_mu;
            static std::unordered_map<uint64_t, bool> memo;
            bool skip;
            {
                std::lock_guard lk(memo_mu);
                const auto it = memo.find(cp->key);
                if (it != memo.end()) {
                    skip = it->second;
                } else {
                    const std::string codes = program_code_keys(s);
                    skip = skip_code::matches(codes, skip_code_list);
                    memo.emplace(cp->key, skip);
                    static int told = 0;
                    if (skip && told++ < 32)
                        logf("vk: draws of program %016llx left out (RX_VK_SKIP_CODE); its code:%s", (unsigned long long)cp->key,
                             codes.c_str());
                }
            }
            if (skip) return;
        }
    }
    // Not ready (translating, or its pipeline building): left out of the frame when its targets hold an earlier
    // frame's picture already, which shows for a frame without it; waited for otherwise (a pass drawn once, a target
    // made this frame), as totk-pc-recomp's TOTK_PIPE_SKIP rule. RX_VK_PIPE_WAIT=1: always waited for.
    // (waited for by default: a draw left out showed as missing geometry in races, where many programs are new;
    // RX_VK_PIPE_SKIP=1: left out when its targets hold an earlier frame's picture, as before)
    // (hookshot: left out by default. Waiting, Ocarina froze 0.2-3 s at each area's first new programs, the most reported stutter;
    // RX_VK_PIPE_SKIP=0: waited for, as mkw-recomp needs)
    static const bool always_wait = getenv("RX_VK_PIPE_SKIP") && atoi(getenv("RX_VK_PIPE_SKIP")) == 0;
    // (hookshot, issue 47: and only where the next frame draws it again, targets_redrawn. RX_VK_SKIP_PARTS=1: as before, any draw
    // into targets that hold an earlier frame's picture)
    static const bool skip_parts = getenv("RX_VK_SKIP_PARTS") && atoi(getenv("RX_VK_SKIP_PARTS")) != 0;
    const bool persistent = targets_persistent(at) && (skip_parts || targets_redrawn(at, s));
    const bool may_skip = !always_wait && persistent;
    auto wait_for = [&](const std::atomic<bool>& ready, bool shader) {
        PROF_EXACT();
        const auto t0 = std::chrono::steady_clock::now();
        const bool fresh = jobs_wait(ready);
        st_waits++;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        st_wait_ms += ms;
        if (shader) st_shader_wait_ms += ms;
        return fresh;
    };
    // (hookshot) The storm hold: while the translation queue is still hundreds deep — a cold area's arrival, its fade
    // rewriting the texture handles every game frame, each frame's state its own variant — this thread's draws are
    // left out, and the frames show the area's earlier picture, as a draw left out always does. The frames the storm
    // itself drew sampled the fade's transient words (white/cyan blowouts, green bands — the picture regression that
    // 9c42f5e7's render-instead-of-stall unmasked); the queue's depth is the churn's own clock, so the first presents
    // past the hold sample the state by then settled. The waits below still run, and still steal: it is what drains
    // the queue at the pace the storm always had. Engaged above RX_VK_STORM_HOLD jobs (200), released below half of
    // that: the queue saws with each frame's pushes, and a hold that flickered with it drew the churn's transient
    // frames anyway. 0: off, drawn as before
    static const int hold_deep = getenv("RX_VK_STORM_HOLD") ? atoi(getenv("RX_VK_STORM_HOLD")) : 0;   // (off by default since v0.4.3: the steal it covers for is off; RX_VK_STORM_HOLD=200 with RX_VK_JOB_STEAL=1)
    static const int64_t hold_off = (int64_t)std::max(1, hold_deep / 2);
    thread_local bool hold_on = false;
    const int64_t pending_jobs = jobs_pending();
    if (hold_deep <= 0) hold_on = false;
    else if (hold_on) {
        if (pending_jobs < hold_off) hold_on = false;
    } else if (pending_jobs >= (int64_t)hold_deep) hold_on = true;
    static std::atomic<int64_t> st_storm_holds{0};
    if (hold_on && cp->ready.load(std::memory_order_acquire)) {
        st_pending_skips++;
        st_storm_holds.fetch_add(1, std::memory_order_relaxed);
        if (st_storm_holds.load(std::memory_order_relaxed) < 8 ||
            st_storm_holds.load(std::memory_order_relaxed) % 2000 == 0)
            logf("vk: a translation storm: %d jobs queued, the frame's draws left out until it drains",
                 jobs_pending());
        return;
    }
    if (!cp->ready.load(std::memory_order_acquire)) {
        if (may_skip) return void(st_pending_skips++);
        // (hookshot) The translation finished in this draw's own wait: stolen to run here (9c42f5e7) or a worker seen
        // finishing it just then. Its variant's answers were read mid-storm — a cold area's arrival fades textures in and
        // out and rewrites the handles every frame — and what the frames of the storm drew from them showed as white/cyan
        // blowouts and green bands, the picture regression of #1714's unmasked race. The draw is left out of this frame, as
        // a draw whose program was not ready always was; the frame itself keeps the steal's win (the translation ran, the
        // queue advanced, no 6-12 s stall), and the next frame's pick re-validates the variant against the state by then.
        // RX_VK_STOLEN_SKIP=0: drawn at once, as 9c42f5e7 alone
        static const bool stolen_skip = !getenv("RX_VK_STOLEN_SKIP") || atoi(getenv("RX_VK_STOLEN_SKIP")) != 0;
        const bool fresh = wait_for(cp->ready, true);
        if (stolen_skip && fresh) {
            st_pending_skips++;
            st_stolen_skips.fetch_add(1, std::memory_order_relaxed);
            if (st_stolen_skips.load(std::memory_order_relaxed) < 12 ||
                st_stolen_skips.load(std::memory_order_relaxed) % 1000 == 0)
                logf("vk: a draw waited out its own translation (program %016llx): left out for this frame",
                     (unsigned long long)cp->key);
            return;
        }
    }
    if (exposure_log_on() && cp) exposure_note(s, *cp);
    if (cp->failed) SKIP_HERE();
    Program& p = const_cast<Program&>(*cp);
    if (p.fallback) topo = p.fallback_topology;   // (the mesh program's points, lines or triangles)
    PipeEntry* pe;
    {
        PROF_HOT(PROF_PIPE);
        if (!p.layout) {
            PROF_EXACT();
            ParLock lk;
            ensure_layout(p);
        }
        FINE_SCOPE(FT_PIPELINE);
        pe = graphics_pipeline(p, s, at, topo, types);
    }
    if (!pe->ready.load(std::memory_order_acquire)) {
        // (TICK-008) A pipeline still being built: the draw is left out of the frame, except into targets made this frame (a shadow
        // map, a new effect target), which wait as before so the picture never lacks it (user's choice, 2026-11-09: no flicker on a
        // first visit). RX_VK_PIPE_WAIT_FRESH=0: those are left out too (counted: st_pipe_fresh_skips); RX_VK_PIPE_SKIP=0: every draw waits
        static const bool wait_fresh = !getenv("RX_VK_PIPE_WAIT_FRESH") || strcmp(getenv("RX_VK_PIPE_WAIT_FRESH"), "0") != 0;
        if (may_skip || !(always_wait || wait_fresh)) {
            st_pending_skips++;
            if (!may_skip) st_pipe_fresh_skips.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        wait_for(pe->ready, false);
    }
    const VkPipeline pl = pe->pipeline;
    if (!pl) SKIP_HERE();
    cur_program = &p;
    dlss_trace_draw(s, p, d, at);
    skin_check(s, p, d, at);
    if (!record_progs().empty() && std::find(record_progs().begin(), record_progs().end(), p.key) != record_progs().end())
        record_draw(s, p, d, at);
    if (occ_proxy_skip_on() && !dump_on() && !d.mesh && !(program_inert(p) && draw_writes_nothing(p, s, at))) {
        // (TICK-061, diagnosis: the first 24 programs a frame draws most whose draws F2 keeps, and why, logged once each)
        // (each thread's own count, no lock: the log is once a program on the thread that reaches 2000 first, 24 at most)
        thread_local std::unordered_map<uint64_t, int64_t> seen;
        if (seen.size() < 4096 && ++seen[p.key] == 2000) {
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 24) {
                int sb = 0, img = 0, tex = 0, tb = 0, gm = 0, fs = 0;
                for (const Stage& st : p.stages) {
                    sb += (int)st.info.storage_buffers_descriptors.size(), img += (int)(st.info.image_descriptors.size() + st.info.image_buffer_descriptors.size());
                    tex += (int)st.info.texture_descriptors.size(), tb += (int)st.info.texture_buffer_descriptors.size();
                    gm += st.info.stores_global_memory || st.info.uses_global_memory;
                    fs += st.vk_stage == VK_SHADER_STAGE_FRAGMENT_BIT;
                }
                uint32_t wm = 0;
                for (int i = 0; i < at.n; i++) wm |= (at.color[i] ? target_write_mask(p, s, i) : 0u) << (4 * i);
                logf("vk: F2 keeps the draws of program %016llx (2000 so far): storage %d, images %d, textures %d, texel buffers %d, "
                     "global memory %d, fragment stage %d; colour masks %#x, depth %d write %d, stencil test %d", (unsigned long long)p.key,
                     sb, img, tex, tb, gm, fs, wm, at.depth ? 1 : 0, (int)s.ds.dw, (int)(at.depth && has_stencil(at.depth_format) && s.ds.st));
            }
        }
    }
    if (!d.mesh && occ_proxy_skip_on() && !dump_on() && program_inert(p) && draw_writes_nothing(p, s, at)) {   // (TICK-061, F2)
        {
            // (draw_hazards is not called: for this draw it does nothing. Its writes are 0 by draw_writes_nothing, and what it
            // samples (gfx_sampled) is empty in the old path: its program has no texture, so its own descriptor build set it
            // empty, or it reused the descriptors of the same program, whose build set it empty. With no writes and nothing
            // sampled draw_hazards neither logs nor ends the pass. Here gfx_sampled still holds the last recorded draw's, which is
            // what a later draw reusing that draw's descriptors needs, so it is neither read nor changed)
            FINE_SCOPE(FT_HAZARD);
            begin_pass(cmd, at);
        }
        ps.prog = p.key;
        if (!ps.first_prog) ps.first_prog = p.key;
        st_draws++, ps.draws++;
        if (const int64_t k = st_occ_proxy_skips.fetch_add(1, std::memory_order_relaxed) + 1; k % 200000 == 0)
            logf("vk: draws left out, writing and sampling nothing (RX_VK_OCC_PROXY_SKIP): %lld so far, frame %llu", (long long)k,
                 (unsigned long long)texture_frame_no());
        return;
    }
    // (RX_VK_DUMP_EACH=<key>,...: in a dump, the pass ends after each draw of those programs, so every one of them is
    // read back on its own)
    struct EachGuard {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        ~EachGuard() { if (cmd) end_pass(cmd); }
    } each_guard;
    {
        static const std::vector<uint64_t> each = [] {
            std::vector<uint64_t> v;
            if (const char* e = getenv("RX_VK_DUMP_EACH"))
                for (const char* q = e; *q;) {
                    char* end;
                    const uint64_t k = strtoull(q, &end, 16);
                    if (end == q) break;
                    v.push_back(k);
                    q = *end ? end + 1 : end;
                }
            return v;
        }();
        // (a key of 1: every draw into a target at least 2048 wide; 2,<width in hex>: every draw into targets that wide)
        if (!each.empty() && dump_on() &&
            (std::find(each.begin(), each.end(), p.key) != each.end() ||
             (each[0] == 1 && at.width >= 2048) || (each[0] == 2 && each.size() > 1 && at.width == each[1])))
            each_guard.cmd = cmd;
    }
    if (sw().log_draws) {
        static int logged;
        if (logged++ < 40)
            logf("vk: draw: program %016llx, %d stages, %d targets %ux%u, colour 0 %#llx format %d, topology %d, count %d",
                 (unsigned long long)p.key, (int)p.stages.size(), at.n, at.width, at.height,
                 (unsigned long long)(at.color[0] ? at.color[0]->gpu : 0), (int)at.color_format[0], (int)topo, d.count);
    }
    // (the descriptors depend on the program, the bindings, the texture slots and the stream's uniform updates:
    // when none changed since the last draw, the set it pushed or bound stands)
    // (hookshot) and no texture went since (texture_destroy_generation: an image evicted, or made again at another size or format,
    // in the same recording): the set it pushed or bound named that texture's old views, which go once the GPU is past the texture's
    // last use, a use the reused set no longer counted. The draws after it sampled freed memory, whatever the driver put there next:
    // plants and fences glowing white on a Legion Go S, which RX_VK_NO_DESC_REUSE=1 took away (issue 1126). RX_VK_DESC_REUSE_DGEN=0: as before
    const uint64_t reg_gen = registrations_generation(), ub_gen = ub_generation();
    static const bool no_reuse = getenv("RX_VK_NO_DESC_REUSE") != nullptr;   // (A/B: every draw builds its own)
    static const bool dgen_off = getenv("RX_VK_DESC_REUSE_DGEN") && atoi(getenv("RX_VK_DESC_REUSE_DGEN")) == 0;
    const uint64_t dgen = dgen_off ? 0 : texture_destroy_generation();
    // (hookshot, issue 640: a mesh fallback draw also pushes the writes at the compute bind point, for its task and mesh
    // dispatches, so they must be its own. A dispatch since its last draw built the compute program's over them: pushed
    // with the fallback's layout, its task and mesh stages read another program's buffers, and descriptors the push left
    // as they were: stale addresses, like the one RADV faulted on in Hyrule Field (#792))
    const bool slotgen = desc_slotgen_on();
    const bool same_desc = !no_reuse && last_desc_valid && last_desc_prog == &p && last_desc_bind == s.bind_gen &&
                           last_desc_ub == ub_gen && last_desc_dgen == dgen &&
                           (slotgen ? desc_slots_kept(reg_gen) : last_desc_reg == reg_gen) &&
                           (!p.fallback || dw().prog == &p);
    if (p.fallback && !same_desc && dw().prog != &p && last_desc_valid && last_desc_prog == &p) {
        static std::atomic<int> told{0};
        if (told++ < 3) logf("vk: fallback: %016llx's descriptors built again after a dispatch", (unsigned long long)p.key);
    }
    if (same_desc && slotgen && last_desc_reg != reg_gen) st_desc_slotgen_saves++;   // (the global check would have rebuilt)
    if (!same_desc) {
        PROF_HOT(PROF_DESC);
        FINE_N(FC_BUILDS, 1);
        trace_tex_hit = nullptr, trace_tex_mask = 0;
        build_descriptors(s, p);   // (textures first used here are uploaded by the recording's upload command buffer)
        FINE_N(FC_WRITES, (int64_t)dw().writes.size());
        static std::string last_logged;
        if (dump_on()) {
            char head[256];
            int hn = snprintf(head, sizeof head, "prog %016llx %ux%u:", (unsigned long long)p.key, at.width, at.height);
            for (int i = 0; i < at.n && hn < 200; i++)   // (the targets' NVN formats and the views' Vulkan ones)
                if (at.color[i])
                    hn += snprintf(head + hn, sizeof head - hn, " c%d:#%u/n%#x/v%d", i, at.color[i]->id,
                                   s.rt.has_cview[i] && s.rt.cview[i].set_format ? (int)s.rt.cview[i].format
                                                                                 : (int)s.rt.color[i]->d.format,
                                   (int)at.color_format[i]);
            if (last_logged != head + dump_inputs())
                last_logged = head + dump_inputs(), logf("vk: dump-in %03d draw %s", dump_seq(), last_logged.c_str());
        }
    }
    if (dump_on()) log_dump_draw(s, p, d);
    PROF_HOT(PROF_CMDS);
    texture_watch_prog(p.key);
    if (!same_desc) gfx_sampled() = draw_sampled();
    hdr_find(s, p, at);   // (RX_VK_HDR_FIND)
    // (HeyListen, after Longshot a7621bd7 / 83b161e3) NVIDIA DLAA before the game's bloom and tone map: the frame's draws into
    // the scene and those that read it, and at the bloom's first draw DLAA on the scene, before this draw reads it
    // (vk_temporal.cpp, pre_bloom.h). Its passes and its actor motion's draws bound a pipeline, a viewport, vertex streams and
    // descriptors of their own: this draw sets all of its again, as after the network's stand-in (temporal_stand_in). Kept, a
    // draw of the scene's size took the actor motion's upright viewport for the game's upside-down one (Longshot #1672)
    const bool pre_recorded = temporal_pre_bloom(cmd, p.key, gfx_sampled(), at.color, at.n);
    if (pre_recorded) {
        last_dyn_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;
        vertex_set.on = false, last_ib.on = false;
    }
    static const bool barrier_every = getenv("RX_VK_BARRIER_EVERY_DRAW") != nullptr;   // (an experiment: a pass each)
    if (barrier_every) end_pass(cmd);
    uint16_t writes;
    {
        FINE_SCOPE(FT_HAZARD);
        writes = draw_hazards(cmd, p, s, at);
    }
    BufferRef mesh_args{};
    uint32_t mesh_args_at = 0;
    // (RX_VK_MESH_ARGS=1: rewritten {count, 1, 1, first}. Off: it did not bring the canopies back, and it ends the pass at each draw)
    // (amd-fix, 11-07: on by default with the firsts (RX_VK_MESH_FIRST), the canopies back; RX_VK_MESH_ARGS=0: off. A single indirect
    // draw has no stride of its own: its entry is NVN's 16 bytes, taken as 8 the first was never copied)
    static const bool mesh_rewrite = getenv("RX_VK_MESH_ARGS") && atoi(getenv("RX_VK_MESH_ARGS")) != 0;   // (sharkiix version: off, as upstream)
    if (mesh_rewrite && d.mesh && d.indirect) {   // (out of the pass: a copy cannot be made in one)
        const uint32_t n = (uint32_t)std::clamp(d.max, 1, (int)kMeshArgs - 1), stride = d.stride ? (uint32_t)d.stride : 16u;
        BufferRef src = gpu_buffer_bind(d.indirect, (uint64_t)n * stride, false);
        if (!src) SKIP_HERE();
        if (mesh_args_ready()) {
            static const uint32_t word = getenv("RX_VK_MESH_COUNT_WORD") ? (uint32_t)atoi(getenv("RX_VK_MESH_COUNT_WORD")) : 0;
            end_pass(cmd);
            full_barrier(cmd);
            static thread_local std::vector<VkBufferCopy> copies;
            copies.clear();
            mesh_args_at = mesh_args_take(n);
            static const bool show = getenv("RX_VK_LOG_MESH_INDIRECT") != nullptr;
            if (show && mesh_args_at > 4096 && (mesh_args_at & 4095) < n) {   // (the counts the GPU copied, of the last 4096)
                uint32_t nonzero = 0, most = 0;
                uint64_t sum = 0;
                for (uint32_t i = mesh_args_at - 4096; i < mesh_args_at; i++) {
                    const uint32_t v = mesh_args_words[i * 4];
                    nonzero += v != 0, most = std::max(most, v), sum += v;
                }
                logf("vk: mesh counts: %u of 4096 not zero, largest %u, sum %llu", nonzero, most, (unsigned long long)sum);
            }
            {   // (amd-fix, RX_VK_LOG_MESH_PERPROG=1, diagnosis: per mesh program, the counts and firsts the GPU wrote, 4096 to 8192 entries ago)
                static const bool per_prog = getenv("RX_VK_LOG_MESH_PERPROG") != nullptr;
                if (per_prog) {
                    static std::vector<uint64_t> prog_at(kMeshArgs);
                    static uint32_t last_window = 0;
                    static int windows_told = 0;
                    const uint32_t window = mesh_args_at / 4096;
                    if (window != last_window && mesh_args_at >= 8192 && windows_told < 40) {
                        last_window = window;
                        windows_told++;
                        struct Agg { uint32_t entries = 0, nonzero = 0, most = 0, first_most = 0; uint64_t sum = 0; };
                        std::map<uint64_t, Agg> agg;
                        for (uint32_t i = mesh_args_at - 8192; i < mesh_args_at - 4096; i++) {
                            Agg& a = agg[prog_at[i]];
                            const uint32_t v = mesh_args_words[i * 4];
                            a.entries++, a.nonzero += v != 0, a.most = std::max(a.most, v), a.sum += v;
                            a.first_most = std::max(a.first_most, mesh_args_words[i * 4 + 3]);
                        }
                        for (const auto& [k, a] : agg)
                            logf("vk: mesh perprog window %u prog %016llx: %u entries, %u not zero, largest %u, sum %llu, largest first %u", window,
                                 (unsigned long long)k, a.entries, a.nonzero, a.most, (unsigned long long)a.sum, a.first_most);
                    }
                    for (uint32_t i = 0; i < n; i++) prog_at[(mesh_args_at + i) % kMeshArgs] = p.key;
                }
            }
            for (uint32_t i = 0; i < n; i++) {
                copies.push_back({src.offset + i * stride + word * 4, (VkDeviceSize)(mesh_args_at + i) * 16, 4});
                // (the first: NVN2's entry is {count, first}. Word 3 is the culling pass's own (an atomic max, read back as "70 0 0 70",
                // "60 0 0 950"): taken as the first, it sent the draws past the records. RX_VK_MESH_FIRST_WORD=<n>)
                static const uint32_t first_word = getenv("RX_VK_MESH_FIRST_WORD") ? (uint32_t)atoi(getenv("RX_VK_MESH_FIRST_WORD")) : 1u;
                if (stride >= 8) copies.push_back({src.offset + i * stride + first_word * 4, (VkDeviceSize)(mesh_args_at + i) * 16 + 12, 4});
            }
            vkCmdCopyBuffer(cmd, src.buffer, mesh_args_buffer, (uint32_t)copies.size(), copies.data());
            full_barrier(cmd);
            mesh_args = {mesh_args_buffer, (VkDeviceSize)mesh_args_at * 16, (VkDeviceSize)n * 16, 0};
        }
    }
    if (p.fallback && d.mesh && !fallback_record(cmd, p, d)) SKIP_HERE();
    {
        FINE_SCOPE(FT_HAZARD);
        begin_pass(cmd, at);
    }
    probe_draw(s, p, ps.draws);   // (hookshot: RX_VK_PROBE)
    gprof_run(cmd, p.key);   // (RX_VK_PASS_TIMING=2)
    ps.prog = p.key;
    if (!ps.first_prog) ps.first_prog = p.key;
    for (const Texture* t : gfx_sampled()) {
        if (std::find(ps.sampled.begin(), ps.sampled.end(), t) != ps.sampled.end()) continue;
        ps.sampled.push_back(t);
        int slot = -1;
        for (int i = 0; i < at.n; i++)
            if (at.color[i] == t) { slot = i; break; }
        if (slot < 0 && at.depth == t) slot = 8;
        if (slot >= 0) ps.sampled_targets |= (uint16_t)(1u << slot);
    }
    ps.written |= writes;
    if (pl != last_pipeline) {
        FINE_SCOPE(FT_BINDPL);
        FINE_N(FC_BIND_PIPE, 1);
        if (res_log_on()) res_note_pipeline_use(pl);   // (TICK-038)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl), last_pipeline = pl;
    }
    if (!same_desc || pre_recorded) {   // (after DLAA's commands its pushes are gone: temporal_pre_bloom)
        FINE_SCOPE(FT_BINDDESC);
        if (!bind_descriptors(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p)) SKIP_HERE();
        last_desc_valid = true, last_desc_prog = &p, last_desc_bind = s.bind_gen, last_desc_reg = reg_gen;
        last_desc_ub = ub_gen, last_desc_dgen = dgen;
        if (slotgen)   // (RX_VK_DESC_SLOTGEN: what its handles resolved; the next such draw may keep it through other slots' churn)
            last_desc_fp() = desc_fp(), last_desc_fp_dummy = desc_fp_dummy, last_desc_samp = desc_samp_seen,
            last_desc_destroy = desc_destroy_seen;
    } else {
        st_desc_reused++;
    }
    {
        FINE_SCOPE(FT_PUSHC);
        push_constants(cmd, p, &at);
    }
    if (watched(p.key)) {   // (RX_VK_WATCH_PROG: the draw's state, 80 lines)
        static std::atomic<int> n{0};
        if (n++ < 80) {
            char line[600];
            int k = snprintf(line, sizeof line, "vk: watch draw %016llx %ux%u targets %d: blend_en %#x logic %u wm", (unsigned long long)p.key,
                             at.width, at.height, at.n, (unsigned)s.color.blend_en, (unsigned)s.color.logic_op);
            for (int i = 0; i < at.n && k < 500; i++) {
                const nvn_blend_st& b = s.blend[i];
                k += snprintf(line + k, sizeof line - k, " [%d: wm %x src %#x dst %#x eq %u / a %#x %#x %u fmt %d]", i,
                              (unsigned)(s.chmask.mask >> (4 * i) & 15), (unsigned)b.src_rgb, (unsigned)b.dst_rgb, (unsigned)b.eq_rgb,
                              (unsigned)b.src_a, (unsigned)b.dst_a, (unsigned)b.eq_a, (int)at.color_format[i]);
            }
            snprintf(line + k, sizeof line - k, " blendc %.2f %.2f %.2f %.2f depth %d", s.blend_color[0], s.blend_color[1],
                     s.blend_color[2], s.blend_color[3], at.depth ? 1 : 0);
            logf("%s", line);
        }
    }
    {
        FINE_SCOPE(FT_DYN);
        dynamic_state(cmd, s, at, topo, d.mesh && !p.fallback);
    }
    if (trace_tex_hit && texture_frame_no() % 30 == 0) {
        static std::atomic<int> told{0};
        if (told++ < 600) {
            char ds[80] = "", poly[40] = "";
            int k = 0;
            for (size_t i = 0; i < sizeof s.ds && k < 70; i++) k += snprintf(ds + k, sizeof ds - k, "%02x", ((const uint8_t*)&s.ds)[i]);
            k = 0;
            for (size_t i = 0; i < sizeof s.poly && k < 30; i++) k += snprintf(poly + k, sizeof poly - k, "%02x", ((const uint8_t*)&s.poly)[i]);
            logf("vk: trace frame %llu tex #%u %ux%u prog %016llx target %ux%u x%d depth %d fmt0 %d wm %x blend_en %x topo %d count %d inst %d first %d base %d "
                 "%s%s%s groups %d ds %s poly %s vp0 %.0f %.0f %.0f %.0f sc %d %d %d %d cull-ish vs0 %#llx",
                 (unsigned long long)texture_frame_no(), trace_tex_hit->id, trace_tex_hit->width, trace_tex_hit->height, (unsigned long long)p.key, at.width,
                 at.height, at.n, at.depth ? 1 : 0, (int)at.color_format[0], (unsigned)s.chmask.mask, (unsigned)s.color.blend_en, (int)topo, d.count,
                 d.instances, d.first, d.base_vertex, d.indexed ? "indexed " : "", d.indirect ? "indirect " : "", d.mesh ? "mesh " : "", d.groups[0], ds, poly,
                 s.viewport[0][0], s.viewport[0][1], s.viewport[0][2], s.viewport[0][3], s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3],
                 (unsigned long long)s.vbuf[0].address);
            if (d.mesh) {   // (what the task and mesh stages have bound, and the first words of the small tables: pointers)
                for (int st : {6, 7})
                    for (int i = 0; i < 4; i++) {
                        const nvn_p_bind_buf& b = s.ssbo[st][i];
                        if (!b.addr) continue;
                        uint64_t w[10]{};
                        memory_read_as_submitted(b.addr, w, std::min<uint64_t>(sizeof w, b.size));
                        logf("vk: trace   stage %d ssbo %d %#llx size %#llx: %llx %llx %llx %llx %llx %llx %llx %llx", st, i, (unsigned long long)b.addr,
                             (unsigned long long)b.size, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3],
                             (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7]);
                    }
            }
        }
    }
    if (d.indexed && s.restart) {   // (RX_VK_LOG_RESTART=1: the restarts draws ask for, by topology, index type and index)
        static const bool log_restart = getenv("RX_VK_LOG_RESTART") != nullptr;
        if (log_restart) {
            static std::mutex m;
            static std::map<uint64_t, int64_t> seen;
            const uint64_t key = (uint64_t)topo << 40 | (uint64_t)d.itype << 32 | s.restart_index;
            std::lock_guard l(m);
            const int64_t n = ++seen[key];
            if ((n == 1 && seen.size() <= 60) || n == 1000 || n == 100000)
                logf("vk: restart (%lld draws so far): prog %016llx topology %d index type %d restart index %#x%s count %d",
                     (long long)n, (unsigned long long)p.key, (int)topo, d.itype, s.restart_index,
                     d.indirect ? " indirect" : "", d.count);
        }
    }
    StatsQuery stats_query(cmd, p.key, d);   // (EvilOOT, RX_VK_DRAW_STATS)
    OccQuery occ_query(cmd, p.key, d, stats_query.cmd != VK_NULL_HANDLE);   // (TICK-033, RX_VK_OCC_REAL)
    if (stats_frame_now() && stats_prog_wanted(p.key)) {   // (and the wanted programs' buffers: a mesh draw's stages, fragment)
        for (const int stage : {6, 7, 1}) stats_dump_stage(p.key, s, stage);
        // (and as the GPU holds them, after it: RX_VK_GPU_DUMP=1 only; its bindings can sync a pool's mirror mid-frame, and
        // the culling chain then measured as broken)
        static const bool gpu_dump_on = getenv("RX_VK_GPU_DUMP") != nullptr;
        if (stats_query.cmd && gpu_dump_on) stats_query.state = &s, stats_query.prog = p.key;
    }
    if (d.mesh) {
        if (p.fallback) {   // (hookshot: the generated vertex stage reads the records; one vertex a corner)
            {
                // (the generated vertex stage reads no vertex stream, but the pipeline keeps the stride dynamic like every
                // other graphics pipeline, and a draw needs it bound in the command buffer (validation, VUID-vkCmdDraw-pStrides-04913:
                // a first draw of a recording that is a fallback draw had none). One empty stream; the next ordinary draw binds its own)
                if (!last_vb_valid && zero_buffer) {
                    const VkBuffer b = zero_buffer;
                    const VkDeviceSize off = 0, size = 65536, stride = 0;
                    vkCmdBindVertexBuffers2(cmd, 0, 1, &b, &off, &size, &stride);
                }
                checkpoint_work('F', 0);   // (the count is the records')
                vkCmdDrawIndirect(cmd, fallback_draw_records, 0, 1, sizeof(VkDrawIndirectCommand));   // (its count: the records taken)
            }
            st_mesh++;
            st_draws++, ps.draws++;
            return;
        }
        {   // (amd-fix, diagnosis) RX_VK_LOG_MESH_PROGS=1: each mesh program's first draw and every 600th, with what its
            // indirect arguments hold in guest memory (0 groups: the culling pass before it left nothing)
            static const bool per_prog = getenv("RX_VK_LOG_MESH_PROGS") != nullptr;
            if (dump_on() && d.indirect && dump_mesh_indirect().size() < 256) dump_mesh_indirect().push_back({p.key, d.indirect});
            if (per_prog) {
                static std::mutex mm;
                static std::unordered_map<uint64_t, uint64_t> seen;
                uint64_t n;
                {
                    std::lock_guard lk(mm);
                    n = seen[p.key]++;
                }
                if (n % 600 == 0 && seen.size() < 400) {
                    uint32_t w[3] = {(uint32_t)d.groups[0], (uint32_t)d.groups[1], (uint32_t)d.groups[2]}, draws = (uint32_t)std::max(d.max, 1);
                    if (d.indirect) host->gpu_read(d.indirect, w, sizeof w);
                    if (d.count_addr) host->gpu_read(d.count_addr, &draws, 4);
                    logf("vk: mesh prog %016llx draw #%llu frame %llu: groups %u %u %u, %u draws (max %d), indirect %#llx count %#llx; codes%s",
                         (unsigned long long)p.key, (unsigned long long)n, (unsigned long long)texture_frame_no(), w[0], w[1], w[2],
                         draws, d.max, (unsigned long long)d.indirect, (unsigned long long)d.count_addr, program_code_keys(s).c_str());
                }
            }
        }
        {   // (RX_VK_LOG_MESH_DRAWS=1: how large each is, the largest so far, 400 lines)
            static const bool log_sizes = getenv("RX_VK_LOG_MESH_DRAWS") != nullptr;
            if (log_sizes) {
                static std::atomic<int> told{0};
                static std::atomic<uint32_t> most{0};
                uint32_t w[3] = {(uint32_t)d.groups[0], (uint32_t)d.groups[1], (uint32_t)d.groups[2]}, draws = (uint32_t)std::max(d.max, 1);
                if (d.indirect) host->gpu_read(d.indirect, w, sizeof w);
                if (d.count_addr) host->gpu_read(d.count_addr, &draws, 4);
                const uint64_t size = (uint64_t)w[0] * w[1] * w[2] * draws;
                uint32_t was = most.load();
                if (size > was && told++ < 400) {
                    most = (uint32_t)std::min<uint64_t>(size, 0xFFFFFFFFu);
                    logf("vk: mesh draw prog %016llx frame %llu: groups %u %u %u, %u draws (max %d, stride %d), indirect %#llx count %#llx", (unsigned long long)p.key,
                         (unsigned long long)texture_frame_no(), w[0], w[1], w[2], draws, d.max, d.stride, (unsigned long long)d.indirect, (unsigned long long)d.count_addr);
                }
            }
        }
        {   // (RX_VK_LOG_MESH_INDIRECT=1: what indirect mesh draws hold, 300 lines)
            static const bool on = getenv("RX_VK_LOG_MESH_INDIRECT") != nullptr;
            static std::atomic<int> told{0};
            if (on && d.indirect && told < 300) {
                uint32_t w[12]{}, cnt = 0;
                host->gpu_read(d.indirect, w, sizeof w);
                if (d.count_addr) host->gpu_read(d.count_addr, &cnt, 4);
                told++;
                logf("vk: mesh indirect prog %016llx: stride %d max %d count %u (at %#llx): %u %u %u %u %u %u %u %u", (unsigned long long)p.key, d.stride, d.max, cnt,
                     (unsigned long long)d.count_addr, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
        if (d.indirect) {
            ledger_mark(d.indirect, (uint64_t)std::max(d.max, 1) * (uint64_t)std::max(d.stride, 12), kUseIndirect);   // (RX_VK_LEDGER)
            if (d.count_addr) ledger_mark(d.count_addr, 4, kUseIndirect);
            BufferRef cnt = d.count_addr ? gpu_buffer_bind(d.count_addr, 4, false) : BufferRef{};
            if (mesh_conv_buf) {   // (EvilOOT: the records as {count, 1, 1}, mesh_indirect_convert)
                // (hookshot: as many records as mesh_indirect_convert wrote, not the game's maximum)
                const uint32_t conv_n = (uint32_t)std::clamp(d.max, 1, 4096);
                if (mesh_dbg) mesh_count_note(p.key, mesh_conv_off);
                checkpoint_work('N', conv_n);
                if (checkpoints_on()) checkpoint_flags(mesh_has_task(p), cnt && vkCmdDrawMeshTasksIndirectCountEXT, true);
                if (cnt && vkCmdDrawMeshTasksIndirectCountEXT)
                    vkCmdDrawMeshTasksIndirectCountEXT(cmd, mesh_conv_buf, mesh_conv_off, cnt.buffer, cnt.offset, conv_n, 12);
                else
                    vkCmdDrawMeshTasksIndirectEXT(cmd, mesh_conv_buf, mesh_conv_off, conv_n, 12);
                mesh_conv_buf = VK_NULL_HANDLE;
                st_mesh++;
                st_draws++, ps.draws++;
                return;
            }
            if (d.max < 0 || d.stride < 0) SKIP_HERE();   // (hookshot: the game's ints; a negative count was ~4e9 draws)
            if (!mesh_args.buffer) {
                mesh_args = gpu_buffer_bind(d.indirect, (uint64_t)std::max(d.max, 1) * std::max(d.stride, 12), false);
                if (!mesh_args) SKIP_HERE();
            }
            const uint32_t step = mesh_args.buffer == mesh_args_buffer && mesh_args_buffer ? 16u : (uint32_t)std::max(d.stride, 12);
            if (mesh_args.buffer == mesh_args_buffer && mesh_args_buffer) {   // (the task programs' firsts: this draw's entries)
                const uint64_t firsts = mesh_args_address + mesh_args.offset + 12;
                vkCmdPushConstants(cmd, p.layout, p.push_stages, Shader::Backend::SPIRV::MESH_FIRST_PUSH_OFFSET, 8, &firsts);
            }
            checkpoint_work('N', (uint32_t)std::max(d.max, 1));
            if (checkpoints_on()) checkpoint_flags(mesh_has_task(p), cnt && vkCmdDrawMeshTasksIndirectCountEXT, false);
            if (cnt && vkCmdDrawMeshTasksIndirectCountEXT)
                vkCmdDrawMeshTasksIndirectCountEXT(cmd, mesh_args.buffer, mesh_args.offset, cnt.buffer, cnt.offset, d.max, step);
            else
                vkCmdDrawMeshTasksIndirectEXT(cmd, mesh_args.buffer, mesh_args.offset, (uint32_t)std::max(d.max, 1), step);
        } else {
            if (mesh_args_ready()) {   // (a direct draw's first: in an entry of its own, or entry 0's 0)
                uint32_t at = 0;
                if (d.first_task) {
                    at = mesh_args_take(1);
                    mesh_args_words[at * 4 + 3] = (uint32_t)d.first_task;
                }
                const uint64_t firsts = mesh_args_address + (uint64_t)at * 16 + 12;
                vkCmdPushConstants(cmd, p.layout, p.push_stages, Shader::Backend::SPIRV::MESH_FIRST_PUSH_OFFSET, 8, &firsts);
            }
            checkpoint_work('M', (uint32_t)std::max(d.groups[0], 0), (uint32_t)std::max(d.groups[1], 1), (uint32_t)std::max(d.groups[2], 1));
            if (checkpoints_on()) checkpoint_flags(mesh_has_task(p), false, false);
            vkCmdDrawMeshTasksEXT(cmd, (uint32_t)std::max(d.groups[0], 0), (uint32_t)std::max(d.groups[1], 1),
                                  (uint32_t)std::max(d.groups[2], 1));
        }
        st_mesh++;
        st_draws++, ps.draws++;
        return;
    }
    // vertex streams (from their pools, strides dynamic) and the zero stream
    // (the same streams are bound for draw after draw: the pool lookup, which was ~8% of a race under the lock, is
    // skipped until the submission or the mirror changes. The whole set, already bound on this command buffer, skips
    // the lookup and the bind)
    const int64_t fine_vertex_t0 = g_fine ? prof_now() : 0;
    uint64_t vaddr[16]{};
    uint64_t vsize[16]{};
    uint32_t vstride_key[16]{};
    for (int i = 0; i < 16; i++) {
        if (i == 15 || !s.vbuf[i].address) continue;
        vaddr[i] = s.vbuf[i].address, vsize[i] = s.vbuf[i].size, vstride_key[i] = s.vstream[i].stride;
    }
    const uint64_t vstamp = gpu_bind_stamp();
    if (!vertex_set_same(vstamp, vaddr, vsize, vstride_key, vertex_set.stamp, vertex_set.addr, vertex_set.size,
                         vertex_set.stride, vertex_set.on)) {
        VkBuffer vbufs[16];
        VkDeviceSize voffs[16], vsizes[16], vstrides[16];
        bool complete = true;
        for (uint32_t b = 0; b < 16; b++) {
            vbufs[b] = zero_buffer, voffs[b] = 0, vsizes[b] = 65536, vstrides[b] = 0;
            if (b == 15 || !s.vbuf[b].address) continue;
            const uint64_t vb_addr = s.vbuf[b].address, vb_size = std::max<uint64_t>(s.vbuf[b].size, 1);
            const uint64_t vb_stamp = gpu_bind_stamp();
            ledger_mark(vb_addr, vb_size, kUseVertex);   // (RX_VK_LEDGER)
            BufferRef r;
            if (vertex_bind_reuse(vb_addr, vb_size, vb_stamp, kept_vb[b].addr, kept_vb[b].size, kept_vb[b].stamp, kept_vb[b].ok))
                r = kept_vb[b].ref;
            else {
                r = gpu_buffer_bind(vb_addr, vb_size, false);
                kept_vb[b] = {vb_addr, vb_size, gpu_bind_stamp(), r, r ? 1 : 0};
            }
            if (!r) {
                complete = false;
                continue;
            }
            vbufs[b] = r.buffer, voffs[b] = r.offset, vsizes[b] = s.vbuf[b].size ? s.vbuf[b].size : VK_WHOLE_SIZE;
            vstrides[b] = s.vstream[b].stride;
        }
        // (hookshot: only the bindings that changed, vertex_bind_range; what is bound after it is the same 16. Every other
        // graphics bind on a replay command buffer (masked clears, the temporal passes, a new recording) clears
        // last_vb_valid, so the 16 remembered are what the command buffer has. RX_VK_VB_RANGE=0: all 16 at each change, as before)
        static const bool vb_all = getenv("RX_VK_VB_RANGE") && atoi(getenv("RX_VK_VB_RANGE")) == 0;
        uint32_t vb_first = 0, vb_count = 0;
        vertex_bind_range(vbufs, voffs, vsizes, vstrides, last_vb.b, last_vb.o, last_vb.z, last_vb.s, last_vb_valid, &vb_first, &vb_count);
        if (vb_count && vb_all) vb_first = 0, vb_count = 16;
        if (vb_count) {
            FINE_N(FC_VBIND, 1);
            vkCmdBindVertexBuffers2(cmd, vb_first, vb_count, vbufs + vb_first, voffs + vb_first, vsizes + vb_first, vstrides + vb_first);
            std::memcpy(last_vb.b, vbufs, sizeof vbufs), std::memcpy(last_vb.o, voffs, sizeof voffs);
            std::memcpy(last_vb.z, vsizes, sizeof vsizes), std::memcpy(last_vb.s, vstrides, sizeof vstrides);
            last_vb_valid = true;
        }
        if (complete && gpu_bind_stamp() == vstamp) {
            vertex_set.stamp = vstamp;
            for (int i = 0; i < 16; i++) {
                const bool used = i != 15 && vaddr[i];
                vertex_set.addr[i] = used ? vaddr[i] : 0;
                vertex_set.size[i] = used ? (vsize[i] ? vsize[i] : 1) : 0;
                vertex_set.stride[i] = used ? vstride_key[i] : 0;
            }
            vertex_set.on = true;
        } else
            vertex_set.on = false;
    }
    if (g_fine) fine_ns[FT_VERTEX] += prof_now() - fine_vertex_t0;
    const VkIndexType itype = d.itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : d.itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (d.indexed && d.itype == 0 && !dev.index_uint8) SKIP_HERE();
    if (d.prim == NVN_DRAW_PRIMITIVE_QUADS) {   // (quads as two triangles each: indices made in the stream)
        if (d.indirect || d.count < 4) SKIP_HERE();
        if (d.count > (1 << 24)) SKIP_HERE();   // (hookshot: 4M quads at most; the game's count sized host vectors of up to 16 GB)
        const uint32_t quads = (uint32_t)d.count / 4, unit = d.itype == 0 ? 1 : d.itype == 1 ? 2 : 4;
        std::vector<uint32_t> src(d.indexed ? (size_t)quads * 4 : 0);
        if (d.indexed) {
            std::vector<uint8_t> raw((size_t)quads * 4 * unit);
            if (!memory_read_as_submitted(d.ibuf, raw.data(), raw.size())) SKIP_HERE();
            for (size_t i = 0; i < src.size(); i++) {
                uint32_t v = 0;
                std::memcpy(&v, raw.data() + i * unit, unit);
                src[i] = v;
            }
        }
        const uint64_t at6 = stream_alloc((uint64_t)quads * 6 * 4, 16);
        if (at6 == UINT64_MAX) SKIP_HERE();
        uint32_t* out = reinterpret_cast<uint32_t*>(cur->mapped + at6);
        for (uint32_t q = 0; q < quads; q++) {
            const uint32_t b = q * 4;
            const uint32_t v[4] = {d.indexed ? src[b] : (uint32_t)d.first + b, d.indexed ? src[b + 1] : (uint32_t)d.first + b + 1,
                                   d.indexed ? src[b + 2] : (uint32_t)d.first + b + 2, d.indexed ? src[b + 3] : (uint32_t)d.first + b + 3};
            const uint32_t tri[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
            std::memcpy(out + q * 6, tri, sizeof tri);
        }
        bind_index(cmd, cur->stream, at6, VK_INDEX_TYPE_UINT32);
        checkpoint_work('Q', quads * 6, (uint32_t)std::max(1, d.instances));
        vkCmdDrawIndexed(cmd, quads * 6, (uint32_t)std::max(1, d.instances), 0, d.indexed ? d.base_vertex : 0,
                         (uint32_t)d.base_instance);
        st_draws++, ps.draws++;
        return;
    }
    if (d.indirect) {
        // (hookshot) the records the draw reads, at the stride it reads them with (20 for indexed draws), all bound; a
        // negative count or stride from the game is no draw
        if (d.max < 0 || d.stride < 0) SKIP_HERE();
        const int ind_stride = std::max(d.stride, d.indexed ? 20 : 16);
        checkpoint_work('I', (uint32_t)std::max(d.max, 1));
        ledger_mark(d.indirect, (uint64_t)std::max(d.max, 1) * (uint64_t)ind_stride, kUseIndirect);   // (RX_VK_LEDGER)
        if (d.count_addr) ledger_mark(d.count_addr, 4, kUseIndirect);
        BufferRef args = gpu_buffer_bind(d.indirect, (uint64_t)std::max(d.max, 1) * (uint64_t)ind_stride, false);
        BufferRef cnt = d.count_addr ? gpu_buffer_bind(d.count_addr, 4, false) : BufferRef{};
        if (!args) SKIP_HERE();
        if (d.indexed) {
            ledger_mark(d.ibuf, 4, kUseIndex);
            BufferRef ib = gpu_buffer_bind(d.ibuf, 4, false);
            if (!ib) SKIP_HERE();
            bind_index(cmd, ib.buffer, ib.offset, itype);
            if (cnt) vkCmdDrawIndexedIndirectCount(cmd, args.buffer, args.offset, cnt.buffer, cnt.offset, std::max(d.max, 1), ind_stride);
            else vkCmdDrawIndexedIndirect(cmd, args.buffer, args.offset, std::max(d.max, 1), ind_stride);
        } else {
            if (cnt) vkCmdDrawIndirectCount(cmd, args.buffer, args.offset, cnt.buffer, cnt.offset, std::max(d.max, 1), ind_stride);
            else vkCmdDrawIndirect(cmd, args.buffer, args.offset, std::max(d.max, 1), ind_stride);
        }
    } else if (d.indexed) {
        const uint32_t unit = d.itype == 0 ? 1 : d.itype == 1 ? 2 : 4;
        BufferRef ib;
        {
            FINE_SCOPE(FT_VERTEX);
            ledger_mark(d.ibuf, (uint64_t)std::max(d.count, 1) * unit, kUseIndex);   // (RX_VK_LEDGER)
            ib = gpu_buffer_bind(d.ibuf, (uint64_t)std::max(d.count, 1) * unit, false);
            if (!ib) SKIP_HERE();
            FINE_N(FC_IBIND, 1);
            bind_index(cmd, ib.buffer, ib.offset, itype);
        }
        FINE_SCOPE(FT_DRAWCALL);
        checkpoint_work('E', (uint32_t)d.count, (uint32_t)std::max(1, d.instances));
        vkCmdDrawIndexed(cmd, (uint32_t)d.count, (uint32_t)std::max(1, d.instances), 0, d.base_vertex, (uint32_t)d.base_instance);
    } else {
        FINE_SCOPE(FT_DRAWCALL);
        checkpoint_work('V', (uint32_t)d.count, (uint32_t)std::max(1, d.instances));
        vkCmdDraw(cmd, (uint32_t)d.count, (uint32_t)std::max(1, d.instances), (uint32_t)d.first, (uint32_t)d.base_instance);
    }
    st_draws++, ps.draws++;
}

/// A dispatch over 2 ms is logged, 60 times, and only while RX_VK_SLOW_DISPATCH is set. Off, the clock is not read.
bool slow_dispatch_timing() {
    static const int on = getenv("RX_VK_SLOW_DISPATCH") != nullptr;
    return on;
}

struct DispatchClock {   // (a dispatch over 2 ms: logged, with where its time went)
    std::chrono::steady_clock::time_point t0 = slow_dispatch_timing() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}, t_prog{}, t_pipe{}, t_desc{};
    uint64_t key = 0;
    bool made_pipeline = false;
    ~DispatchClock() {
        if (!slow_dispatch_timing()) return;
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        static int logged;
        if (ms < 2.0 || logged >= 60) return;
        logged++;
        auto at = [&](std::chrono::steady_clock::time_point t) {
            return t.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(t - t0).count() : -1.0;
        };
        logf("vk: slow dispatch %.1f ms: program %016llx (picked at %.1f), pipeline%s at %.1f, descriptors at %.1f",
             ms, (unsigned long long)key, at(t_prog), made_pipeline ? " made" : "", at(t_pipe), at(t_desc));
    }
};

/// (hookshot) OoT's neural upscaler (about 13 tensor-core compute passes a frame over storage-buffer feature maps,
/// 1920x1080 in, 2560x1440 out) does not translate: its matrix multiplies and a few other native opcodes have no
/// lowering. Its last pass reads the network's result, the input colour and a noise texture and writes the output
/// image; left out, the output stays black under the UI drawn over it. Here a rejected compute pass that writes a
/// storage image larger than the largest colour texture it samples (and does not also write) is replaced by a linear
/// blit of that texture into the image: the frame at its full size, without the network's detail. (An image the
/// pass also samples is one of its inputs, never the output.)
/// RX_VK_NO_UPSCALE_STUB=1: left out, as before. The first replacement of each program is logged.
/// (shaders/movie_upscale.comp) The upscale in a shader: it reads and writes the two images as the network's own passes do, where a blit came
/// out black on an RX 9070 XT. False: no way to, the blit is tried
bool movie_upscale(VkCommandBuffer cmd, uint32_t in_handle, uint32_t out_handle, const Texture* out) {
    static const bool off = getenv("RX_VK_MOVIE_BLIT") != nullptr;
    if (off || !dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    Texture* a = nullptr;
    Texture* b = nullptr;
    uint32_t la = 0, lb = 0;
    const VkImageView in = texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_UNDEFINED, &a, &la);
    const VkImageView to = texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, VK_FORMAT_R8G8B8A8_UNORM, &b, &lb);
    if (!in || !to || !b) return false;
    static VkPipeline pipe = VK_NULL_HANDLE;
    static VkPipelineLayout layout = VK_NULL_HANDLE;
    static VkSampler linear = VK_NULL_HANDLE;
    static const bool made = [] {
        VkDescriptorSetLayoutBinding bind[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = 2, li.pBindings = bind;
        VkDescriptorSetLayout set{};
        vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set);
        const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof MOVIE_UPSCALE_COMP_SPV, mi.pCode = MOVIE_UPSCALE_COMP_SPV;
        VkShaderModule m{};
        IntelDriverGate gate;
        if (guarded_shader_module(&mi, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = layout;
        if (!m || guarded_compute(VK_NULL_HANDLE, &ci, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        vkDestroyShaderModule(dev.device, m, nullptr);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.25f;
        vkCreateSampler(dev.device, &si, nullptr, &linear);
        return pipe != VK_NULL_HANDLE;
    }();
    if (!made) return false;
    const VkDescriptorImageInfo in_info{linear, in, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo out_info{VK_NULL_HANDLE, to, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[0].pImageInfo = &in_info;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w[1].pImageInfo = &out_info;
    const uint32_t size[2] = {std::max(1u, out->width >> lb), std::max(1u, out->height >> lb)};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2, w);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, size);
    vkCmdDispatch(cmd, (size[0] + 7) / 8, (size[1] + 7) / 8, 1);
    descriptors_disturbed();
    return true;
}

// (hookshot, issue 109) The movie upscaler's networks cost 12 ms a frame on an RTX 3050 and 83-93 ms on an RTX 4060 with a newer
// driver: the intro movie ran at 10 fps and the game never got past it. When a frame of few draws (a movie) takes over 40 ms
// of GPU time (vk_replay.cpp), they are left out from then on, as on AMD: the linear upscale below, the same picture a little
// softer. The gameplay networks are never left out. RX_VK_NETWORK_KEEP=all: never
std::atomic<bool> movie_networks_slow{false};
bool gameplay_network(uint64_t key) { return key == 0xfd029a317a9a4b94ull || key == 0x9dc98871eb7fc101ull; }

static movie_upscale_policy::Mode movie_upscale_mode() {
    // Read on first dispatch, after host/hos_main.c has loaded hookshot_env.txt.
    static const auto mode = movie_upscale_policy::parse(getenv("RX_VK_MOVIE_UPSCALE"));
    return mode;
}

static bool movie_keep_all_networks() {
    static const bool keep = getenv("RX_VK_NETWORK_KEEP") && !strcmp(getenv("RX_VK_NETWORK_KEEP"), "all");
    return keep;
}

static bool typed_upscale(VkCommandBuffer cmd, VkImageView input_view, VkImageView output_view,
                          Texture* out, uint32_t level, uint64_t key, const char* keys, bool temporal);
static bool temporal_rejected_network(VkCommandBuffer cmd, uint64_t key, Texture* src, const nvn_tex* in, Texture* dst,
                                      const nvn_tex* out);

bool upscale_stub(VkCommandBuffer cmd, const NvnState& s, uint64_t key) {
    static const bool off = getenv("RX_VK_NO_UPSCALE_STUB") != nullptr;
    if (off) return false;
    nvn_tex out{}, in{};
    uint64_t out_area = 0, in_area = 0, in_h = 0, out_h = 0;
    uint64_t img_gpu[32]{}, tex_gpu[128]{};
    int nimg = 0, ntex = 0;
    uint32_t ntex_slots = 0, nimg_slots = 0;   // (the program's own slots: later ones hold other passes' bindings)
    cs_slot_counts(s, &ntex_slots, &nimg_slots);
    ntex_slots = std::min(ntex_slots, 128u), nimg_slots = std::min(nimg_slots, 32u);
    for (uint32_t i = 0; i < ntex_slots; i++) {   // (what the pass samples: an image it also samples is an input, not the output)
        nvn_tex t;
        nvn_view v;
        bool hv = false;
        if (s.tex[5][i] && texture_of((int)(s.tex[5][i] & 0xFFFFF), &t, &v, &hv)) tex_gpu[ntex++] = t.gpu;
    }
    for (uint32_t i = 0; i < nimg_slots; i++) {
        nvn_tex t;
        nvn_view v;
        bool hv = false;
        if (!s.img[5][i] || !texture_of((int)(s.img[5][i] & 0xFFFFF), &t, &v, &hv)) continue;
        img_gpu[nimg++] = t.gpu;
        if (std::find(tex_gpu, tex_gpu + ntex, t.gpu) != tex_gpu + ntex) continue;
        const uint64_t a = (uint64_t)t.d.width * t.d.height;
        if (t.d.depth <= 1 && a > out_area) out = t, out_area = a, out_h = s.img[5][i];
    }
    if (!out_area) return false;
    for (uint32_t i = 0; i < ntex_slots; i++) {
        nvn_tex t;
        nvn_view v;
        bool hv = false;
        if (!s.tex[5][i] || !texture_of((int)(s.tex[5][i] & 0xFFFFF), &t, &v, &hv)) continue;
        if (t.d.depth > 1 || t.d.target != out.d.target) continue;
        if (std::find(img_gpu, img_gpu + nimg, t.gpu) != img_gpu + nimg) continue;   // (the network's own buffers)
        const uint64_t a = (uint64_t)t.d.width * t.d.height;
        if (a > in_area) in = t, in_area = a, in_h = s.tex[5][i];
    }
    // (a gameplay network may keep the size: 9dc98871 at Render resolution 100%. Left out (no f16 shaders) and skipped here, the
    // 3D picture was black under the HUD)
    if (!in_area || out_area < in_area || (out_area == in_area && !gameplay_network(key))) return false;
    Texture* src = texture_get(&in, true);
    Texture* dst = texture_get(&out, false);
    if (!src || !dst || !src->image || !dst->image || (src->aspect | dst->aspect) != VK_IMAGE_ASPECT_COLOR_BIT || src->bc ||
        dst->bc || src->samples != VK_SAMPLE_COUNT_1_BIT || dst->samples != VK_SAMPLE_COUNT_1_BIT)
        return false;
    // (hookshot) an upscale network that did not translate here (the Render resolution's fixed shares have one each, and on a
    // GPU without cooperative matrices they are left out): the temporal upscaler is offered it first, as temporal_stand_in
    // offers the networks that did translate. It takes only the scene into the picture; anything else goes on below
    if (temporal_wanted() && temporal_rejected_network(cmd, key, src, &in, dst, &out)) return true;
    VkFormatProperties fs{}, fd{};
    vkGetPhysicalDeviceFormatProperties(dev.physical, src->format, &fs);
    vkGetPhysicalDeviceFormatProperties(dev.physical, dst->format, &fd);
    if (!(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fd.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        return false;
    end_pass(cmd);
    full_barrier(cmd);
    if (dst->format == VK_FORMAT_R8G8B8A8_UNORM && movie_upscale(cmd, in_h, out_h, dst)) {
        full_barrier(cmd);
        texture_written(dst);
        static std::atomic<bool> logged_movie_upscale{false};
        if (!logged_movie_upscale.exchange(true, std::memory_order_relaxed))
            logf("vk: movie upscaling uses the linear compute path");
        return true;
    }
    // The generic failed-network path can receive RGB9E5 bits in a shared R11 image.
    // Match the observed base-level 2D case only; other fallbacks keep their existing blit.
    const bool gameplay_taau = key == 0xfd029a317a9a4b94ull;
    if (gameplay_taau && scene_tonemap_on() &&
        (src->format == VK_FORMAT_R16G16B16A16_SFLOAT || dst->format == VK_FORMAT_R16G16B16A16_SFLOAT)) {
        if (src->format != VK_FORMAT_R16G16B16A16_SFLOAT || dst->format != VK_FORMAT_R16G16B16A16_SFLOAT ||
            in.d.format != NVN_FORMAT_RGB9E5F || out.d.format != NVN_FORMAT_R11G11B10F ||
            !(fd.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
            src->type != VK_IMAGE_TYPE_2D || dst->type != VK_IMAGE_TYPE_2D || src->depth != 1 || dst->depth != 1 ||
            src->layers != 1 || dst->layers != 1 || src->levels != 1 || dst->levels != 1 || src->skip || dst->skip ||
            in.d.swizzle_r != NVN_TEXTURE_SWIZZLE_R || in.d.swizzle_g != NVN_TEXTURE_SWIZZLE_G ||
            in.d.swizzle_b != NVN_TEXTURE_SWIZZLE_B) return false;
        nvn_tex selected_in{}, selected_out{};
        nvn_view iv{}, ov{};
        bool ihv = false, ohv = false;
        if (texture_of((int)(in_h & 0xFFFFF), &selected_in, &iv, &ihv) &&
            texture_of((int)(out_h & 0xFFFFF), &selected_out, &ov, &ohv) && !ihv && !ohv &&
            selected_in.serial == in.serial && selected_out.serial == out.serial) {
            VkFormat input_format = VK_FORMAT_UNDEFINED, output_format = VK_FORMAT_UNDEFINED;
            const VkImageView input_view = texture_view(src, &in, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample,
                                                         &input_format, VK_FORMAT_R16G16B16A16_SFLOAT);
            const VkImageView output_view = texture_view(dst, &out, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage,
                                                          &output_format, VK_FORMAT_R16G16B16A16_SFLOAT);
            if (input_view && output_view && input_format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                output_format == VK_FORMAT_R16G16B16A16_SFLOAT &&
                typed_upscale(cmd, input_view, output_view, dst, 0, key, "PsychoV35 TAAU fallback", true)) {
                full_barrier(cmd);
                texture_written(dst);
                return true;
            }
        }
        return false; // Do not blit perceptually encoded PsychoV35 input into a linear output.
    }
    // (the gameplay networks too, when they are left out (no f16 shaders, vk_shader.cpp): their input is the scene's RGB9E5 bits
    // in a B10G11R11 image, and the blit below read them as B10G11R11, full-screen yellow and magenta noise with RX_VK_NO_F16.
    // Through the typed upscale as the TAAU network's stand-in (taau_stand_in), its edges smoothed in the shader)
    if (in.d.format == NVN_FORMAT_RGB9E5F && out.d.format == NVN_FORMAT_R11G11B10F &&
        src->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && dst->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
        (fd.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
        src->type == VK_IMAGE_TYPE_2D && dst->type == VK_IMAGE_TYPE_2D && src->depth == 1 && dst->depth == 1 &&
        src->layers == 1 && dst->layers == 1 && src->levels == 1 && dst->levels == 1 && !src->skip && !dst->skip &&
        in.d.swizzle_r == NVN_TEXTURE_SWIZZLE_R && in.d.swizzle_g == NVN_TEXTURE_SWIZZLE_G &&
        in.d.swizzle_b == NVN_TEXTURE_SWIZZLE_B) {
        VkFormatProperties e5{};
        vkGetPhysicalDeviceFormatProperties(dev.physical, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, &e5);
        const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        nvn_tex selected_in{}, selected_out{};
        nvn_view iv{}, ov{};
        bool ihv = false, ohv = false;
        if ((e5.optimalTilingFeatures & sampled) == sampled &&
            texture_of((int)(in_h & 0xFFFFF), &selected_in, &iv, &ihv) &&
            texture_of((int)(out_h & 0xFFFFF), &selected_out, &ov, &ohv) && !ihv && !ohv &&
            selected_in.serial == in.serial && selected_out.serial == out.serial) {
            VkFormat input_format = VK_FORMAT_UNDEFINED, output_format = VK_FORMAT_UNDEFINED;
            const VkImageView input_view = texture_view(src, &in, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, &input_format);
            const VkImageView output_view = texture_view(dst, &out, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage,
                                                         &output_format, VK_FORMAT_B10G11R11_UFLOAT_PACK32);
            if (input_format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && output_format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                typed_upscale(cmd, input_view, output_view, dst, 0, key, "generic RGB9E5 fallback",
                              gameplay_network(key) && !upscaler_off())) {   // (Upscaler Off: no edge smoothing either)
                full_barrier(cmd);
                texture_written(dst);
                return true;
            }
        }
    }
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {(int32_t)src->width, (int32_t)src->height, 1};
    bl.dstOffsets[1] = {(int32_t)dst->width, (int32_t)dst->height, 1};
    vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    full_barrier(cmd);
    texture_written(dst);
    if (dump_on()) {   // (in a frame dump: what it read and what it made)
        dump_image(cmd, src, 0, 0, "upscale stub source");
        dump_image(cmd, dst, 0, 0, "upscale stub result");
    }
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() < 32 && seen.insert(key).second)
        logf("vk: program %016llx does not translate: its dispatch replaced by a linear upscale of %#llx %ux%u (vk %d) "
             "into %#llx %ux%u (vk %d)", (unsigned long long)key, (unsigned long long)in.gpu, in.d.width, in.d.height,
             (int)src->format, (unsigned long long)out.gpu, out.d.width, out.d.height, (int)dst->format);
    (void)in_h, (void)out_h;
    return true;
}

/// (hookshot) OoT's gameplay TAAU network (fd029a317a9a4b94) stood in for: its input upscaled into its output
/// (shaders/taau_upscale.comp). The network's translation loses the blue channel, the yellow cast over every
/// scene; with this the picture is right, without the network's temporal anti-aliasing.
/// RX_VK_TAAU_KEYS=<program key prefixes,...> (default fd029a317a9a4b94; "-": none, the network runs)
bool taau_key(uint64_t key) {
    // (the real network where the card runs it as the console does; on the software multiplies it cost most of a frame on AMD's cards, and a
    // locked 60 with this in its place: Hookshot issue 52)
    static const char* keys = getenv("RX_VK_TAAU_KEYS") ? getenv("RX_VK_TAAU_KEYS")
                              : Shader::Backend::SPIRV::mma_software || !float16_shaders()
                                    ? "fd029a317a9a4b94" : "-";   // (no f16 shaders: vk_shader.cpp leaves the network out)
    char hex[24];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
    bool match = false;
    for (const char* q = keys; *q && !match;) {
        const char* e = q;
        while (*e && *e != ',') e++;
        match = e > q && *q != '-' && !strncmp(hex, q, (size_t)(e - q));
        q = *e ? e + 1 : e;
    }
    return match;
}
static bool typed_upscale(VkCommandBuffer cmd, VkImageView input_view, VkImageView output_view,
                          Texture* out, uint32_t level, uint64_t key, const char* keys, bool temporal) {
    if (!dev.push_descriptor || !vkCmdPushDescriptorSetKHR || !input_view || !output_view || !out) return false;
    const bool rgba = out->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    // (RGBA16F: Native PsychoV35's scene, the TAAU network's stand-in and the Upscaler's Off: HeyListen, after Longshot b80a68f1)
    if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !(rgba && scene_tonemap_on())) return false;
    struct Pipeline {
        VkPipeline pipe = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkSampler linear = VK_NULL_HANDLE;
        bool tried = false;
    };
    static Pipeline pipelines[2];
    Pipeline& pipeline = pipelines[rgba ? 1 : 0];
    auto& pipe = pipeline.pipe;
    auto& layout = pipeline.layout;
    auto& linear = pipeline.linear;
    auto& tried = pipeline.tried;
    if (!tried) {
        tried = true;
        VkDescriptorSetLayoutBinding b[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                             {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = 3, li.pBindings = b;
        VkDescriptorSetLayout set{};
        vkCreateDescriptorSetLayout(dev.device, &li, nullptr, &set);
        const VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1, pi.pSetLayouts = &set, pi.pushConstantRangeCount = 1, pi.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(dev.device, &pi, nullptr, &layout);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = rgba ? sizeof TAAU_UPSCALE_RGBA_COMP_SPV : sizeof TAAU_UPSCALE_COMP_SPV;
        mi.pCode = rgba ? TAAU_UPSCALE_RGBA_COMP_SPV : TAAU_UPSCALE_COMP_SPV;
        VkShaderModule m{};
        IntelDriverGate gate;
        if (guarded_shader_module(&mi, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = layout;
        if (!m || guarded_compute(VK_NULL_HANDLE, &ci, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        vkDestroyShaderModule(dev.device, m, nullptr);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.25f;
        vkCreateSampler(dev.device, &si, nullptr, &linear);
        logf("vk: typed upscale helper %s, %s output%s", keys, rgba ? "RGBA16F" : "packed",
             pipe ? "" : ": its pipeline was not made");
    }
    if (!pipe) return false;
    uint32_t size[3] = {std::max(1u, out->width >> level), std::max(1u, out->height >> level), 0};
    // (the stand-in's last output, which bounds this one's pixels: shaders/taau_upscale.comp. Made again at a new size,
    // the first frame unbounded; RX_VK_TAAU_NO_CLAMP=1: never bounded)
    struct History {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkImageView view = VK_NULL_HANDLE;
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint64_t frame = 0;
    };
    static History histories[2];
    History& history = histories[temporal ? 0 : 1];
    auto& hist = history.image;
    auto& hist_mem = history.memory;
    auto& hist_view = history.view;
    auto& hist_w = history.width;
    auto& hist_h = history.height;
    auto& hist_frame = history.frame;
    // (off by default: bounding a pixel by the last frame's smeared everything that moves (the user: blurry, warping)
    // and only dimmed the flashes; RX_VK_TAAU_CLAMP=1 turns it on)
    static const bool no_clamp = getenv("RX_VK_TAAU_CLAMP") == nullptr;
    // (the ones a new size replaced: given back 120 presents later, when no work in flight can use them. They were left, the
    // output's size changing with the dynamic resolution. RX_VK_TAAU_HIST_LEAK=1: as before)
    struct OldHist { VkImage image; VmaAllocation mem; VkImageView view; uint64_t frame; };
    static std::vector<OldHist> old_hist;
    static const bool leak = getenv("RX_VK_TAAU_HIST_LEAK") != nullptr;
    for (size_t i = 0; i < old_hist.size();) {
        if (texture_frame_no() - old_hist[i].frame < 120) { i++; continue; }
        if (old_hist[i].view) vkDestroyImageView(dev.device, old_hist[i].view, nullptr);
        destroy_image(old_hist[i].image, old_hist[i].mem);
        old_hist.erase(old_hist.begin() + (ptrdiff_t)i);
    }
    if (hist_w != size[0] || hist_h != size[1] || history.format != out->format) {
        // (the one before is kept for the while: work in flight may still use it)
        if (hist && !leak) old_hist.push_back({hist, hist_mem, hist_view, texture_frame_no()});
        hist = VK_NULL_HANDLE, hist_view = VK_NULL_HANDLE;
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D, ic.format = out->format;
        ic.extent = {size[0], size[1], 1}, ic.mipLevels = 1, ic.arrayLayers = 1, ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL, ic.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        if (!create_image(ic, hist, hist_mem)) { hist = VK_NULL_HANDLE; return false; }
        VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vc.image = hist, vc.viewType = VK_IMAGE_VIEW_TYPE_2D, vc.format = ic.format;
        vc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(dev.device, &vc, nullptr, &hist_view);
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};   // (UNDEFINED -> GENERAL, once)
        ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE, ib.srcAccessMask = VK_ACCESS_2_NONE;
        ib.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        ib.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = hist, ib.subresourceRange = vc.subresourceRange;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1, di.pImageMemoryBarriers = &ib;
        vkCmdPipelineBarrier2(cmd, &di);
        hist_w = size[0], hist_h = size[1], history.format = out->format, hist_frame = 0;
    }
    // (bounded when the history is last frame's or so: a pause of the TAAU, a menu, starts again unbounded)
    const uint64_t now_frame = texture_frame_no();
    size[2] = temporal && !no_clamp && hist_frame && now_frame - hist_frame <= 3 ? 1u : 0u;
    // (the gameplay stand-in's edges: FXAA in the shader. RX_VK_TAAU_FXAA=0: the plain linear upscale as before; 2: the
    // pixels it changed drawn red)
    static const int fxaa = getenv("RX_VK_TAAU_FXAA") ? atoi(getenv("RX_VK_TAAU_FXAA")) : 1;
    if (temporal && fxaa) size[2] |= fxaa == 2 ? 6u : 2u;
    hist_frame = now_frame;
    const VkDescriptorImageInfo in_info{linear, input_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo out_info{VK_NULL_HANDLE, output_view, VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo hist_info{VK_NULL_HANDLE, hist_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3]{{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                              {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstBinding = 0, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &in_info;
    w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[1].pImageInfo = &out_info;
    w[2].dstBinding = 2, w[2].descriptorCount = 1, w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[2].pImageInfo = &hist_info;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 3, w);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, size);
    vkCmdDispatch(cmd, (size[0] + 7) / 8, (size[1] + 7) / 8, 1);
    descriptors_disturbed();   // (the program's own pushes are gone from the bind point)
    if (temporal) probe_image(cmd, out, level, 0, key, 't');
    return true;
}
/// (hookshot) AMD FSR 3.1 in place of the gameplay upscale network (vk_fsr3.cpp), and its frame interpolation's inputs from it
/// (vk_fsr3_inputs.cpp): the same dispatches as temporal_stand_in below, found the same way. true: FSR 3.1 wrote the output
bool fsr3_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {
    if (p.stages.empty() || !fsr3_inputs::wanted()) return false;
    const Stage& st = p.stages[0];
    if (!gameplay_network(p.key) && !(st.info.uses_cooperative_matrix && st.info.texture_descriptors.size() == 1 &&
                                      st.info.image_descriptors.size() == 1))
        return false;
    if (st.info.texture_descriptors.empty() || st.info.image_descriptors.empty()) return false;
    size_t ntex = 0;   // (build_descriptors' image infos: the textures', then the images')
    for (const auto& d : st.info.texture_descriptors) ntex += d.count;
    if (dw().imgs.size() <= ntex || !dw().imgs[0].imageView || !dw().imgs[ntex].imageView) return false;
    Texture* in = nullptr;
    const uint32_t in_handle = handle_of(s, st, st.info.texture_descriptors[0], 0);
    if (in_handle) texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_UNDEFINED, &in);
    const auto& od = st.info.image_descriptors[0];
    Texture* out = nullptr;
    uint32_t level = 0;
    const uint32_t out_handle = handle_of(s, st, od, 0);
    if (out_handle) texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, image_format(od.format), &out, &level);
    VkImageView input_view = dw().imgs[0].imageView, output_view = dw().imgs[ntex].imageView;
    if (in && out && fsr3_inputs::colour_pair(in->format, out->format) && out->format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        input_view = texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_R16G16B16A16_SFLOAT);
        output_view = texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, VK_FORMAT_R16G16B16A16_SFLOAT);
    }
    return fsr3_upscale(cmd, p.key, gameplay_network(p.key), in, input_view, out, output_view, level);
}
/// (hookshot) A temporal upscaler (NVIDIA DLSS, AMD FSR 4: vk_temporal.cpp) in place of the gameplay upscale network, when
/// one is on and its inputs are as expected: the network's input as it samples it, its output's storage view
bool temporal_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {
    // (the 4/3 network and the full-size pass by their keys; the game's other gameplay upscales, one network for each fixed
    // share of the Render resolution row (67%: 6676619641320ba7, 58% f10782f9e58b6a7c, 50% 72ba090c70fe2614, 33%
    // 0775215f8da720e6 at 2048x1152), by their shape: a network sampling one texture into one image. temporal_upscale takes those
    // only when the texture is this frame's scene, in the gameplay networks' input format, and the image the picture)
    if (!temporal_wanted() || p.stages.empty()) return false;
    const Stage& st = p.stages[0];
    if (!gameplay_network(p.key) && !(st.info.uses_cooperative_matrix && st.info.texture_descriptors.size() == 1 &&
                                      st.info.image_descriptors.size() == 1))
        return false;
    if (st.info.texture_descriptors.empty() || st.info.image_descriptors.empty()) return false;
    size_t ntex = 0;   // (build_descriptors' image infos: the textures', then the images')
    for (const auto& d : st.info.texture_descriptors) ntex += d.count;
    if (dw().imgs.size() <= ntex || !dw().imgs[0].imageView || !dw().imgs[ntex].imageView) return false;
    Texture* in = nullptr;
    const uint32_t in_handle = handle_of(s, st, st.info.texture_descriptors[0], 0);
    if (in_handle) texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_UNDEFINED, &in);
    const auto& od = st.info.image_descriptors[0];
    Texture* out = nullptr;
    uint32_t level = 0;
    const uint32_t out_handle = handle_of(s, st, od, 0);
    if (out_handle) texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, image_format(od.format), &out, &level);
    VkImageView in_view = dw().imgs[0].imageView, out_view = dw().imgs[ntex].imageView;
    if (in && out && temporal_colour_pair(in->format, out->format) && out->format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        in_view = texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_R16G16B16A16_SFLOAT);
        out_view = texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, VK_FORMAT_R16G16B16A16_SFLOAT);
    }
    const bool done = temporal_upscale(cmd, p.key, gameplay_network(p.key), in, in_view, out, out_view, level);
    // (its actor motion may have drawn with a pipeline, viewport and descriptors of its own: the next draw sets all of its again)
    last_dyn_valid = false, last_desc_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;
    vertex_set.on = false, last_ib.on = false;
    return done;
}
/// (hookshot) The same for an upscale network that did not translate on this GPU (upscale_stub found its input, the largest
/// texture it samples, and its output, the largest image it writes): the Render resolution row's fixed shares (67%
/// 6676619641320ba7, 58%, 50%, 33%) are cooperative-matrix networks, left out without cooperative matrices (AMD RDNA 2), so
/// without this DLSS and FSR would only ever see the game's 3/4. True: the temporal upscaler wrote the output
static bool temporal_rejected_network(VkCommandBuffer cmd, uint64_t key, Texture* src, const nvn_tex* in, Texture* dst,
                                      const nvn_tex* out) {
    end_pass(cmd);
    VkFormat in_format = VK_FORMAT_UNDEFINED, out_format = VK_FORMAT_UNDEFINED;
    const VkImageView in_view = texture_view(src, in, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, &in_format);
    const bool rgba_pair = (temporal_colour_pair(src->format, dst->format) || fsr3_inputs::colour_pair(src->format, dst->format)) &&
                           dst->format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const VkImageView out_view = texture_view(dst, out, nullptr, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, &out_format,
                                              rgba_pair ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_B10G11R11_UFLOAT_PACK32);
    // (the gameplay networks' own formats: their input is the square root of the scene colour, which the fixed shares' networks
    // sample as RGB9E5 from the B10G11R11 image it is kept in (shaders/taau_upscale.comp), the 3/4 one as B10G11R11. Decoded,
    // both are what the temporal upscaler's prep pass takes. The movies' (RGBA8) are not the scene)
    const bool packed_pair = out_format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 &&
                             (in_format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || in_format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32);
    if (!in_view || !out_view || (!packed_pair && !temporal_colour_pair(in_format, out_format) &&
                                !fsr3_inputs::colour_pair(in_format, out_format)))
        return false;
    const bool done = temporal_upscale(cmd, key, false, src, in_view, dst, out_view, 0);
    last_dyn_valid = false, last_desc_valid = false, last_vb_valid = false, last_pipeline = VK_NULL_HANDLE;
    vertex_set.on = false, last_ib.on = false;
    if (!done) return false;
    full_barrier(cmd);
    texture_written(dst);
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() < 32 && seen.insert(key).second)
        logf("vk: network %016llx does not translate here: the temporal upscaler draws its upscale (%ux%u -> %ux%u)",
             (unsigned long long)key, src->width, src->height, dst->width, dst->height);
    return true;
}
/// (hookshot) The Upscaler row's Off: the gameplay upscale networks (the keyed ones, and the Render resolution's fixed shares by
/// their shape and formats, as temporal_upscale takes them) are not run: their dispatch draws the plain linear upscale of the
/// 3D input, the typed path the stub has (shaders/taau_upscale.comp) with no FXAA and no history bound. At 100% (the same-size
/// pass 9dc98871) that is the scene's colour decoded into the picture, a copy. No temporal state is touched. After fsr3_stand_in
/// (frame generation's inputs are still made) and temporal_stand_in (not drawing when Off), before the TAAU stand-in
static bool off_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {
    if (p.stages.empty() || !dev.push_descriptor || !vkCmdPushDescriptorSetKHR) return false;
    const Stage& st = p.stages[0];
    const bool known = gameplay_network(p.key);
    if (!known && !(st.info.uses_cooperative_matrix && st.info.texture_descriptors.size() == 1 &&
                    st.info.image_descriptors.size() == 1))
        return false;
    if (st.info.texture_descriptors.empty() || st.info.image_descriptors.empty() || !upscaler_off()) return false;
    size_t ntex = 0;   // (build_descriptors' image infos: the textures', then the images')
    for (const auto& d : st.info.texture_descriptors) ntex += d.count;
    if (dw().imgs.size() <= ntex || !dw().imgs[0].imageView || !dw().imgs[ntex].imageView) return false;
    const auto& od = st.info.image_descriptors[0];
    const uint32_t out_handle = handle_of(s, st, od, 0);
    if (!out_handle) return false;
    Texture* out = nullptr;
    uint32_t level = 0;
    texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, image_format(od.format), &out, &level);
    if (!out) return false;
    // (Native PsychoV35: its RGBA16F scene into the RGBA16F picture, the typed upscale's RGBA16F shader, which decodes the
    // network input's perceptual colour as the packed one does; never a pair of the two formats. After Longshot b80a68f1)
    const bool rgba = out->format == VK_FORMAT_R16G16B16A16_SFLOAT && scene_tonemap_on();
    if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && !rgba) return false;
    const uint32_t in_handle = handle_of(s, st, st.info.texture_descriptors[0], 0);
    if (!known || rgba) {   // (another network of the upscale's shape: only the scene into the picture, the rest runs)
        Texture* in = nullptr;
        if (in_handle) texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_UNDEFINED, &in);
        if (!in || in->format != out->format) return false;
        if (!known && (out->width >> level) <= in->width) return false;
    }
    VkImageView input_view = dw().imgs[0].imageView, output_view = dw().imgs[ntex].imageView;
    if (rgba) {
        input_view = texture_view_of(in_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_R16G16B16A16_SFLOAT);
        output_view = texture_view_of(out_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, VK_FORMAT_R16G16B16A16_SFLOAT);
        if (!input_view || !output_view) return false;
    }
    if (!typed_upscale(cmd, input_view, output_view, out, level, p.key, "Upscaler Off", false))
        return false;
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() < 32 && seen.insert(p.key).second)
        logf("vk: Upscaler Off: network %016llx not run, its dispatch draws the plain upscale into %ux%u",
             (unsigned long long)p.key, out->width >> level, out->height >> level);
    return true;
}
bool taau_stand_in(VkCommandBuffer cmd, const NvnState& s, const Program& p) {
    if (!taau_key(p.key) || !dev.push_descriptor || !vkCmdPushDescriptorSetKHR || p.stages.empty()) return false;
    const Stage& st = p.stages[0];
    if (st.info.texture_descriptors.empty() || st.info.image_descriptors.empty()) return false;
    size_t ntex = 0;   // (build_descriptors' image infos: the textures', then the images')
    for (const auto& d : st.info.texture_descriptors) ntex += d.count;
    if (dw().imgs.size() <= ntex || !dw().imgs[0].imageView || !dw().imgs[ntex].imageView) return false;
    const auto& od = st.info.image_descriptors[0];
    Texture* out = nullptr;
    uint32_t level = 0;
    const uint32_t handle = handle_of(s, st, od, 0);
    if (!handle) return false;
    texture_view_of(handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, image_format(od.format), &out, &level);
    if (!out) return false;
    VkImageView input_view = dw().imgs[0].imageView, output_view = dw().imgs[ntex].imageView;
    if (out->format == VK_FORMAT_R16G16B16A16_SFLOAT && scene_tonemap_on()) {
        const uint32_t input_handle = handle_of(s, st, st.info.texture_descriptors[0], 0);
        Texture* input = nullptr;
        if (!input_handle) return false;
        texture_view_of(input_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_UNDEFINED, &input);
        if (!input || input->format != VK_FORMAT_R16G16B16A16_SFLOAT) return false;
        input_view = texture_view_of(input_handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Sample, VK_FORMAT_R16G16B16A16_SFLOAT);
        output_view = texture_view_of(handle, VK_IMAGE_VIEW_TYPE_2D, ViewUse::Storage, VK_FORMAT_R16G16B16A16_SFLOAT);
        if (!input_view || !output_view) return false;
    } else if (out->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
        return false;
    }
    const char* keys = getenv("RX_VK_TAAU_KEYS") ? getenv("RX_VK_TAAU_KEYS") : "fd029a317a9a4b94";
    return typed_upscale(cmd, input_view, output_view, out, level, p.key, keys, true);
}

// ---- (TICK-038) resources: the pipelines the game binds (RX_VK_RES_LOG=1), and compute programs' shader modules let go ----
namespace {
std::mutex res_use_mu;
std::unordered_set<VkPipeline> res_used;
}
void res_note_pipeline_use(VkPipeline pl) {
    std::lock_guard lk(res_use_mu);
    res_used.insert(pl);
}
size_t res_pipelines_used() {
    std::lock_guard lk(res_use_mu);
    return res_used.size();
}
/// RX_VK_CS_MODULE_DROP: a compute program's shader module is let go once its pipeline is made (nothing reads it after: the
/// dispatch builds from it only while compute_pipeline is null). The driver keeps each module's SPIR-V; 0: kept, as before
bool cs_module_drop() {
    static const bool v = !getenv("RX_VK_CS_MODULE_DROP") || atoi(getenv("RX_VK_CS_MODULE_DROP")) != 0;
    return v;
}
std::atomic<int64_t> cs_modules_dropped{0};
std::atomic<int64_t> cs_cache_hits{0}, cs_cache_misses{0};   // (compute pipelines built in play: in the pipeline cache or not)
void res_pipeline_counts(size_t* graphics, size_t* used, int64_t* modules_dropped) {
    *graphics = pipelines.size();   // (the replay's map, read for a count only)
    *used = res_pipelines_used();
    *modules_dropped = cs_modules_dropped.load();
    logf("vk: compute pipelines built in play: %lld from the pipeline cache (or under 30 ms), %lld compiled", (long long)cs_cache_hits.load(),
         (long long)cs_cache_misses.load());
    if (const int64_t z = null_writes[kNwAddressZero].load(), nb = null_writes[kNwNotBuffer].load(), np = null_writes[kNwNotBound].load(); z + nb + np)
        logf("vk: written storage buffers that resolved to nothing (RX_VK_NULL_WRITE %s): %lld in all, address 0 %lld, not a buffer %lld, pool not bound %lld",
             null_write_on() ? "on: scratch bound" : "off", (long long)(z + nb + np), (long long)z, (long long)nb, (long long)np);
}
void cs_module_release(Program& p) {
    if (!cs_module_drop() || !p.compute || !std::atomic_ref<VkPipeline>(p.compute_pipeline).load(std::memory_order_acquire) || p.stages.empty() || !p.stages[0].module) return;
    vkDestroyShaderModule(dev.device, p.stages[0].module, nullptr);
    p.stages[0].module = VK_NULL_HANDLE;
    cs_modules_dropped++;
}

bool precompile_wanted() {
#if defined(HOOKSHOT_RELEASE)
    bool on = true;
#else
    bool on = false;
#endif
    if (const char* e = getenv("RX_VK_PRECOMPILE")) on = atoi(e) != 0;
    return on;
}
namespace {
std::atomic<bool> prepare_halt{false};
}
void prepare_stop() { prepare_halt.store(true); }
bool prepare_stopped() { return prepare_halt.load(std::memory_order_relaxed); }
std::string prepared_path() {
    char exe[MAX_PATH];
    const DWORD exe_n = game_module_path(exe, MAX_PATH);
    std::string path(exe, exe_n);
    return path.substr(0, path.find_last_of("\\/") + 1) + "hookshot_prepared.bin";
}
// (hookshot) What the last prepare here was made for, in hookshot_prepared.bin: the translator (program_cache_stamp), and since the
// seed grows apart from it (4221050d: networks, the movie upscaler, the particle sets, 19 compute programs more) the seed it went
// through (hookshot_shaders.bin), by a hash of its contents. By the translator alone, a folder prepared before such an update never
// prepared the new programs: each was translated in play when first met, 200-400 ms apiece. The seed is 500 MB and more: its hash is
// taken again only when its size or write time is not the recorded one, and a hash that is still the same records the new time.
// A write time says nothing of a write that came within its resolution of the one recorded (whole seconds from libstdc++'s
// std::filesystem on Windows, 2 s on FAT, a timer tick at best): as git does for its index, the shortcut is only taken for a seed
// written well before the record was (2 s); one written closer to it is hashed
namespace {
struct PreparedFor {
    uint64_t stamp = 0, seed_hash = 0, seed_size = 0;
    int64_t seed_time = 0, recorded_at = 0;   // (Windows: 100 ns FILETIME ticks; elsewhere: filesystem-clock nanoseconds)
    bool timed = false;   // (recorded_at read: records of four words had none)
    uint64_t kept = 0;   // (the program keys the cache held when the prepare ended; 0: a record from before it was written)
};
#ifdef _WIN32
constexpr int64_t kSeedTimeSlack = 20'000'000;   // (two seconds in FILETIME ticks)
#else
constexpr int64_t kSeedTimeSlack = 2'000'000'000;
#endif
enum class PrepareWhy { None, First, Translator, Seed, Cache };
/// The seed's size and write time (false: there is none). Windows keeps native 100 ns FILETIME ticks: converting that
/// epoch to nanoseconds overflows int64_t. Other platforms keep filesystem-clock nanoseconds
bool seed_file_id(const std::string& path, uint64_t* size, int64_t* time) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    *time = (int64_t)((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime);
#else
    std::error_code ec;
    const auto fsize = std::filesystem::file_size(path, ec);
    if (ec) return false;
    const auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    *size = (uint64_t)fsize;
    *time = (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();
#endif
    return true;
}
/// Now, on the clock seed_file_id's times are of
int64_t file_clock_now() {
#ifdef _WIN32
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return (int64_t)((uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime);
#else
    return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::filesystem::file_time_type::clock::now().time_since_epoch()).count();
#endif
}
/// The seed's contents, hashed four words at a time: ~0.1 s for its 540 MB read from memory, 0.3-0.6 s from the disk (a byte at a
/// time, as hash_bytes, 0.45 s from memory). Only after the file was written again
bool seed_file_hash(const std::string& path, uint64_t* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    constexpr uint64_t p1 = 0x9E3779B185EBCA87ull, p2 = 0xC2B2AE3D27D4EB4Full;
    const auto mix = [](uint64_t h, uint64_t w) {
        h += w * p2;
        return ((h << 31) | (h >> 33)) * p1;
    };
    uint64_t lane[4] = {p1 + p2, p2, 0, 0 - p1}, total = 0, tail = 0xCBF29CE484222325ull;
    std::vector<uint8_t> buf(1 << 20);   // (a multiple of 32: only the last read leaves bytes over)
    for (size_t n; (n = fread(buf.data(), 1, buf.size(), f)) > 0;) {
        total += n;
        size_t at = 0;
        for (; at + 32 <= n; at += 32) {
            uint64_t w[4];
            std::memcpy(w, buf.data() + at, 32);
            for (int k = 0; k < 4; k++) lane[k] = mix(lane[k], w[k]);
        }
        for (; at < n; at++) tail = (tail ^ buf[at]) * 1099511628211ull;
    }
    const bool ok = !ferror(f);
    fclose(f);
    uint64_t h = total * p1 ^ tail;
    for (const uint64_t l : lane) h = mix(h ^ mix(0, l), p1);
    *out = h;
    return ok;
}
/// The record: 0 none, 1 the translator alone (written before the seed was), 2 both. A record without the time it was written
/// at (four words) never takes the shortcut: its seed is hashed, and the record written again with it
int prepared_read(const std::string& path, PreparedFor* had) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint64_t r[6]{};
    const size_t n = fread(r, 8, 6, f);
    fclose(f);
    had->stamp = r[0], had->seed_hash = r[1], had->seed_size = r[2], had->seed_time = (int64_t)r[3];
    had->recorded_at = (int64_t)r[4], had->timed = n >= 5, had->kept = n >= 6 ? r[5] : 0;
    return n >= 4 ? 2 : n >= 1 ? 1 : 0;
}
void prepared_write(const std::string& path, const PreparedFor& now) {
    const uint64_t r[6] = {now.stamp, now.seed_hash, now.seed_size, (uint64_t)now.seed_time, (uint64_t)file_clock_now(), now.kept};
    if (FILE* f = fopen(path.c_str(), "wb")) fwrite(r, 8, 6, f), fclose(f);
}
/// A preparation began here: the record says no translator (stamp 0), so that the next start prepares again whichever way this one
/// ends. Not removed: a start that finds no record is a first start, and drops what the driver's ledger lists
void prepared_mark_running(const std::string& path, PreparedFor now) {
    now.stamp = 0, now.kept = 0;
    prepared_write(path, now);
}
/// Whether to prepare (and why), and what for: *now, written once the prepare is done. No seed: the translator alone, as before.
/// *hash_ms: how long the seed's hash took (0: not taken)
PrepareWhy prepare_check(const std::string& path, const std::string& seed, uint64_t stamp, PreparedFor* now, double* hash_ms) {
    *hash_ms = 0;
    PreparedFor had;
    const int read = prepared_read(path, &had);
    now->stamp = stamp;
    now->kept = had.kept;
    const bool have_seed = seed_file_id(seed, &now->seed_size, &now->seed_time);
    bool same_seed = !have_seed;
    if (have_seed && read == 2 && had.seed_size == now->seed_size && had.seed_time == now->seed_time &&
        had.timed && had.seed_time < had.recorded_at - kSeedTimeSlack) {
        now->seed_hash = had.seed_hash, same_seed = true;   // (not read again)
    } else if (have_seed) {
        const auto t0 = std::chrono::steady_clock::now();
        const bool hashed = seed_file_hash(seed, &now->seed_hash);
        *hash_ms = std::max(1e-3, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        same_seed = hashed && read == 2 && had.seed_hash == now->seed_hash &&
                    had.seed_size == now->seed_size;
        // (rewritten as it was, by a build: the new time kept, so the next start does not hash it again; a seed written just
        // before the record is hashed again until a record is written 2 s after it)
        if (same_seed && had.stamp == stamp) prepared_write(path, *now);
    }
    if (!read) return PrepareWhy::First;
    if (had.stamp != stamp) return PrepareWhy::Translator;
    if (!same_seed) return PrepareWhy::Seed;
    // (the record says prepared, but the program cache may not have kept it: a start with other shader settings (RX_VK_OOT_*, ...,
    // vk_shader.cpp's disk_stamp) makes the cache again under its stamp, and closed before it prepared, the next start under the
    // usual settings found an empty cache and a record that said prepared: every program then translated in play, 200-400 ms
    // each. Less than half of what the last prepare left, or none for a record from before the count: prepared again)
    const size_t kept = program_cache_kept();
    if (kept != SIZE_MAX && (had.kept ? kept < had.kept / 2 : kept == 0)) return PrepareWhy::Cache;
    return PrepareWhy::None;
}
} // namespace
namespace {
std::atomic<bool> lazy_update{false};
/// The guard of a lazy update: ninety-odd of the seed's graphics programs, evenly spread, are translated now (the program cache they go to
/// is the new translator's, empty), and how many have a fingerprint the ledger has not seen is counted. *measured: how many were
/// translated (the others were kept already, from an earlier start of this update, or do not translate)
double update_changed_share(size_t* measured) {
    *measured = 0;
    const size_t programs = seed_open();
    if (!programs) return 0;
    std::vector<uint64_t> fps;
    fingerprint_probe = &fps;
    const size_t want = 96, stride = std::max<size_t>(1, programs / want);
    for (size_t i = stride / 2; i < programs && fps.size() < want; i += stride) {
        if (seed_is_compute(i)) continue;
        if (std::unique_ptr<Program> p = seed_program(i, 0))
            for (Stage& st : p->stages)
                if (st.module) vkDestroyShaderModule(dev.device, st.module, nullptr);
    }
    fingerprint_probe = nullptr;
    size_t changed = 0;
    for (const uint64_t fp : fps) {
        if (!ledger_has(ledger_id_seen(fp))) changed++;
        ledger_add(ledger_id_seen(fp));
    }
    seed_close();
    *measured = fps.size();
    return fps.empty() ? 0 : (double)changed / fps.size();
}
}  // namespace
bool prepare_is_lazy_update() { return lazy_update.load(); }
/// (hookshot) An update: every shader was prepared once here, for another translator. Each release changes the translator, and every
/// update then held the game's start for the whole seed again (116k variants, minutes on most PCs: "shaders need to be prepared every
/// update"). Now only what the notes name is built before the game (precompile_run), the rest in play behind it (prepare_background):
/// with a lazy update (RX_VK_SHADER_KEY on, see below) nothing is translated before the game, a program a draw needs is translated by that
/// draw, and the rest behind it, the driver given only what its fingerprint says it has not built
bool prepare_is_update() {
    // (RX_VK_PREPARE_BACKGROUND=1: the old opt-in, whatever the driver's caches hold. Otherwise, with RX_VK_SHADER_KEY on, an update
    // is lazy by itself when the last preparation here was whole and the driver's caches are what it left: RX_VK_LAZY_UPDATE=0 off.
    // The first start, a wiped cache and a preparation cut short are never one: they prepare before the game, as a first start does)
    if (getenv("RX_VK_PREPARE_AGAIN")) return false;
    const char* bg = getenv("RX_VK_PREPARE_BACKGROUND");
    const bool forced = bg && atoi(bg) != 0;
    const bool lazy = !bg && shader_key_on() && (!getenv("RX_VK_LAZY_UPDATE") || atoi(getenv("RX_VK_LAZY_UPDATE")) != 0);
    if (!forced && !lazy) return false;
    FILE* f = fopen(prepared_path().c_str(), "rb");
    if (!f) return false;
    uint64_t have = 0;
    const bool read = fread(&have, 8, 1, f) == 1;
    fclose(f);
    // (a record of stamp 0 is the mark of a preparation that began and did not end, prepared_mark_running)
    if (!read || !have || have == program_cache_stamp()) return false;
    if (forced) return true;
    pcache_init();
    if (!pcache_loaded) {   // (the driver's data is not here: what the ledger lists is not in it, and the ledger is dropped with it)
        logf("vk: the pipeline cache has no data of the last start's: the shaders are prepared before the game");
        return false;
    }
    if (!ledger_size()) {   // (nothing listed as built: the last preparation was before the key, or its ledger is gone)
        logf("vk: no driver builds are listed (hookshot_compiled.bin): the shaders are prepared before the game");
        return false;
    }
    // (a translator change that alters many shaders would have the game compile them as it meets them: stalls of seconds, with the
    // driver at its slowest, where a start that prepares first spends the same time before the game. A sample says which this is)
    size_t measured = 0;
    const double share = update_changed_share(&measured);
    static const double limit = getenv("RX_VK_LAZY_MAX_CHANGED") ? atof(getenv("RX_VK_LAZY_MAX_CHANGED")) / 100.0 : 0.10;
    logf("vk: update: %.0f%% of %zu sampled programs translate differently than the last start's (the limit is %.0f%%, over it the "
         "shaders are prepared before the game)", share * 100, measured, limit * 100);
    if (measured >= 16 && share > limit) return false;
    lazy_update.store(true);
    return true;
}
namespace {
/// (RX_VK_RES_LOG's "process memory" numbers for the preparation's progress line: the process's private commit and what its C heaps
/// hold in use, in MB. A preparation that holds on to what it translated shows here, long before the commit limit refuses a
/// C++ allocation)
void prepare_mem(unsigned long long* private_mb, unsigned long long* heaps_mb) {
    struct Counters {
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
            QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    };
    struct Summary { DWORD cb; SIZE_T allocated, committed, reserved, max_reserve; };
    using MemInfo = BOOL(WINAPI*)(HANDLE, Counters*, DWORD);
    using HeapSum = BOOL(WINAPI*)(HANDLE, DWORD, Summary*);
    static const MemInfo mem_info = (MemInfo)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    static const HeapSum heap_sum = (HeapSum)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "HeapSummary");
    Counters m{};
    m.cb = sizeof m;
    if (mem_info) mem_info(GetCurrentProcess(), &m, sizeof m);
    unsigned long long alloc = 0;
    HANDLE heaps[64];
    const DWORD nh = GetProcessHeaps(64, heaps);
    for (DWORD i = 0; i < nh && i < 64 && heap_sum; i++) {
        Summary su{};
        su.cb = sizeof su;
        if (heap_sum(heaps[i], 0, &su)) alloc += su.allocated;
    }
    *private_mb = (unsigned long long)(m.PrivateUsage >> 20);
    *heaps_mb = alloc >> 20;
}
/// The commit the system still has to give (MB): its limit, RAM and the page file, less what every process has committed. A C++
/// allocation that finds none is refused ("0 MB of commit left"): the game's own, and any other program's. The driver's compiles
/// ask the process's heaps for what they need, and a program of a million SPIR-V words took gigabytes of them, all there was
unsigned long long commit_left_mb() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (unsigned long long)(m.ullAvailPageFile >> 20) : ~0ull;
}
}  // namespace
std::atomic<double> prepare_cost_fraction{-1.0};   // (-1: no preparation runs here; see prepare_work.h)
double prepare_progress_by_cost() { return prepare_cost_fraction.load(); }
namespace {
/// (hookshot) Where a preparation's time goes, by kind of work: how many, the thread time in them and the longest one. A graphics
/// program, a native mesh program, a mesh fallback program (the three: the program's translation and what it keeps), a compute
/// program (its texture variants' translations; the driver's builds of them are the next) and the driver's builds (a compute pipeline
/// of each variant, a graphics program's stage libraries)
struct PrepKind {
    std::atomic<int64_t> n{0}, us{0}, max_us{0};
    void add(std::chrono::steady_clock::time_point since) {
        const int64_t d = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - since).count();
        n++, us += d;
        int64_t m = max_us.load();
        while (d > m && !max_us.compare_exchange_weak(m, d)) {}
    }
    void log(const char* what) const {
        if (n.load()) logf("vk: prepare profile: %-24s %7lld x, %8.1f s thread time, %6.0f ms each, longest %.1f s", what, (long long)n.load(),
                           us.load() / 1e6, us.load() / 1e3 / (double)n.load(), max_us.load() / 1e6);
    }
};
/// (hookshot) The preparation's workers. A worker is one thread working through the seed's programs; the driver's compiles are most of
/// a worker's time (80% on an RTX 3050), and a thread inside one is the CPU's, not ours, so as many as the machine has threads is
/// right: half of them, as it was, left the other half of the CPU idle for the whole of a first start's wait. What the driver's
/// compiles hold in memory bounds it instead (prepare_commit_gate). RX_VK_PREPARE_WORKERS=<n>: that many (1 to 64); the old count
/// (half the CPUs) is RX_VK_PREPARE_WORKERS=old
unsigned prepare_worker_count(bool background, unsigned cpus) {
    return prepare_work::workers(background, cpus, getenv("RX_VK_PREPARE_WORKERS"));
}
/// (hookshot) Which texture variants of a compute program the driver builds before the game. A compute program is translated for the
/// texture types its questions can get (up to 64 variants, seed_compute), the plain answers (every texture a 2D colour image: what the
/// game binds to nearly all of them) first. RX_VK_WARM_VARIANTS=all: every variant is built, as before (10330 compute pipelines for 6306
/// programs: on an RTX 3050 the variants were half of the pipelines and 78% of the driver's time, 3054 s of 3893); base (the default): only the plain one is, here and in the precompile, and a variant the game does bind is built when its
/// first dispatch asks (a worker, the dispatch left out meanwhile as for any compute pipeline) and kept for the next start
bool warm_base_only() {
    static const bool base = [] {
        const char* e = getenv("RX_VK_WARM_VARIANTS");
        return !(e && !strcmp(e, "all"));
    }();
    return base;
}
/// (hookshot) Which compute programs the driver builds before the game: with a list of the ones play dispatched (hookshot_computes.bin:
/// a start that has played, or a list that came with the build) only those, the rest when first dispatched as on any later start
/// (RX_VK_LAZY_COMPUTE); without one, every kept one, as there was nothing to go by. RX_VK_WARM_COMPUTES=all: every one whatever the
/// list says; used: those of the list (a list of fewer than 32 keys is no list)
bool warm_used_only(const std::unordered_set<uint64_t>& used) {
    const char* e = getenv("RX_VK_WARM_COMPUTES");
    if (e && !strcmp(e, "all")) return false;
    return used.size() >= 32 || (e && !strcmp(e, "used") && !used.empty());
}
/// (hookshot) A GPU without mesh shaders: the precompile builds the compute pipelines of every program translated for the fallback
/// (1286 here), not only those the notes name. On an RTX 3050 that is 3 minutes; AMD's compiler took 3 to 12 s on each. RX_VK_PRECOMPILE_FALLBACKS=noted:
/// only those the notes (and the state list) name; the rest are built at their first draw, as the others are
bool fallbacks_noted_only() {
    const char* e = getenv("RX_VK_PRECOMPILE_FALLBACKS");
    return e && !strcmp(e, "noted");
}
thread_local unsigned warm_seen = 0;   // (the variants of the program this worker is on that were asked for so far: the first is the plain one)
/// (hookshot) A driver compile asks for memory (a program of a million SPIR-V words took gigabytes, and several of them at once
/// are what the commit limit refuses): before one begins, while the system's commit left is under a reserve and another worker is
/// in the driver, it waits, so that more workers than before cannot be what runs the PC out. Alone it goes on, as before
struct CommitGate {
    static std::atomic<int>& inside() {
        static std::atomic<int> n{0};
        return n;
    }
    static std::atomic<int64_t>& waited_ms() {
        static std::atomic<int64_t> ms{0};
        return ms;
    }
    CommitGate() {
        static const unsigned long long reserve = getenv("RX_VK_PREPARE_COMMIT_MB") ? strtoull(getenv("RX_VK_PREPARE_COMMIT_MB"), nullptr, 10) : 3072;
        const auto t0 = std::chrono::steady_clock::now();
        while (reserve && inside().load() > 0 && commit_left_mb() < reserve && !prepare_stopped()) Sleep(100);
        const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (ms > 50) waited_ms() += ms;
        inside()++;
    }
    ~CommitGate() { inside()--; }
    CommitGate(const CommitGate&) = delete;
    CommitGate& operator=(const CommitGate&) = delete;
};
}  // namespace
// ---- (Longshot c392df3a, Tomiss, PR #426: compute_gate.h, widened here to the start-up graphics pipelines: issue 88) how many of
// the GPU driver's compiles run at once. A driver's compile takes the PC's RAM, not the GPU's: Mesa's Intel compiler spent
// gigabytes on the game's large programs, and the start-up build ran twelve at once on twelve threads on a 31 GB PC until the
// kernel killed the process at 1,360 of 16,358 (the reporter's log, issue 88, with the shader preparation finished). The same
// shape upstream measured on an Iris Xe. The gate weighs each compile (gpu/vk/compile_gate.h) and starts one beside the others
// only while their weights fit in what was available past the gate's reserve, and the memory is still there now.
namespace {
/// (Linux) What the process's cgroup v2 limits leave, in MB: the least of memory.max less memory.current over its cgroup and
/// those above it (a limit on a parent slice binds too); kUnknownMB with none set, or no cgroup v2 (compile_gate.h
/// cgroup_room_mb). MemAvailable does not see them, and a cgroup's OOM killer ends the game whatever the system has
uint64_t cgroup_available_mb() {
#if defined(__linux__)
    FILE* f = std::fopen("/proc/self/cgroup", "r");
    if (!f) return compile_gate::kUnknownMB;
    char line[512] = "";
    std::string path;
    while (std::fgets(line, sizeof line, f))
        if (const char* at = std::strstr(line, "::")) {   // (v2's one line: "0::/user.slice/...")
            path = at + 2;
            while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
            break;
        }
    std::fclose(f);
    if (path.empty()) return compile_gate::kUnknownMB;
    uint64_t room = compile_gate::kUnknownMB;
    for (;;) {   // (its own cgroup and each above it: a slice's MemoryMax binds what its children do)
        const std::string dir = "/sys/fs/cgroup" + (path == "/" ? std::string() : path);
        auto read1 = [&](const char* name) {
            std::string v;
            if (FILE* g = std::fopen((dir + "/" + name).c_str(), "rb")) {
                char b[64] = "";
                if (std::fgets(b, sizeof b, g)) v = b;
                std::fclose(g);
            }
            return v;
        };
        const std::string max = read1("memory.max"), current = read1("memory.current");
        if (!current.empty()) {
            unsigned long long cur = 0;
            if (std::sscanf(current.c_str(), "%llu", &cur) == 1) room = std::min(room, compile_gate::cgroup_room_mb(max.c_str(), cur));
        }
        if (path == "/" || path.empty()) break;
        const size_t slash = path.find_last_of('/');
        path = slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
    }
    return room;
#else
    return compile_gate::kUnknownMB;
#endif
}
}  // namespace
/// MB the system has for the process now: MemAvailable on Linux (the POSIX shim's GlobalMemoryStatusEx ullAvailPhys reads it
/// from /proc/meminfo), ullAvailPhys on Windows, and the least of that and the cgroup's. kUnknownMB when it cannot be read
uint64_t memory_available_mb() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    const uint64_t avail = GlobalMemoryStatusEx(&m) ? (uint64_t)(m.ullAvailPhys >> 20) : compile_gate::kUnknownMB;
    // (the least of the system's and the cgroup's: kUnknownMB is the largest, so either one alone stands when the other is not known)
    return std::min(avail, cgroup_available_mb());
}
namespace {
/// Whether the gate is on by itself: Linux with Mesa's Intel driver (ANV; the reporter's Arc B580 and upstream's Iris Xe), and
/// Linux with RADV on an integrated GPU on a PC with under 13 GB of RAM (upstream's rule, #1398's 12 GB Radeon 760M). Nothing
/// on Windows: IntelDriverGate and the commit rule already hold its compiles and its allocations, and a Windows run must behave
/// as it did. RX_VK_COMPILE_AT_ONCE=<n> or 0 says otherwise on any GPU
bool compile_gate_by_memory() {
#if defined(__linux__)
    if (dev.props.vendorID == 0x8086) return true;
    if (dev.props.vendorID == 0x1002 && dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof m;
        return compile_gate::radv_by_memory(true, GlobalMemoryStatusEx(&m) ? (uint64_t)(m.ullTotalPhys >> 20) : 0);
    }
#endif
    return false;
}
/// The gate, set once: RX_VK_COMPILE_AT_ONCE=<n> (at most n at once, whatever the memory; 0: no limit, as before), or by
/// memory with the budget of what is available now
compile_gate::Gate& compile_compiles() {
    static compile_gate::Gate gate;
    static std::once_flag once;
    std::call_once(once, [] {
        const char* e = getenv("RX_VK_COMPILE_AT_ONCE");
        if (e && *e) {
            const unsigned n = (unsigned)std::max(0, atoi(e));
            if (n) {
                gate.set(n, false);
                logf("vk: the driver's compiles: at most %u at once (RX_VK_COMPILE_AT_ONCE)", n);
            } else {
                logf("vk: the driver's compiles: no gate (RX_VK_COMPILE_AT_ONCE=0)");
            }
        } else if (compile_gate_by_memory()) {
            gate.set(0, true, compile_gate::budget_for(memory_available_mb()));
            logf("vk: the driver's compiles: as many at once as the memory holds (RX_VK_COMPILE_AT_ONCE=<n> or 0 says otherwise)");
        }
    });
    return gate;
}
/// What the memory past the reserve leaves for the compiles, and whether it is the memory that decides (compile_gate_begin)
std::atomic<bool> compile_gate_memory_rule{false};
std::atomic<uint64_t> compile_gate_budget_mb{0};
/// The budget is what is available when a phase starts (the preparation's own step may be long enough for it to change). Returns
/// the budget in MB (0: no memory rule, or not known), which the start-up build reads to keep out a compile that could not fit
/// beside anything, or at all
uint64_t compile_gate_begin() {
    compile_gate::Gate& gate = compile_compiles();
    if (!gate.on()) {
        compile_gate_memory_rule.store(false, std::memory_order_relaxed);
        return 0;
    }
    // (only the memory rule has a budget to refresh: an asked count stands)
    const char* e = getenv("RX_VK_COMPILE_AT_ONCE");
    if (e && *e) {
        compile_gate_memory_rule.store(false, std::memory_order_relaxed);
        return 0;
    }
    const uint64_t avail = memory_available_mb();
    const uint64_t budget = compile_gate::budget_for(avail);
    gate.set(0, true, budget);
    compile_gate_budget_mb.store(budget, std::memory_order_relaxed);
    compile_gate_memory_rule.store(true, std::memory_order_relaxed);
    logf("vk: the compiles: as many at once as %llu MB hold (%llu MB available now, %llu left to the PC), at most %u; one of "
         "%llu MB at the least (gpu/vk/compile_gate.h)", (unsigned long long)budget,
         (unsigned long long)(avail == compile_gate::kUnknownMB ? 0 : avail), (unsigned long long)compile_gate::kReserveMB,
         compile_gate::kMost, (unsigned long long)compile_gate::kWordsSmallMB);
    return budget;
}
/// What a compile weighs at the gate: a program's SPIR-V (all its stages, in 32-bit words, as the start-up build's pipelines are
/// weighed); a compute program's size is not known before it is read, so a pinned one (every compute stage, where the device
/// runs 32 lanes) weighs kPinnedMB and another kFreeMB, as upstream's
uint64_t compile_weight(const Program& p) {
    uint64_t words = 0;
    for (const Stage& st : p.stages) words += st.spirv_words;
    if (words) return compile_gate::weight_words(words);
    const bool pinned = (dev.sg32_stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;
    return compile_gate::weight(pinned, 0, p.fallback);
}
/// (the start-up build, the prediction) Whether this program's compile is too heavy to run at all: the driver was never given what
/// its compiler cannot finish, and a compile heavier than everything past the reserve would be the process's death (issue 88:
/// a 66-151k-word program takes ~1 GB, the two of 502k and 1014k took 11 and 29 GB on the PC 3050, and RX_VK_PREDICT_MAX_KWORDS
/// already leaves those two to the draw that needs them). Only where the memory rule is on: a Windows run is unchanged
bool compile_too_heavy(const Program& p) {
    if (!compile_gate_memory_rule.load(std::memory_order_relaxed)) return false;
    return !compile_gate::fits_alone(compile_weight(p), true, compile_gate_budget_mb.load(std::memory_order_relaxed));
}
/// A turn at the gate around one driver compile. None taken when the gate is off (a Windows run, or any GPU it is not on for):
/// every call returns at once. A compile that held its turn for a long time hands the memory it freed back to the system as it
/// leaves (compile_gate.h trims: glibc's malloc_trim, nothing on other C libraries)
struct CompileTurn {
    compile_gate::Gate* gate;
    uint64_t weight_mb;
    std::chrono::steady_clock::time_point t0;
    bool stopped = false;
    explicit CompileTurn(uint64_t weight) : gate(compile_compiles().on() ? &compile_compiles() : nullptr), weight_mb(weight) {
        if (!gate) return;
        t0 = std::chrono::steady_clock::now();
        if (!gate->enter(weight_mb, memory_available_mb, [] { return prepare_stopped(); })) gate = nullptr, stopped = true;
    }
    ~CompileTurn() {
        if (!gate) return;
        const int64_t held_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        gate->leave(weight_mb);
#if defined(__linux__) && defined(__GLIBC__)
        if (compile_gate::trims(true, held_ms)) malloc_trim(0);
#else
        (void)held_ms;
#endif
    }
    CompileTurn(const CompileTurn&) = delete;
    CompileTurn& operator=(const CompileTurn&) = delete;
};
}  // namespace

/// Every shader of the game, before its first start (vk_shader.cpp, the seed): once for this translator. True: prepared now.
/// background: in play, behind the game: a few workers at idle priority, translation only (the driver's part is left to first use:
/// its compiler crashing in here would take the game with it)
bool prepare_run(void (*progress)(size_t done, size_t total), bool background) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string path = prepared_path();
    PreparedFor prepared_for;
    double hash_ms = 0;
    const PrepareWhy why = prepare_check(path, seed_path(), program_cache_stamp(), &prepared_for, &hash_ms);
    if (hash_ms > 0)
        logf("vk: the shader list was hashed (%.0f MB in %.0f ms): written since the last check, or too close to it to go by its time",
             prepared_for.seed_size / 1048576.0, hash_ms);
    if (why == PrepareWhy::None && !getenv("RX_VK_PREPARE_AGAIN")) {
        if (getenv("RX_VK_SEED_CHECK")) seed_open();
        return false;
    }
    // (the translator is the same: what the program cache keeps stays, and seed_translate and seed_compute leave it out. The
    // programs the seed has gained are what is made)
    if (why == PrepareWhy::Seed)
        logf("vk: the game's shader list (hookshot_shaders.bin) is not the one the shaders were prepared from: preparing again "
             "(only the programs not kept yet)");
    if (why == PrepareWhy::Cache)
        logf("vk: the program cache holds %zu program keys of the %llu the last preparation left: preparing again",
             program_cache_kept(), (unsigned long long)prepared_for.kept);
    if (why == PrepareWhy::First && shader_key_on()) {
        // (no record at all: a first start, or the record was wiped with whatever else was. What the ledger lists as built may be in a
        // driver cache that is gone too, and nothing can tell, so it is trusted no more: every driver build is made again, which the
        // driver's own cache answers at once where it still has them)
        pcache_init();
        ledger_drop_all();
    }
    const size_t programs = seed_open();
    if (!programs) return false;
    // (the record says no translator while the shaders are prepared and is written again when they are: a preparation closed or
    // killed part way left the old record standing, and the next start trusted it whenever the cache held more than half of what it
    // had held, or anything at all under a record from before the count: thousands of programs were then translated in play. With
    // that record the next start prepares again, and only what the cache does not hold yet)
    // (an update's preparation behind the game leaves the record as it is: closed before it ended, the next start is an update again)
    if (!background) prepared_mark_running(path, prepared_for);
    const size_t total = programs * 3;   // (each program for float, one and two integer bone inputs)
    // The driver's part: each program's stage libraries, built once and let go, for the driver's own disk cache. What stays with us is
    // nothing: a kept library costs memory, and our pipeline cache would be read in whole at every start. A driver whose compiler
    // crashes on one of them (it happens: TotK Recomp's issue 30) would crash here at every start: the file says this step began and
    // did not end, and the next start goes without it. Those libraries are then built when a draw first needs them
    const std::string warming = path + ".warming";
    // (a lazy update's preparation behind the game does it too, and only for what the ledger does not list: a few programs, whose
    // fingerprint is not one the driver has built. The others are left out as in any preparation with the key on)
    const bool lazy = background && lazy_update.load();
    const bool driver_part = !background || lazy;
    bool warm = gpl_wanted() && !getenv("RX_VK_NO_WARM") && driver_part;
    bool warm_cs = !getenv("RX_VK_NO_WARM") && driver_part;
    if (!driver_part) {
    } else if (FILE* f = fopen(warming.c_str(), "rb")) {
        fclose(f);
        warm = warm_cs = false;
        logf("vk: the last start did not finish the driver's part of the shaders: left out");
    } else if (warm || warm_cs) {
        if (FILE* w = fopen(warming.c_str(), "wb")) fclose(w);
    }
    auto blank = std::make_unique<NvnState>();
    blank->nvattr = 16;
    pcache_init();   // (once, here: the workers below build compute pipelines into it)
    std::atomic<size_t> next{0}, done{0}, made{0}, libs{0}, variants{0}, asking{0}, capped{0};
    // (hookshot) The compute programs first: each is its texture variants, up to 64 translations and as many driver compiles, seconds
    // to tens of seconds on one worker, where a graphics program is a few milliseconds. In the seed's order they were spread over its
    // second half, the last of them the seed's very last program: the other workers ran out of work and waited on the last few
    // (~20 s of a ~165 s prepare with fewer and fewer CPUs busy, 12 workers on a 24-thread PC). Taken first, the graphics programs
    // fill in behind them
    std::vector<uint32_t> order(programs);
    {
        size_t at = 0;
        for (size_t k = 0; k < programs; k++)
            if (seed_is_compute(k)) order[at++] = (uint32_t)k;
        for (size_t k = 0; k < programs; k++)
            if (!seed_is_compute(k)) order[at++] = (uint32_t)k;
    }
    // (hookshot) compute pipelines too, for the driver's own cache: built in play they stopped the game while the driver compiled
    // them, seconds each for the culling programs (53caac5f: 6.5 s, 41 s inlined), which looked like a hang
    std::atomic<int64_t> warm_us{0};   // (the driver's part, the workers' time added up)
    PrepKind k_graphics, k_mesh, k_fallback, k_compute, k_warm_compute, k_warm_variant, k_warm_graphics;   // (the profile: where the time went)
    struct WarmClock {
        std::atomic<int64_t>& sum;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~WarmClock() { sum += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(); }
    };
    // (in play the workers run at idle priority, and a thread inside the driver's compiler holds locks the game's own pipeline builds
    // wait on: starved by the game's threads it kept them waiting for seconds, up to 80 s in one run on the PC 3050. The driver's part
    // runs one step above, as a full preparation's does)
    struct DriverTurn {
        const bool on;
        explicit DriverTurn(bool lift) : on(lift) { if (on) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
        ~DriverTurn() { if (on) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE); }
    };
    const std::unordered_set<uint64_t> used_computes = computes_read();
    const bool used_only = warm_used_only(used_computes);
    std::atomic<size_t> left_to_play{0};
    const auto warm_compute = [&](Program& p) {
        if (!warm_cs || taau_key(p.key)) return;
        if (used_only && !used_computes.count(p.key)) {   // (not one play dispatched: its first dispatch builds it)
            left_to_play++;
            return;
        }
        const bool variant = warm_seen++ != 0;
        if (variant && warm_base_only()) return;   // (a variant: left to its first dispatch)
        WarmClock wc{warm_us};
        const uint64_t id = shader_key_on() ? ledger_id_compute(p) : 0;
        if (id && ledger_has(id)) {   // (RX_VK_SHADER_KEY: the driver has this very compute shader and layout already)
            ledger_reused++;
            return;
        }
        const DriverTurn turn{background};
        const auto t_warm = std::chrono::steady_clock::now();
        const CommitGate commit;
        ensure_layout(p);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = p.stages[0].module;
        ci.stage.pName = "main";
        pin_subgroup(ci.stage, p.workgroup[0]);
        ci.layout = p.layout;
        struct Timed {
            PrepKind& k;
            std::chrono::steady_clock::time_point t;
            ~Timed() { k.add(t); }
        } timed{variant ? k_warm_variant : k_warm_compute, t_warm};
        if (getenv("RX_VK_NOOPT")) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        precompile_compute_quick(ci);   // (Intel's one-at-a-time builds: without the optimizer, as a dispatch's first)
        VkPipeline pl = VK_NULL_HANDLE;
        // (into the app's pipeline cache, saved to mkw_vk_pipelines.bin: without it the next start compiled them again in play
        // where the driver's own cache missed them, AMD's 15 s culling compiles each time)
        {
            // (Longshot c392df3a: the preparation's warm compiles are as many at once as the memory holds: on an Iris Xe this is
            // the step that was killed at 24 GB, upstream's measurement)
            const CompileTurn turn(compile_weight(p));
            if (turn.stopped) return;
            if (gated_compute(pcache, &ci, &pl) == VK_SUCCESS && pl) {
                vkDestroyPipeline(dev.device, pl, nullptr);
                libs++;
                pcache_built_since_save++;
                if (id) ledger_add(id);
            }
        }
        if (p.layout) vkDestroyPipelineLayout(dev.device, p.layout, nullptr), p.layout = VK_NULL_HANDLE;
        if (p.set_layout) vkDestroyDescriptorSetLayout(dev.device, p.set_layout, nullptr), p.set_layout = VK_NULL_HANDLE;
    };
    // (the bar counts work by what it costs, not programs: 3000 programs of the seed are compute programs whose driver builds take
    // 0.3 s to tens of seconds each, 30000 are graphics programs of a few ms. By programs the bar sat at 7% for the first 8 minutes
    // of a 9 minute preparation on an RTX 3050, and the builder's time left, which goes by it, said hours; prepare_work.h)
    std::vector<uint32_t> cost(programs);
    uint64_t cost_total = 0;
    for (size_t k = 0; k < programs; k++) {
        cost[k] = prepare_work::cost(seed_is_compute(k), seed_is_compute(k) && used_only && !used_computes.count(seed_compute_key(k)));
        cost_total += cost[k];
    }
    std::atomic<uint64_t> cost_done{0};
    prepare_cost_fraction.store(0.0);
    const auto worker = [&] {
        ub_thread_off();   // (a background thread: see ub_thread_off)
        // (below normal, and half the CPUs, below: on all of them at High the driver's compiles froze the whole PC)
        SetThreadPriority(GetCurrentThread(), background ? THREAD_PRIORITY_IDLE : THREAD_PRIORITY_BELOW_NORMAL);
        for (;;) {
            if (prepare_stopped()) return;
            const size_t i = next++;
            if (i >= total) return;
            const size_t k = order[i / 3];
            if (seed_is_compute(k)) {   // (hookshot) a compute program with each of its texture variants, each warmed
                if (i % 3 == 0) {
                    const auto t_seed = std::chrono::steady_clock::now();
                    warm_seen = 0;
                    const SeedVariants v = seed_compute(k, 64, warm_compute);
                    k_compute.add(t_seed);
                    made += v.variants != 0, variants += v.variants, asking += v.variants > 1, capped += v.capped && v.variants;
                    cost_done += cost[k];
                }
                done++;
                continue;
            }
            const auto t_prog = std::chrono::steady_clock::now();
            std::unique_ptr<Program> seeded = seed_program(k, (int)(i % 3));
            if (seeded) (seeded->fallback ? k_fallback : seeded->mesh ? k_mesh : k_graphics).add(t_prog);
            {   // (hookshot: a program that took over a second to prepare is named, a few dozen times: the no-mesh fallback's first
                // start ran about 100 times slower than a mesh GPU's)
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_prog).count();
                static std::atomic<int> slow_logged{0};
                if (ms > 1000 && slow_logged.fetch_add(1, std::memory_order_relaxed) < 40)
                    logf("vk: preparing program %016llx (part %d) took %.0f ms%s%s",
                         (unsigned long long)(seeded ? seeded->key : 0), (int)(i % 3), ms,
                         seeded && seeded->fallback ? " (mesh fallback)" : "", seeded && seeded->failed ? " (failed)" : "");
            }
            if (std::unique_ptr<Program> p = std::move(seeded)) {
                if (!p->failed) made++;
                const uint64_t lib_id = shader_key_on() && !p->failed && warm && !p->compute ? ledger_id_libs(*p) : 0;
                if (lib_id && ledger_has(lib_id)) {   // (RX_VK_SHADER_KEY: the driver has these stage libraries already)
                    ledger_reused++;
                } else if (!p->failed && warm && !p->compute) {
                    WarmClock wc{warm_us};
                    const DriverTurn turn{background};
                    const auto t_libs = std::chrono::steady_clock::now();
                    const CommitGate commit;
                    ensure_layout(*p);
                    Targets at{};
                    int built = 0;
                    for (int part = 1; part <= 2; part++)
                        if (const VkPipeline lib = build_graphics(*p, *blank, at, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, part)) {
                            vkDestroyPipeline(dev.device, lib, nullptr);
                            libs++, built++;
                        }
                    if (lib_id && built == 2) ledger_add(lib_id);
                    k_warm_graphics.add(t_libs);
                }
                for (Stage& st : p->stages)
                    if (st.module) vkDestroyShaderModule(dev.device, st.module, nullptr);
                if (p->layout) vkDestroyPipelineLayout(dev.device, p->layout, nullptr);
                if (p->set_layout) vkDestroyDescriptorSetLayout(dev.device, p->set_layout, nullptr);
            }
            if (i % 3 == 0) cost_done += cost[k];
            done++;
        }
    };
    std::vector<std::thread> threads;
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    const unsigned workers = prepare_worker_count(background, cpus);
    // (a lazy update: first the programs the pipeline state lists name, the ones the game draws with, and the pipelines the notes name
    // for them, the driver's only for those whose ledger id is missing, then the rest of the seed)
    if (lazy) state_list_prepare(nullptr, true);
    if (lazy && !prepare_stopped()) predict_run(nullptr, true, true);
    for (unsigned i = 0; i < workers; i++) threads.emplace_back(worker);
    auto stat_at = std::chrono::steady_clock::now();
    while (done.load() < total && !prepare_stopped()) {
        if (progress) progress(done.load() / 3, programs);   // (in programs, as the log counts them, not programs times variants)
        if (!background && cost_total) prepare_cost_fraction.store((double)std::min<uint64_t>(cost_done.load(), cost_total) / (double)cost_total);
        Sleep(100);
        if ((!background || lazy) && std::chrono::steady_clock::now() - stat_at > std::chrono::seconds(background ? 30 : 10)) {
            // (every 10 s: what a run cut short by its time limit still says, and what the driver built so far is listed already)
            stat_at = std::chrono::steady_clock::now();
            if (shader_key_on()) ledger_commit();
            unsigned long long private_mb = 0, heaps_mb = 0;
            prepare_mem(&private_mb, &heaps_mb);
            logf("vk: prepare: %.0f s, %zu of %zu steps; translation %.0f s and the driver's part %.0f s of work so far; %lld driver builds left "
                 "out, %lld listed; private %llu MB, C heaps %llu MB", std::chrono::duration<double>(stat_at - t0).count(), done.load(), total,
                 shader_translate_ms() / 1000.0, warm_us.load() / 1e6, (long long)ledger_reused.load(), (long long)ledger_built.load(),
                 private_mb, heaps_mb);
        }
        if (next.load() >= total + threads.size()) break;
    }
    for (std::thread& t : threads) t.join();
    if (shader_key_on()) ledger_commit();
    if (!background) state_list_prepare(nullptr);   // (in play it is too late for the precompile: an update's is state_list_run, before)
    if (!getenv("RX_VK_SEED_CHECK")) seed_close();
    if (prepare_stopped()) {   // (closed: what was made is in the program cache already, the rest is made at the next start)
        logf("vk: shader preparation stopped at %zu of %zu (the window was closed): the next start goes on from there", done.load(),
             total);
        remove(warming.c_str());
        return false;
    }
    int64_t translated = 0, failed = 0;
    shader_stats(&translated, &failed);
    logf("vk: prepared %zu shader programs%s (%zu new, %lld failed, %zu stage libraries; %zu compute variants, %zu programs with "
         "texture variants, %zu at the limit) in %.1f s", programs, background ? " in play" : "", made.load(), (long long)failed, libs.load(), variants.load(),
         asking.load(), capped.load(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    logf("vk: shader key %s: %lld driver builds left out (the same SPIR-V and layout were built before), %lld built and listed; "
             "translation %.1f s and the driver's part %.1f s of work, on %u workers",
             shader_key_on() ? "on" : "off", (long long)ledger_reused.load(), (long long)ledger_built.load(), shader_translate_ms() / 1000.0, warm_us.load() / 1e6, workers);
    {   // (where the time went; one line a kind, thread time)
        double prof[6];
        shader_profile(prof);
        logf("vk: prepare profile: %u workers on %u CPUs, wall %.1f s; translation %.1f s of thread time, of which the driver's shader modules "
             "%.1f s, the cache's fingerprints %.1f s, packing %.1f s, waiting for the cache file %.1f s and writing it %.1f s; the driver's "
             "builds waited %.1f s for memory", workers, cpus, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
             prof[0], prof[1], prof[2], prof[3], prof[4], prof[5], CommitGate::waited_ms().load() / 1e3);
        k_graphics.log("graphics programs");
        k_mesh.log("native mesh programs");
        k_fallback.log("mesh fallback programs");
        k_compute.log("compute (all variants)");
        k_warm_compute.log("compute pipelines, plain (drv)");
        k_warm_variant.log("compute pipelines, variants");
        k_warm_graphics.log("stage libraries (driver)");
    }
    if (driver_part) remove(warming.c_str());
    if (const size_t kept = program_cache_kept(); kept != SIZE_MAX) prepared_for.kept = kept;
    prepared_write(path, prepared_for);
    return true;
}
bool state_list_pending() { return shipped_list_pending(); }
/// (hookshot) An update (prepare_is_update): the seed is prepared in play, behind the game, too late for the precompile; the pipelines
/// of the state list are made now, before it, so that precompile_run builds them before the game as on a first start
void state_list_run() {
    if (getenv("RX_VK_NO_STATE_LIST") || !seed_open()) return;
    state_list_prepare(nullptr);
    if (!getenv("RX_VK_SEED_CHECK")) seed_close();
}
void prepare_background() {
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
        if (!lazy_update.load()) Sleep(20000);   // (past the title screen's own loading; a lazy update has the draws' programs to make now)
        logf("vk: an update%s: the shaders not met yet are translated in play, behind the game", lazy_update.load() ? " (lazy: nothing was made before the game)" : "");
        if (prepare_run(nullptr, true) && lazy_update.load() && !prepare_stopped()) predict_run(nullptr, true);
    }).detach();
}

/// (TICK-008) The precompile waits for its pipelines (the start shows a console until they are built) only when asked: RX_VK_PRECOMPILE_WAIT=1.
/// Otherwise it queues them and the game starts; the draws that meet one still being built are left out of their frame
bool precompile_blocking() {
    static const bool wait = getenv("RX_VK_PRECOMPILE_WAIT") && atoi(getenv("RX_VK_PRECOMPILE_WAIT")) != 0;
    return wait;
}

namespace {
/// The precompile's work, on the heap for good: its threads may go on after precompile_run returns (the background, or a start that
/// gave up waiting for a pipeline that never finishes: two of 4926 on an RX 5700 XT, "stuck at 99%", Discord 11-08)
struct Pre {
    struct Item {
        std::unique_ptr<PipeNote> note;
        const Program* program;
        PipeEntry* entry;
        Targets at;
    };
    std::vector<Item> items;          // graphics pipelines, in the order the notes were written (the order the game first used them)
    std::vector<Program*> computes;   // the compute programs the cache keeps
    std::vector<Program*> fallbacks;  // the programs translated for the mesh fallback: their compute pipelines (a GPU without mesh shaders)
    std::atomic<size_t> next{0}, done{0}, made{0}, failed{0};
    std::atomic<unsigned> running{0};
    std::atomic<bool> abandoned{false};   // (Hookshot 4ae22a7a: the game went on without them: no worker takes a new pipeline)
    size_t total = 0;
    size_t carried = 0, missing = 0;
    bool background = false;
    std::chrono::steady_clock::time_point t0;
    std::string marker;
};
void precompile_finished(Pre* pre) {
    logf("vk: precompile: %zu graphics (%zu made, %zu not) and %zu compute pipelines built in %.0f ms%s", pre->items.size(),
         pre->made.load(), pre->failed.load(), pre->computes.size(),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pre->t0).count(),
         pre->background ? " (in the background, while the game ran)" : "");
    remove(pre->marker.c_str());
    // (longshot #903) pipelines that took this long were compiled, not found in the caches: the pipeline cache written now, not
    // after 64 MB of growth in play (RADV keeps a pipeline as its key and its shaders' hashes: 4.3 MB for 32,438 pipelines on a
    // Steam Deck, so nothing was written and the next start had to find them all again). precompile_save.h. RX_VK_PRECOMPILE_SAVE_S
    static const int save_s = precompile_save::after_s(getenv("RX_VK_PRECOMPILE_SAVE_S"));
    const double build_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - pre->t0).count();
    const char* prepare_only = getenv("LONGSHOT_PREPARE_ONLY");
    if (precompile_save::due(build_s, save_s, false, false, prepare_only && atoi(prepare_only) != 0)) {
        const auto s0 = std::chrono::steady_clock::now();
        pipeline_cache_save(true);
        logf("vk: precompile: its pipelines took %.0f s (compiled, not found in the caches): the pipeline cache written, in %.0f ms "
             "(RX_VK_PRECOMPILE_SAVE_S=0: not)", build_s,
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count());
    }
}
}  // namespace

void precompile_run(void (*progress)(size_t done, size_t total), bool wait) {
    const auto t0 = std::chrono::steady_clock::now();
    // (Longshot c392df3a, widened here to these graphics pipelines: how many of the driver's compiles run at once, gpu/vk/
    // compile_gate.h. Nothing when the gate is off, as on Windows)
    compile_gate_begin();
    // (a start that dies in here would die in here every time, the notes being what it builds from: a player saw the console reach
    // 542 of 543 and the game close. The file says this step began and did not end: the next start lets the notes go and plays)
    // (two starts in a row, not one: closing the window while it ran counted as a crash, and the next start then skipped the
    // precompile and compiled everything in play: "stuck at 99%, then fine on the second start", 2026-11-08)
    // (TICK-008: the notes are then moved aside to hookshot_pipelines.bin.dropped, not deleted; the marker is cleared by the last
    // worker, so a background precompile cut short by closing the game counts as not ended, as a waited one did)
    const std::string running = pipe_notes_path() + ".running";
    int ended_inside = 0;
    if (FILE* f = fopen(running.c_str(), "rb")) {
        if (fscanf(f, "%d", &ended_inside) != 1) ended_inside = 1;
        fclose(f);
        if (ended_inside >= 2) {
            remove(running.c_str());
            const std::string dropped = pipe_notes_path() + ".dropped";
            remove(dropped.c_str());
            rename(pipe_notes_path().c_str(), dropped.c_str());
            logf("vk: the last two starts did not get through the precompile: its notes are moved to hookshot_pipelines.bin.dropped");
            return;
        }
        logf("vk: the last start did not get through the precompile (closed, or it crashed there): trying it once more");
    }
    if (FILE* f = fopen(running.c_str(), "wb")) fprintf(f, "%d\n", ended_inside + 1), fclose(f);
    pcache_init();
    bool carried = false;
    std::vector<std::unique_ptr<PipeNote>> notes = pipe_notes_read(&carried);
    {   // (the file in the current format now: a carried or old one is rewritten here, at the start, not in play)
        std::lock_guard lk(note_mu);
        notes_open_locked();
    }
    if (!getenv("RX_VK_NO_STATE_LIST")) {   // (upstream: the state list opened now, before play, not at the first new pipeline in play)
        std::lock_guard lk(state_mu);
        if (!state_file) state_open_locked();
    }
    std::unordered_set<uint64_t> wanted;
    for (const auto& n : notes) wanted.insert(n->prog);
    Pre* pre = new Pre;
    pre->t0 = t0, pre->marker = running, pre->background = !wait;
    const size_t notes_n = notes.size();
    // (TICK-038) The kept compute programs stay on disk, their pipelines made at their first dispatch, when the pipeline cache began
    // with this driver's data: the precompile made all 7808 of them (every texture variant of every compute program) and kept their
    // modules and pipelines, ~2 GB of the driver's memory, of which the Market binds ~110. Read from the program cache and built from
    // the pipeline cache when first dispatched (on a worker, the dispatch waiting up to 50 ms as for any compute pipeline), a few ms
    // each, at the scene's load. A new driver's start (the cache's data refused) precompiles them all as before.
    // RX_VK_LAZY_COMPUTE=0: precompiled always, as before; 2: lazy even without the cache's data
    // (Longshot c392df3a, adapted: on Mesa's Intel driver the preparation and a first start build only the listed compute
    // programs (`RX_VK_PRECOMPILE_COMPUTES=listed`), because every program read in is a VkShaderModule the driver holds, and
    // on an Arc B580 14,676 of them were read before the start-up build's 16,358 pipelines (issue 88). Here the same saving is
    // the switch this tree already has: under the compile gate's memory rule (Linux, Mesa's Intel driver by default) the
    // compute programs stay on disk until their first dispatch, exactly as TICK-038 already does once the pipeline cache began
    // with this driver's data. Windows and every GPU without the rule keep the old choice)
    const int lazy_mode = getenv("RX_VK_LAZY_COMPUTE") ? atoi(getenv("RX_VK_LAZY_COMPUTE")) : 1;
    const bool gated_memory = compile_gate_memory_rule.load(std::memory_order_relaxed);
    const bool lazy_compute = lazy_mode == 2 || (lazy_mode == 1 && (pcache_loaded || gated_memory));
    // (upstream, Sync 11: without the lazy start, the compute programs play dispatched (hookshot_computes.bin), or every kept one
    // when there is no list yet. RX_VK_PRECOMPILE_COMPUTES=all / none)
    const char* which = getenv("RX_VK_PRECOMPILE_COMPUTES");
    // (STUT1, RX_VK_LAZY_COMPUTE_WARM=1, off by default: 4 interleaved rounds on the PC 3050 showed no gain, dispatch time in slow submits 1.4-1.9 s against 1.0-1.8 s) The lazy start also builds, on the precompile's workers behind the start, the compute
    // programs the plays before dispatched (hookshot_computes.bin: a few hundred, from the pipeline cache): an area's load
    // dispatched ~10 of them in one submit, each waiting up to 50 ms, "slow submit: 888 ms: dispatch 267" (PC 3050, 0.9-1.3 s of the
    // slow submits of a Market to Hyrule Field load)
    static const bool warm = getenv("RX_VK_LAZY_COMPUTE_WARM") && atoi(getenv("RX_VK_LAZY_COMPUTE_WARM")) != 0;
    const std::unordered_set<uint64_t> used = lazy_compute && !warm ? std::unordered_set<uint64_t>{} : computes_read();
    const bool all_computes = !lazy_compute && (which ? !strcmp(which, "all") : used.empty());
    if ((lazy_compute ? warm : !all_computes) && !(which && !strcmp(which, "none"))) wanted.insert(used.begin(), used.end());
    std::unordered_map<uint64_t, unsigned> variants_seen;   // (RX_VK_WARM_VARIANTS=base: the first variant a key has is the plain one)
    size_t variants_left = 0;
    const size_t programs_n = program_cache_load_some(wanted, [&](Program& p) {
        ensure_layout(p);
        if (p.compute && warm_base_only() && variants_seen[p.key]++ && !p.fallback) variants_left++;   // (not built before the game)
        else if (p.compute) pre->computes.push_back(&p);
        else if (p.fallback) pre->fallbacks.push_back(&p);   // (hookshot: the mesh fallback's compute pipelines too, before play)
    }, all_computes, !fallbacks_noted_only());
    if (variants_left) logf("vk: precompile: %zu texture variants of compute programs are left to their first dispatch (RX_VK_WARM_VARIANTS=all: built)", variants_left);
    if (lazy_compute)
        logf("vk: precompile: the compute programs stay on disk until dispatched (%s)", gated_memory && !pcache_loaded
                                                                                            ? "gpu/vk/compile_gate.h: the memory "
                                                                                              "rule's driver (RX_VK_LAZY_COMPUTE=0: "
                                                                                              "precompiled)"
                                                                                            : "RX_VK_LAZY_COMPUTE");
    else logf("vk: precompile: compute programs: %s (%zu keys listed as dispatched in play)",
              all_computes ? "every kept one" : (which && !strcmp(which, "none")) ? "none" : "those play dispatched", used.size());
    // The entries are made here, before the game draws: a draw that asks for one finds it (not ready) and does not make another
    // (RX_VK_PREDICT_MAX_KWORDS, as the prediction reads it: the start-up build leaves the same programs to their first draw)
    static const unsigned long long predict_max_kwords =
        getenv("RX_VK_PREDICT_MAX_KWORDS") ? strtoull(getenv("RX_VK_PREDICT_MAX_KWORDS"), nullptr, 10)
                                           : predictlimits::kDefaultMaxKWords;
    size_t too_heavy = 0, pipelines_too_heavy = 0;
    {
        ParLock lk;
        for (auto& n : notes) {
            const Program* p = program_find(n->prog, n->variant);
            if (!p) {
                pre->missing++;
                continue;
            }
            // (Longshot c392df3a's rule, for the start-up build: the driver is not given what its compiler cannot finish. A
            // program over RX_VK_PREDICT_MAX_KWORDS is left to the draw that needs it, as the prediction leaves it; one whose
            // compile does not fit the memory past the reserve at all is too. No entry is made, so the draw that first needs it
            // builds it as it would have without a note; only where the memory rule is on, so a Windows run is unchanged)
            if (compile_gate_memory_rule.load(std::memory_order_relaxed)) {
                uint64_t words = 0;
                for (const Stage& st : p->stages) words += st.spirv_words;
                const bool big = predictlimits::too_big(words, predict_max_kwords);
                if (big || compile_too_heavy(*p)) {
                    too_heavy++;
                    pipelines_too_heavy++;   // (one note, one pipeline: the notes name each pipeline they need)
                    if (too_heavy <= 20)
                        logf("vk: precompile: %016llx left out, %llu k words of SPIR-V%s: its driver compile %s; a draw that needs "
                             "it builds it", (unsigned long long)p->key, (unsigned long long)(words / 1000),
                             big ? (" (RX_VK_PREDICT_MAX_KWORDS " + std::to_string(predict_max_kwords) + ")").c_str() : "",
                             big ? "takes minutes and gigabytes" : "does not fit the memory (gpu/vk/compile_gate.h)");
                    continue;
                }
            }
            Targets at = note_targets(*n);
            const uint64_t key = pipeline_key(*p, n->s, at, (VkPrimitiveTopology)n->topo);   // (not the note's own: it is made again)
            if (!pipelines.emplace(key, std::make_unique<PipeEntry>()).second) continue;
            pipelines.find(key)->second->prog.store(p, std::memory_order_release);   // (whose pipeline it is: graphics_pipeline)
            {
                std::lock_guard nlk(note_mu);
                noted.insert(key);
            }
            pre->items.push_back({std::move(n), p, pipelines.find(key)->second.get(), at});
        }
    }
    notes.clear();
    pre->carried = carried ? pre->items.size() : 0;
    for (Program* p : pre->computes) {
        int none = 0;
        p->compute_state.compare_exchange_strong(none, 1);   // (being built: a dispatch that meets it before it is ready is left out)
    }
    pre->total = pre->computes.size() + pre->fallbacks.size() + pre->items.size();
    if (too_heavy)
        logf("vk: precompile: %zu of %zu noted pipelines left out for their size (%zu more than the notes' own programs leave for "
             "play): the driver is not given what its compiler could never finish (RX_VK_PREDICT_MAX_KWORDS, gpu/vk/compile_gate.h)",
             pipelines_too_heavy, notes_n, too_heavy);
    const size_t total = pre->total;
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    // (below normal, half the CPUs, in the background: the game's own threads come first. Waited for: all of them, as before)
    const unsigned nthreads = wait ? std::min(cpus, 64u) : std::clamp(cpus / 2, 2u, 8u);
    pre->running = total ? nthreads : 0;
    if (!total) precompile_finished(pre);
    const auto worker = [pre, total, wait] {
        if (!wait) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        ub_thread_off();   // (a background thread: see ub_thread_off)
        for (;;) {
            if (prepare_stopped() || pre->abandoned.load(std::memory_order_relaxed)) break;
            const size_t i = pre->next++;
            if (i >= total) break;
            if (i < pre->computes.size()) {
                Program& p = *pre->computes[i];
                if (compile_too_heavy(p)) {   // (the driver is not given what it could never finish: a dispatch makes its own)
                    p.compute_state.store(0, std::memory_order_release);
                    static std::atomic<int> told{0};
                    if (told++ < 20)
                        logf("vk: precompile: compute %016llx is not built: its compile does not fit the memory (gpu/vk/"
                             "compile_gate.h); a dispatch that needs it makes it", (unsigned long long)p.key);
                } else {
                    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    ci.stage.module = p.stages[0].module;
                    ci.stage.pName = "main";
                    pin_subgroup(ci.stage, p.workgroup[0]);
                    ci.layout = p.layout;
                    VkPipeline made = VK_NULL_HANDLE;
                    const bool quick = precompile_compute_quick(ci);
                    {
                        // (as many of the driver's compiles at once as the memory holds; a stopped preparation takes no turn)
                        const CompileTurn turn(compile_weight(p));
                        if (!turn.stopped && gated_compute(pcache, &ci, &made) != VK_SUCCESS) made = VK_NULL_HANDLE;
                    }
                    if (made) {
                        std::atomic_ref<VkPipeline>(p.compute_pipeline).store(made, std::memory_order_release);
                        if (quick) p.optimize_owed.store(true, std::memory_order_release);   // (its module stays for compute_optimize)
                        else cs_module_release(p);   // (TICK-038)
                        p.compute_state.store(2, std::memory_order_release);
                    } else {
                        p.compute_state.store(0, std::memory_order_release);   // (a dispatch makes its own, and says why it fails)
                    }
                }
            } else if (i < pre->computes.size() + pre->fallbacks.size()) {
                // (a program a draw is already building is left to it: compute_state, as in fallback_pipelines_ready)
                Program& p = *pre->fallbacks[i - pre->computes.size()];
                int none = 0;
                if (p.compute_state.compare_exchange_strong(none, 1)) {
                    // (a fallback stage is large: its weight says how much; a stopped preparation leaves it to play)
                    const CompileTurn turn(compile_weight(p));
                    if (turn.stopped) p.compute_state.store(0, std::memory_order_release);
                    else {
                        fallback_compute_pipelines(p);
                        p.compute_state.store(2, std::memory_order_release);
                    }
                }
            } else {
                Pre::Item& it = pre->items[i - pre->computes.size() - pre->fallbacks.size()];
                const CompileTurn turn(compile_weight(*it.program));
                if (!turn.stopped) {   // (a stopped preparation leaves it for play: the entry stays not-ready, a draw builds it)
                    VkPipeline made = build_graphics(*it.program, it.note->s, it.at, (VkPrimitiveTopology)it.note->topo);
                    if (made) pre->made++;
                    else pre->failed++;
                    if (made) it.entry->pipeline.store(made);
                    it.entry->ready.store(true, std::memory_order_release);   // (a draw leaves out a pipeline that failed, as any)
                }
                it.note.reset();
            }
            pre->done++;
        }
        if (--pre->running == 0) precompile_finished(pre);
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < (total ? nthreads : 0); i++) threads.emplace_back(worker);
    for (std::thread& t : threads) t.detach();   // (never joined: they may outlive this call)
    logf("vk: precompile: %zu programs; %zu graphics pipelines from %zu notes%s (%zu notes name no program) and %zu compute pipelines "
         "and %zu mesh fallback programs queued in %.0f ms on %u threads, %s", programs_n, pre->items.size(), notes_n,
         carried ? " carried over from another shader translation" : "", pre->missing, pre->computes.size(), pre->fallbacks.size(),
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), total ? nthreads : 0,
         wait ? "waited for" : "built in the background");
    if (!wait || !total) return;
    // (waiting: a start whose last pipelines never finish goes on to the game after PRE_STALL_S seconds with no progress, the
    // stragglers left running. RX_VK_PRECOMPILE_STALL_S=<s>, 0: wait for all)
    static const int stall_s = getenv("RX_VK_PRECOMPILE_STALL_S") ? atoi(getenv("RX_VK_PRECOMPILE_STALL_S")) : 30;
    size_t last_done = 0;
    auto last_change = std::chrono::steady_clock::now();
    while (pre->done.load() < total && !prepare_stopped()) {
        if (progress) progress(pre->done.load(), total);
        Sleep(100);
        const auto now = std::chrono::steady_clock::now();
        if (pre->done.load() != last_done) last_done = pre->done.load(), last_change = now;
        // (Hookshot 4ae22a7a, NotRexed: whether or not every pipeline was handed out. With every worker inside the driver on a pipeline
        // it never returns from, none was, and the start waited for good on AMD's RX 6600 / 6600 XT, the CPU idle. Not every one
        // handed out: the workers take no new pipeline, the rest are made in play)
        else if (stall_s > 0 && now - last_change > std::chrono::seconds(stall_s)) {
            const bool all_out = pre->next.load() >= total;
            if (!all_out) pre->abandoned = true;
            logf("vk: precompile: %zu of %zu done, nothing finished for %d s: the game starts, the rest %s", pre->done.load(), total,
                 stall_s, all_out ? "go on behind it" : "are made in play");
            pre->background = true;
            break;
        }
    }
}

// (hookshot) The pipelines the notes predict for the programs no note names (vk_predict.cpp), after a prepare that prepared: each built
// once for the driver's own cache and let go. Not into ours (mkw_vk_pipelines.bin, read whole at every start: ~40000 more pipelines made
// it 1.7 GB), nor kept: each program is borrowed from hookshot_programs.bin for its pipelines and let go
/// noted (a lazy update, in play): the pipelines the notes name themselves, not those they predict, built into our pipeline cache too:
/// what precompile_run would have built before the game, for the programs the state list has made by now, so that the draws that
/// meet a changed shader find the driver's compile done
/// (hookshot) The programs whose prediction was given up (a driver compile that outlasted the wait, see predict_run), in
/// hookshot_predict_slow.txt beside the game: a line "device <vendor>:<device>:<driver>" and a line "<key> <k words> <seconds>" each.
/// Read for the same device and driver only (a new driver may do better); the next prediction leaves them to the draw that needs them
namespace {
std::string predict_slow_path() {
    std::string p = prepared_path();
    return p.substr(0, p.size() - strlen("hookshot_prepared.bin")) + "hookshot_predict_slow.txt";
}
std::string predict_slow_device() {
    char b[96];
    snprintf(b, sizeof b, "device %x:%x:%x", (unsigned)dev.props.vendorID, (unsigned)dev.props.deviceID, (unsigned)dev.props.driverVersion);
    return b;
}
std::mutex predict_slow_mu;
std::unordered_set<uint64_t>& predict_slow_set() {   // (under predict_slow_mu)
    static std::unordered_set<uint64_t> keys;
    static bool read = false;
    if (!read) {
        read = true;
        if (FILE* f = fopen(predict_slow_path().c_str(), "rb")) {
            char line[256];
            bool same = false;
            if (fgets(line, sizeof line, f)) same = predict_slow_device() == std::string(line).substr(0, std::string(line).find_first_of("\r\n"));
            while (same && fgets(line, sizeof line, f)) {
                unsigned long long k = 0;
                if (sscanf(line, "%llx", &k) == 1 && k) keys.insert(k);
            }
            fclose(f);
        }
    }
    return keys;
}
bool predict_slow_has(uint64_t key) {
    std::lock_guard lk(predict_slow_mu);
    return predict_slow_set().count(key) != 0;
}
void predict_slow_add(uint64_t key, uint64_t kwords, long long seconds) {
    std::lock_guard lk(predict_slow_mu);
    auto& keys = predict_slow_set();
    if (!keys.insert(key).second) return;
    const std::string path = predict_slow_path();
    FILE* f = fopen(path.c_str(), "rb");
    const bool fresh = !f;
    if (f) fclose(f);
    if (fresh) {
        f = fopen(path.c_str(), "wb");
        if (f) fprintf(f, "%s\n", predict_slow_device().c_str());
    } else if (!(f = fopen(path.c_str(), "ab")))
        return;
    if (f) fprintf(f, "%016llx %llu %lld\n", (unsigned long long)key, (unsigned long long)kwords, seconds), fclose(f);
}
}  // namespace

void predict_run(void (*progress)(size_t done, size_t total), bool background, bool noted) {
    const auto t0 = std::chrono::steady_clock::now();
    // (hookshot) What the workers use lives on the heap, held by each of them: a worker whose driver compile outlasts the wait
    // (RX_VK_PREDICT_WAIT_S) is detached and may run on after this function has returned
    struct Shared {
        std::vector<std::unique_ptr<PipeNote>> notes, made_up;
        std::unordered_map<uint64_t, std::vector<const PipeNote*>> by_program;
        std::vector<uint64_t> keys;
        std::atomic<size_t> next{0}, done{0}, made{0}, read_in{0}, matched{0}, too_big{0}, pipelines_too_big{0}, slow_skipped{0};
        std::atomic<bool> out_of_time{false}, out_of_memory{false}, abandoned{false};
        std::atomic<uint64_t> in_flight[32]{};   // (each worker's program now: the progress line names them)
        std::atomic<uint32_t> in_note[32]{};     // (and the pipeline of it the worker builds: its index among that program's)
        std::atomic<int64_t> in_since[32]{};     // (and since when, ms)
        std::atomic<uint32_t> in_words[32]{};    // (and its SPIR-V's size, 1000 words)
        std::atomic<unsigned> worker_n{0};
    };
    const std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    // (Longshot c392df3a, widened here to these graphics pipelines: the prediction's builds are the same driver compiles as the
    // start-up build's, and issue 88's run spent 142 s of them before the game; gpu/vk/compile_gate.h)
    compile_gate_begin();
    const std::vector<std::unique_ptr<PipeNote>> notes = pipe_notes_read();
    if (notes.empty() || (!noted && getenv("RX_VK_NO_PREDICT"))) return;
    size_t programs = 0;
    if (!noted) shared->made_up = predict_pipelines(notes, &programs);
    const std::vector<std::unique_ptr<PipeNote>>& predicted = noted ? notes : shared->made_up;
    auto& by_program = shared->by_program;
    for (const auto& n : predicted) by_program[n->prog].push_back(n.get());
    auto& keys = shared->keys;
    for (const auto& kv : by_program) keys.push_back(kv.first);
    auto& next = shared->next;
    auto& done = shared->done;
    auto& made = shared->made;
    auto& read_in = shared->read_in;
    auto& matched = shared->matched;
    // (hookshot) A time budget, RX_VK_PREDICT_S (90 s by default; 0: none): once #1051 let the prediction find its programs again it
    // predicted ~28000 pipelines, and a first start after an update spent many minutes on them before the game's first frame (20 with
    // the CPU busy). Those not built by then are built in play when first drawn, as before the prediction worked
    static const int budget_s = getenv("RX_VK_PREDICT_S") ? atoi(getenv("RX_VK_PREDICT_S")) : 90;
    auto& out_of_time = shared->out_of_time;
    auto& out_of_memory = shared->out_of_memory;
    // (The pipelines are made for the driver's own cache and let go; a program of 502k SPIR-V words and one of 1014k (the game's
    // two largest) each took the driver's compiler over three minutes and, still growing at 11 GB of the process's commit, all the
    // PC had: C++ allocations were refused at the commit limit (0 MB of commit left), the compile failed (-13) and the memory stayed
    // taken. The others, up to 133k words, take 25 s and a few hundred MB. Over RX_VK_PREDICT_MAX_KWORDS (100: 100k words, v0.4.3; 0: no
    // limit) a program is left to the draw that needs it, as it was before the prediction. And the prediction stops, the rest left
    // to play, when the system has less than RX_VK_PREDICT_RESERVE_MB (3072) of commit left)
    static const unsigned long long max_kwords = getenv("RX_VK_PREDICT_MAX_KWORDS") ? strtoull(getenv("RX_VK_PREDICT_MAX_KWORDS"), nullptr, 10) : predictlimits::kDefaultMaxKWords;
    static const unsigned long long reserve_mb = getenv("RX_VK_PREDICT_RESERVE_MB") ? strtoull(getenv("RX_VK_PREDICT_RESERVE_MB"), nullptr, 10) : predictlimits::kDefaultReserveMB;
    auto& too_big = shared->too_big;
    auto& pipelines_too_big = shared->pipelines_too_big;
    auto& slow_skipped = shared->slow_skipped;
    auto& in_flight = shared->in_flight;
    auto& in_note = shared->in_note;
    auto& in_since = shared->in_since;
    auto& in_words = shared->in_words;
    auto& worker_n = shared->worker_n;
    // (RX_VK_PREDICT_WAIT_S, 60 by default; 0: wait for ever, as before: once the budget has run out (or the work is done) the
    // builds in hand are given this long. A driver's compile cannot be taken back, and one on a GTX 1660 SUPER (Turing) sat in
    // vkCreateGraphicsPipelines for 2 hours on a 144k-word program (the same takes ~25 s on Ampere): the builder, which sees the
    // status line, never ended the step. After the wait the program is left to play, noted in hookshot_predict_slow.txt so the
    // next prediction leaves it out, and its worker is let go on its own)
    static const int wait_s = getenv("RX_VK_PREDICT_WAIT_S") ? atoi(getenv("RX_VK_PREDICT_WAIT_S")) : 60;
    // (test only, RX_VK_TEST_SLOW_PIPELINE=<program key in hex>:<seconds>: that program's predicted builds sleep, as a hung driver would)
    static const std::pair<uint64_t, int> test_slow = [] {
        const char* e = getenv("RX_VK_TEST_SLOW_PIPELINE");
        unsigned long long k = 0;
        int s = 0;
        return e && sscanf(e, "%llx:%d", &k, &s) == 2 ? std::make_pair((uint64_t)k, s) : std::make_pair((uint64_t)0, 0);
    }();
    const auto worker = [shared, t0, noted, background] {
        Shared& S = *shared;
        auto& next = S.next;
        auto& done = S.done;
        auto& made = S.made;
        auto& read_in = S.read_in;
        auto& matched = S.matched;
        auto& out_of_time = S.out_of_time;
        auto& out_of_memory = S.out_of_memory;
        auto& too_big = S.too_big;
        auto& pipelines_too_big = S.pipelines_too_big;
        auto& slow_skipped = S.slow_skipped;
        auto& in_flight = S.in_flight;
        auto& in_note = S.in_note;
        auto& in_since = S.in_since;
        auto& in_words = S.in_words;
        auto& worker_n = S.worker_n;
        auto& by_program = S.by_program;
        auto& keys = S.keys;
        ub_thread_off();   // (a background thread: see ub_thread_off)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // (as prepare_run's: these are the driver's compiles, see DriverTurn)
        driver_cache_only = !noted;
        const unsigned me = worker_n++ % 32;
        std::atomic<uint64_t>& mine = in_flight[me];
        for (;;) {
            mine = 0;
            if (prepare_stopped() || S.abandoned.load()) return;
            if (!background && budget_s > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(budget_s)) {
                out_of_time = true;
                return;
            }
            if (predictlimits::low_commit(commit_left_mb(), reserve_mb)) {
                if (!out_of_memory.exchange(true))
                    logf("vk: predict: %llu MB of commit left (RX_VK_PREDICT_RESERVE_MB %llu): the prediction stops here, the rest are built in play",
                         commit_left_mb(), reserve_mb);
                return;
            }
            const size_t i = next++;
            if (i >= keys.size()) return;
            const auto& list = by_program[keys[i]];
            if (!noted && predict_slow_has(keys[i])) {   // (a program whose driver compile once outlasted the wait: left to the draw)
                slow_skipped++;
                done += list.size();
                continue;
            }
            mine = keys[i];
            program_cache_borrow(keys[i], [&](Program& p) {
                ensure_layout(p);
                read_in++;
                uint64_t words = 0;
                for (const Stage& st : p.stages) words += st.spirv_words;
                in_words[me] = (uint32_t)(words / 1000);
                if (predictlimits::too_big(words, max_kwords)) {
                    too_big++;
                    pipelines_too_big += list.size();
                    logf("vk: predict: program %016llx left out, %llu k words of SPIR-V (RX_VK_PREDICT_MAX_KWORDS %llu): its driver compile takes "
                         "minutes and gigabytes; a draw that needs it builds it", (unsigned long long)p.key, (unsigned long long)(words / 1000),
                         max_kwords);
                    return;
                }
                uint32_t ni = 0;
                for (const PipeNote* n : list)
                    if (n->variant == p.variant) {
                        matched++;
                        in_note[me] = ni++;
                        in_since[me] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                        const uint64_t id = shader_key_on() ? ledger_id_pipeline(p, n->s, note_targets(*n), (VkPrimitiveTopology)n->topo) : 0;
                        if (id && ledger_has(id)) {   // (RX_VK_SHADER_KEY: the driver has this SPIR-V and state already)
                            ledger_reused++;
                            continue;
                        }
                        const auto t_build = std::chrono::steady_clock::now();
                        if (test_slow.first && test_slow.first == p.key) Sleep((DWORD)test_slow.second * 1000);
                        {
                            // (Longshot c392df3a: the same driver compiles, before the game: a turn at the gate, weighed by
                            // this program's SPIR-V. The 502k- and 1014k-word programs are left out above, so what takes a
                            // turn here is what the driver builds in 25 s and a few hundred MB to a couple of gigabytes)
                            const CompileTurn turn(compile_weight(p));
                            if (!turn.stopped) {
                                const VkPipeline pl = build_graphics(p, n->s, note_targets(*n), (VkPrimitiveTopology)n->topo);
                                if (S.abandoned.load()) return;   // (let go while it built: nothing is waiting for it, and what it used may be gone: no more work)
                                if (pl) vkDestroyPipeline(dev.device, pl, nullptr), made++;
                            }
                        }
                        const double build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_build).count();
                        static std::atomic<int> slow_logged{0};
                        if (build_ms > 3000 && slow_logged.fetch_add(1, std::memory_order_relaxed) < 40) {
                            unsigned long long private_mb = 0, heaps_mb = 0;
                            prepare_mem(&private_mb, &heaps_mb);
                            logf("vk: predicted pipeline of program %016llx (%llu k words of SPIR-V) took %.0f ms (private %llu MB, C heaps %llu MB)",
                                 (unsigned long long)p.key, (unsigned long long)(words / 1000), build_ms, private_mb, heaps_mb);
                        }
                    }
            });
            done += list.size();
        }
    };
    std::vector<std::thread> threads;
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    // (in play: a quarter of the CPUs, two to four, below normal priority, and no time budget: with the ledger it builds only what changed)
    const unsigned workers = background ? std::clamp(cpus / 4, 2u, 4u) : std::clamp(cpus / 2, 1u, 32u);
    for (unsigned i = 0; i < workers; i++) threads.emplace_back(worker);
    auto stat_at = std::chrono::steady_clock::now();
    // (a line every 10 s with the programs the workers have in hand and the process's memory, and at once when its commit jumps:
    // the driver's compile of one program took gigabytes, more than the PC had, long after the time budget)
    const auto status = [&] {
        stat_at = std::chrono::steady_clock::now();
        unsigned long long private_mb = 0, heaps_mb = 0;
        prepare_mem(&private_mb, &heaps_mb);
        char flying[32 * 48 + 1] = "";
        int n = 0;
        for (unsigned k = 0; k < std::min<unsigned>(worker_n.load(), 32) && n < 32 * 47; k++)
            if (const uint64_t key = in_flight[k].load())
                n += snprintf(flying + n, sizeof flying - n, " %016llx#%u(%uk words)@%llds", (unsigned long long)key, in_note[k].load(),
                              in_words[k].load(),
                              (long long)(std::chrono::duration_cast<std::chrono::milliseconds>(stat_at - t0).count() - in_since[k].load()) / 1000);
        logf("vk: predict: %.0f s, %zu of %zu pipelines, %zu made; programs in hand:%s; private %llu MB, C heaps %llu MB",
             std::chrono::duration<double>(stat_at - t0).count(), done.load(), predicted.size(), made.load(), flying, private_mb, heaps_mb);
    };
    uint64_t last_mb = 0;
    auto last_look = std::chrono::steady_clock::now();
    const auto watch = [&] {
        bool jump = false;
        if (std::chrono::steady_clock::now() - last_look > std::chrono::seconds(1)) {
            last_look = std::chrono::steady_clock::now();
            unsigned long long pm = 0, hm = 0;
            prepare_mem(&pm, &hm);
            jump = pm > last_mb + 700;
            last_mb = pm;
        }
        if (jump || std::chrono::steady_clock::now() - stat_at > std::chrono::seconds(10)) status();
    };
    while (next.load() < keys.size() + threads.size() && !prepare_stopped() && !out_of_time.load() && !out_of_memory.load()) {
        if (progress) progress(done.load(), predicted.size());
        Sleep(100);
        watch();
    }
    const auto building = [&] { return std::count_if(std::begin(in_flight), std::end(in_flight), [](const auto& k) { return k.load() != 0; }); };
    const auto grace_from = std::chrono::steady_clock::now();
    while (!prepare_stopped() && building()) {
        // (the builds in hand are let finish: a driver's compile cannot be taken back. But not for ever, RX_VK_PREDICT_WAIT_S)
        if (!background && wait_s > 0 && std::chrono::steady_clock::now() - grace_from > std::chrono::seconds(wait_s)) break;
        Sleep(100);
        watch();
    }
    if (building()) {
        // (still in the driver: told, written down so that the next prediction leaves each out, and the threads let go. Their
        // state is on the heap (shared), and once `abandoned` they do nothing more when the driver returns. In the prepare run
        // the process's exit ends the compile; the pipeline cache's save does not wait for it (the builds here use no cache)
        shared->abandoned = true;
        const auto now = std::chrono::steady_clock::now();
        for (unsigned k = 0; k < std::min<unsigned>(worker_n.load(), 32); k++)
            if (const uint64_t key = in_flight[k].load()) {
                const long long secs = (std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count() - in_since[k].load()) / 1000;
                logf("vk: predict: program %016llx (%uk words of SPIR-V) is still in the driver after %lld s (RX_VK_PREDICT_WAIT_S %d): "
                     "given up, left out of the next predictions; a draw that needs it builds it in play",
                     (unsigned long long)key, in_words[k].load(), secs, wait_s);
                predict_slow_add(key, in_words[k].load(), secs);
            }
        for (std::thread& t : threads) t.detach();
    } else
        for (std::thread& t : threads) t.join();
    if (shader_key_on()) ledger_commit();
    logf("vk: %s%s %zu pipelines from %zu notes for %zu program variants no note names (%zu of %zu programs read, %zu pipelines had their variant): "
         "%zu made, %lld left out (listed as built, all steps so far), in %.1f s on %u workers%s",
         noted ? "noted pipelines warmed" : "predicted", background ? " (in play)" : "", predicted.size(), notes.size(), programs, read_in.load(),
         keys.size(), matched.load(), made.load(), (long long)ledger_reused.load(),
         std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), workers,
         out_of_time.load() ? " (the time budget, RX_VK_PREDICT_S, ran out: the rest are built in play)" : "");
    if (slow_skipped.load()) logf("vk: predict: %zu programs left out: their driver compile once outlasted the wait (hookshot_predict_slow.txt)", slow_skipped.load());
    if (too_big.load() || out_of_memory.load())
        logf("vk: predict: %zu programs (%zu pipelines) left out for their size, %s", too_big.load(), pipelines_too_big.load(),
             out_of_memory.load() ? "and the prediction stopped for want of commit" : "the others as they were asked for");
}

/// (longshot #404) Intel's one-at-a-time compute builds (serial_builds_on): a dispatch's pipeline is made first without the
/// optimizer (intel_quick_compute, RX_VK_COMPUTE_QUICK_FIRST), then with it here, behind the builds queued first. With the
/// optimizer Intel's compiler took 0.3 s to 6.5 min for each of 37 of the game's dispatches on a UHD Graphics (i3-1115G4), 22
/// minutes on its one thread, each pass over the scene (its light, its exposure) left out until its turn. The dispatch binds this
/// one once made; the first stays for the session (a recording may hold it). The program's module, kept for this, goes after it
void compute_optimize(Program& q) {
    struct Release { Program& q; ~Release() { cs_module_release(q); } } release{q};
    if (q.failed || q.stages.empty() || !q.stages[0].module || !q.layout) return;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = q.stages[0].module;
    ci.stage.pName = "main";
    pin_subgroup(ci.stage, q.workgroup[0]);
    ci.layout = q.layout;
    const auto t0 = std::chrono::steady_clock::now();
    VkPipeline made = VK_NULL_HANDLE;
    if (const VkResult r = gated_compute(pcache, &ci, &made); r != VK_SUCCESS || !made) {
        static std::atomic<int> told{0};
        if (told++ < 20)
            logf("vk: compute pipeline of %016llx: the optimized build was not made (%d); the one without the optimizer stays",
                 (unsigned long long)q.key, (int)r);
        return;
    }
    q.compute_optimized.store(made, std::memory_order_release);
    pcache_built_since_save++;
    st_pipelines++;
    const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    static std::atomic<int> told{0};
    if (ms >= 200 || told++ < 20)
        logf("vk: compute pipeline of %016llx: optimized in %lld ms, bound from now on", (unsigned long long)q.key, (long long)ms);
}

/// (RX_VK_HDR_FIND=1: hdr_find's twin for dispatches: a compute program that samples a floating-point scene image and writes an
/// 8-bit or 10-bit one, said once with the images' sizes and NVN formats and its uniform buffers' sizes)
static void hdr_find_dispatch(const NvnState& s, uint64_t key) {
    static const bool on = getenv("RX_VK_HDR_FIND") != nullptr;
    if (!on) return;
    uint32_t ntex = 0, nimg = 0;
    cs_slot_counts(s, &ntex, &nimg);
    ntex = std::min(ntex, 128u), nimg = std::min(nimg, 32u);
    char line[900];
    int n = snprintf(line, sizeof line, "vk: HDR find: compute %016llx (frame %llu) grid %ux%ux%u samples", (unsigned long long)key,
                     (unsigned long long)texture_frame_no(), s.grid[0], s.grid[1], s.grid[2]);
    bool scene = false, out = false;
    auto describe = [&](const char* what, uint64_t h, bool sampled) {
        nvn_tex t;
        nvn_view v;
        bool hv = false;
        if (!h || !texture_of((int)(h & 0xFFFFF), &t, &v, &hv)) return;
        const unsigned f = (unsigned)t.d.format;
        if (sampled) scene |= f == NVN_FORMAT_R11G11B10F || f == NVN_FORMAT_RGB9E5F || f == NVN_FORMAT_RGBA16F;
        else out |= f == NVN_FORMAT_RGBA8 || f == NVN_FORMAT_RGBA8_SRGB || f == NVN_FORMAT_RGB10A2 || f == NVN_FORMAT_BGRA8 ||
                    f == NVN_FORMAT_BGRA8_SRGB;
        if (n < 700)
            n += snprintf(line + n, sizeof line - n, "%s %#llx %ux%u nvn %#x;", what, (unsigned long long)t.gpu, t.d.width, t.d.height, f);
    };
    for (uint32_t i = 0; i < ntex; i++) describe(" tex", s.tex[5][i], true);
    n += snprintf(line + n, sizeof line - n, " writes");
    for (uint32_t i = 0; i < nimg; i++) describe(" img", s.img[5][i], false);
    if (!scene || !out) return;
    n += snprintf(line + n, sizeof line - n, " uniform buffers:");
    for (int i = 0; i < 8; i++)
        if (s.ubo[5][i].addr && n < 840)
            n += snprintf(line + n, sizeof line - n, " c%d %#llx+%#llx;", i + 3, (unsigned long long)s.ubo[5][i].addr,
                          (unsigned long long)s.ubo[5][i].size);
    static std::mutex mu;
    static std::unordered_set<uint64_t> seen;
    std::lock_guard lk(mu);
    if (seen.size() < 64 && seen.insert(key).second) logf("%s", line);
}

void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    if (par_stats_on) par_stats_draw();
    PROF(PROF_DISPATCH);
    DispatchClock dclock;
    s.grid[0] = indirect ? 1 : x, s.grid[1] = indirect ? 1 : y, s.grid[2] = indirect ? 1 : z;
    if (indirect) {   // (the program may read its grid from c0: the counts as the GPU will see them, best effort)
        uint32_t g[3] = {1, 1, 1};
        if (memory_read_as_submitted(indirect, g, 12)) s.grid[0] = g[0], s.grid[1] = g[1], s.grid[2] = g[2];
    }
    const uint8_t types[8]{};
    const Program* cp = program_for(s, ProgramKind::Compute, types);
    dclock.t_prog = slow_dispatch_timing() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // (a program still being translated is waited for, as a draw's is: a dispatch left out is work never done again.
    // MKW updates its terrain height clipmaps a region at a time as the camera moves, and makes its sky's tables
    // once; a skipped update left holes and flat patches in the terrain, garbage in the sky)
    if (cp && !cp->ready.load(std::memory_order_acquire)) {
        const auto t0 = std::chrono::steady_clock::now();
        jobs_wait(cp->ready);
        st_waits++;
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        st_wait_ms += ms, st_shader_wait_ms += ms;
    }
    if (!cp || cp->failed) {   // (hookshot) the program's resources, once a control section: what a rejected dispatch reads/writes
        static std::mutex mu2;
        static std::unordered_set<uint64_t> seen2;
        const uint64_t id = s.prog.sd[0].control;
        std::lock_guard lk2(mu2);
        if (seen2.size() < 64 && seen2.insert(id).second) {
            char line[1400];
            int n = snprintf(line, sizeof line, "vk: rejected compute %016llx (control %#llx) grid %ux%ux%u:",
                             (unsigned long long)compute_code_key(s), (unsigned long long)id, s.grid[0], s.grid[1], s.grid[2]);
            auto desc = [&](const char* what, uint64_t h) {
                nvn_tex t;
                nvn_view v;
                bool hv = false;
                if (!h || !texture_of((int)(h & 0xFFFFF), &t, &v, &hv)) return;
                if (n < (int)sizeof line - 96)
                    n += snprintf(line + n, sizeof line - n, " %s %#llx %ux%ux%u fmt %#x;", what,
                                  (unsigned long long)t.gpu, t.d.width, t.d.height, t.d.depth, (unsigned)t.d.format);
            };
            for (int i = 0; i < 8; i++) desc("img", s.img[5][i]);
            for (int i = 0; i < 16; i++) desc("tex", s.tex[5][i]);
            for (int i = 0; i < 16; i++)
                if (s.ssbo[5][i].addr && n < (int)sizeof line - 64)
                    n += snprintf(line + n, sizeof line - n, " ssbo%d %#llx+%#llx;", i, (unsigned long long)s.ssbo[5][i].addr,
                                  (unsigned long long)s.ssbo[5][i].size);
            for (int i = 0; i < 8; i++)
                if (s.ubo[5][i].addr && n < (int)sizeof line - 64)
                    n += snprintf(line + n, sizeof line - n, " ubo%d %#llx+%#llx;", i, (unsigned long long)s.ubo[5][i].addr,
                                  (unsigned long long)s.ubo[5][i].size);
            logf("%s", line);
        }
    }
    if (cp && cp->failed) {   // (hookshot) which programs' dispatches are left out, once each, with their grids
        static std::mutex mu;
        static std::unordered_set<uint64_t> seen;
        std::lock_guard lk(mu);
        if (seen.size() < 64 && seen.insert(cp->key).second)
            logf("vk: dispatches of program %016llx are left out (it did not translate): grid %ux%ux%u%s",
                 (unsigned long long)cp->key, s.grid[0], s.grid[1], s.grid[2], indirect ? " (indirect)" : "");
    }
    // (RX_VK_FORCE_UPSCALE_STUB=1, a test: every upscale network takes the fallback, as on a GPU where they do not translate. The
    // neon colours of issue 425/508 come from that path, and an NVIDIA PC never took it)
    // (=1: the networks other than gameplay's, which on AMD take this path; gameplay's has its stand-in there. =2: all)
    static const int force_stub = getenv("RX_VK_FORCE_UPSCALE_STUB") ? atoi(getenv("RX_VK_FORCE_UPSCALE_STUB")) : 0;
    const bool slow_network = cp && !cp->failed && !cp->stages.empty() &&
                              (movie_upscale_policy::use_linear(movie_upscale_mode(),
                                  movie_networks_slow.load(std::memory_order_relaxed), movie_keep_all_networks(),
                                  cp->stages[0].info.uses_cooperative_matrix, gameplay_network(cp->key)) ||
                               (force_stub && cp->stages[0].info.uses_cooperative_matrix &&
                                (force_stub >= 2 || !gameplay_network(cp->key))));
    if ((!cp || cp->failed || slow_network) && upscale_stub(cmd, s, compute_code_key(s))) return void(st_dispatches++);
    if (slow_network) SKIP_HERE();   // (an earlier pass of the network: the stub is its last pass)
    if (!cp || !cp->ready.load(std::memory_order_acquire) || cp->failed) SKIP_HERE();
    hdr_find_dispatch(s, cp->key);   // (RX_VK_HDR_FIND)
    {   // (hookshot, RX_VK_SKIP_PROG=<key prefix,...>: those programs' dispatches left out, for diagnosis)
        static const char* skip = getenv("RX_VK_SKIP_PROG");
        if (skip) {
            char hex[24];
            snprintf(hex, sizeof hex, "%016llx", (unsigned long long)cp->key);
            for (const char* q = skip; *q;) {
                const char* e = q;
                while (*e && *e != ',') e++;
                if (e > q && !strncmp(hex, q, (size_t)(e - q))) return;
                q = *e ? e + 1 : e;
            }
        }
    }
    dclock.key = cp->key;
    texture_watch_prog(cp->key);
    {   // (hookshot, RX_VK_LOG_PUSH=<key prefix>: once, that program's push constants and the c0 words it reads)
        static const char* want = getenv("RX_VK_LOG_PUSH");
        static bool done;
        char hex[24];
        snprintf(hex, sizeof hex, "%016llx", (unsigned long long)cp->key);
        if (want && !done && !strncmp(hex, want, strlen(want))) {
            done = true;
            char line[1600];
            int n = snprintf(line, sizeof line, "vk: program %s: %u bytes pushed; c0:", hex, s.cs_push_n);
            for (uint32_t off = 0; off < 0x100 && n < (int)sizeof line - 16; off += 4)
                n += snprintf(line + n, sizeof line - n, " %08x", driver_bank_word(s, 0, 5, off));
            logf("%s", line);
        }
    }
    Program& p = const_cast<Program&>(*cp);
    if (!p.layout) {
        ParLock lk;
        ensure_layout(p);
    }
    // (the compute pipeline is compiled on a worker, not here with the recording lock held: AMD's compiler took 15-23 s on the
    // culling programs, every recording thread stopped behind it and the screen stayed black after the opening (6950 XT, RX 6600,
    // 2026-11-08). The dispatch waits up to 50 ms for it, which covers NVIDIA's compiles; one still compiling after that is left
    // out until it is ready, as graphics draws are. RX_VK_COMPUTE_WAIT=1: waited for, as before)
    static const bool compute_wait = getenv("RX_VK_COMPUTE_WAIT") != nullptr;
    // (longshot #621: compute_pipeline is written by the build worker and the precompile's threads while dispatches read it: always atomically)
    const auto cpipe = [&p] { return std::atomic_ref<VkPipeline>(p.compute_pipeline).load(std::memory_order_acquire); };
    if (!compute_wait && !cpipe() && !taau_key(p.key) && p.compute_state.load(std::memory_order_acquire) != 2) {
        int none = 0;
        const bool fresh = p.compute_state.compare_exchange_strong(none, 1);
        if (fresh) {
            { ParLock lk; pcache_init(); }
            const Program* job = &p;
            // (issue 496: one at a time on Intel: serial_builds_on)
            static const bool serial = serial_builds_on();
            auto build = [job] {
                Program& q = const_cast<Program&>(*job);
                VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                ci.stage.module = q.stages[0].module;
                ci.stage.pName = "main";
                pin_subgroup(ci.stage, q.workgroup[0]);
                ci.layout = q.layout;
                static const bool noopt = getenv("RX_VK_NOOPT") != nullptr;
                if (noopt) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
                // (longshot #404) on Intel's one-at-a-time builds, without the optimizer first: compute_optimize
                const bool quick = serial && intel_quick_compute() && !(ci.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT);
                if (quick) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
                const auto t0 = std::chrono::steady_clock::now();
                VkPipeline made = VK_NULL_HANDLE;
                // (TICK-038) whether the pipeline cache had it: a hit is not a reason to write the 1 GB cache file again
                VkPipelineCreationFeedback fb{};
                VkPipelineCreationFeedbackCreateInfo fci{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
                fci.pipelineStageCreationFeedbackCount = 0;
                fci.pPipelineCreationFeedback = &fb;
                ci.pNext = &fci;
                if (const VkResult r = gated_compute(pcache, &ci, &made); r != VK_SUCCESS || !made) {
                    static std::atomic<int> told{0};
                    if (told++ < 20) logf("vk: compute pipeline of %016llx not made (%d)", (unsigned long long)q.key, (int)r);
                    q.failed = true;
                } else {
                    std::atomic_ref<VkPipeline>(q.compute_pipeline).store(made, std::memory_order_release);
                    if (shader_key_on()) ledger_add(ledger_id_compute(q));
                    if (quick) jobs_push_serial([job] { compute_optimize(const_cast<Program&>(*job)); });   // (keeps its module)
                    else cs_module_release(q);   // (TICK-038)
                }
                const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
                if (ms >= 200)
                    logf("vk: compute pipeline of %016llx: SLOW compile, %lld ms (on a worker%s)", (unsigned long long)q.key, (long long)ms,
                         quick ? ", without the optimizer" : "");
                st_pipeline_us += ms * 1000;
                st_pipelines++;
                // (NVIDIA leaves the hit bit clear even for what it had: a build under 30 ms is taken as one, the lazy compute
                // programs' ~2.5 ms each at the Market. One not counted is still saved at exit, which saves whatever the count)
                const bool hit = ((fb.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&
                                  (fb.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT)) ||
                                 (pcache_loaded && ms < 30);
                (hit ? cs_cache_hits : cs_cache_misses)++;
                if (!hit) pcache_built_since_save++;
                q.compute_state.store(2, std::memory_order_release);
            };
            // (first: a dispatch is a pass over the scene (its light, its exposure), left out while it builds; the mesh fallback's
            // are a model each, and the optimized builds of earlier dispatches go behind both)
            if (serial) jobs_push_serial(std::move(build), true);
            else jobs_push(std::move(build), true);
        }
        // (only the dispatch that started the build waits: later ones while it still compiles are left out at once, not 50 ms
        // each, which stalled every frame by 50 ms per compute program still building)
        const auto t0 = std::chrono::steady_clock::now();
        while (fresh && p.compute_state.load(std::memory_order_acquire) != 2 &&
               std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50))
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (p.compute_state.load(std::memory_order_acquire) != 2) {
            static std::atomic<int> told{0};
            if (told++ < 40) logf("vk: dispatch of %016llx left out: its compute pipeline is still compiling", (unsigned long long)p.key);
            st_pending_skips++;
            return;
        }
        if (p.failed || !cpipe()) SKIP_HERE();
    }
    ParLock plk(!cpipe());
    if (!cpipe() && !taau_key(p.key)) {   // (a stood-in network is never compiled)
        pcache_init();
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = p.stages[0].module;
        ci.stage.pName = "main";
        pin_subgroup(ci.stage, p.workgroup[0]);
        ci.layout = p.layout;
        {   // (local) RX_VK_NOOPT=1, as for graphics pipelines above
            static const bool noopt = getenv("RX_VK_NOOPT") != nullptr;
            if (noopt) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
        }
        const auto t0 = std::chrono::steady_clock::now();
        {   // (EvilOOT, 11-07: a freeze after Load was the driver's compiler on one compute pipeline, here, on the submit's thread)
            static std::atomic<int> told{0};
            if (told++ < 400) logf("vk: compute pipeline of %016llx: compiling", (unsigned long long)p.key);
        }
        VkPipeline built = VK_NULL_HANDLE;   // (published only once made: other threads read it meanwhile)
        if (const VkResult made = gated_compute(pcache, &ci, &built); made != VK_SUCCESS) {
            static std::atomic<int> told{0};
            if (told++ < 20) logf("vk: compute pipeline of %016llx not made (%d)", (unsigned long long)p.key, (int)made);
            p.failed = true;
            SKIP_HERE();
        }
        std::atomic_ref<VkPipeline>(p.compute_pipeline).store(built, std::memory_order_release);
        {
            const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            if (ms >= 200) logf("vk: compute pipeline of %016llx: SLOW compile, %lld ms", (unsigned long long)p.key, (long long)ms);
        }
        st_pipeline_us += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        st_pipelines++;
        pcache_built_since_save++;
        if (cpipe()) cs_module_release(p);   // (TICK-038)
    }
    if (plk.on) par_mutex().unlock(), plk.on = false;
    dclock.t_pipe = slow_dispatch_timing() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    end_pass(cmd);
    static const bool no_grid_patch = getenv("RX_VK_NO_GRID_PATCH") != nullptr;
    grid_patch_want = indirect && !no_grid_patch, grid_patch_at = UINT64_MAX;
    build_descriptors(s, p);
    grid_patch_want = false;
    dclock.t_desc = slow_dispatch_timing() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (dump_on())
        logf("vk: dump-in %03d dispatch %ux%ux%u prog %016llx:%s", dump_seq(), x, y, z, (unsigned long long)p.key,
             dump_inputs().c_str());
    if (dump_on()) {   // (amd-fix, diagnosis) what a dispatch can reach: its storage and uniform buffers, its pointer table
        char line[1800];
        int n = snprintf(line, sizeof line, "vk: dump-reach %03d prog %016llx%s:", dump_seq(), (unsigned long long)p.key,
                         !p.stages.empty() && p.stages[0].info.stores_global_memory ? " (stores global)" : "");
        for (int i = 0; i < 16 && n < 1700; i++)
            if (s.ssbo[5][i].addr)
                n += snprintf(line + n, sizeof line - n, " sb%d %#llx+%#llx", i, (unsigned long long)s.ssbo[5][i].addr,
                              (unsigned long long)s.ssbo[5][i].size);
        for (int i = 0; i < 16 && n < 1700; i++)
            if (s.ubo[5][i].addr)
                n += snprintf(line + n, sizeof line - n, " ub%d %#llx+%#llx", i, (unsigned long long)s.ubo[5][i].addr,
                              (unsigned long long)s.ubo[5][i].size);
        if (!p.stages.empty()) {   // the translated program's own storage bindings: which constant-buffer word names each
            const auto& sd = p.stages[0].info.storage_buffers_descriptors;
            for (size_t i = 0; i < sd.size() && n < 1700; i++) {
                const uint32_t off = sd[i].cbuf_offset;
                const uint64_t a = stage_cbuf_word(s, p.stages[0], sd[i].cbuf_index, off) |
                                   (uint64_t)stage_cbuf_word(s, p.stages[0], sd[i].cbuf_index, off + 4) << 32;
                n += snprintf(line + n, sizeof line - n, " ssbo%zu=c%u+%#x%s%s->%#llx", i, sd[i].cbuf_index, off,
                              sd[i].is_written ? "w" : "", sd[i].count > 1 ? "[t]" : "", (unsigned long long)a);
            }
        }
        logf("%s", line);
    }
    // (dispatches after other work wait for it: the WebGPU renderer's dispatches were ordered the same way by
    // wgpu's own tracking, and MKW's compute chains read what the one before wrote)
    static const bool no_order = getenv("RX_VK_NO_DISPATCH_ORDER") != nullptr;
    // (hookshot, RX_VK_GAME_ORDER: a direct dispatch right after another, nothing recorded between, gets none: game_order)
    const bool after_dispatch = !indirect && cmd == last_dispatch_cmd &&
                                work_epoch.load(std::memory_order_relaxed) == last_dispatch_epoch && game_order() &&
                                !par_active.load(std::memory_order_relaxed);
    if (work_since_barrier && !no_order && !after_dispatch) dispatch_barrier(cmd, "barrier (dispatch)");
    else if (after_dispatch && ++st_dispatch_unordered % 50000 == 1)
        logf("vk: %lld dispatches right after another ordered by the game's barriers so far, with none of their own (RX_VK_GAME_ORDER)",
             (long long)st_dispatch_unordered);
    work_since_barrier = true;
    {   // (ported from hookshot 53eb0441, pixl27, RX_VK_METER_ZERO=0 turns it off) OoT's exposure meter: 435a0138b9013040 and
        // 592079442d7129da add a frame's sums into the first 16 bytes of their block, which the game reads and sets back to zero.
        // With the game ahead of the GPU its zeros reach the mirror late, or a write-back lands over them first and the old sums
        // come back: the next frame's are added on top (twice the light). Cleared here right before each program
        if (meter_zero_on() && (p.key == 0x435a0138b9013040ull || p.key == 0x592079442d7129daull) && meter_out.buffer &&
            meter_out.size >= 16 && !(meter_out.offset & 3)) {
            static std::atomic<int64_t> st_zeroed{0};
            full_barrier(cmd);
            vkCmdFillBuffer(cmd, meter_out.buffer, meter_out.offset, 16, 0);
            full_barrier(cmd);
            if (st_zeroed.fetch_add(1) % 4000 == 0) logf("vk: exposure sums cleared on the GPU before their programs: %lld so far (RX_VK_METER_ZERO)", (long long)st_zeroed.load());
        }
    }
    if (!p.stages.empty() && p.stages[0].info.uses_cooperative_matrix) {   // (the game's networks, once each: what they read and write)
        static std::mutex mu;
        static std::unordered_map<uint64_t, int> seen;
        std::lock_guard lk(mu);
        if (seen[p.key]++ == 0) {
            char line[900];
            int n = snprintf(line, sizeof line, "vk: network %016llx: dispatch %ux%ux%u, %zu textures %zu images:", (unsigned long long)p.key, x, y, z,
                             p.stages[0].info.texture_descriptors.size(), p.stages[0].info.image_descriptors.size());
            for (const Texture* t : draw_sampled())
                if (t && n < 860) n += snprintf(line + n, sizeof line - n, " %ux%ux%u/%u", t->width, t->height, t->depth, (unsigned)t->format);
            logf("%s", line);
        }
    }
    if (fsr3_stand_in(cmd, s, p)) return void(st_dispatches++);
    if (temporal_stand_in(cmd, s, p)) return void(st_dispatches++);
    if (off_stand_in(cmd, s, p)) return void(st_dispatches++);
    if (taau_stand_in(cmd, s, p)) return void(st_dispatches++);
    const VkPipeline pipe = cpipe();
    if (!pipe) SKIP_HERE();
    dump_ssbos(cmd, p, "before");
    if (p.optimize_owed.load(std::memory_order_relaxed) && p.optimize_owed.exchange(false, std::memory_order_acq_rel)) {
        Program* q = &p;   // (made without the optimizer before play: precompile_run)
        jobs_push_serial([q] { compute_optimize(*q); });
    }
    const VkPipeline optimized = p.compute_optimized.load(std::memory_order_acquire);   // (compute_optimize)
    const VkPipeline bound = optimized ? optimized : pipe;
    if (res_log_on()) res_note_pipeline_use(bound);   // (TICK-038)
    if (!p.dispatched.exchange(true, std::memory_order_relaxed)) compute_note(p.key);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bound);
    if (!bind_descriptors(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p)) SKIP_HERE();
    push_constants(cmd, p, nullptr);
    gprof_begin(cmd);
    if (stats_frame_now()) {   // (EvilOOT, RX_VK_DRAW_STATS) the pointers this dispatch's small buffers hold
        const auto scan = [&](const nvn_p_bind_buf& b, uint64_t most) {
            if (!b.addr || !b.size) return;
            const uint64_t n = std::min<uint64_t>(b.size, most) & ~7ull;
            std::vector<uint64_t> w(n / 8);
            if (w.empty() || !memory_read_as_submitted(b.addr, w.data(), n)) return;
            std::lock_guard bl(stats_bind_mu);
            for (const uint64_t v : w)
                if (!(v & 3) && v >= (1ull << 32) && v < (1ull << 40) && host->gpu_guest(v) && stats_ptrs.size() < 400000)
                    stats_ptrs.push_back({p.key, b.addr, v});
        };
        for (const nvn_p_bind_buf& b : s.ssbo[5]) scan(b, 0x400);
        for (const nvn_p_bind_buf& b : s.ubo[5]) scan(b, 0x1000);
        {   // (second level) pointers inside the pointer table's regions that nothing this dispatch has covers
            const PointerEntry* e = pointer_table(s, 5);
            const auto covered = [&](uint64_t v) {
                for (const nvn_p_bind_buf& b : s.ssbo[5])
                    if (b.addr && v >= b.addr && v < b.addr + b.size) return true;
                for (int i = 0; i < 3; i++)
                    if (e[i].addr && v >= e[i].addr && v < e[i].addr + e[i].size) return true;
                return false;
            };
            for (int i = 0; i < 3; i++) {
                if (!e[i].addr || !e[i].size) continue;
                const uint64_t n = std::min<uint64_t>(e[i].size, 256u << 10) & ~7ull;
                std::vector<uint64_t> w(n / 8);
                if (w.empty() || !memory_read_as_submitted(e[i].addr, w.data(), n)) continue;
                uint32_t found = 0;
                std::lock_guard bl(stats_bind_mu);
                for (const uint64_t v : w)
                    if (!(v & 3) && v >= (1ull << 32) && v < (1ull << 40) && host->gpu_guest(v) && gpu_pool_end(v) && !covered(v) &&
                        found++ < 64 && stats_ptrs2.size() < 400000)
                        stats_ptrs2.push_back({p.key, e[i].addr, v});
            }
        }
        if (stats_prog_wanted(p.key)) stats_dump_stage(p.key, s, 5);   // (with each buffer's non-zero words)
    }
    if (stats_dir()) watch_note_sb(p.key, s);   // (RX_VK_WATCH_SB)
    {
        StatsQuery stats_query(cmd, p.key, indirect, x, y, z);   // (EvilOOT, RX_VK_DRAW_STATS)
        {   // (RX_VK_GPU_DUMP=1 with RX_VK_DRAW_STATS_PROGS: the wanted dispatches' storage buffers as the GPU holds them after)
            static const bool gpu_dump_on = getenv("RX_VK_GPU_DUMP") != nullptr;
            if (gpu_dump_on && stats_query.cmd && stats_prog_wanted(p.key)) stats_query.state = &s, stats_query.prog = p.key;
        }
        if (indirect) {
            ledger_mark(indirect, 12, kUseIndirect);   // (RX_VK_LEDGER)
            BufferRef args = gpu_buffer_bind(indirect, 12, false);
            if (!args) SKIP_HERE();
            if (grid_patch_at != UINT64_MAX) {   // (PR 252) the grid as the GPU has it, into the program's c0 (the full barrier
                // above: the pass that wrote the arguments is done)
                const VkBufferCopy gc{args.offset, grid_patch_at, 12};
                checkpoint_mark(cmd, p.key, 'p', 'C', 1, 12);
                vkCmdCopyBuffer(cmd, args.buffer, grid_patch_buf, 1, &gc);
                VkMemoryBarrier2 gb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                gb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, gb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                gb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, gb.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
                VkDependencyInfo gd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                gd.memoryBarrierCount = 1, gd.pMemoryBarriers = &gb;
                vkCmdPipelineBarrier2(cmd, &gd);
                grid_patch_at = UINT64_MAX;
            }
            checkpoint(cmd, p, 'i');   // (a device lost's report names it: a grid read from GPU memory)
            if (dump_on() && dump_ssbo_wanted(p.key)) {   // (RX_VK_DUMP_SSBO: the grid as the GPU holds it, and where)
                char what[128];
                snprintf(what, sizeof what, "%03d prog %016llx indirect grid at %#llx", dump_seq(), (unsigned long long)p.key,
                         (unsigned long long)indirect);
                dump_buffer(cmd, args.buffer, args.offset, 12, what);
            }
            checkpoint_work('n', 0);
            vkCmdDispatchIndirect(cmd, args.buffer, args.offset);
        } else {
            checkpoint_work('x', x, y, z);
            vkCmdDispatch(cmd, x, y, z);
        }
        last_dispatch_cmd = cmd, last_dispatch_epoch = work_epoch.load(std::memory_order_relaxed);
    }
    if (dump_on() && dump_ssbo_wanted(p.key)) dump_ssbos(cmd, p, "after");
    if (gprof_on()) {
        char label[128];   // (G2: the grid, and "mma" for the game's matrix networks, native or software multiplies)
        snprintf(label, sizeof label, "dispatch %s prog %016llx%s grid %ux%ux%u", indirect ? "indirect" : "", (unsigned long long)p.key,
                 !p.stages.empty() && p.stages[0].info.uses_cooperative_matrix ? " mma" : "", indirect ? 0u : x, indirect ? 0u : y, indirect ? 0u : z);
        gprof_end(cmd, label);
    } else if (tprof_on()) {
        tprof_end(cmd, profile_report::kCompute, p.key);   // (the performance profile: a compute dispatch, by its program)
    }
    st_dispatches++;
    if (pass_trace_on())
        trace_line("dispatch %ux%ux%u%s prog %016llx", x, y, z, indirect ? " indirect" : "", (unsigned long long)p.key);
    if (probe_on())
        for (size_t i = 0; i < dump_written().size(); i++) probe_image(cmd, dump_written()[i].tex, dump_written()[i].level, 0, p.key, 'd');
    if (dump_on())
        for (size_t i = 0; i < dump_written().size(); i++) {
            char what[64];
            snprintf(what, sizeof what, "dispatch %ux%ux%u prog %016llx img%zu", x, y, z, (unsigned long long)p.key, i);
            dump_image(cmd, dump_written()[i].tex, dump_written()[i].level, 0, what);
        }
}

// (hookshot) In the parallel replay's dry pass, in stream order on the replay thread: the images this draw or dispatch
// writes marked as the GPU's (texture_written) before any worker runs. Workers record out of order, and that mark was
// made as each reached its draw: one ahead of it sampled a render target or compute output as never written and
// uploaded stale guest memory over it, an upload that runs after the earlier segment's work. Targets change address
// and size as the camera moves: flashes of a lit scene gone black or white in motion (22 per 1000 frames moving, 45
// against 2 with serial replay). RX_VK_NO_DRY_MARKS=1: as before
void dry_mark_written(NvnState& s, bool compute, bool first) {
    static const bool off = getenv("RX_VK_NO_DRY_MARKS") != nullptr;
    if (off) return;
    if (!compute) {
        static thread_local decltype(s.rt) last;
        if (!first && !std::memcmp(&last, &s.rt, sizeof last)) return;
        std::memcpy((void*)&last, (const void*)&s.rt, sizeof last);
        const Targets at = targets(s);
        ParLock lk;   // (TICK-056, RX_VK_SEG_EARLY: segments recorded meanwhile read these marks; nothing when none is)
        for (int i = 0; i < at.n; i++)
            if (at.color[i]) texture_written(at.color[i], "the dry pass (a draw's target)");
        if (at.depth) texture_written(at.depth, "the dry pass (a draw's depth)");
        return;
    }
    const uint8_t types[8]{};
    const Program* p = program_for(s, ProgramKind::Compute, types);
    if (!p || !p->ready.load(std::memory_order_acquire) || p->failed) return;
    for (const Stage& st : p->stages)
        for (const auto& d : st.info.image_descriptors) {
            if (!d.is_written) continue;
            const VkImageViewType vt = view_type(d.type);
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(s, st, d, k);
                Texture* tex = nullptr;
                uint32_t level = 0;
                if (handle && texture_view_of(handle, vt, ViewUse::Storage, image_format(d.format), &tex, &level) && tex) {
                    ParLock lk;   // (TICK-056, as above)
                    texture_written(tex, "the dry pass (a dispatch's storage image)");
                }
            }
        }
}

void draw_stats(int64_t* draws, int64_t* skipped, int64_t* dispatches, int64_t* pipes) {
    *draws = st_draws, *skipped = st_skipped, *dispatches = st_dispatches, *pipes = st_pipelines.load();
}
void draw_stats2(int64_t* passes, int64_t* mesh, double* pipeline_ms) {
    *passes = st_passes, *mesh = st_mesh, *pipeline_ms = st_pipeline_us.load() / 1000.0;
}
int64_t draw_pipe_fresh_skips() { return st_pipe_fresh_skips.load(std::memory_order_relaxed); }
void draw_stats3(int64_t* pending_skips, int64_t* waits, double* wait_ms) {
    *pending_skips = st_pending_skips, *waits = st_waits, *wait_ms = st_wait_ms;
}
double draw_shader_wait_ms() { return st_shader_wait_ms; }
uint64_t pipeline_state_stamp() { return kStateStamp; }
bool draw_stats4(int64_t* links, double* link_ms, int64_t* lib_hits, int64_t* lib_cold, int64_t* lib_built, double* lib_build_ms) {
    *links = st_links.load(), *link_ms = st_link_us.load() / 1000.0, *lib_hits = st_lib_hits.load(), *lib_cold = st_lib_cold.load();
    *lib_built = st_lib_built.load(), *lib_build_ms = st_lib_build_us.load() / 1000.0;
    return gpl_wanted();
}
std::mutex rec_mu;
std::map<uint64_t, std::vector<std::string>> draw_records;   // texture frame -> its recorded draws
const std::vector<uint64_t>& record_progs() {
    static const std::vector<uint64_t> v = [] {
        std::vector<uint64_t> r;
        if (const char* e = getenv("RX_VK_FLASH_RECORD"))
            for (const char* q = e; *q;) {
                char* end;
                const uint64_t k = strtoull(q, &end, 16);
                if (end == q) break;
                r.push_back(k);
                q = *end ? end + 1 : end;
            }
        return r;
    }();
    return v;
}
void record_draw(const NvnState& s, const Program& p, const DrawArgs& d, const Targets& at) {
    char line[1024];
    int n = snprintf(line, sizeof line, "prog %016llx count %d first %d inst %d bv %d bi %d idx %d ibuf %#llx ind %#llx/%#llx",
                     (unsigned long long)p.key, d.count, d.first, d.instances, d.base_vertex, d.base_instance,
                     (int)d.indexed, (unsigned long long)d.ibuf, (unsigned long long)d.indirect,
                     (unsigned long long)d.count_addr);
    n += snprintf(line + n, sizeof line - n, " target %ux%u n%d samples %d c0 %d depth %d vp %g %g %g %g scissor %d %d %d %d", at.width, at.height, at.n,
                  (int)at.samples, (int)at.color_format[0], (int)at.depth_format, s.viewport[0][0], s.viewport[0][1], s.viewport[0][2], s.viewport[0][3],
                  s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
    n += snprintf(line + n, sizeof line - n, "%s", program_textures(s, p).c_str());
    if (d.indirect) {   // (what the GPU will take the draw's size from)
        uint32_t w[5]{};
        if (host->gpu_read(d.indirect, w, sizeof w))
            n += snprintf(line + n, sizeof line - n, " args %u %u %u %d %u", w[0], w[1], w[2], (int)w[3], w[4]);
    }
    for (const Stage& st : p.stages) {   // (each stage's uniform buffers: address, size, a hash of the first 512 bytes)
        const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
        for (int i = 0; i < 16 && n < 900; i++) {
            const nvn_p_bind_buf& u = s.ubo[ns][i];
            if (!u.addr || !(i + 3 < Shader::Info::MAX_CBUFS && st.info.constant_buffer_used_sizes[i + 3])) continue;
            uint8_t b[512];
            const uint64_t k = std::min<uint64_t>(sizeof b, u.size ? u.size : sizeof b);
            uint64_t h = 1469598103934665603ull;
            if (host->gpu_read(u.addr, b, k))
                for (uint64_t j = 0; j < k; j++) h = (h ^ b[j]) * 1099511628211ull;
            n += snprintf(line + n, sizeof line - n, " s%d.c%d %#llx+%#llx h%016llx", ns, i + 3, (unsigned long long)u.addr,
                          (unsigned long long)u.size, (unsigned long long)h);
        }
    }
    {
        static std::atomic<int> told{0};
        if (told++ < 300) logf("vk: record: %s", line);
    }
    // (RX_VK_RECORD_FLOATS=<words>: the fragment stage's first uniform buffer as floats, each draw, one line: which constant does what)
    static const int words = getenv("RX_VK_RECORD_FLOATS") ? atoi(getenv("RX_VK_RECORD_FLOATS")) : 0;
    if (words > 0) {
        const nvn_p_bind_buf& u = s.ubo[1][0];
        std::vector<float> f((size_t)std::min(words, 128));
        if (u.addr && host->gpu_read(u.addr, f.data(), f.size() * 4)) {
            std::string o = "vk: floats " + std::to_string(texture_frame_no());
            char b[24];
            for (const float v : f) snprintf(b, sizeof b, " %.4g", v), o += b;
            logf("%s", o.c_str());
        }
    }
    std::lock_guard lk(rec_mu);
    const uint64_t f = texture_frame_no();
    draw_records[f].push_back(line);
    while (draw_records.size() > 6) draw_records.erase(draw_records.begin());
}
void pass_traces_log(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(trace_mu);
    for (uint64_t f = lo; f <= hi; f++) {
        auto it = pass_traces.find(f);
        if (it == pass_traces.end()) continue;
        std::vector<std::pair<uint64_t, std::string>> v = it->second;
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        logf("vk: passes of frame %llu (%zu):", (unsigned long long)f, v.size());
        for (const auto& [key, l] : v) logf("vk:   [%llu] %s", (unsigned long long)(key >> 24), l.c_str());
    }
}
void draw_records_log(uint64_t lo, uint64_t hi) {
    std::lock_guard lk(rec_mu);
    for (uint64_t f = lo; f <= hi; f++) {
        auto it = draw_records.find(f);
        if (it == draw_records.end()) continue;
        logf("vk: draws of frame %llu (%zu):", (unsigned long long)f, it->second.size());
        for (const std::string& l : it->second) logf("vk:   %s", l.c_str());
    }
}
int64_t draw_desc_reused() {
    return st_desc_reused;
}
int64_t draw_slotgen_saves() {   // (RX_VK_DESC_SLOTGEN: of those, what the global registrations' generation check would have rebuilt)
    return st_desc_slotgen_saves;
}

// (HeyListen) the process's private commit now, in MB (0 where it cannot be read): the cache save logs it around its fetch, to show
// what the fetch cost in memory (the vector: its whole size; the mapped file: none)
uint64_t pcache_private_mb() {
#ifdef _WIN32
    struct Counters {
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage,
            QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage;
    };
    using MemInfo = BOOL(WINAPI*)(HANDLE, Counters*, DWORD);
    static const MemInfo info = (MemInfo)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    Counters c{};
    c.cb = sizeof c;
    return info && info(GetCurrentProcess(), &c, sizeof c) ? (uint64_t)(c.PrivateUsage >> 20) : 0;
#else
    return 0;
#endif
}
void pcache_log_save(const char* how, size_t n, uint64_t private_before_mb, uint64_t private_during_mb, int64_t ms) {
    logf("vk: pipeline cache saved (%s): %llu MB in %lld ms; process private commit %llu MB before the fetch, %llu MB with its data in hand",
         how, (unsigned long long)(n >> 20), (long long)ms, (unsigned long long)private_before_mb, (unsigned long long)private_during_mb);
}

// (HeyListen, RX_VK_PCACHE_MAP, default on) The cache's data fetched straight into the file's own pages: the driver writes into a
// mapped view of the temporary file, so the 1.2 GB never exists as private memory (the vector it replaces was 1.2 GB of commit and
// of working set until the write ended; mapped file pages are the system's to write out and to drop). The driver's data is still
// made whole in one call: Vulkan has no way to fetch it in pieces. False: nothing written (the caller then uses a vector).
bool pcache_save_mapped(const std::string& tmp, size_t n) {
#ifdef _WIN32
    static const bool on = !getenv("RX_VK_PCACHE_MAP") || atoi(getenv("RX_VK_PCACHE_MAP")) != 0;
    if (!on || !n) return false;
    HANDLE f = CreateFileA(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t before = pcache_private_mb();
    HANDLE m = CreateFileMappingA(f, nullptr, PAGE_READWRITE, (DWORD)((uint64_t)n >> 32), (DWORD)n, nullptr);
    void* view = m ? MapViewOfFile(m, FILE_MAP_WRITE, 0, 0, n) : nullptr;
    size_t got = n;
    if (view && vkGetPipelineCacheData(dev.device, pcache, &got, view) == VK_SUCCESS && got <= n) ok = true;
    if (ok)
        pcache_log_save("mapped file", got, before, pcache_private_mb(),
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
    if (view) UnmapViewOfFile(view);
    if (m) CloseHandle(m);
    if (ok && got < n) {   // (the cache shrank between the two calls: the file ends where the data does)
        LARGE_INTEGER end;
        end.QuadPart = (LONGLONG)got;
        ok = SetFilePointerEx(f, end, nullptr, FILE_BEGIN) && SetEndOfFile(f);
    }
    CloseHandle(f);
    if (!ok) DeleteFileA(tmp.c_str());
    return ok;
#else
    (void)tmp, (void)n;
    return false;
#endif
}

void program_fingerprinted(uint64_t fp) {
    if (fingerprint_probe) fingerprint_probe->push_back(fp);
    else ledger_add(ledger_id_seen(fp));
}
void shader_key_commit() {
    if (shader_key_on()) ledger_commit();
}

// (longshot #404) a save put off for builds under way, owed to the serial builds' thread (pipeline_cache_between_builds), and
// that thread's save there
std::atomic<bool> pcache_save_owed{false};
thread_local bool pcache_save_between = false;
/// (longshot #404) Intel's compute builds are one at a time (jobs_push_serial), and its compiler took 30 s to 6.5 minutes on some
/// of the game's compute programs on a UHD Graphics: one build or another was under way the whole of a 15-minute run, the queued
/// fetch's 10 s never found a moment without one, and every save in play was put off. Between two of the serial thread's builds
/// the save runs there: no compute build is under way, and the graphics builds it queues behind take milliseconds
void pipeline_cache_between_builds() {
    if (!pcache_save_owed.exchange(false)) return;
    pcache_save_between = true;
    struct Done { ~Done() { pcache_save_between = false; } } done;
    pipeline_cache_save(false);
}
// (HeyListen, S8) A save in play, by RX_VK_PCACHE_PLAY_SAVE: 2 every time it is asked, as before; 1 (the default) only with
// room for it and not more often than every kPlaySaveMinS; 0 never (a forced save, at exit, the prepare or Restart now, writes
// what play built). The owner's session (RTX 5070, 31 GB of RAM, 28 GB of private commit, 25.9 GB in use): the cache had
// grown to 1.27 GB and was saved 7 times in play, each one 1.27 GB of new file pages fetched and then written to disk; after
// two of them the game stopped for 3.5, 2.5, 3.2 and 14.2 s while 204 and 269 MB of its heap pages that HL_MEM_COLD had taken
// out of the working set came back: pages the new file pages had pushed out of RAM, read back from the page file. Room: the
// physical memory available is at least twice the cache plus kPlaySaveSpareMB, so that its pages take free RAM, not the
// game's. 0 save, 1 put off for room, 2 put off for the interval, 3 never in play
constexpr uint64_t kPlaySaveSpareMB = 4096;
constexpr int64_t kPlaySaveMinS = 300;
int pcache_play_save_gate(int mode, uint64_t avail_bytes, uint64_t cache_bytes, int64_t since_last_s) {
    if (mode >= 2) return 0;
    if (mode <= 0) return 3;
    if (avail_bytes < 2 * cache_bytes + (kPlaySaveSpareMB << 20)) return 1;
    if (since_last_s >= 0 && since_last_s < kPlaySaveMinS) return 2;
    return 0;
}
std::atomic<uint64_t> pcache_known_bytes{0};   // (the cache's size at its last save or load: the room the next save needs)
std::atomic<int64_t> pcache_saved_at_s{-1};      // (steady clock seconds of the last save in play; -1: none yet)
int pcache_play_save_mode() {
    static const int mode = [] {
        const char* e = getenv("RX_VK_PCACHE_PLAY_SAVE");
        return e && *e ? atoi(e) : 1;
    }();
    return mode;
}
uint64_t pcache_available_bytes() {
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    return GlobalMemoryStatusEx(&m) ? (uint64_t)m.ullAvailPhys : UINT64_MAX;
#else
    return UINT64_MAX;
#endif
}
void pipeline_cache_save(bool force) {
    const bool between = pcache_save_between;
    // (RX_VK_PCACHE_SAVE_ALWAYS=1, for measurements: every save asked in play goes ahead to the gate even with fewer than 32
    // pipelines built since the last one, as play with many new pipelines would have it)
    static const bool always = getenv("RX_VK_PCACHE_SAVE_ALWAYS") && atoi(getenv("RX_VK_PCACHE_SAVE_ALWAYS")) != 0;
    if (!pcache || (!force && !between && !always && pcache_built_since_save < 32)) return;
    if (!force && !between) {   // (a save asked in play: RX_VK_PCACHE_PLAY_SAVE's gate)
        const int64_t now_s =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const int64_t last = pcache_saved_at_s.load();
#ifdef _WIN32
        if (!pcache_known_bytes.load()) {   // (the size it had when it was loaded, from its file)
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExA(pcache_path.c_str(), GetFileExInfoStandard, &fa))
                pcache_known_bytes = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
        }
#endif
        const uint64_t avail = pcache_available_bytes(), size = pcache_known_bytes.load();
        const int gate = pcache_play_save_gate(pcache_play_save_mode(), avail, size, last < 0 ? -1 : now_s - last);
        if (gate) {
            static int64_t told;
            if (gate != 2 && (++told <= 4 || told % 32 == 0))
                logf("vk: pipeline cache save put off in play (%s; %lld pipelines owed): written at exit or later",
                     gate == 3 ? "RX_VK_PCACHE_PLAY_SAVE=0" : "too little free memory for it", (long long)pcache_built_since_save.load());
            if (gate == 1 && (told <= 4 || told % 32 == 0))
                logf("vk:   (%llu MB available, %llu MB of cache: a save in play wants twice the cache and %llu MB more)",
                     (unsigned long long)(avail >> 20), (unsigned long long)(size >> 20), (unsigned long long)kPlaySaveSpareMB);
            return;
        }
        pcache_saved_at_s = now_s;
    }
    const int64_t built = pcache_built_since_save.exchange(0);
    // (on a thread of its own unless forced, at exit: ~100 MB fetched and written took the replay thread 100+ ms at
    // each scene change; the cache is internally synchronized, pipelines go on being built meanwhile)
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) {
        if (between) {   // (a background save under way: owed again, for after the next build)
            pcache_built_since_save += built;
            pcache_save_owed = true;
            return;
        }
        if (!force) {
            pcache_built_since_save += built;   // (still to save)
            return;
        }
        while (busy.load()) Sleep(1);   // (the one under way, then this one)
        busy = true;
    }
    auto save = [force, between, built] {
        struct Done { ~Done() { busy = false; } } done;
#ifdef _WIN32
        // (HeyListen, S8: a save in play runs in Windows' background mode, on its own thread: its disk writes at low I/O
        // priority and the pages it touches, the file's among them, at low memory priority, so that RAM gives them up before
        // the game's. RX_VK_PCACHE_PLAY_SAVE=2: as before)
        if (!force && !between && pcache_play_save_mode() < 2) SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
        // (Intel: no build under way while the cache's data is fetched, PcacheFetch. A forced save waits for the builds; the
        // background one polls, and is put off when builds ran throughout; after kFetchPutOffs of those in a row it queues for
        // kFetchQueueMs, so a stretch of back-to-back builds cannot put it off for good. The mapped write holds the lock across
        // its one fetch into the file's pages; the vector path fetches under it and writes outside it)
        static std::atomic<int> put_off{0};
        const bool queue = force || between || put_off.load() >= kFetchPutOffs;
        const int wait_ms = force ? -1 : queue ? kFetchQueueMs : kFetchPollMs;
        bool had = true;
        auto fetch = [&](size_t* n, void* data) {
            PcacheFetch lock(wait_ms, queue);
            had = lock.held();
            return had && vkGetPipelineCacheData(dev.device, pcache, n, data) == VK_SUCCESS;
        };
        const auto put_off_now = [&] {
            const int times = ++put_off;
            if (times <= 4 || times % 16 == 0)
                logf("vk: pipeline cache save put off (%d in a row): pipelines were being built throughout%s", times,
                     serial_builds_on() ? "; owed to the serial builds' thread, between two of its builds" : "");
            pcache_built_since_save += built;   // (still to save: the next request writes them, not only after 32 more)
            if (serial_builds_on()) pcache_save_owed = true;
        };
        try {
            size_t n = 0;
            // (hookshot, 11-10) the size last read or written: nothing new in the cache, not fetched nor written again (pcache_saved_size)
            if (fetch(&n, nullptr) && n && n != pcache_saved_size) {
                bool mapped = false;
                {
                    PcacheFetch lock(wait_ms, queue);
                    had = lock.held();
                    mapped = had && pcache_save_mapped(pcache_path + ".tmp", n);
                }
                if (!had) return put_off_now();
                if (mapped) {
                    MoveFileExA((pcache_path + ".tmp").c_str(), pcache_path.c_str(), MOVEFILE_REPLACE_EXISTING);
                    pcache_known_bytes = n;
                    pcache_saved_size = n;
                    put_off = 0;
                    return;
                }
                const auto fetch_t0 = std::chrono::steady_clock::now();
                const uint64_t private_before = pcache_private_mb();
                std::vector<uint8_t> data(n);
                if (fetch(&n, data.data())) {
                    pcache_log_save("vector", n, private_before, pcache_private_mb(),
                                    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - fetch_t0).count());
                    const std::string tmp = pcache_path + ".tmp";
                    if (FILE* f = fopen(tmp.c_str(), "wb")) {
                        const bool ok = fwrite(data.data(), 1, n, f) == n;
                        fclose(f);
                        if (ok && MoveFileExA(tmp.c_str(), pcache_path.c_str(), MOVEFILE_REPLACE_EXISTING)) pcache_known_bytes = n, pcache_saved_size = n;
                    }
                } else if (!had) {
                    return put_off_now();   // (the size had, the data not: put off as well, not lost)
                }
            } else if (!had) {
                return put_off_now();
            }
            put_off = 0;
        } catch (const std::exception& e) {
            logf("vk: pipeline cache save failed: %s", e.what());
        }
    };
    if (force || between) save();
    else {
        try {
            std::thread(save).detach();
        } catch (const std::exception& e) {
            busy = false;
            logf("vk: could not start pipeline cache save: %s", e.what());
        }
    }
}

} // namespace mkwvk

namespace mkwvk {
/// (longshot #252) The Intel shader compiles row (Debug tab: side by side) or RX_VK_NO_INTEL_SERIAL=1 over it: Intel's compiler on
/// Windows is entered by several threads at once, as on every other GPU. Off (the default): one at a time, vk_internal.h IntelDriverGate
bool intel_compiles_side_by_side() {
    static const bool on = menu_env_on("RX_VK_NO_INTEL_SERIAL", "intel_shader_compiles");
    return on;
}
/// (longshot #252) The guarded driver calls, for the files outside this one. Declaring guarded_* itself in the header would be a
/// second function (the definitions sit in the unnamed namespace) and every call in this file would be ambiguous
VkResult extern_guarded_graphics(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo* ci, VkPipeline* pl) {
    return guarded_graphics(cache, ci, pl);
}
VkResult extern_guarded_compute(VkPipelineCache cache, const VkComputePipelineCreateInfo* ci, VkPipeline* pl) {
    return guarded_compute(cache, ci, pl);
}
VkResult extern_guarded_shader_module(const VkShaderModuleCreateInfo* ci, VkShaderModule* m) {
    return guarded_shader_module(ci, m);
}
/// (hookshot) n bytes copied into the recording's stream, as a buffer binding (vk_memory.cpp gpu_buffer_bind: a read whose pool is
/// still being imported in the background); empty when there is no recording or no room
BufferRef stream_copy(const void* src, uint64_t n) {
    if (!cur || !n) return {};
    const uint64_t align = std::max<uint64_t>({256, dev.props.limits.minUniformBufferOffsetAlignment,
                                               dev.props.limits.minStorageBufferOffsetAlignment});
    const uint64_t at = stream_alloc(n, align);
    if (at == UINT64_MAX) return {};
    std::memcpy(cur->mapped + at, src, n);
    return {cur->stream, at, n, 0};
}
} // namespace mkwvk
