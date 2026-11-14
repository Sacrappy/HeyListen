// (HeyListen, 2026-11-14) RX_VK_LEDGER=<seconds>: the memory ledger (see vk_ledger.h). The console holds a texture's bytes once;
// here they may be in the game's pool storage (guest memory: RAM and commit), a VRAM mirror of that pool, the Vulkan image made
// from it, the BC cache, staging, a capture. Nobody had the one table that says, class by class, how many times: this is it.
//
// How each column is known:
// - classes: every live texture the back end holds an image for, by its NVN storage (ASTC by block size, other block-compressed,
//   uncompressed) or as a render target (color, depth, finalized but kept); every page of the game's live pools by what the GPU
//   used it for since the pool was made (marks or'ed in by the bindings: vertex, index, uniform, storage, indirect and GPU-written,
//   other; and texture storage whose image is gone), else by the pool's flags (shader code, physical, never used: by CPU access);
//   the rest of the guest window is the game's plain memory (heaps and address regions outside pools);
// - console bytes: a texture's storage size in its NVN format (each storage counted once), a buffer class's pool pages;
// - guest committed and in RAM: the host's page census of those pages (hos_ledger_pages, host/hos_hostsplit.c);
// - mirror and import: the pages of the live VRAM mirrors and of the live imports, classified the same way;
// - image: the Vulkan image (bytes and padding VMA gave it), with the format it became;
// - what belongs to no resource: the allocations by kind (vk_vma.cpp's counts), VMA's free block space by pool, VRAM outside VMA
//   (pipelines, shader modules, descriptor pools, swapchain, upscaler, the driver's own), and the host census's groups
//   (hos_hostsplit_totals: heaps, images, stacks, the driver's private arenas);
// - reconciled: the VRAM column against what the driver says the process uses (the VRAM breakdown's "heaps" figure), the system
//   column against the working set, each with its remainder printed.
// The snapshot is taken on the replay's thread (the texture table is its own); the counting, the page census and the host's census
// (0.5-1 s) run on a thread of their own. Off (the default) nothing is marked, counted or logged.
#include "vk_ledger.h"
#include "vk_internal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <tuple>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

namespace mkwvk {

thread_local uint16_t ledger_use = kUseOther;

namespace {
int ledger_secs() {
    static const int v = getenv("RX_VK_LEDGER") ? std::max(0, atoi(getenv("RX_VK_LEDGER"))) : 0;
    return v;
}

// ---- the marks: one 16-bit word a 4 KB guest page below 1 TB, in leaves of 16 MB made on first use ----
constexpr uint64_t kPagesPerLeaf = 4096;
constexpr uint64_t kLeaves = (1ull << 40) / (kPagesPerLeaf << 12);
std::atomic<uint16_t*> mark_leaves[kLeaves];
std::atomic<uint32_t> mark_clears{0};   // (a pool made: the per-thread memo of ranges marked goes stale)
uint16_t* mark_leaf(uint64_t i, bool make) {
    uint16_t* l = mark_leaves[i].load(std::memory_order_acquire);
    if (l || !make) return l;
    uint16_t* n = static_cast<uint16_t*>(calloc(kPagesPerLeaf, sizeof(uint16_t)));
    if (!n) return nullptr;
    if (!mark_leaves[i].compare_exchange_strong(l, n, std::memory_order_acq_rel)) {
        free(n);
        return l;
    }
    return n;
}
uint16_t mark_at(uint64_t page) {
    const uint16_t* l = mark_leaf(page / kPagesPerLeaf, false);
    return l ? __atomic_load_n(&l[page % kPagesPerLeaf], __ATOMIC_RELAXED) : 0;
}
struct MarkMemo { uint64_t gpu = 0, size = 0; uint32_t clears = 0; uint16_t use = 0; };
thread_local MarkMemo mark_memo[64];   // (the same ranges are bound draw after draw: marked once)
}  // namespace

bool ledger_on() {
    static const bool v = ledger_secs() > 0;
    return v;
}

void ledger_mark_slow(uint64_t gpu, uint64_t size, uint16_t use) {
    if (!size || gpu >= (1ull << 40)) return;
    size = std::min<uint64_t>(size, (1ull << 40) - gpu);
    const uint32_t clears = mark_clears.load(std::memory_order_relaxed);
    MarkMemo& m = mark_memo[((gpu >> 4) ^ (size * 0x9E3779B1u) ^ use) & 63];
    if (m.gpu == gpu && m.size == size && m.use == use && m.clears == clears) return;
    m = MarkMemo{gpu, size, clears, use};
    for (uint64_t p = gpu >> 12, e = (gpu + size + 4095) >> 12; p < e; p++) {
        uint16_t* l = mark_leaf(p / kPagesPerLeaf, true);
        if (!l) return;
        uint16_t* w = &l[p % kPagesPerLeaf];
        if ((__atomic_load_n(w, __ATOMIC_RELAXED) & use) != use) __atomic_fetch_or(w, use, __ATOMIC_RELAXED);
    }
}

uint16_t ledger_use_of(uint64_t page) { return ledger_on() ? mark_at(page) : 0; }

void ledger_pool_made(uint64_t lo, uint64_t size) {
    if (lo >= (1ull << 40)) return;
    for (uint64_t p = lo >> 12, e = std::min<uint64_t>((lo + size + 4095) >> 12, (1ull << 28)); p < e; p++)
        if (uint16_t* l = mark_leaf(p / kPagesPerLeaf, false)) __atomic_store_n(&l[p % kPagesPerLeaf], (uint16_t)0, __ATOMIC_RELAXED);
    mark_clears.fetch_add(1, std::memory_order_relaxed);
}

namespace {
// ---- the host's side, found by name in the executable (none on other systems: the system columns are then empty) ----
using PagesFn = int (*)(uint64_t, uint64_t, uint8_t*);
using TotalsFn = int (*)(uint64_t*, int);
using LivePoolsFn = int (*)(uint64_t (*)[2], int);
using SpillFn = uint64_t (*)();
template <class F> F host_fn(const char* name) {
#ifdef _WIN32
    return (F)(void*)GetProcAddress(GetModuleHandleA(nullptr), name);
#else
    (void)name;
    return nullptr;
#endif
}
// (hos_hostsplit_totals' values, in its order)
enum { T_WS, T_PRIVATE, T_RAM_COUNTED, T_WIN_RES, T_WIN_COM, T_BAR, T_GROUP_COM, T_GROUP_RES = T_GROUP_COM + 9, T_BIG_RES = T_GROUP_RES + 9,
       T_BIG_COM, T_WALK_MS, T_N };
constexpr int G_TAG = 1, G_BAR = 2, G_HEAP = 3, G_STACK = 4, G_IMAGE = 5, G_DRIVER_IMAGE = 6, G_MAPPED = 7, G_OTHER = 8;   // (its groups)

constexpr uint64_t MB(uint64_t b) { return b >> 20; }

struct Row {
    std::string name;
    uint64_t res_n = 0, console = 0, pool = 0, gcom = 0, gres = 0, mirror = 0, import = 0, vk = 0, vk_n = 0;
    std::map<uint32_t, uint64_t> fmt;               // (Vulkan format: bytes)
    std::unordered_map<uint64_t, uint64_t> stores;  // (a texture class's storages, each counted once)
};
const char* fmt_name(uint32_t f) {
    switch (f) {
    case 131: case 132: case 133: case 134: return "BC1";
    case 135: case 136: return "BC2";
    case 137: case 138: return "BC3";
    case 139: case 140: return "BC4";
    case 141: case 142: return "BC5";
    case 143: case 144: return "BC6H";
    case 145: case 146: return "BC7";
    case 37: case 43: case 44: case 50: return "RGBA8";
    case 97: return "RGBA16F";
    case 109: return "RGBA32F";
    case 122: case 98: case 100: return "R11G11B10/half";
    case 64: return "RGB10A2";
    case 126: return "D32";
    case 129: return "D24S8";
    case 130: return "D32S8";
    case 124: return "D16";
    default: return nullptr;
    }
}

struct Ledger {
    std::vector<Row> rows;
    std::map<std::string, size_t> by_name;
    size_t row(const std::string& n) {
        auto it = by_name.find(n);
        if (it != by_name.end()) return it->second;
        rows.push_back(Row{n});
        return by_name[n] = rows.size() - 1;
    }
};

// what a texture is, as a class name
std::string tex_class(const LedgerTex& t) {
    if (t.gpu_written) return t.released ? "render targets, finalized, kept" : t.depth_aspect ? "render targets, depth/stencil" : "render targets, color";
    char b[64];
    if (t.astc) snprintf(b, sizeof b, "textures ASTC %dx%d", t.block_w, t.block_h);
    else if (t.block_w > 1) snprintf(b, sizeof b, "textures BC and other compressed");
    else snprintf(b, sizeof b, "textures uncompressed");
    return b;
}

// the class of a pool page with no live texture: by its marks, then by the pool's flags (NVN: 1 CPU no access, 2 CPU uncached, 4 CPU
// cached, 8 GPU no access, 0x40 shader code, 0x100 physical)
const char* page_class(uint16_t m, uint32_t flags) {
    if (flags & 0x40) return "pools: shader code";
    if (flags & 0x100) return "pools: physical";
    if (m & (kUseIndirect | kUseGpuWrite)) return "buffers: indirect and GPU-written";
    if (m & kUseStorage) return "buffers: storage";
    if (m & kUseUniform) return "buffers: uniform";
    if (m & kUseIndex) return "buffers: index";
    if (m & kUseVertex) return "buffers: vertex";
    if (m & kUseOther) return "buffers: other bindings (copies, texel)";
    if (m & kUseRendered) return "texture storage, rendered, no live image";
    if (m & kUseSampled) return "texture storage, no live image";
    if (flags & 8) return "pools never used: GPU no access";
    if (flags & 1) return "pools never used: CPU no access";
    if (flags & 2) return "pools never used: CPU uncached";
    return "pools never used: CPU cached";
}

// the texture classes' pages: a row number + 1 a page, in leaves made for one count
struct TexPages {
    std::vector<std::unique_ptr<uint8_t[]>> leaves = std::vector<std::unique_ptr<uint8_t[]>>(kLeaves);
    void paint(uint64_t lo, uint64_t size, uint8_t v) {
        if (lo >= (1ull << 40) || !size) return;
        for (uint64_t p = lo >> 12, e = std::min<uint64_t>((lo + size + 4095) >> 12, 1ull << 28); p < e; p++) {
            auto& l = leaves[p / kPagesPerLeaf];
            if (!l) l.reset(new uint8_t[kPagesPerLeaf]());
            if (!l[p % kPagesPerLeaf]) l[p % kPagesPerLeaf] = v;
        }
    }
    uint8_t at(uint64_t p) const {
        const auto& l = leaves[p / kPagesPerLeaf];
        return l ? l[p % kPagesPerLeaf] : 0;
    }
};

struct Pool { uint64_t lo, hi; uint32_t flags; };

void count(const LedgerSnap& s, const char* why, double secs) {
    static const PagesFn pages_fn = host_fn<PagesFn>("hos_ledger_pages");
    static const TotalsFn totals_fn = host_fn<TotalsFn>("hos_hostsplit_totals");
    static const LivePoolsFn live_fn = host_fn<LivePoolsFn>("hos_gpu_live_pools");
    static const SpillFn spill_fn = host_fn<SpillFn>("hos_drop_spill_bytes");
    const auto t0 = std::chrono::steady_clock::now();
    Ledger L;

    // the textures, and their pages
    TexPages tp;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, int>, std::pair<uint64_t, uint64_t>> shapes;   // (render targets: n, bytes)
    uint64_t tex_vk = 0;
    for (const LedgerTex& t : s.textures) {
        const size_t r = L.row(tex_class(t));
        Row& R = L.rows[r];
        R.res_n++, R.vk += t.vk_bytes, R.vk_n++, R.fmt[t.vk_format] += t.vk_bytes, tex_vk += t.vk_bytes;
        uint64_t& st = R.stores[t.gpu];
        if (t.guest_size > st) R.console += t.guest_size - st, st = t.guest_size;
        if (r < 254) tp.paint(t.gpu, t.guest_size, (uint8_t)(r + 1));
        if (t.gpu_written) {
            auto& sh = shapes[{t.width, t.height, t.vk_format, t.released ? 1 : 0}];
            sh.first++, sh.second += t.vk_bytes;
        }
    }

    // the game's live pools (clipped where one lies over another), with their flags
    std::vector<Pool> pools;
    {
        std::vector<std::array<uint64_t, 2>> live(16384);
        const int n = live_fn ? live_fn(reinterpret_cast<uint64_t(*)[2]>(live.data()), (int)live.size()) : 0;
        std::map<uint64_t, std::pair<uint64_t, uint32_t>> flags;
        for (const LedgerPoolFlags& f : s.pool_flags) flags[f.lo] = {f.hi, f.flags};
        for (int i = 0; i < n; i++) {
            if (live[i][1] <= live[i][0] || live[i][0] >= (1ull << 40)) continue;
            uint32_t fl = 0;
            auto it = flags.upper_bound(live[i][0]);
            if (it != flags.begin() && std::prev(it)->second.first > live[i][0]) fl = std::prev(it)->second.second;
            pools.push_back({live[i][0] & ~4095ull, (live[i][1] + 4095) & ~4095ull, fl});
        }
        std::sort(pools.begin(), pools.end(), [](const Pool& a, const Pool& b) { return a.lo < b.lo; });
        uint64_t cur = 0;
        std::vector<Pool> clipped;
        for (Pool p : pools) {
            p.lo = std::max(p.lo, cur);
            if (p.lo >= p.hi) continue;
            cur = p.hi;
            clipped.push_back(p);
        }
        pools.swap(clipped);
    }
    // a page's row: its live texture's class, else its marks and pool flags; -1 flags: outside every pool
    auto classify = [&](uint64_t page, int64_t flags) -> size_t {
        if (const uint8_t t = tp.at(page)) return t - 1;
        if (flags < 0) return L.row("outside any pool (an import chunk's reach)");
        return L.row(page_class(mark_at(page), (uint32_t)flags));
    };
    // every page of [lo, hi) with its pool's flags (-1 between pools)
    auto walk = [&](uint64_t lo, uint64_t hi, auto&& fn) {
        auto it = std::upper_bound(pools.begin(), pools.end(), lo, [](uint64_t a, const Pool& p) { return a < p.lo; });
        if (it != pools.begin() && std::prev(it)->hi > lo) --it;
        for (uint64_t a = lo; a < hi;) {
            while (it != pools.end() && it->hi <= a) ++it;
            if (it == pools.end() || it->lo >= hi) {
                for (; a < hi; a += 4096) fn(a >> 12, (int64_t)-1);
                break;
            }
            for (; a < std::min(it->lo, hi); a += 4096) fn(a >> 12, (int64_t)-1);
            for (; a < std::min(it->hi, hi); a += 4096) fn(a >> 12, (int64_t)it->flags);
        }
    };

    // the pools' pages: console bytes (buffers' and unused storage's), guest commit and RAM
    uint64_t pool_bytes = 0, pool_com = 0, pool_res = 0;
    std::vector<uint8_t> state(4096);
    // (the mixture: what each pool page was ever used for, texture storage, a buffer binding, both; the buffer ranges' runs inside
    // the pools and the 64 KB granules they touch: what a mirror and an import limited to the buffer ranges would cover)
    constexpr uint16_t kBuf = kUseVertex | kUseIndex | kUseUniform | kUseStorage | kUseIndirect | kUseGpuWrite | kUseOther;
    constexpr uint16_t kTex = kUseSampled | kUseRendered;
    struct Mix { uint64_t tex = 0, buf = 0, both = 0, none = 0, runs = 0, grans = 0; };
    Mix all_mix, mixed_mix, pure_tex_mix, pure_buf_mix;
    uint64_t mixed_n = 0, pure_tex_n = 0, pure_buf_n = 0, none_n = 0;
    uint64_t run_hist[6]{}, run_bytes[6]{};   // (buffer runs by size: <=4 KB, <=64 KB, <=1 MB, <=16 MB, <=64 MB, more)
    struct Top { uint64_t lo, size; uint32_t flags; Mix m; };
    std::vector<Top> tops;
    std::map<uint32_t, Mix> by_flags;
    for (const Pool& p : pools) {
        Mix m;
        uint64_t run = 0, last_gran = ~0ull;
        auto end_run = [&] {
            if (!run) return;
            const int k = run <= 4096 ? 0 : run <= (64u << 10) ? 1 : run <= (1u << 20) ? 2 : run <= (16u << 20) ? 3 : run <= (64u << 20) ? 4 : 5;
            run_hist[k]++, run_bytes[k] += run, m.runs++, run = 0;
        };
        for (uint64_t a = p.lo; a < p.hi; a += 4096) {
            const uint64_t page = a >> 12;
            const uint16_t k = mark_at(page);
            const bool t = (k & kTex) || tp.at(page), b = (k & kBuf) != 0;
            if (t && b) m.both += 4096;
            else if (t) m.tex += 4096;
            else if (b) m.buf += 4096;
            else m.none += 4096;
            if (b) {
                run += 4096;
                if ((a >> 16) != last_gran) last_gran = a >> 16, m.grans++;
            } else end_run();
        }
        end_run();
        auto add = [](Mix& d, const Mix& s) {
            d.tex += s.tex, d.buf += s.buf, d.both += s.both, d.none += s.none, d.runs += s.runs, d.grans += s.grans;
        };
        add(all_mix, m), add(by_flags[p.flags], m);
        const bool has_t = m.tex + m.both > 0, has_b = m.buf + m.both > 0;
        if (has_t && has_b) add(mixed_mix, m), mixed_n++, tops.push_back({p.lo, p.hi - p.lo, p.flags, m});
        else if (has_t) add(pure_tex_mix, m), pure_tex_n++;
        else if (has_b) add(pure_buf_mix, m), pure_buf_n++;
        else none_n++;
    }
    for (const Pool& p : pools)
        for (uint64_t lo = p.lo; lo < p.hi; lo += 4096ull << 12) {
            const uint64_t n = std::min<uint64_t>(4096, (p.hi - lo) >> 12);
            if (!pages_fn || !pages_fn(lo, n, state.data())) std::fill(state.begin(), state.end(), 0);
            for (uint64_t i = 0; i < n; i++) {
                const uint64_t page = (lo >> 12) + i;
                const uint8_t t = tp.at(page);
                Row& R = L.rows[t ? t - 1 : classify(page, p.flags)];
                if (!t) R.console += 4096;
                R.pool += 4096, pool_bytes += 4096;
                if (state[i] & 1) R.gcom += 4096, pool_com += 4096;
                if (state[i] & 2) R.gres += 4096, pool_res += 4096;
            }
        }
    // the mirrors and the imports, page by page; and what the mirrors' pages were ever used for
    uint64_t mir_all = 0, mir_buf = 0, mir_tex = 0, mir_none = 0, mir_out = 0, imp_all = 0, imp_out = 0;
    for (const LedgerRange& m : s.mirrors)
        walk(m.lo, m.hi, [&](uint64_t page, int64_t flags) {
            L.rows[classify(page, flags)].mirror += 4096, mir_all += 4096;
            const uint16_t k = flags < 0 ? 0 : mark_at(page);
            if (flags < 0) mir_out += 4096;
            else if (k & (kUseVertex | kUseIndex | kUseUniform | kUseStorage | kUseIndirect | kUseGpuWrite | kUseOther)) mir_buf += 4096;
            else if (k & (kUseSampled | kUseRendered) || tp.at(page)) mir_tex += 4096;
            else mir_none += 4096;
        });
    for (const LedgerRange& m : s.imports)
        walk(m.lo, m.hi, [&](uint64_t page, int64_t flags) {
            L.rows[classify(page, flags)].import += 4096, imp_all += 4096;
            if (flags < 0) imp_out += 4096;
        });

    // the host's census: the window, and what is outside it
    uint64_t T[T_N]{};
    const bool census = totals_fn && totals_fn(T, T_N) == T_N;
    {
        Row& H = L.rows[L.row("game memory outside pools (heaps, regions, image)")];
        H.gcom = census && T[T_WIN_COM] > pool_com ? T[T_WIN_COM] - pool_com : 0;
        H.gres = census && T[T_WIN_RES] > pool_res ? T[T_WIN_RES] - pool_res : 0;
        H.console = H.gcom;   // (on the console the game's memory is what it committed: the same bytes)
    }

    // the table
    logf("vk: ledger: ---- %s, %.0f s: %zu textures, %zu live pools (%llu MB), %zu mirrors, %zu imports ----", why, secs, s.textures.size(),
         pools.size(), (unsigned long long)MB(pool_bytes), s.mirrors.size(), s.imports.size());
    logf("vk: ledger: %-42s %6s %8s %7s %7s %7s %7s %6s %7s %6s %6s  %s", "class", "count", "console", "g.comm", "g.RAM", "mirror", "image", "n img",
         "import", "VRAM/c", "RAM/c", "image formats; measured image / console");
    std::vector<size_t> order(L.rows.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return L.rows[a].name < L.rows[b].name; });
    uint64_t sum_console = 0, sum_mirror = 0, sum_vk = 0, sum_gres = 0, sum_gcom = 0, sum_import = 0;
    for (size_t i : order) {
        const Row& R = L.rows[i];
        sum_console += R.console, sum_mirror += R.mirror, sum_vk += R.vk, sum_gres += R.gres, sum_gcom += R.gcom, sum_import += R.import;
        std::string f;
        std::vector<std::pair<uint64_t, uint32_t>> fs;
        for (auto& [k, v] : R.fmt) fs.push_back({v, k});
        std::sort(fs.rbegin(), fs.rend());
        for (size_t k = 0; k < fs.size() && k < 3; k++) {
            char b[48];
            const char* n = fmt_name(fs[k].second);
            if (n) snprintf(b, sizeof b, "%s%s %llu", k ? ", " : "", n, (unsigned long long)MB(fs[k].first));
            else snprintf(b, sizeof b, "%sfmt %u %llu", k ? ", " : "", fs[k].second, (unsigned long long)MB(fs[k].first));
            f += b;
        }
        if (R.vk && R.console) {
            char b[32];
            snprintf(b, sizeof b, "; x%.2f", (double)R.vk / (double)R.console);
            f += b;
        }
        const double c = R.console ? (double)R.console : 0.0;
        char vr[16] = "-", rr[16] = "-";
        if (c) snprintf(vr, sizeof vr, "%.2f", (double)(R.mirror + R.vk) / c), snprintf(rr, sizeof rr, "%.2f", (double)R.gres / c);
        logf("vk: ledger: %-42s %6llu %8llu %7llu %7llu %7llu %7llu %6llu %7llu %6s %6s  %s", R.name.c_str(), (unsigned long long)R.res_n,
             (unsigned long long)MB(R.console), (unsigned long long)MB(R.gcom), (unsigned long long)MB(R.gres), (unsigned long long)MB(R.mirror),
             (unsigned long long)MB(R.vk), (unsigned long long)R.vk_n, (unsigned long long)MB(R.import), vr, rr, f.c_str());
    }
    logf("vk: ledger: %-42s %6s %8llu %7llu %7llu %7llu %7llu %6s %7llu %6.2f %6.2f", "all game resources", "", (unsigned long long)MB(sum_console),
         (unsigned long long)MB(sum_gcom), (unsigned long long)MB(sum_gres), (unsigned long long)MB(sum_mirror), (unsigned long long)MB(sum_vk), "",
         (unsigned long long)MB(sum_import), sum_console ? (double)(sum_mirror + sum_vk) / (double)sum_console : 0.0,
         sum_console ? (double)sum_gres / (double)sum_console : 0.0);

    // the mirrors: what their pages were used for
    logf("vk: ledger: mirrors %llu MB over: pages bound as buffers %llu MB, pages that only held texture storage %llu MB, pool pages never "
         "bound %llu MB, outside every pool %llu MB (a mirror per range bound as a buffer would hold the first); retired, not freed yet "
         "%llu MB. Imports %llu MB (outside every pool %llu)",
         (unsigned long long)MB(mir_all), (unsigned long long)MB(mir_buf), (unsigned long long)MB(mir_tex), (unsigned long long)MB(mir_none),
         (unsigned long long)MB(mir_out), (unsigned long long)MB(s.retired_mirror_bytes), (unsigned long long)MB(imp_all),
         (unsigned long long)MB(imp_out));
    // the pools' mixture: texture storage against buffer bindings, over each pool's life so far
    {
        auto mixline = [](const char* what, uint64_t n, const Mix& m) {
            logf("vk: ledger: pools %s: %llu, MB: only texture storage %llu, only bound as buffers %llu, both (a range reused, or a buffer "
                 "over a texture) %llu, neither %llu; buffer runs %llu over %llu granules of 64 KB (%llu MB)",
                 what, (unsigned long long)n, (unsigned long long)MB(m.tex), (unsigned long long)MB(m.buf), (unsigned long long)MB(m.both),
                 (unsigned long long)MB(m.none), (unsigned long long)m.runs, (unsigned long long)m.grans, (unsigned long long)(m.grans >> 4));
        };
        mixline("all", pools.size(), all_mix);
        mixline("mixed (texture storage and buffers)", mixed_n, mixed_mix);
        mixline("texture storage only", pure_tex_n, pure_tex_mix);
        mixline("buffers only", pure_buf_n, pure_buf_mix);
        logf("vk: ledger: pools never used by the GPU: %llu", (unsigned long long)none_n);
        static const char* const rn[6] = {"<=4 KB", "<=64 KB", "<=1 MB", "<=16 MB", "<=64 MB", ">64 MB"};
        char hb[400];
        int at = 0;
        for (int k = 0; k < 6; k++)
            at += snprintf(hb + at, sizeof hb - at, " %s %llu (%llu MB);", rn[k], (unsigned long long)run_hist[k], (unsigned long long)MB(run_bytes[k]));
        logf("vk: ledger: buffer runs inside pools by size, count (MB):%s", hb);
        std::string fl;
        for (const auto& [f, m] : by_flags) {
            char b[160];
            snprintf(b, sizeof b, " %#x: tex %llu, buf %llu, both %llu, none %llu, runs %llu;", f, (unsigned long long)MB(m.tex),
                     (unsigned long long)MB(m.buf), (unsigned long long)MB(m.both), (unsigned long long)MB(m.none), (unsigned long long)m.runs);
            if (fl.size() < 800) fl += b;
        }
        logf("vk: ledger: the mixture by pool flags, MB:%s", fl.c_str());
        std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b) { return a.size > b.size; });
        for (size_t i = 0; i < tops.size() && i < 12; i++)
            logf("vk: ledger: mixed pool %#llx %llu MB flags %#x: texture %llu MB, buffers %llu MB, both %llu MB, neither %llu MB; %llu buffer "
                 "runs over %llu granules", (unsigned long long)tops[i].lo, (unsigned long long)MB(tops[i].size), tops[i].flags,
                 (unsigned long long)MB(tops[i].m.tex), (unsigned long long)MB(tops[i].m.buf), (unsigned long long)MB(tops[i].m.both),
                 (unsigned long long)MB(tops[i].m.none), (unsigned long long)tops[i].m.runs, (unsigned long long)tops[i].m.grans);
    }
    // the pool table's chunks: the live pools in each against those the game finalized (pools it made there that are no longer
    // live), what no live pool covers, its import and mirror, how long since it was bound
    {
        std::vector<LedgerRange> made;   // (every pool the game made, live or finalized, as the back end heard of them; clipped)
        for (const LedgerPoolFlags& f : s.pool_flags) made.push_back({f.lo, f.hi});
        std::sort(made.begin(), made.end(), [](const LedgerRange& a, const LedgerRange& b) { return a.lo < b.lo; });
        auto covered = [](const auto& v, uint64_t lo, uint64_t hi) {   // (bytes of [lo, hi) inside the sorted ranges, overlaps once)
            uint64_t n = 0, cur = lo;
            auto it = std::upper_bound(v.begin(), v.end(), lo, [](uint64_t a, const auto& p) { return a < p.lo; });
            if (it != v.begin()) --it;
            for (; it != v.end() && it->lo < hi; ++it) {
                const uint64_t a = std::max(cur, it->lo), b = std::min(hi, it->hi);
                if (b > a) n += b - a, cur = b;
            }
            return n;
        };
        struct Agg { uint64_t n = 0, bytes = 0, imp = 0, mir = 0, live = 0, fin = 0; };
        Agg all, orphan, part;
        uint64_t orphan_ago[4]{}, orphan_ago_b[4]{};   // (orphans by submissions since bound: <600, <6000, <60000, more)
        std::vector<std::pair<uint64_t, const LedgerChunk*>> orphans;
        for (const LedgerChunk& c : s.chunks) {
            const uint64_t size = c.hi - c.lo, live = covered(pools, c.lo, c.hi), any = covered(made, c.lo, c.hi);
            const uint64_t fin = any > live ? any - live : 0;
            auto add = [&](Agg& a) {
                a.n++, a.bytes += size, a.imp += c.import ? size : 0, a.mir += c.mirror ? size : 0, a.live += live, a.fin += fin;
            };
            add(all);
            if (!live) {
                add(orphan), orphans.push_back({size, &c});
                const int k = c.bound_ago < 600 ? 0 : c.bound_ago < 6000 ? 1 : c.bound_ago < 60000 ? 2 : 3;
                orphan_ago[k]++, orphan_ago_b[k] += size;
            } else if (fin) add(part);
        }
        auto aggline = [](const char* what, const Agg& a) {
            logf("vk: ledger: chunks %s: %llu, %llu MB (import held %llu MB, mirror %llu MB); live pools in them %llu MB, finalized pools "
                 "%llu MB, no pool at all %llu MB", what, (unsigned long long)a.n, (unsigned long long)MB(a.bytes), (unsigned long long)MB(a.imp),
                 (unsigned long long)MB(a.mir), (unsigned long long)MB(a.live), (unsigned long long)MB(a.fin),
                 (unsigned long long)MB(a.bytes > a.live + a.fin ? a.bytes - a.live - a.fin : 0));
        };
        aggline("in the pool table", all);
        aggline("whose pools are all finalized (no live pool left)", orphan);
        aggline("with live and finalized pools", part);
        logf("vk: ledger: chunks with no live pool, by submissions since last bound: <600 %llu (%llu MB), <6000 %llu (%llu MB), <60000 %llu "
             "(%llu MB), more %llu (%llu MB); retired chunks waiting for the GPU %llu (%llu MB); imports given back under pressure, "
             "waiting %llu", (unsigned long long)orphan_ago[0], (unsigned long long)MB(orphan_ago_b[0]), (unsigned long long)orphan_ago[1],
             (unsigned long long)MB(orphan_ago_b[1]), (unsigned long long)orphan_ago[2], (unsigned long long)MB(orphan_ago_b[2]),
             (unsigned long long)orphan_ago[3], (unsigned long long)MB(orphan_ago_b[3]), (unsigned long long)s.retired_chunks,
             (unsigned long long)MB(s.retired_chunk_bytes), (unsigned long long)s.evicted_chunks);
        std::sort(orphans.rbegin(), orphans.rend());
        for (size_t i = 0; i < orphans.size() && i < 8; i++) {
            const LedgerChunk& c = *orphans[i].second;
            logf("vk: ledger: chunk with no live pool %#llx %llu MB: import %s, mirror %s%s, bound %llu submissions ago, mirror synced %llu "
                 "mirror epochs ago", (unsigned long long)c.lo, (unsigned long long)MB(c.hi - c.lo), c.import ? "held" : "given back",
                 c.mirror ? "on" : "off", c.gpu_written ? ", written by the GPU" : "", (unsigned long long)c.bound_ago,
                 (unsigned long long)c.synced_ago);
        }
    }
    // render targets by shape
    {
        std::vector<std::pair<uint64_t, std::string>> rs;
        for (const auto& [k, v] : shapes) {
            const auto& [w, h, f, rel] = k;
            const char* n = fmt_name(f);
            char b[96];
            if (n) snprintf(b, sizeof b, " %ux%u %s x%llu %llu MB%s;", w, h, n, (unsigned long long)v.first, (unsigned long long)MB(v.second), rel ? " finalized" : "");
            else snprintf(b, sizeof b, " %ux%u fmt %u x%llu %llu MB%s;", w, h, f, (unsigned long long)v.first, (unsigned long long)MB(v.second), rel ? " finalized" : "");
            rs.push_back({v.second, b});
        }
        std::sort(rs.rbegin(), rs.rend());
        std::string line;
        for (size_t i = 0; i < rs.size() && i < 14; i++) line += rs[i].second;
        logf("vk: ledger: render targets by size and format (the largest):%s", line.c_str());
    }

    // VRAM: what belongs to no game resource, and the reconciliation with the driver's count
    uint64_t kb[(int)MemKind::Count][4]{}, kc[(int)MemKind::Count][4]{};
    mem_kind_totals(kb, kc);
    uint64_t v[8];
    vma_ledger_stats(v);
    auto vram = [&](MemKind k) { return kb[(int)k][0] + kb[(int)k][1]; };
    const uint64_t img_acct = vram(MemKind::Texture) + vram(MemKind::Target);
    const uint64_t img_other = img_acct > tex_vk ? img_acct - tex_vk : 0;
    const uint64_t mir_acct = vram(MemKind::Mirror);
    const uint64_t mir_live = mir_acct > s.retired_mirror_bytes ? mir_acct - s.retired_mirror_bytes : 0;
    uint64_t acct_vram = 0;
    for (int k = 0; k < (int)MemKind::Count; k++) acct_vram += kb[k][0] + kb[k][1];
    const uint64_t free_img = v[0] - std::min(v[0], v[1]), free_rt = v[2] - std::min(v[2], v[3]);
    const uint64_t free_all = v[4] - std::min(v[4], v[5]), free_buf = free_all - std::min(free_all, free_img + free_rt);
    const uint64_t outside = v[6] - std::min(v[6], v[4]);
    const uint64_t uncounted = v[5] > acct_vram ? v[5] - acct_vram : 0;
    const uint64_t vram_rows = acct_vram + free_all + outside + uncounted;
    logf("vk: ledger: VRAM, MB: texture images %llu (the table's image column), other images %llu (spare images %llu, render-target pool %llu, "
         "the rest: images of the menu, upscaler and present that VMA made), mirrors live %llu (the table's mirror column %llu) and retired "
         "%llu, write-back shadows %llu, streams %llu, staging %llu, readback %llu, scratch (BC encode) %llu, other buffers %llu",
         (unsigned long long)MB(tex_vk), (unsigned long long)MB(img_other), (unsigned long long)MB(s.spare_bytes),
         (unsigned long long)MB(s.rt_pool_bytes), (unsigned long long)MB(mir_live), (unsigned long long)MB(mir_all),
         (unsigned long long)MB(s.retired_mirror_bytes), (unsigned long long)MB(vram(MemKind::WbShadow)), (unsigned long long)MB(vram(MemKind::Stream)),
         (unsigned long long)MB(vram(MemKind::Staging)), (unsigned long long)MB(vram(MemKind::Readback)), (unsigned long long)MB(vram(MemKind::Scratch)),
         (unsigned long long)MB(vram(MemKind::Other) + vram(MemKind::Capture) + vram(MemKind::BcCache)));
    logf("vk: ledger: VRAM, MB: free in VMA blocks %llu (image blocks %llu, render-target blocks %llu, buffer blocks %llu); outside VMA %llu "
         "(pipelines, shader modules, descriptor pools, swapchain, the driver's own); VMA allocations not counted by kind %llu",
         (unsigned long long)MB(free_all), (unsigned long long)MB(free_img), (unsigned long long)MB(free_rt), (unsigned long long)MB(free_buf),
         (unsigned long long)MB(outside), (unsigned long long)MB(uncounted));
    logf("vk: ledger: VRAM reconciled: the rows %llu MB against the heaps' %llu MB used of %llu allowed (the VRAM breakdown's figure): "
         "remainder %lld MB. Of it the game's resources (images + live mirrors) %llu MB for %llu MB of console bytes: x%.2f",
         (unsigned long long)MB(vram_rows), (unsigned long long)MB(v[6]), (unsigned long long)MB(v[7]),
         (long long)(((int64_t)v[6] - (int64_t)vram_rows) / (1 << 20)), (unsigned long long)MB(tex_vk + mir_live),
         (unsigned long long)MB(sum_console), sum_console ? (double)(tex_vk + mir_live) / (double)sum_console : 0.0);

    // system memory
    auto sys = [&](MemKind k) { return kb[(int)k][2] + kb[(int)k][3]; };
    const uint64_t spill = spill_fn ? spill_fn() : 0;
    if (census) {
        const uint64_t win = T[T_WIN_RES], rest = T[T_RAM_COUNTED] > win ? T[T_RAM_COUNTED] - win : 0;
        logf("vk: ledger: RAM, MB: the game's memory %llu (its pools %llu, outside pools %llu; committed %llu); the back end's mapped host "
             "memory %llu (captures %llu, BC cache %llu in %llu entries, other %llu); C heaps %llu; images %llu (graphics drivers' %llu); "
             "thread stacks %llu; mapped files %llu; private with no owner %llu (of it allocations of 128 MB and up, the driver's arenas, %llu "
             "in RAM of %llu committed)",
             (unsigned long long)MB(win), (unsigned long long)MB(pool_res), (unsigned long long)MB(win > pool_res ? win - pool_res : 0),
             (unsigned long long)MB(T[T_WIN_COM]), (unsigned long long)MB(T[T_GROUP_RES + G_TAG]), (unsigned long long)MB(sys(MemKind::Capture)),
             (unsigned long long)MB(sys(MemKind::BcCache)), (unsigned long long)s.bcc_entries,
             (unsigned long long)MB(T[T_GROUP_RES + G_TAG] > sys(MemKind::Capture) + sys(MemKind::BcCache) ? T[T_GROUP_RES + G_TAG] - sys(MemKind::Capture) - sys(MemKind::BcCache) : 0),
             (unsigned long long)MB(T[T_GROUP_RES + G_HEAP]), (unsigned long long)MB(T[T_GROUP_RES + G_IMAGE] + T[T_GROUP_RES + G_DRIVER_IMAGE]),
             (unsigned long long)MB(T[T_GROUP_RES + G_DRIVER_IMAGE]), (unsigned long long)MB(T[T_GROUP_RES + G_STACK]),
             (unsigned long long)MB(T[T_GROUP_RES + G_MAPPED]), (unsigned long long)MB(T[T_GROUP_RES + G_OTHER]), (unsigned long long)MB(T[T_BIG_RES]),
             (unsigned long long)MB(T[T_BIG_COM]));
        logf("vk: ledger: RAM reconciled: counted %llu MB (the game's %llu, the rest %llu) against the working set %llu MB: remainder %lld MB; "
             "private commit %llu MB (the window %llu, beside VRAM and the arenas: private no owner %llu committed); VRAM mapped through the BAR "
             "(not RAM) %llu MB; the drop's spill on disk %llu MB; the census took %llu ms",
             (unsigned long long)MB(T[T_RAM_COUNTED]), (unsigned long long)MB(win), (unsigned long long)MB(rest), (unsigned long long)MB(T[T_WS]),
             (long long)(((int64_t)T[T_WS] - (int64_t)T[T_RAM_COUNTED]) / (1 << 20)), (unsigned long long)MB(T[T_PRIVATE]),
             (unsigned long long)MB(T[T_WIN_COM]), (unsigned long long)MB(T[T_GROUP_COM + G_OTHER]), (unsigned long long)MB(T[T_BAR]),
             (unsigned long long)MB(spill), (unsigned long long)T[T_WALK_MS]);
        logf("vk: ledger: ours against the console: VRAM %llu MB + RAM %llu MB = %llu MB, for the game's %llu MB of memory (committed %llu): x%.2f",
             (unsigned long long)MB(v[6]), (unsigned long long)MB(T[T_WS]), (unsigned long long)MB(v[6] + T[T_WS]), (unsigned long long)MB(win),
             (unsigned long long)MB(T[T_WIN_COM]), T[T_WIN_COM] ? (double)(v[6] + T[T_WS]) / (double)T[T_WIN_COM] : 0.0);
    } else
        logf("vk: ledger: RAM: no host census here (the executable lacks hos_hostsplit_totals, or not Windows); BC cache %llu MB, captures %llu MB",
             (unsigned long long)MB(sys(MemKind::BcCache)), (unsigned long long)MB(sys(MemKind::Capture)));
    size_t g = 0, used = 0;
    int64_t dropped = 0;
    res_pipeline_counts(&g, &used, &dropped);
    logf("vk: ledger: textures uploaded so far %lld (ASTC transcoded on the GPU %lld), %lld MB read from guest memory; graphics pipelines "
         "made %zu, bound %zu; the ledger took %lld ms",
         (long long)s.uploads, (long long)s.astc_gpu, (long long)(s.bytes_uploaded >> 20), g, used,
         (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

// ---- the thread ----
std::mutex mu;
std::condition_variable cv;
std::unique_ptr<LedgerSnap> pending, last;
double pending_secs = 0, last_secs = 0;
bool busy = false;
std::chrono::steady_clock::time_point start;

void thread_main() {
    for (;;) {
        std::unique_ptr<LedgerSnap> s;
        double secs;
        {
            std::unique_lock lk(mu);
            cv.wait(lk, [] { return pending != nullptr; });
            s = std::move(pending), secs = pending_secs, busy = true;
        }
        count(*s, "ledger", secs);
        std::lock_guard lk(mu);
        last = std::move(s), last_secs = secs, busy = false;
        cv.notify_all();
    }
}
void at_exit() {   // (a normal exit: the last snapshot counted again, with the pages and the census as they are now)
    std::unique_lock lk(mu);
    if (!cv.wait_for(lk, std::chrono::seconds(5), [] { return !busy && !pending; }) || !last) return;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    count(*last, "ledger at exit (the last snapshot's resources)", secs);
}
}  // namespace

void ledger_frame() {
    if (!ledger_on()) return;
    static bool started = false;
    static double next = 0;
    const auto now = std::chrono::steady_clock::now();
    if (!started) {
        started = true, start = now, next = ledger_secs();
        std::thread(thread_main).detach();
        atexit(at_exit);
        logf("vk: ledger on (RX_VK_LEDGER=%d): a table every %d s and at exit, lines \"vk: ledger:\"", ledger_secs(), ledger_secs());
        return;
    }
    const double secs = std::chrono::duration<double>(now - start).count();
    if (secs < next) return;
    {
        std::lock_guard lk(mu);
        if (busy || pending) return;   // (the last one still counting: the next present tries again)
    }
    next = secs + ledger_secs();
    auto s = std::make_unique<LedgerSnap>();
    texture_ledger_snapshot(*s);
    memory_ledger_snapshot(*s);
    std::lock_guard lk(mu);
    pending = std::move(s), pending_secs = secs;
    cv.notify_all();
}

} // namespace mkwvk
