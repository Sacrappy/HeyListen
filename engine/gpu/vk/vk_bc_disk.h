// Copyright (C) 2026 Sacrappy
// TEXCACHE (after perf/bc7c's BC7C): the BC blocks of transcoded ASTC textures (vk_texture.cpp's BC cache key, TICK-051) kept on
// disk between sessions, so a texture seen in an earlier session needs no ASTC decode and no BC encode on the GPU.
// RX_VK_BC_DISK=1 (off by default). The packs' rules (format, eviction, read-ahead order) are texcache_pack.h's.
//
// The render thread never waits for the disk. One thread of its own does all the file work: the scan that builds the index at
// start, the reads ahead (the records after one the render thread asked for, in creation order: a zone's textures), the writes
// (at low IO and CPU priority) and the copies forward. A read ahead lands in a bounded pool of host buffers (vk_texture.cpp's
// sink); the render thread only takes a buffer already there (take()) and asks for the records after it (probe()): a texture
// whose blocks are not in the pool yet is transcoded as without the cache. Writes are bounded too (RX_VK_BC_DISK_WRITE_MB of
// blocks waiting): past that a transcode is not written (it will be in a later session).
// The files are made at runtime from the player's own game, in their own folder: never in the repo, a release or a package.
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

#include "texcache_pack.h"

namespace mkwvk::bcdisk {

// (TEXCACHE) RX_VK_BC_DISK=1 / 0 (unset: on for integrated GPUs that transcode ASTC and discrete cards under 10 GB of VRAM, texcache_pack.h on_by_default): the BC blocks of transcoded ASTC textures kept on disk between sessions, read ahead off the render thread
/// -1: RX_VK_BC_DISK unset (the device decides), else 0 or 1
inline int env_choice() {
    static const int v = getenv("RX_VK_BC_DISK") && *getenv("RX_VK_BC_DISK") ? (atoi(getenv("RX_VK_BC_DISK")) != 0) : -1;
    return v;
}
inline uint64_t env_mb(const char* name, int def, int floor) {
    const char* e = getenv(name);
    return (uint64_t)std::max(floor, e && *e ? atoi(e) : def) << 20;
}

struct Store {
    std::mutex mu;   // the index; held for lookups and bookkeeping only, never across a disk access
    texcache::Index ix;
    std::unordered_set<uint64_t> queued;   // (mu) keys with a write or copy forward waiting
    std::filesystem::path path[2];
    uint64_t encoder = 0;
    std::atomic<bool> ready{false};        // the scan is done
    texcache::AheadPlan ap;                // (mu) the read-ahead's runs
    // (mu) the maps' lists (texcache_pack.h MapLists): the map now, the keys appended to it in the last seconds (moved to the next
    // map when its name comes), and the map whose list is being read (from mr_pos; paused while the pool is full)
    texcache::MapLists ml;
    std::filesystem::path maps_path;
    std::string map_name;
    int map_cur = -1, mr_map = -1;
    size_t mr_pos = 0;
    bool mr_paused = false, ml_dirty = false;
    uint64_t mr_gen = 0;
    std::deque<std::pair<int64_t, uint64_t>> recent;   // (steady ms, key)
    std::atomic<int64_t> st_map_changes{0}, st_map_read{0}, st_map_read_bytes{0}, st_map_full{0}, st_map_listed{0};
    // counters (the log line)
    std::atomic<int64_t> st_hits{0}, st_hit_bytes{0}, st_probe_found{0}, st_probe_missing{0}, st_written{0}, st_write_bytes{0};
    std::atomic<int64_t> st_bad{0}, st_ahead{0}, st_ahead_bytes{0}, st_ahead_full{0}, st_forward{0}, st_rotations{0};
    std::atomic<int64_t> st_write_skipped{0}, st_scan_ms{0}, st_stale_deleted{0}, st_read_ms{0}, st_write_ms{0};
    std::atomic<int64_t> write_pending{0};   // bytes handed to the writer and not yet written
    std::atomic<bool> write_off{false};     // writes stopped for this session (an IO error, or the disk's last GB): reads go on
    std::atomic<const char*> write_off_why{nullptr};
    uint64_t write_cap = 0;
    // the thread's work
    std::mutex wq_mu;
    std::condition_variable wq_cv;
    struct Job {
        int kind;   // 0 write, 1 copy forward, 2 read ahead, 4 read the map's list, 5 save the maps' lists
        uint64_t key;
        const uint8_t* data;
        uint32_t size;
        std::function<void()> done;
        uint32_t from, count;
        std::vector<uint8_t> blob = {};
    };
    std::deque<Job> wq;     // writes and copies forward
    std::deque<Job> aq;     // reads ahead (they go first: a zone is loading)
    std::ifstream rd[2];
    uint64_t rd_seq[2] = {~0ull, ~0ull};
    std::fstream wr;
    int wr_gen = -1;
    // set by vk_texture.cpp: a pool buffer of `size` bytes made and filled by fill(ptr) (false: pool full or the fill failed)
    std::function<int(uint64_t key, uint32_t size, const std::function<bool(uint8_t*)>& fill)> sink;   // 1 put, 0 full, -1 failed
    std::function<bool(uint64_t key)> have;   // in the pool already
};

inline Store& store() {
    static Store* s = new Store;   // (never destroyed: its thread outlives the statics)
    return *s;
}

inline int64_t ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

/// (the thread) one record's blocks into out, hash checked
inline bool read_blocks(Store& s, const texcache::Loc& l, uint64_t pack_seq, uint8_t* out) {
    std::ifstream& f = s.rd[l.gen];
    if (!f.is_open() || s.rd_seq[l.gen] != pack_seq) {
        f.close();
        f.clear();
        f.open(s.path[l.gen], std::ios::binary);
        s.rd_seq[l.gen] = pack_seq;
    }
    if (!f) return false;
    f.clear();
    f.seekg((std::streamoff)l.at);
    f.read((char*)out, l.size);
    return f && (uint64_t)f.gcount() == l.size && texcache::hash_blocks(out, l.size) == l.hash;
}

/// (the thread) the index from both packs; packs of another format or encoder are deleted, torn tails cut
inline void scan(Store& s) {
    const auto t0 = std::chrono::steady_clock::now();
    bool valid[2] = {false, false};
    uint64_t seqs[2] = {0, 0}, ends[2] = {0, 0};
    std::vector<texcache::Rec> recs[2];
    for (int g = 0; g < 2; g++) {
        std::error_code ec;
        if (!std::filesystem::exists(s.path[g], ec)) continue;
        const uint64_t end = std::filesystem::file_size(s.path[g], ec);
        std::ifstream f(s.path[g], std::ios::binary);
        texcache::FileHeader h{};
        if (ec || !f || !f.read((char*)&h, sizeof h) || !texcache::header_ok(h, s.encoder)) {
            f.close();
            std::filesystem::remove(s.path[g], ec);   // (another encoder's or a broken one: gone)
            s.st_stale_deleted++;
            continue;
        }
        const uint64_t keep = texcache::scan_records(
            [&](uint64_t at, void* dst, uint64_t n) {
                f.clear();
                f.seekg((std::streamoff)at);
                return (bool)f.read((char*)dst, (std::streamsize)n);
            },
            end, recs[g]);
        f.close();
        if (keep < end) std::filesystem::resize_file(s.path[g], keep, ec);   // (a torn tail)
        valid[g] = true, seqs[g] = h.seq, ends[g] = keep;
    }
    std::vector<uint8_t> mb;
    {
        std::error_code ec;
        std::ifstream f(s.maps_path, std::ios::binary);
        if (f) mb.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    {
        std::lock_guard lk(s.mu);
        s.ix.load(valid, seqs, ends, recs);
        if (!s.ml.load(mb.data(), mb.size(), s.encoder)) s.ml = texcache::MapLists{};
        if (!s.map_name.empty()) {   // (a map seen before the scan ended: its list is read now)
            s.map_cur = s.ml.enter(s.map_name);
            s.mr_map = s.map_cur, s.mr_pos = 0, s.mr_gen++;
        }
    }
    s.st_scan_ms = ms_since(t0);
    s.ready.store(true, std::memory_order_release);
}

/// (the thread) a record appended to the current pack (rotating first when it is full); false when the file failed
inline bool append(Store& s, uint64_t key, const uint8_t* data, uint32_t size) {
    int gen;
    uint64_t at, seq;
    {
        std::lock_guard lk(s.mu);
        if (!s.ix.wants(key)) return true;
        if (s.ix.rotate_due(size)) {   // (the older pack goes; the appends go to a new one in its place)
            const int o = 1 - s.ix.cur;
            s.wr.close(), s.wr_gen = -1;
            s.rd[o].close(), s.rd_seq[o] = ~0ull;
            std::error_code ec;
            std::filesystem::remove(s.path[o], ec);
            s.ix.rotate();
            s.ap.reset();
            s.st_rotations++;
        }
        gen = s.ix.cur, at = s.ix.append_at(), seq = s.ix.seq[gen];
    }
    // (the file work with s.mu released: the render thread's lookups never wait for the disk)
    {   // (never the disk's last GB: past it, no more writes this session)
        std::error_code ec;
        const std::filesystem::space_info sp = std::filesystem::space(s.path[gen].parent_path(), ec);
        if (!ec && !texcache::room_to_write(sp.available, (uint64_t)size + sizeof(texcache::RecHeader) + sizeof(texcache::FileHeader))) {
            s.write_off_why = "less than 1 GB free on its disk";
            s.write_off = true;
            return false;
        }
    }
    if (s.wr_gen != gen) {
        s.wr.close();
        std::error_code ec;
        std::filesystem::create_directories(s.path[gen].parent_path(), ec);
        const bool fresh = at == sizeof(texcache::FileHeader);
        s.wr.open(s.path[gen], std::ios::binary | std::ios::in | std::ios::out | (fresh ? std::ios::trunc : std::ios::openmode{}));
        if (!s.wr) {   // (a read-only folder, a locked file: no writes this session, the cache still read)
            s.write_off_why = "its folder could not be written";
            s.write_off = true;
            return false;
        }
        s.wr_gen = gen;
        if (fresh) {
            const texcache::FileHeader h = texcache::make_header(s.encoder, seq);
            s.wr.write((const char*)&h, sizeof h);
        }
    }
    const texcache::RecHeader r = texcache::make_record(key, data, size);
    s.wr.seekp((std::streamoff)at);
    s.wr.write((const char*)&r, sizeof r);
    s.wr.write((const char*)data, size);
    s.wr.flush();
    if (!s.wr) {   // (a write error, a full disk: the record's tail is cut at the next scan; no writes this session)
        s.wr.close(), s.wr_gen = -1;
        s.write_off_why = "a write failed";
        s.write_off = true;
        return false;
    }
    std::lock_guard lk(s.mu);
    if (s.ix.cur == gen && s.ix.append_at() == at) s.ix.appended(key, size, r.hash);
    return true;
}

// (TEXCACHE) RX_VK_BC_DISK_WRITE_BG=1 (0 by default): the BC disk cache's writes in Windows' background mode (low IO and CPU priority);
// 0: normal IO priority, the thread's CPU priority lowest. Laptop list 2: background mode left 212 transcodes unwritten in a cold run
// (backlog full), normal priority 2, and the next warm run's uploads while turning were 39 ms a frame against 82
inline bool write_bg() {
    static const bool v = getenv("RX_VK_BC_DISK_WRITE_BG") && atoi(getenv("RX_VK_BC_DISK_WRITE_BG")) != 0;
    return v;
}
inline void background(bool on) {
#ifdef _WIN32
    if (!write_bg()) {
        SetThreadPriority(GetCurrentThread(), on ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_NORMAL);
        return;
    }
    SetThreadPriority(GetCurrentThread(), on ? THREAD_MODE_BACKGROUND_BEGIN : THREAD_MODE_BACKGROUND_END);
#else
    (void)on;
#endif
}

inline void run(Store* sp) {
    Store& s = *sp;
    scan(s);   // (at normal priority: headers only; in background mode it took 7.5 s over 2 GB on the main PC, cache unused meanwhile)
    {   // (a run that starts inside a map (a save loaded there) entered it before the scan ended: its list is read now. texcache4
        // read nothing from the list in such runs: the laptop's list 3, "0 read from the map's list" with 547-601 keys listed)
        bool first;
        {
            std::lock_guard lk(s.mu);
            first = s.mr_map >= 0;
        }
        if (first) {
            std::lock_guard lk(s.wq_mu);
            s.aq.push_front({4, 0, nullptr, 0, {}, 0, 0});
        }
    }
    background(true);   // (the writes at low IO and CPU priority: the game reads its own files meanwhile)
    bool bg = true;
    for (;;) {
        Store::Job j{};
        {
            std::unique_lock lk(s.wq_mu);
            s.wq_cv.wait(lk, [&] { return !s.wq.empty() || !s.aq.empty(); });
            if (!s.aq.empty()) j = s.aq.front(), s.aq.pop_front();
            else j = s.wq.front(), s.wq.pop_front();
        }
        if (j.kind == 4) {   // the map's list, in its first-use order, at normal priority (a zone is loading)
            if (bg) background(false), bg = false;
            const auto t0 = std::chrono::steady_clock::now();
            for (;;) {
                std::vector<uint64_t> keys;
                uint64_t gen;
                {
                    std::lock_guard lk(s.mu);
                    if (s.mr_map < 0 || (size_t)s.mr_map >= s.ml.maps.size()) break;
                    const std::vector<uint64_t>& all = s.ml.maps[(size_t)s.mr_map].keys;
                    if (s.mr_pos >= all.size()) {
                        s.mr_map = -1;   // (done: the creation-order read-ahead may run again)
                        break;
                    }
                    keys.assign(all.begin() + (ptrdiff_t)s.mr_pos, all.begin() + (ptrdiff_t)std::min(all.size(), s.mr_pos + 32));
                    gen = s.mr_gen;
                }
                size_t done = 0;
                bool full = false;
                for (uint64_t key : keys) {
                    if (!(s.have && s.have(key))) {
                        texcache::Loc l;
                        uint64_t seq;
                        bool on = false;
                        {
                            std::lock_guard lk(s.mu);
                            if (const texcache::Loc* q = s.ix.find(key)) l = *q, seq = s.ix.seq[l.gen], on = true;
                        }
                        if (on) {
                            const int r = s.sink ? s.sink(key, l.size, [&](uint8_t* out) { return read_blocks(s, l, seq, out); }) : 0;
                            if (r == 0) { full = true; break; }
                            if (r < 0) s.st_bad++;
                            else s.st_map_read++, s.st_map_read_bytes += l.size;
                        }
                    }
                    done++;
                }
                std::lock_guard lk(s.mu);
                if (gen != s.mr_gen) continue;   // (another map meanwhile: its list from its start)
                s.mr_pos += done;
                if (full) {
                    s.mr_paused = true;   // (took() goes on once textures took blocks)
                    s.st_map_full++;
                    break;
                }
            }
            s.st_read_ms += ms_since(t0);
            continue;
        }
        if (j.kind == 5) {   // the maps' lists to their file (whole, through a temporary one)
            std::error_code ec;
            std::filesystem::create_directories(s.maps_path.parent_path(), ec);
            std::filesystem::path tmp = s.maps_path;
            tmp += ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                f.write((const char*)j.blob.data(), (std::streamsize)j.blob.size());
            }
            std::filesystem::rename(tmp, s.maps_path, ec);
            continue;
        }
        if (j.kind == 2) {   // read ahead at normal priority: a zone is loading and its textures are wanted now
            if (bg) background(false), bg = false;
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<uint64_t> keys;
            {
                std::lock_guard lk(s.mu);
                keys = s.ix.ahead(j.from, j.count);
            }
            for (uint64_t key : keys) {
                if (s.have && s.have(key)) continue;
                texcache::Loc l;
                uint64_t seq;
                {
                    std::lock_guard lk(s.mu);
                    const texcache::Loc* p = s.ix.find(key);
                    if (!p) continue;
                    l = *p, seq = s.ix.seq[l.gen];
                }
                const int r = s.sink ? s.sink(key, l.size, [&](uint8_t* out) { return read_blocks(s, l, seq, out); }) : 0;
                if (r == 0) { s.st_ahead_full++; break; }   // (the pool is full: the rest waits for the next ask)
                if (r < 0) { s.st_bad++; continue; }
                s.st_ahead++, s.st_ahead_bytes += l.size;
            }
            s.st_read_ms += ms_since(t0);
            continue;
        }
        if (!bg) background(true), bg = true;
        const auto t0 = std::chrono::steady_clock::now();
        if (j.kind == 0) {
            if (append(s, j.key, j.data, j.size)) s.st_written++, s.st_write_bytes += j.size;
            if (j.done) j.done();
            s.write_pending -= j.size;
        } else {   // copy forward: a record of the older pack used again, appended to the current one
            texcache::Loc l;
            uint64_t seq;
            bool ok = false;
            {
                std::lock_guard lk(s.mu);
                const texcache::Loc* p = s.ix.find(j.key);
                if (p && p->gen != s.ix.cur) l = *p, seq = s.ix.seq[l.gen], ok = true;
            }
            if (ok) {
                std::vector<uint8_t> b(l.size);
                if (read_blocks(s, l, seq, b.data()) && append(s, j.key, b.data(), l.size)) s.st_forward++;
            }
        }
        {
            std::lock_guard lk(s.mu);
            s.queued.erase(j.key);
        }
        s.st_write_ms += ms_since(t0);
    }
}

/// Starts the index scan and the thread (once, from the texture setup); false when off
inline bool start(uint64_t encoder, const std::filesystem::path& game_dir) {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [&] {
        Store& s = store();
        std::filesystem::path dir = game_dir;
        // (TEXCACHE) RX_VK_BC_DISK_DIR=<folder> (the game folder by default): where the two pack files of the BC disk cache live (made at runtime from your own game: never share or ship them)
        if (const char* e = getenv("RX_VK_BC_DISK_DIR"); e && *e) {
            const std::string v(e);
            dir = std::filesystem::path(std::u8string(v.begin(), v.end()));
        }
        s.encoder = encoder;
        // (TEXCACHE) RX_VK_BC_DISK_MB=<MB> (4096 by default): the BC disk cache's size on disk, both packs: a rolling window of the
        // zones last played (a 6-zone tour wrote 2 GB, 150 s of Hyrule Field 0.75-0.85 GB); the older half goes when the newer one is
        // full, what was used again copied forward first. Its folder also keeps 1 GB free at least
        s.ix.cap = env_mb("RX_VK_BC_DISK_MB", 4096, 64);
        // (TEXCACHE) RX_VK_BC_DISK_WRITE_MB=<MB> (256 by default): blocks waiting for the BC disk cache's writer at most; transcodes past it are not written
        s.write_cap = env_mb("RX_VK_BC_DISK_WRITE_MB", 256, 16);
        s.path[0] = dir / "hookshot_texcache_a.pack";
        s.path[1] = dir / "hookshot_texcache_b.pack";
        s.maps_path = dir / "hookshot_texcache_maps.bin";
        std::thread(run, &s).detach();
        ok = true;
    });
    return ok;
}

/// (render thread) a transcode's blocks are worth writing: not on disk (or only in the older pack), not queued, and the
/// writer's backlog has room. True reserves the room: submit() or unreserve() follows
inline bool wants(uint64_t key, uint32_t size) {
    Store& s = store();
    if (!s.ready.load(std::memory_order_acquire) || s.write_off.load(std::memory_order_relaxed)) return false;
    if (s.write_pending.load(std::memory_order_relaxed) + size > (int64_t)s.write_cap) {
        s.st_write_skipped++;
        return false;
    }
    {
        std::lock_guard lk(s.mu);
        if (!s.ix.wants(key) || !s.queued.insert(key).second) return false;
    }
    s.write_pending += size;
    return true;
}
inline void unreserve(uint64_t key, uint32_t size) {
    Store& s = store();
    s.write_pending -= size;
    std::lock_guard lk(s.mu);
    s.queued.erase(key);
}
/// The blocks (valid until done() is called on the thread) written
inline void submit(uint64_t key, const uint8_t* data, uint32_t size, std::function<void()> done) {
    Store& s = store();
    {
        std::lock_guard lk(s.wq_mu);
        s.wq.push_back({0, key, data, size, std::move(done), 0, 0});
    }
    s.wq_cv.notify_one();
}

// (TEXCACHE) RX_VK_BC_DISK_AHEAD=<records> (96 by default): records read ahead (creation order) after one the render thread found on disk
inline uint32_t ahead_count() {
    static const uint32_t v = getenv("RX_VK_BC_DISK_AHEAD") ? (uint32_t)std::max(1, atoi(getenv("RX_VK_BC_DISK_AHEAD"))) : 96;
    return v;
}

/// (render thread, after the pool had no blocks for key) whether the disk has them; if so the records after it are read ahead
/// (this texture is transcoded now; the rest of its zone comes in from the pool). Never waits for the disk: an index lookup
/// under a mutex the thread holds for bookkeeping only
inline bool probe(uint64_t key, uint32_t size) {
    Store& s = store();
    if (!s.ready.load(std::memory_order_acquire)) return false;
    uint32_t from = 0, count = 0;
    bool ask;
    {
        std::lock_guard lk(s.mu);
        const texcache::Loc* l = s.ix.find(key);
        if (!l || l->size != size) {
            s.st_probe_missing++;
            return false;
        }
        s.st_probe_found++;
        ask = s.mr_map < 0 && s.ap.next(l->order, ahead_count(), from, count);
    }
    if (ask) {
        {
            std::lock_guard lk(s.wq_mu);
            s.aq.push_back({2, 0, nullptr, 0, {}, from, count});
        }
        s.wq_cv.notify_one();
    }
    return true;
}

/// (JOBS1, render thread) a record's blocks read here and now into out (size bytes), hash checked, on a file handle of its own (the
/// thread's stay its own); false when the disk does not have it, the pack rotated meanwhile, or the read failed. For the large
/// textures whose transcode costs an integrated GPU seconds (vk_texture.cpp bcd_sync_wanted)
inline bool read_now(uint64_t key, uint32_t size, uint8_t* out) {
    Store& s = store();
    if (!s.ready.load(std::memory_order_acquire)) return false;
    texcache::Loc l;
    uint64_t seq;
    {
        std::lock_guard lk(s.mu);
        const texcache::Loc* p = s.ix.find(key);
        if (!p || p->size != size) return false;
        l = *p, seq = s.ix.seq[l.gen];
    }
    std::ifstream f(s.path[l.gen], std::ios::binary);
    if (!f) return false;
    f.seekg((std::streamoff)l.at);
    f.read((char*)out, l.size);
    if (!f || (uint64_t)f.gcount() != l.size || texcache::hash_blocks(out, l.size) != l.hash) return false;
    std::lock_guard lk(s.mu);
    return s.ix.seq[l.gen] == seq;   // (the same pack still: its records were not overwritten by a rotation)
}

/// (render thread, a new texture about to be held back) whether the disk has its blocks; if so its record is read before anything
/// else, and the run after it asked for
inline bool want_now(uint64_t key, uint32_t size) {
    Store& s = store();
    if (!s.ready.load(std::memory_order_acquire)) return false;
    uint32_t order, from = 0, count = 0;
    bool ask;
    {
        std::lock_guard lk(s.mu);
        const texcache::Loc* l = s.ix.find(key);
        if (!l || l->size != size) return false;
        order = l->order;
        ask = s.ap.next(order, ahead_count(), from, count);
    }
    {
        std::lock_guard lk(s.wq_mu);
        s.aq.push_front({2, 0, nullptr, 0, {}, order, 1});   // (this one first)
        if (ask) s.aq.push_back({2, 0, nullptr, 0, {}, from, count});
    }
    s.wq_cv.notify_one();
    return true;
}

/// (render thread) a record taken from the pool: counted, copied forward when it lies in the older pack, and the read-ahead
/// kept a run in front of it
inline void took(uint64_t key, uint32_t size) {
    Store& s = store();
    s.st_hits++, s.st_hit_bytes += size;
    bool forward = false, ask = false, resume = false;
    uint32_t from = 0, count = 0;
    {
        std::lock_guard lk(s.mu);
        const texcache::Loc* l = s.ix.find(key);
        if (!l) return;
        if (l->gen != s.ix.cur && s.queued.insert(key).second) forward = true;
        ask = s.mr_map < 0 && s.ap.next(l->order, ahead_count(), from, count);
        if (s.mr_paused) s.mr_paused = false, resume = true;
    }
    if (!forward && !ask && !resume) return;
    {
        std::lock_guard lk(s.wq_mu);
        if (forward) s.wq.push_back({1, key, nullptr, 0, {}, 0, 0});
        if (ask) s.aq.push_back({2, 0, nullptr, 0, {}, from, count});
        if (resume) s.aq.push_front({4, 0, nullptr, 0, {}, 0, 0});
    }
    s.wq_cv.notify_one();
}

/// (render thread, every few presents) the host's map name ("" before a save is loaded): on a change, the keys appended to the old
/// map in the last 3 s go to the new one, and the new map's list is read before anything else
inline void set_map(const char* name) {
    Store& s = store();
    if (!name || !*name) return;
    std::vector<uint8_t> blob;
    {
        std::lock_guard lk(s.mu);
        if (s.map_name == name) return;
        s.map_name = name;
        if (!s.ready.load(std::memory_order_acquire)) return;   // (the scan enters it when it ends)
        const int old = s.map_cur;
        s.map_cur = s.ml.enter(name);
        std::vector<uint64_t> moved;
        const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        for (const auto& [t, key] : s.recent)
            if (now - t <= 3000) moved.push_back(key);
        s.recent.clear();
        s.ml.move(old, s.map_cur, moved);
        s.mr_map = s.map_cur, s.mr_pos = 0, s.mr_paused = false, s.mr_gen++;
        s.st_map_changes++;
        if (s.ml_dirty) blob = s.ml.save(s.encoder), s.ml_dirty = false;
    }
    {
        std::lock_guard lk(s.wq_mu);
        s.aq.push_front({4, 0, nullptr, 0, {}, 0, 0});
        if (!blob.empty()) s.wq.push_back({5, 0, nullptr, 0, {}, 0, 0, std::move(blob)});
    }
    s.wq_cv.notify_one();
}
/// (render thread) a texture of key wanted on the map now: appended to its list when new there
inline void use(uint64_t key) {
    Store& s = store();
    if (!s.ready.load(std::memory_order_acquire)) return;
    std::lock_guard lk(s.mu);
    if (s.map_cur < 0 || !s.ml.note(s.map_cur, key)) return;
    s.ml_dirty = true, s.st_map_listed++;
    const int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    s.recent.emplace_back(now, key);
    while (!s.recent.empty() && now - s.recent.front().first > 3000) s.recent.pop_front();
}
/// (render thread, now and then) the maps' lists saved when they changed
inline void save_maps() {
    Store& s = store();
    if (!s.ready.load(std::memory_order_acquire)) return;
    std::vector<uint8_t> blob;
    {
        std::lock_guard lk(s.mu);
        if (!s.ml_dirty) return;
        blob = s.ml.save(s.encoder), s.ml_dirty = false;
    }
    {
        std::lock_guard lk(s.wq_mu);
        s.wq.push_back({5, 0, nullptr, 0, {}, 0, 0, std::move(blob)});
    }
    s.wq_cv.notify_one();
}

inline uint64_t disk_bytes() {
    Store& s = store();
    std::lock_guard lk(s.mu);
    return s.ix.total();
}

}  // namespace mkwvk::bcdisk
